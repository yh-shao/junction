# shaofs / Caladan Storage Quota Policy

Last updated: 2026-05-29

本文档梳理当前 Caladan storage quota 机制的实现状态。该机制位于 Caladan runtime 与 iokernel 之间，作用对象是使用 `lib/caladan/runtime/storage.c` 发起的 SPDK storage IO。shaofs 通过 storage 层访问 NVMe，因此也被该机制覆盖。

重要结论：

1. 当前机制已经显式区分 **elastic / work-conserving** 与 **hard_cap** 两种模式。
2. `quota_enabled = false` 可以一键关闭 runtime 侧所有 quota admission。
3. 默认 `storage_quota_mode = elastic`，保留 adaptive refill 和 global pool borrow，用于低负载下充分使用空闲硬件能力。
4. `storage_quota_mode = hard_cap` 会禁止 global pool borrow，并在 iokernel refill 阶段把每个 task 的周期额度限制到 `storage_quota_iops` / `storage_quota_bytes` 配置值。
5. 单 task 实验显示 hard_cap 可以把 throughput / IOPS 稳定限制到目标附近，且默认 elastic 模式相对 quota disabled 没有可观性能退化。

## 1. 代码位置

核心文件：

- `lib/caladan/inc/iokernel/quota.h`
  - 定义共享 quota 数据结构、策略常量。
- `lib/caladan/runtime/quota.c`
  - runtime 侧 admission、local token cache、shared bucket 拉取、global pool borrow、等待与 refund。
- `lib/caladan/runtime/storage.c`
  - storage IO 入口处调用 quota admission。
- `lib/caladan/iokernel/quota.c`
  - iokernel 侧周期性 refill、动态目标计算、优先级分配、global pool 更新。
- `lib/caladan/iokernel/main.c`
  - iokernel 主循环中周期性调用 `refillQuota()`。
- `lib/caladan/runtime/cfg.c`
  - 解析 quota 配置项。
- `lib/caladan/runtime/ioqueues.c`
  - runtime 初始化时分配 `runtime_info` 并调用 `storage_quota_init_runtime()`。

相关测试：

- `junction/fs/mytest/test_shaofs_quota_rate.c`
  - 当前用于验证 quota 是否影响单 task 内 storage IO 速率的微基准。

## 2. 设计目标

当前 quota 策略试图解决的问题是：当多个 Junction runtime / task 同时访问 SPDK NVMe 时，iokernel 能够按 task 需求、优先级和全局硬件容量给每个 task 分配 IO token，避免所有 task 无控制地直接冲击设备。

该策略强调两个目标：

1. Work conservation
   - 系统负载较低时，允许活跃 task 使用闲置硬件能力，避免因为静态限额导致设备空闲。
   - 这正是 global pool borrow 的设计意图。

2. Adaptive sharing
   - iokernel 根据每个 task 最近一个 refill 周期内上报的 demand 计算下一周期的 quota。
   - 当总需求低于硬件可分配能力时，按目标需求分配；当总需求超过硬件能力时，按优先级和比例进行裁剪。

它当前仍没有完整实现：

1. 显式 per-application SLA API。
2. 基于事件的精确唤醒。
3. 多 task 场景下的完整公平性实验验证。
4. 跨 runtime 的优先级隔离与突发流量评估。

## 3. 策略常量

定义位置：`lib/caladan/inc/iokernel/quota.h`

当前主要常量如下：

```c
#define REFILL_TIME       0.1
#define TO_US             1000000ULL

#define HARDWARE_IOPS     1721252
#define HARDWARE_BW       7037 * 1024 * 1024ULL

#define DEFAULT_RATIO     0.001

#define GLOBALPOOL_RATIO  0.2
#define TOTAL_IOPS_ALLOCABLE \
    (HARDWARE_IOPS * REFILL_TIME * (1.0 - GLOBALPOOL_RATIO))
#define TOTAL_BW_ALLOCABLE \
    (HARDWARE_BW * REFILL_TIME * (1.0 - GLOBALPOOL_RATIO))

#define ALPHA             0.4
#define R1                0.6
#define R2                0.85
#define G_MAX             1
#define G_MIN             -0.5
#define THETA             0.2
#define VIP_RSV           0.1

#define BORROW_BATCH      0.1
#define QUOTA_DEFAULT_REFILL_US ((uint64_t)(REFILL_TIME * TO_US))
```

含义：

- `REFILL_TIME = 0.1`
  - iokernel quota refill 的逻辑周期，当前为 100 ms。
- `HARDWARE_IOPS`
  - 配置中的硬件 IOPS 上限，当前为 `1721252`。
- `HARDWARE_BW`
  - 配置中的硬件带宽上限，当前为 `7378829312 B/s`，约 `7037 MiB/s`。
- `GLOBALPOOL_RATIO = 0.2`
  - 理论上预留 20% 给 global pool / 弹性空间。
- `TOTAL_*_ALLOCABLE`
  - iokernel 正常分配阶段使用的可分配资源，等于硬件能力的 80% 乘以周期长度。
- `ALPHA = 0.4`
  - EWMA 中历史值权重；当前 demand 权重为 `1 - ALPHA = 0.6`。
- `R1 = 0.6`, `R2 = 0.85`
  - 动态扩缩容的低/高阈值。
- `G_MAX = 1`, `G_MIN = -0.5`
  - 最大扩容系数 `+100%`，最大缩容系数 `-50%`。
- `THETA = 0.2`
  - 过载时低优先级侧的保留比例参数。
- `VIP_RSV = 0.1`
  - runtime 从 global pool borrow 时保留的水位参数。
- `BORROW_BATCH = 0.1`
  - runtime 从 shared bucket 或 global pool 批量取 token 时，默认批量为当前 quota 的 10%，同时不小于本次 IO 的 immediate need。

注意：`quota.h` 中的宏是编译期常量。当前配置项 `storage_quota_refill_us` 不会改变 iokernel 主循环中调用 `refillQuota()` 的周期；iokernel 仍使用 `QUOTA_DEFAULT_REFILL_US`。

## 4. 数据结构

### 4.1 QuotaDim

定义位置：`lib/caladan/inc/iokernel/quota.h`

```c
typedef struct {
    volatile int64_t bucket;
    volatile int64_t quota;
    volatile uint64_t epoch;
    volatile int64_t demand;
    volatile int64_t last_demand;

    double ewma;
    double target;
    int    init;
} QuotaDim;
```

字段含义：

- `bucket`
  - runtime 可从该共享桶批量领取的 token 数。
  - iokernel 每次 refill 后重置它。
  - runtime 不在每次 IO 时直接原子扣 shared bucket，而是先批量拉到 per-thread local cache，降低共享写竞争。

- `quota`
  - 当前周期分配给该 task 的 quota。
  - runtime 用它决定批量拉 token 的 batch size。
  - iokernel 用它计算 `R = ewma / oldQuota`，辅助判断下一周期是否需要扩缩容。

- `epoch`
  - iokernel 每次 refill 该维度后递增。
  - runtime local cache 通过比较 epoch 感知 shared quota 是否已经更新。

- `demand`
  - runtime 每次申请 IO admission 时上报的需求。
  - iokernel refill 时通过 atomic exchange 读取并清零。

- `last_demand`
  - iokernel 上一次 refill 观察到的 demand。
  - hard_cap 模式使用它判断该维度是否仍有活跃需求；有需求时直接按配置 cap 发放本周期 token，避免 adaptive EWMA 在低 cap 下造成长期欠发放。

- `ewma`
  - iokernel 私有管理字段，保存历史平滑 demand。

- `target`
  - iokernel 计算出的下一周期理想 quota。

- `init`
  - 该维度是否已经初始化过 adaptive state。

### 4.2 QuotaInfo

定义位置：`lib/caladan/inc/iokernel/quota.h`

```c
typedef struct {
    int         priority;
    atomic64_t enabled;
    atomic64_t mode;
    spinlock_t lock;
    QuotaDim   iops;
    QuotaDim   bytes;
    int64_t    iops_cap;
    int64_t    bytes_cap;
    atomic64_t wake_epoch;
} QuotaInfo;
```

字段含义：

- `priority`
  - 当前代码中标注为 TODO，实际优先级主要来自 `p->sched_cfg.priority`。

- `enabled`
  - runtime 控制开关。
  - 为 0 时 iokernel 不对该 task 做 quota refill。

- `mode`
  - 当前 runtime 的 quota 模式。
  - `STORAGE_QUOTA_MODE_ELASTIC`：adaptive / work-conserving。
  - `STORAGE_QUOTA_MODE_HARD_CAP`：按配置 cap 限制每周期发放量，并禁止 global borrow。

- `iops_cap` / `bytes_cap`
  - hard_cap 模式下每个 refill 周期最多发放的 IOPS / bytes token。
  - runtime 初始化时由 `storage_quota_iops` / `storage_quota_bytes` 按 `QUOTA_DEFAULT_REFILL_US` 换算得到。

- `lock`
  - 保护 shared bucket、quota、epoch 更新，以及 runtime 批量取 token。

- `iops`
  - IOPS 维度 token。
  - 当前每次 storage operation 计为 1 IOP，即使该 operation 是 vector IO 或多 LBA IO。

- `bytes`
  - byte bandwidth 维度 token。
  - 每次 IO 的 byte cost 为 `lba_count * block_size`。

- `wake_epoch`
  - iokernel 每次完成 task 的 quota refill 后递增。
  - 当前 runtime wait loop 主要使用 `timer_sleep()`，没有真正基于 `wake_epoch` 做事件驱动唤醒。

### 4.3 GlobalQuotaPool

定义位置：`lib/caladan/inc/iokernel/quota.h`

```c
typedef struct {
    spinlock_t       l;
    volatile int64_t iops;
    volatile int64_t bytes;
} GlobalQuotaPool;
```

字段含义：

- `iops`
  - 当前 refill 周期内 global pool 剩余 IOPS token。

- `bytes`
  - 当前 refill 周期内 global pool 剩余 byte token。

- `l`
  - 保护 global pool 更新和借用。

global pool 的策略含义：

- iokernel refill 后将未分配的硬件能力放入 global pool。
- runtime 在 local cache 和 task shared bucket 都不足时，可以从 global pool 借 token。
- 该路径使系统在低负载下尽量 work-conserving，但也会让单 task 突破配置值。

### 4.4 Runtime local cache

定义位置：`lib/caladan/runtime/quota.c`

```c
struct quota_bucket {
    int64_t tokens;
    int64_t quota;
    uint64_t epoch;
};

struct storage_quota_local {
    struct quota_bucket iops;
    struct quota_bucket bytes;
    uint64_t wake_epoch;
    bool initialized;
};

static DEFINE_PERTHREAD(struct storage_quota_local, storage_quota_local);
```

runtime local cache 是 per-thread 的。它保存当前线程本地可消费的 IOPS / bytes token，避免每次 IO 都修改共享内存中的 `QuotaInfo.bucket`。

local cache 刷新逻辑：

- 若 local 未初始化，或 local epoch 与 shared epoch 不一致，则进入 `quota_refresh_local()`。
- `quota_refresh_one()` 会读取 shared `quota`，将 local `quota` 更新为 shared quota，并将 local `tokens` 清零。
- local token 不跨 epoch 保留。

## 5. 配置项

定义位置：`lib/caladan/runtime/cfg.c`

当前支持的 quota 配置项：

```conf
quota_enabled = true
storage_quota_enabled = true
storage_quota_mode = elastic
storage_quota_borrow_global = true
storage_quota_refill_us = 100000
storage_quota_iops = 1721252
storage_quota_bytes = 7378829312
```

### 5.1 quota_enabled / storage_quota_enabled

两者是别名。

允许值：

```conf
true
false
1
0
on
off
```

作用：

- `false`：runtime storage path 直接跳过 quota admission；iokernel 也不会为该 runtime refill quota。
- `true`：启用 storage quota admission。

这是当前一键关闭所有 quota 相关逻辑的主要开关。

示例：

```conf
quota_enabled = false
```

### 5.2 storage_quota_mode

允许值：

```conf
elastic
work_conserving
work-conserving
hard_cap
hard-cap
hardcap
```

默认值：

```c
cfg_storage_quota_mode = STORAGE_QUOTA_MODE_ELASTIC;
```

作用：

- `elastic` / `work_conserving`：启用 adaptive quota，并默认允许 global pool borrow。适合追求硬件利用率。
- `hard_cap`：runtime 初始化时把配置 cap 写入 `QuotaInfo`，iokernel refill 后再执行 cap clamp；runtime borrow_global 路径直接关闭。适合验证或执行单 task 限速。

示例：

```conf
quota_enabled = true
storage_quota_mode = hard_cap
storage_quota_iops = 1000000
storage_quota_bytes = 67108864
```

### 5.3 storage_quota_borrow_global

允许值同上。

默认值：

```c
cfg_storage_quota_borrow_global_enabled = true;
```

作用：

- `true`：local cache 和 task shared bucket 都不足时，允许 runtime 从 iokernel global pool 借 token。
- `false`：关闭该弹性借用路径。

这是为了验证 quota 剩余机制而新增的临时开关。默认开启，因此默认行为保持 work-conserving。

注意：在 `storage_quota_mode = hard_cap` 下，即使配置文件显式写了 `storage_quota_borrow_global = true`，runtime borrow 路径也不会启用。

示例：

```conf
quota_enabled = true
storage_quota_borrow_global = false
```

### 5.4 storage_quota_iops

默认值：

```c
cfg_storage_quota_iops = HARDWARE_IOPS; // 1721252
```

当前作用：

- runtime 初始化 `QuotaInfo` 时，用它计算初始周期的 IOPS quota。
- elastic 模式下，runtime 从 global pool borrow 时，用它计算保留水位。
- hard_cap 模式下，runtime 将其换算为 `QuotaInfo.iops_cap`，iokernel 每个 refill 周期最多按该值发放 IOPS token。

### 5.5 storage_quota_bytes

默认值：

```c
cfg_storage_quota_bytes = HARDWARE_BW; // 7378829312 B/s
```

当前作用：

- runtime 初始化 `QuotaInfo` 时，用它计算初始周期的 bytes quota。
- elastic 模式下，runtime 从 global pool borrow 时，用它计算保留水位。
- hard_cap 模式下，runtime 将其换算为 `QuotaInfo.bytes_cap`，iokernel 每个 refill 周期最多按该值发放 byte token。

### 5.6 storage_quota_refill_us

默认值：

```c
cfg_storage_quota_refill_us = QUOTA_DEFAULT_REFILL_US; // 100000 us
```

当前作用：

- runtime 的 `storage_quota_wait()` 在 token 不足时，将该值作为 refill 周期参考，并以最多 1 ms 的粒度短睡眠重试。

当前不影响：

- iokernel `refillQuota()` 的调用周期。iokernel 主循环仍使用 `QUOTA_DEFAULT_REFILL_US`。

这意味着它现在更接近 runtime wait 的参考周期，而不是全局 refill interval。

### 5.7 配置格式

当前 parser 支持以下形式：

```conf
quota_enabled true
quota_enabled=true
quota_enabled = true
```

整数项必须为正数。

## 6. Runtime admission 路径

入口位置：`lib/caladan/runtime/storage.c`

所有主要 storage IO 在提交 SPDK 请求前都会调用：

```c
storage_quota_admit(op, lba_count);
```

核心逻辑：

```c
static inline int storage_quota_admit(enum storage_quota_op op,
                                      uint32_t lba_count)
{
    if (likely(!cfg_storage_quota_enabled)) return 0;
    if (unlikely(block_size == 0)) return -EINVAL;
    if (unlikely(lba_count == 0)) return 0;

    size_t bytes = (size_t)lba_count * block_size;
    if (unlikely(bytes / block_size != lba_count)) return -EINVAL;
    return storage_quota_wait(1, bytes);
}
```

计费规则：

- IOPS cost：每个 storage operation 计为 `1`。
- bytes cost：`lba_count * block_size`。
- 对 vector IO，目前仍计为 1 IOP，但 bytes 会按所有 LBA 累加。
- RMW write 若需要先读旧块，则会分别为 RMW read 和 write admission。

失败回滚：

- 如果 quota admission 成功，但后续 SPDK submission 失败，调用 `storage_quota_cancel()`。
- `storage_quota_cancel()` 最终调用 `storage_quota_refund()`，把 token 退回当前 local cache。

覆盖的主要路径：

- `__storage_write()`
- `__storage_read()`
- `storage_read_aligned()`
- `storage_write_user_dma()`
- batch / vector read paths
- `DMA_read_block()` / `DMA_write_block()`
- `read_blocks_from_disk()`
- `write_blocks_to_disk()`

## 7. Runtime token 获取逻辑

核心位置：`lib/caladan/runtime/quota.c`

### 7.1 初始化

函数：

```c
void storage_quota_init_runtime(void)
```

调用位置：

```c
lib/caladan/runtime/ioqueues.c
```

初始化过程：

1. 清零 per-thread local quota。
2. 清零 `runtime_info->Q`。
3. 初始化 `QuotaInfo.lock`。
4. 设置 `Q.enabled`。
5. 若 quota 未启用，直接返回。
6. 使用配置值计算初始周期 quota：

```c
iops_quota  = cfg_storage_quota_iops  * QUOTA_DEFAULT_REFILL_US / TO_US;
bytes_quota = cfg_storage_quota_bytes * QUOTA_DEFAULT_REFILL_US / TO_US;
```

7. 写入 quota mode 和 hard-cap 配置：

```c
atomic64_write(&q->mode, cfg_storage_quota_mode);
q->iops_cap = iops_quota;
q->bytes_cap = bytes_quota;
```

8. 写入 `Q.iops.bucket/quota` 与 `Q.bytes.bucket/quota`。
9. 设置 epoch 和 wake_epoch 为 1。

注意：这里使用 `QUOTA_DEFAULT_REFILL_US`，不是 `cfg_storage_quota_refill_us`。

### 7.2 Demand accounting

函数：

```c
void storage_quota_account(uint32_t iops, uint64_t bytes)
```

作用：

- 将本次申请计入 `runtime_info->Q.iops.demand` 和 `bytes.demand`。
- iokernel 后续 refill 会读取并清零 demand。

当前 `storage_quota_wait()` 在进入等待循环前 account 一次，不会在每次失败重试时重复 account。

### 7.3 Acquisition pipeline

函数：

```c
static bool quota_try_acquire_tokens(uint32_t iops, uint64_t bytes)
```

当前获取 token 的顺序：

1. `quota_consume_local()`
   - 尝试直接从 per-thread local cache 扣 token。

2. `quota_try_pull_shared()`
   - local 不够时，从 task 共享 `QuotaInfo.bucket` 批量拉取 token 到 local cache。

3. `quota_try_borrow_global()`
   - shared bucket 也不够时，若 `storage_quota_borrow_global = true`，从 global pool 借 token。

4. 全部失败则返回 false。

`storage_quota_wait()` 会循环调用该 pipeline；失败时 sleep：

```c
timer_sleep(cfg_storage_quota_refill_us);
```

### 7.4 Local cache consume

函数：

```c
static bool quota_consume_local(uint32_t iops, uint64_t bytes)
```

逻辑：

1. 若 local 未初始化或 epoch 过期，刷新 local。
2. 检查 local IOPS token 和 bytes token 是否都足够。
3. 两个维度都足够才扣减并返回 true。
4. 任一维度不足则返回 false。

这是 fast path，避免 shared memory 原子写和锁竞争。

### 7.5 Shared bucket pull

函数：

```c
static bool quota_try_pull_shared(uint32_t iops, uint64_t bytes)
```

逻辑：

1. 获取 `QuotaInfo.lock`。
2. 刷新 local epoch。
3. 若 local 已足够，直接消费。
4. 计算 immediate need：

```c
need_iops  = max(cost_iops  - local_iops_tokens, 0)
need_bytes = max(cost_bytes - local_bytes_tokens, 0)
```

5. 计算批量拉取量：

```c
pull = max(need, local_quota * BORROW_BATCH)
```

6. shared bucket 必须至少满足 immediate need；否则失败。
7. 实际 pull 不能超过 shared bucket 当前余额。
8. 从 shared bucket 扣除 pull，加入 local tokens。
9. 再从 local tokens 消费本次 IO。

该路径是当前减少 shared bucket contention 的主要优化。

### 7.6 Global pool borrow

函数：

```c
static bool quota_try_borrow_global(uint32_t iops, uint64_t bytes)
```

开关：

```c
if (cfg_storage_quota_mode == STORAGE_QUOTA_MODE_HARD_CAP) return false;
if (!cfg_storage_quota_borrow_global_enabled) return false;
```

逻辑：

1. 如果 hard_cap 模式或 global borrow 关闭，直接失败。
2. 获取 `QuotaInfo.lock`。
3. 再次尝试 local consume，避免不必要 borrow。
4. 计算 immediate need。
5. 计算 borrow batch：

```c
borrow_iops  = max(need_iops,  local_iops_quota  * BORROW_BATCH, 1024)
borrow_bytes = max(need_bytes, local_bytes_quota * BORROW_BATCH, 4 MiB)
```

6. 获取 `GlobalQuotaPool.l`。
7. 读取 global pool 剩余额度。
8. 对非 LC runtime 保留一部分 VIP 水位：

```c
reserve_iops  = cfg_storage_quota_iops  * VIP_RSV * QUOTA_DEFAULT_REFILL_US / TO_US;
reserve_bytes = cfg_storage_quota_bytes * VIP_RSV * QUOTA_DEFAULT_REFILL_US / TO_US;
```

9. global pool 必须至少满足 immediate need；否则失败。
10. 实际 borrow 不超过 global pool cap。
11. 从 global pool 扣除 borrow，加入 local tokens。
12. 再从 local tokens 消费本次 IO。

策略含义：

- 这是低负载下提升硬件利用率的弹性机制。
- 它会让 task 突破自身 shared bucket quota。
- 因此测试 hard-cap 语义时必须关闭该路径。

## 8. IOKernel refill 逻辑

核心位置：`lib/caladan/iokernel/quota.c`

调用位置：`lib/caladan/iokernel/main.c`

```c
static uint64_t last_refill_time = 0;
uint64_t now = microtime();
if (now - last_refill_time >= QUOTA_DEFAULT_REFILL_US) {
    refillQuota();
    last_refill_time = now;
}
```

当前 refill 周期固定为 `QUOTA_DEFAULT_REFILL_US = 100000 us`。

### 8.1 Global pool 初始化

函数：

```c
void initGlobalQuotaPool(GlobalQuotaPool* gp)
```

初始化：

```c
gp->iops  = HARDWARE_IOPS * REFILL_TIME;
gp->bytes = HARDWARE_BW   * REFILL_TIME;
```

### 8.2 Demand 到 target

函数：

```c
double calc_target(int type, QuotaInfo* Quota)
```

type：

- `0`：IOPS 维度。
- `1`：bytes 维度。

初始化阶段：

```c
demand = atomic_exchange(q->demand, 0);
q->ewma = demand > 0 ? demand : MaxAvailable * DEFAULT_RATIO;
q->target = max(q->ewma, MaxAvailable * DEFAULT_RATIO);
q->init = 1;
```

后续阶段：

```c
oldQuota = q->quota;
demand = atomic_exchange(q->demand, 0);
q->ewma = (1.0 - ALPHA) * demand + ALPHA * q->ewma;
R = q->ewma / oldQuota;
q->target = q->ewma * (1.0 + my_gamma(R));
```

`my_gamma(R)`：

- 若 `R > R2`，认为需求逼近或超过旧 quota，按二次函数扩容，最多 `+100%`。
- 若 `R < R1`，认为旧 quota 明显过大，按二次函数缩容，最多 `-50%`。
- `R1 <= R <= R2` 时保持稳定。

该逻辑是 adaptive quota 的核心。

### 8.3 Priority partition

`refillQuota()` 第一轮遍历所有 clients：

1. 跳过没有 storage 的 proc。
2. 跳过没有 `runtime_info` 的 proc。
3. 跳过 `Quota.enabled == 0` 的 proc。
4. 分别计算 IOPS / bytes target。
5. 按 `p->sched_cfg.priority` 分成两组。

当前代码：

```c
if (p->sched_cfg.priority == SCHED_PRIO_LC) {
    D_H_op += res1;
    D_H_bw += res2;
} else {
    D_L_op += res1;
    D_L_bw += res2;
}
```

注意：变量名 `D_H` / `D_L` 和 `SCHED_PRIO_LC` 的语义命名存在历史遗留，不够清晰。当前文档只描述代码事实，不重新解释业务含义。

### 8.4 Underload and overload assignment

IOPS 维度：

```c
total_D_op = D_H_op + D_L_op;
L_op = total_D_op / TOTAL_IOPS_ALLOCABLE;
```

若 `L_op <= 1.0`：

```c
assign_op = ceil(Quota->iops.target);
```

即系统未过载时，每个 task 基本拿到自己的 target。剩余硬件能力会进入 global pool。

若 `L_op > 1.0`：

先计算两类优先级可用总量：

```c
Q_H_op = min(D_H_op,
             TOTAL_IOPS_ALLOCABLE - min(D_L_op,
                                        TOTAL_IOPS_ALLOCABLE * THETA));
Q_L_op = min(D_L_op, TOTAL_IOPS_ALLOCABLE - Q_H_op);
```

再在各自优先级组内按 target 占比分配。

bytes 维度完全同理。

### 8.5 Assign and epoch

函数：

```c
static void quota_assign_dim(QuotaDim* q, int64_t assign)
```

逻辑：

1. assign 至少为 1。
2. 写入 `bucket`。
3. 写入 `quota`。
4. `epoch++`。

iokernel 对每个 task：

1. 获取 `QuotaInfo.lock`。
2. 分别 assign IOPS 和 bytes。
3. 释放 lock。
4. `wake_epoch++`。

runtime local cache 会在下一次 IO admission 时通过 epoch mismatch 感知新 quota。

### 8.6 Global pool refill

所有 task 分配完成后：

```c
g_ops_val = HARDWARE_IOPS * REFILL_TIME - final_sum_op;
g_bw_val  = HARDWARE_BW   * REFILL_TIME - final_sum_bw;
```

若为负则置 0，然后写入 global pool。

注意：

- global pool 当前是从完整硬件能力中扣除 final assignment，而不是简单固定为 20%。
- `TOTAL_*_ALLOCABLE` 只影响正常分配阶段的目标总量。
- 当系统低负载时，未分配给 task 的大量资源会进入 global pool，供 runtime borrow。

## 9. 并发与同步

当前同步关系：

1. `QuotaInfo.lock`
   - runtime 从 shared bucket 拉 token 时持有。
   - runtime 从 global pool borrow 前也持有。
   - iokernel 更新该 task quota 时持有。

2. `GlobalQuotaPool.l`
   - runtime borrow global pool 时持有。
   - iokernel 更新 global pool 余额时持有。

3. `demand`
   - runtime 使用 atomic fetch add。
   - iokernel 使用 atomic exchange 读取并清零。

4. local cache
   - per-thread 数据。
   - runtime 操作 local quota 时使用 `preempt_disable()` 包住 acquisition / refund 关键段，避免同一 per-thread 状态在调度切换中被不一致访问。

当前锁顺序：

- runtime borrow 路径：先 `QuotaInfo.lock`，再 `GlobalQuotaPool.l`。
- iokernel refill：对每个 task 只持有 `QuotaInfo.lock`；更新 global pool 时只持有 `GlobalQuotaPool.l`。

该顺序当前未形成明显的 q-lock/global-lock 反向死锁。

## 10. 当前语义边界

### 10.1 elastic 模式不是 hard cap

默认 `storage_quota_mode = elastic` 时，`storage_quota_iops` 和 `storage_quota_bytes` 主要影响 runtime 初始化和 borrow reserve，不能限制 iokernel 后续 adaptive refill。

原因：

1. runtime 初始化时确实使用配置值设置初始 `QuotaInfo.bucket/quota`。
2. 但 iokernel 每 100 ms 会根据 demand 重新写 `QuotaInfo.bucket/quota`。
3. elastic 模式下 refill 使用 `TOTAL_IOPS_ALLOCABLE` / `TOTAL_BW_ALLOCABLE` 和 adaptive target，不把配置值作为长期硬上限。
4. 因此低配置值只影响初期和 borrow reserve，不能保证长期速率上限。

### 10.2 hard_cap 模式是周期级 hard cap

`storage_quota_mode = hard_cap` 时：

1. runtime 将 `storage_quota_iops` / `storage_quota_bytes` 换算为每个 refill 周期的 cap，写入共享 `QuotaInfo`。
2. iokernel 仍先执行 demand 统计和 adaptive target 计算，以保留统一的 refill 框架。
3. 分配前执行 cap clamp：若该 task 上一周期有需求，则该维度最多发放 configured cap；若没有需求，则只保留 adaptive 分配结果，避免 idle task 长期占用额度。
4. runtime 的 global pool borrow 路径在 hard_cap 下直接失败。

边界：

- 限速粒度是 `QUOTA_DEFAULT_REFILL_US = 100 ms`，不是每次 IO 的精确 pacing。
- 短时间测试会受启动/结束边界和 100 ms epoch 粒度影响。
- 当前 hard_cap 已在单 task 微基准中验证；多 task hard-cap 公平性还需要进一步实验。

### 10.3 Global borrow 是 intentional feature

global borrow 的目的就是 work conservation：

- 低负载下，活跃 task 可以借用未使用硬件额度。
- 这提高硬件利用率，但会让 elastic 模式下的配置 quota 不表现为硬上限。

关闭方式：

```conf
storage_quota_borrow_global = false
```

关闭后，可以验证 task shared bucket 和 iokernel refill 的剩余效果。

### 10.4 storage_quota_refill_us 目前不是全局 refill 周期

当前：

- runtime wait loop 使用 `cfg_storage_quota_refill_us` 作为参考周期，并以最多 1 ms 的粒度短睡眠重试。
- iokernel 主循环仍固定使用 `QUOTA_DEFAULT_REFILL_US`。

如果未来希望动态调整 refill 周期，需要让 iokernel 也读取配置，或者将该参数写入共享配置区域。

### 10.5 wake_epoch 仍不是完整事件唤醒

`QuotaInfo.wake_epoch` 会被 iokernel refill 递增。runtime wait loop 会先比较 wake_epoch；如果还没有变化，才短睡眠：

```c
timer_sleep(min(cfg_storage_quota_refill_us, 1000));
```

这避免了过去固定 100 ms sleep 带来的严重欠发放，但还不是事件驱动唤醒；最坏情况下仍可能有约 1 ms 的额外等待。

## 11. 使用方法

### 11.1 完全关闭 quota

在 Junction / Caladan 配置文件中加入：

```conf
quota_enabled = false
```

或：

```conf
storage_quota_enabled = false
```

效果：

- storage IO 不经过 quota wait。
- iokernel 不为该 runtime refill quota。

### 11.2 开启 adaptive quota，保留 global borrow

```conf
quota_enabled = true
storage_quota_mode = elastic
storage_quota_borrow_global = true
```

这是当前默认 work-conserving 模式。

适用场景：

- 希望系统低负载时尽量用满设备。
- 不要求单 task 严格限速。

### 11.3 开启 adaptive quota，关闭 global borrow

```conf
quota_enabled = true
storage_quota_mode = elastic
storage_quota_borrow_global = false
```

适用场景：

- 验证 task shared bucket / iokernel refill 是否能影响吞吐。
- 排除 global pool 弹性借用对实验的干扰。

注意：

- 这仍然不是 hard cap，因为 elastic 模式下 iokernel refill 仍会 adaptive 扩配。

### 11.4 开启 hard cap

示例：限制 byte quota 约为 64 MiB/s：

```conf
quota_enabled = true
storage_quota_mode = hard_cap
storage_quota_iops = 1000000
storage_quota_bytes = 67108864
```

示例：限制 IOPS quota 为 500：

```conf
quota_enabled = true
storage_quota_mode = hard_cap
storage_quota_iops = 500
storage_quota_bytes = 1073741824
```

hard_cap 模式下这些值会作为长期上限。`storage_quota_borrow_global` 不需要显式关闭；hard_cap 会自动禁止 borrow。

## 12. 测试方法

### 12.1 测试程序

测试程序：

```text
junction/fs/mytest/test_shaofs_quota_rate.c
```

行为：

1. 使用 `FSHAO:/quota_rate_<pid>.dat` 路径进入 shaofs。
2. 使用 `open(..., O_DIRECT)`。
3. 默认执行 6 秒。
4. 默认 4 KiB block。
5. 默认 256 MiB 文件空间内循环 `pwrite()`。
6. 输出：

```text
QUOTA_RATE_RESULT seconds=... block_kb=... file_mb=... elapsed=... ops=... bytes=... mb=... mbps=... iops=...
```

编译：

```sh
gcc /home/syh/MyProj1/junction/junction/fs/mytest/test_shaofs_quota_rate.c \
  -o /home/syh/MyProj1/junction/build/junction/mytest/test_shaofs_quota_rate \
  -lpthread
```

### 12.2 独立 case 运行原则

每个 case 都应独立运行：

1. 停止旧 iokernel。
2. 重新 `mkfs`。
3. 启动新 iokernel。
4. 用 `timeout` 启动 `junction_run`。
5. 测试结束后杀掉 iokernel。

这样可以避免以下状态串扰：

- 文件系统盘面状态。
- runtime shared memory。
- iokernel global pool。
- 上一个 junction_run 的 quota state。

### 12.3 手动运行示例

准备配置文件，例如：

```sh
cp /home/syh/MyProj1/junction/build/junction/caladan_test.config /tmp/quota_test.config
cat >> /tmp/quota_test.config <<'EOF'
quota_enabled = true
storage_quota_borrow_global = false
storage_quota_iops = 1000000
storage_quota_bytes = 8388608
EOF
```

运行：

```sh
sudo bash /home/syh/mkfs/mkfs.sh
sudo /home/syh/MyProj1/junction/lib/caladan/iokerneld ias

cd /home/syh/MyProj1/junction/build/junction
sudo timeout 30s ./junction_run /tmp/quota_test.config -- \
  mytest/test_shaofs_quota_rate 6 4 256
```

测试结束后：

```sh
sudo pkill iokerneld
```

### 12.4 本轮实验脚本

本轮使用过三个临时脚本：

```text
/tmp/run_shaofs_quota_rate_20260529.sh
/tmp/run_shaofs_quota_rate_noborrow_20260529.sh
/tmp/run_shaofs_quota_hardcap_20260529.sh
```

它们的共同逻辑：

- 每个 case 独立 mkfs。
- 每个 case 独立启动 iokernel。
- 每个 case 独立运行 junction_run。
- `junction_run` 使用 `timeout`，普通 quota 脚本为 30s，hard_cap 脚本为 40s。
- 结束后清理 iokernel。

结果目录：

```text
junction/fs/mytest/scripts/results/quota_rate_20260529_155603
junction/fs/mytest/scripts/results/quota_rate_noborrow_20260529_160645
junction/fs/mytest/scripts/results/quota_hardcap_20260529_172140
```

## 13. 测试结果

### 13.1 Round A: global borrow 默认开启

结果目录：

```text
junction/fs/mytest/scripts/results/quota_rate_20260529_155603
```

该轮测试发生在 `storage_quota_borrow_global` 配置项加入之前；当时等价于 borrow 始终开启。

| case | quota 配置 | 实测 MB/s | 实测 IOPS | 结果 |
|---|---:|---:|---:|---|
| off | disabled | 129.380 | 33121.289 | baseline |
| default | 默认 quota | 129.365 | 33117.513 | 与 off 基本相同 |
| limit_64m | bytes=67108864, iops=1000000 | 129.390 | 33123.929 | 未限到 64 MB/s |
| limit_8m | bytes=8388608, iops=1000000 | 129.368 | 33118.190 | 未限到 8 MB/s |
| limit_iops_500 | iops=500, bytes=1073741824 | 129.390 | 33123.821 | 未限到 500 IOPS |

结论：

- 开启 global borrow 时，低 quota 配置对单 task 吞吐基本没有影响。
- 这说明 work-conserving borrow 和/或 adaptive refill 会让 task 得到远超初始配置的 token。

### 13.2 Round B: 关闭 global borrow

结果目录：

```text
junction/fs/mytest/scripts/results/quota_rate_noborrow_20260529_160645
```

| case | borrow_global | quota 配置 | 实测 MB/s | 实测 IOPS | 结果 |
|---|---:|---:|---:|---:|---|
| off | enabled | disabled | 129.374 | 33119.789 | baseline |
| default_borrow | enabled | 默认 quota | 129.371 | 33119.084 | 与 off 基本相同 |
| limit_64m_noborrow | disabled | bytes=67108864, iops=1000000 | 37.963 | 9718.401 | 吞吐下降，但未达到 64 MB/s |
| limit_8m_noborrow | disabled | bytes=8388608, iops=1000000 | 34.518 | 8836.604 | 吞吐下降，但未达到 8 MB/s |
| limit_iops_500_noborrow | disabled | iops=500, bytes=1073741824 | 37.378 | 9568.774 | 吞吐下降，但未达到 500 IOPS |

结论：

- 关闭 global borrow 后，吞吐从约 `129 MB/s` 降到约 `35-38 MB/s`。
- 这证明 global borrow 确实是之前突破低 quota 的重要原因。
- 但关闭 borrow 后，不同低限额并没有分别收敛到目标值。
- 因此剩余机制仍不是 hard cap；iokernel adaptive refill 会继续根据 demand 提高 task quota。

### 13.3 Round C: hard_cap 模式

结果目录：

```text
junction/fs/mytest/scripts/results/quota_hardcap_20260529_172140
```

该轮测试在新增 `storage_quota_mode = hard_cap`、cap clamp、hard-cap borrow 禁用和 1 ms wait retry 后执行。

| case | mode | quota 配置 | 实测 MB/s | 实测 IOPS | 结果 |
|---|---:|---:|---:|---:|---|
| off | disabled | disabled | 129.497 | 33151.180 | baseline |
| elastic_default | elastic | 默认 quota | 129.433 | 33134.746 | 与 off 基本相同 |
| hardcap_64m | hard_cap | bytes=67108864, iops=1000000 | 64.505 | 16513.200 | 贴近 64 MiB/s 目标 |
| hardcap_8m | hard_cap | bytes=8388608, iops=1000000 | 8.000 | 2047.978 | 贴近 8 MiB/s 目标 |
| hardcap_iops_500 | hard_cap | iops=500, bytes=1073741824 | 1.956 | 500.781 | 贴近 500 IOPS 目标 |

结论：

- 默认 elastic 模式相对 quota disabled 没有明显吞吐损失。
- hard_cap 模式下，byte cap 和 IOPS cap 都能在单 task microbenchmark 中生效。
- 低速 hard cap 的准确性依赖 runtime wait 不再固定睡满 100 ms；当前通过 `wake_epoch` 观察和最多 1 ms retry 解决了此前欠发放问题。

### 13.4 实验状态

- 所有 case 退出状态均为 0。
- 实验结束后确认没有残留 `iokerneld` 或 `junction_run` 进程。
- build 验证通过：
  - `make -j $(nproc)` in `lib/caladan`
  - `/home/syh/MyProj1/junction/scripts/build.sh`
  - `git diff --check` for quota-related files

## 14. 当前策略总结

当前策略可以概括为三层 token 获取模型：

1. Per-thread local token cache
   - fast path。
   - 无 shared bucket 竞争。

2. Per-task shared bucket
   - iokernel 每周期 refill。
   - runtime 批量拉 token 到 local。

3. Global pool borrow
   - work-conserving fallback。
   - 用于低负载下借用空闲硬件额度。
   - 默认开启，可通过 `storage_quota_borrow_global = false` 关闭。

iokernel refill 则是 adaptive demand-based allocator：

1. runtime 上报 demand。
2. iokernel 用 EWMA 平滑需求。
3. 根据旧 quota 与 EWMA 的比例动态扩缩容。
4. 总需求未超过可分配硬件能力时，满足 target。
5. 总需求超过能力时，根据优先级和比例裁剪。
6. 未分配硬件能力写入 global pool。

因此当前机制的准确定位是：

```text
dual-mode storage quota allocator:
  elastic: adaptive, work-conserving, demand-driven sharing
  hard_cap: epoch-granularity per-task IO rate limiting
```

其中，elastic 是默认高性能模式；hard_cap 是显式限速模式。

## 15. 已知问题与后续优化方向

### 15.1 统一 refill_us 语义

当前 `storage_quota_refill_us` 只影响 runtime sleep，不影响 iokernel refill。

建议：

- 要么重命名为 `storage_quota_wait_us`。
- 要么让 iokernel 使用同一配置周期。

### 15.2 使用 wake_epoch 做真正事件唤醒

当前 wait loop 已使用 `wake_epoch` 避免固定睡满 100 ms，但仍依赖短 sleep 轮询，可能带来最多约 1 ms 的额外延迟。

优化方向：

- runtime 在 quota 不足时挂入 wait queue。
- iokernel refill 后通过已有 preemption / notification 机制唤醒相关 runtime。

### 15.3 清理优先级命名

当前 iokernel 中 `D_H` / `D_L` 与 `SCHED_PRIO_LC` 的命名不够直观。

建议改为：

```c
D_lc
D_be
Q_lc
Q_be
```

或使用明确的 `latency_critical` / `best_effort` 命名。

### 15.4 多 task 公平性实验

当前已做的是单 task 限速验证。后续需要补充：

1. 2 个 task 同时运行，配置相同 quota。
2. 2 个 task 同时运行，配置不同 quota。
3. LC + non-LC 混合。
4. 一个 task idle / bursty，另一个 task steady。
5. 开启和关闭 global borrow 对比。

这些实验才能证明 adaptive sharing 和 priority partition 是否符合预期。

## 16. 推荐的当前使用姿势

如果目标是最高性能：

```conf
quota_enabled = false
```

如果目标是保留 adaptive 管理，但不要求 hard cap：

```conf
quota_enabled = true
storage_quota_mode = elastic
storage_quota_borrow_global = true
```

如果目标是调试 quota 剩余机制：

```conf
quota_enabled = true
storage_quota_mode = elastic
storage_quota_borrow_global = false
```

如果目标是严格限制单 task IO 流量：

```conf
quota_enabled = true
storage_quota_mode = hard_cap
storage_quota_iops = 500
storage_quota_bytes = 1073741824
```

注意：hard_cap 当前是 100 ms epoch 粒度限速，适合 task 级 throughput / IOPS 管控；如果要作为论文中的 QoS 机制，还需要补充多 task、突发流量和优先级隔离实验。

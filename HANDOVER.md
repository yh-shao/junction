# Project Handover / ShaOFS 全局项目交接与 AI 上下文恢复文档

> **文档版本**: v4.0 | **最后更新**: 2026-05-09
> **目的**: 使任何 AI Code Agent 读取本文档后，能瞬间加载全部项目上下文，无缝继续开发。

> **2026-05-06 补充说明**: 本文档保留了 2026-04-10 之前关于 ShaOFS 架构、测试和优化的历史沉淀。本次交接修正了与当前代码明显不一致的事实，并追加了 FIO-on-Junction 适配、补丁管理脚本和当前验证状态。历史性能测试结果可能不可靠，已从本文档移除；正式性能数据应以重新跑出的 benchmark 原始输出为准。

> **2026-05-09 补充说明**: 本次交接追加了 ShaOFS 的 I/O completion driven preemption（`IO_PREEMPT`）机制、相关 Caladan/IOKernel 改动、针对性 benchmark 和最新验证结果。当前 `build/CMakeCache.txt` 中 `SHAOFS_IO_PREEMPT=ON`，但 CMake option 默认值仍为 OFF，后续实验应在报告中明确构建开关状态。

---

## 第一章：项目概述与核心目标

### 1.1 一句话定位

ShaOFS 是一个构建在 **Junction LibOS + Caladan uthread runtime** 之上的**用户态高性能文件系统**，使用 **SPDK** 直接访问 NVMe SSD，通过绕过传统内核文件系统路径和利用 uthread 协作式调度（I/O 等待时快速切换线程），目标是在特定学术测试场景下显著提升 IOPS 和带宽利用率。

### 1.2 学术背景

这是一个**学术项目**，目标是在论文中证明：用户态文件系统 + 内核旁路 + 协作式 uthread 调度可以在现代 NVMe SSD 上大幅超越传统内核文件系统。为了极致性能，允许合理简化文件系统逻辑（扬长避短），但必须保证测试场景下的绝对正确性。

### 1.3 核心技术栈

| 组件 | 详情 |
|------|------|
| **主语言** | C++23（GCC 13，`-march=native -muintr -mxsavec -O3 -flto`） |
| **测试程序** | 纯 C（`gcc -O2 -lpthread`），运行在 Junction 容器内 |
| **底层运行时** | [Caladan](https://github.com/shenango/caladan) — uthread 调度、SPDK 存储、内核旁路网络 |
| **LibOS** | [Junction](https://github.com/JunctionOS/junction)（NSDI 2024）— 用户态内核，拦截 syscall |
| **存储后端** | SPDK NVMe 驱动（直接设备访问，无内核参与） |
| **I/O 抢占** | ShaOFS 可选开启 `IO_PREEMPT`：IOKernel 观察 Runtime SPDK completion queue，发现 I/O 完成后向对应 kthread/core 发送 yield/UIPI，使 Runtime 优先执行 storage softirq 和刚完成 I/O 的 uthread |
| **硬件加速** | Intel DSA/DML（运行时硬件路径可选，当前代码通过 `dsa_init()` 初始化；硬件不可用时回退到 CPU memcpy。构建期目前要求能找到静态 `libdml.a` 和 `dml/dml.h`） |
| **构建系统** | CMake + Make，封装脚本 `scripts/build.sh` |
| **磁盘格式化** | 自定义 mkfs 工具（`/home/syh/mkfs/mkfs.sh`） |

### 1.4 2026-05-09 当前开发重点

当前阶段重点是让 ShaOFS 在“少量 runtime kthread + 多个 uthread + CPU-bound uthread 干扰 I/O uthread”的学术场景下展示显著优势。核心思路是：避免 NVMe I/O 已经完成但 Runtime 没机会 poll completion，导致 I/O uthread 被 CPU-bound uthread 长时间阻塞。

已实现并验证的机制是 **I/O completion driven preemption**：

- ShaOFS 初始化后通过共享的 `runtime_info->spdk_uipi` opt-in。
- IOKernel dataplane 轮询每个 Runtime kthread 的 SPDK completion queue 状态。
- 一旦发现 completion，IOKernel 请求目标 core 上正在运行的 uthread yield。
- Runtime 收到 UIPI/yield 后优先运行 storage softirq。
- storage softirq 处理 SPDK completion 后，把等待该 I/O 的 uthread 放到 runqueue 头部。

该机制的目标不是替代所有调度策略，而是专门压低“completion 已到达但无人处理”的尾延迟。

---

## 第二章：全局系统架构

### 2.1 I/O 请求完整路径

```
用户程序调用 write(fd, buf, len)
    │
    ▼
Junction 拦截 syscall → usys_write() [junction/fs/file.cc]
    │
    ├─ 检查 f->get_inode()->get_mode() == SHAOFS ?
    │   ├─ YES → 提取 direct = f->get_flags() & kFlagDirect
    │   │        调用 my_write(inum, buf, &offset, len, direct) [shaofs/syscall.cc]
    │   │            │
    │   │            ├─ direct=false → file_write() [shaofs/file.cc]
    │   │            │   ├─ ic_get_inode(inum)           → Inode Cache 获取 inode
    │   │            │   ├─ ih.write_access()            → 获取 inode 排他写锁
    │   │            │   ├─ inode_bmap_locked(allocate=true) → Extent 查找/分配物理块
    │   │            │   │   ├─ extent_hint 快速命中?    → O(1) 返回
    │   │            │   │   ├─ direct_extents 二分查找  → O(log 6)
    │   │            │   │   ├─ indirect_extents 二分查找 → O(log 170)
    │   │            │   │   └─ alloc_block()            → Group bitmap 分配新块
    │   │            │   ├─ 更新 file_size + mark_dirty()
    │   │            │   ├─ 释放 inode 写锁              ← 锁范围优化：仅覆盖 bmap+size
    │   │            │   ├─ bc_get_handle(phys_blk)      → Block Cache 获取块
    │   │            │   ├─ bh.write_access()            → 获取块排他写锁
    │   │            │   ├─ memcpy(block, user_buf)      → 数据写入缓存
    │   │            │   └─ mark_dirty()                 → 标记脏页（延迟写回）
    │   │            │
    │   │            └─ direct=true → file_write_direct() [shaofs/file.cc]
    │   │                ├─ inode_bmap_locked(allocate=true) → 同上
    │   │                ├─ storage_write_obj()           → 绕过 Block Cache，SPDK DMA 写盘
    │   │                └─ 更新 file_size
    │   │
    │   └─ NO → 走 Junction 原生 VFS 路径（linuxfs/memfs/procfs）
    │
    ▼
  返回写入字节数
```

### 2.2 三级缓存体系

```
┌─────────────────────────────────────────────────────────┐
│                    Dentry Cache                          │
│  16384 entries, 16 shards                               │
│  Key: (parent_inum, filename) → Value: (inum, type)     │
│  Policy: LRU, write-through (不负责写回磁盘)             │
│  Backend: dir_lookup() 从磁盘读取目录项                   │
└─────────────────────┬───────────────────────────────────┘
                      │ namei() 路径解析时查询
                      ▼
┌─────────────────────────────────────────────────────────┐
│                    Inode Cache                            │
│  8192 entries, 16 shards                                │
│  Key: inum (int) → Value: MInode (extends DInode)        │
│  Policy: LRU, write-back                                │
│  Backend: 通过 Block Cache 读写 inode table blocks        │
└─────────────────────┬───────────────────────────────────┘
                      │ ic_get_inode() / ic_alloc_inode()
                      ▼
┌─────────────────────────────────────────────────────────┐
│                    Block Cache                            │
│  65536 entries (256MB), 32 shards                       │
│  Key: BlockID (uint64_t) → Value: BlockData (4KB)        │
│  Policy: LRU, write-back                                │
│  Memory: spdk_dma_zmalloc() 分配（DMA-capable hugepage）  │
│  Backend: DMA_read_block() / DMA_write_block() via SPDK  │
└─────────────────────────────────────────────────────────┘
```

### 2.3 磁盘布局

```
Block 0:          SuperBlock (4KB)
Block 1:          Inode Bitmap (1 block, tracks 32768 inodes)
Block 2-2049:     Inode Table (2048 blocks, 16 inodes/block × 256B)
Block 2050-34817: Indirect Extent Blocks (1 block per inode, 32768 blocks)
Block 34818-N:    Group Descriptor Table (GDT)
Block N+1...:     Data Groups (repeating: 1 bitmap block + 32768 data blocks)
```

### 2.4 I/O completion driven preemption 路径（2026-05-09）

当前 `IO_PREEMPT` 机制跨越 ShaOFS、Caladan Runtime 和 IOKernel：

```
ShaOFS 初始化
    │
    ├─ init_meta() 在 IO_PREEMPT=1 时写 runtime_info->spdk_uipi = 1
    │
    ▼
用户 uthread 提交 SPDK I/O 后 park/yield
    │
    ▼
另一个 CPU-bound uthread 被调度运行
    │
    ▼
NVMe 硬件写入 SPDK completion queue
    │
    ▼
IOKernel dataplane: check_spdk_and_preempt()
    │   ├─ 遍历 Runtime proc
    │   ├─ 跳过未开启 storage 或 spdk_uipi=0 的进程
    │   ├─ 遍历 active kthread
    │   ├─ 检查 storage_hwq 是否有未处理 completion
    │   └─ sched_yield_on_core(th->core) + ksched_send_intrs()
    │
    ▼
Runtime 目标 core 收到 UIPI / SIGUSR2 yield
    │
    ▼
thread_yield()
    │   └─ 先调用 softirq_run()
    │       └─ softirq_run_locked() 检查 storage_available_completions(k)
    │           └─ thread_ready_head_locked(k->storage_softirq)
    │
    ▼
storage_softirq()
    │   └─ spdk_nvme_qpair_process_completions()
    │       └─ seq_complete()/vectorIO_complete()
    │           └─ IO_PREEMPT 模式下 thread_ready_head(等待 I/O 的 uthread)
    │
    ▼
刚完成 I/O 的 uthread 优先恢复执行
```

注意：当前实现不在 UIPI handler 中直接调用 SPDK completion 处理；UIPI 只负责打断当前 uthread 并进入调度/softirq 路径，真正的 completion 仍由 Runtime 的 `storage_softirq` uthread 处理。

---

## 第三章：核心目录结构与文件职责

### 3.1 ShaOFS 源码树

```
junction/fs/shaofs/                    ← 我们的项目代码
├── fs.h                               ← 全局常量 + 磁盘数据结构
│   SuperBlock, DInode(256B), iExtent(24B), BlockData(4KB),
│   Dirent 相关常量, BLOCK_SIZE=4096, INODENUM=32768,
│   ROOT_INO=0, MYPREFIX="FSHAO", DIRECT_EXTENT_NUM=6,
│   IO_PREEMPT 默认值, SHAOFS_REALPATH()/USE_SHAOFS()
│
├── inode.h                            ← MInode = DInode + rwmutex dir_mtx + iExtent extent_hint
├── inode.cc                           ← alloc_inum() / free_inum() (原子 bitmap 操作)
│
├── generic_cache/                     ← 通用缓存框架（模板化）
│   ├── cache.h                        ← Cache<K,V>: getHandle, get, put, flush, flush_entry, invalidate
│   ├── sharded_cache.h                ← ShardedCache<K,V>: N-way 分片封装
│   ├── cache_entry.h                  ← CacheEntry(ref_count+rwmutex) + CacheEntryHandle(RAII)
│   │                                    + ReadAccessor / WriteAccessor
│   ├── backend.h                      ← Backend<K,V> 接口 (read/write)
│   ├── replace_policy.h               ← LRUPolicy (双向链表, O(1) touch/getVictim)
│   ├── hashmap.h                      ← IntrusiveCacheMap (侵入式哈希表)
│   └── objpool.h                      ← ObjectPool (固定大小对象池)
│
├── blockCache.h/cc                    ← NVMeSSD backend (DMA_read/write_block)
│                                        bc_get_handle, bc_flush_block, bc_invalidate_block, bc_flush_all
├── inodeCache.h/cc                    ← InodeBackend (通过 Block Cache 读写 inode table)
│                                        ic_get_inode, ic_alloc_inode, ic_free_inode, ic_flush_inode
├── dentryCache.h/cc                   ← DentryBackend (调用 dir_lookup 从磁盘读)
│                                        DentryKey(parent_inum, name), DentryValue(inum, type)
│
├── extent.h/cc                        ← inode_bmap_locked (extent 查找 + hint 优化 + 分配)
│                                        compact_extents_inplace, lookup_extent
├── group.h/cc                         ← alloc_block (per-core group affinity + preempt-safe)
│                                        free_block, init_group, set_newgroup, sync_all_gdt
├── dir.h/cc                           ← Dirent(512B), dirent_is_empty()
│                                        DirReadGuard/DirWriteGuard (rwmutex)
│                                        dir_lookup, dir_add_entry, dir_delete_entry, dir_is_empty
├── namei.h/cc                         ← namei() / nameiparent() 路径解析 (仅绝对路径)
├── file.h/cc                          ← file_read/write (cached), file_read/write_direct (O_DIRECT)
│                                        truncate_inode, free_inode_data_blocks, final_flush
├── syscall.h/cc                       ← my_open/read/write/mkdir/lseek/fstat/newfstatat/fsync
│                                        fill_stat_from_inode
├── utili.h                            ← SpinGuard, ReadGuard, WriteGuard, RuntimeFSBaseGuard,
│                                        kguard, atomic_read/write/inc/dec
├── dsa.h/cc                           ← Intel DSA/DML 初始化、异步 copy/copyv，硬件不可用时回退 CPU memcpy
├── OPTIMIZATION_REPORT.md             ← 优化报告
└── IOPS_BENCHMARK_REPORT.md           ← IOPS 测试报告
```

### 3.2 VFS 集成层（Junction 侧）

| 文件 | 职责 |
|------|------|
| **`junction/fs/file.cc`** | syscall dispatch: `usys_read/write/pread64/pwrite64/fstat/fsync/lseek` 中检查 `SHAOFS` 模式并转发到 `my_*` 函数 |
| **`junction/fs/core.cc`** | `usys_openat` 和 `usys_mkdir` 中用 `SHAOFS_REALPATH()` 识别 `FSHAO/path`、`FSHAO:/path` 并转发 |
| **`junction/fs/file.h`** | `kFlagDirect=O_DIRECT`, `kFlagTruncate=O_TRUNC`, `FromFlags()`, `File` 类定义 |
| **`junction/kernel/signal.cc`** | Junction UINTR 入口；`InterruptNeeded()` 当前同时检查 preempt cede/yield 和 `storage_available_completions(k)` |
| **`lib/caladan/iokernel/main.c`** | dataplane 中的 `check_spdk_and_preempt()`，负责跨 Runtime 检查 SPDK completion 并触发 yield |
| **`lib/caladan/iokernel/sched.c`** | `sched_yield_on_core()`；当前读取 live `q_ptrs->rcu_gen`，避免 stale metrics 导致持续抢占失效 |
| **`lib/caladan/runtime/storage.c`** | SPDK submit/completion 与 `storage_softirq`；`spdk_uipi` 开启时 completion callback 用 `thread_ready_head()` 唤醒 I/O uthread |
| **`lib/caladan/runtime/softirq.c`** | `softirq_run_locked()` 中 storage completion pending 时将 `storage_softirq` 插入 runqueue 头部 |

### 3.3 测试程序（`junction/fs/mytest/`）

| 文件 | 类型 | 说明 |
|------|------|------|
| `4KB_iops.c` | **主基准测试** | 参数化：线程数/读写比/文件大小/O_DIRECT，主线程顺序准备文件 |
| `bench_mt_iops.c` | 基准测试 | 多线程私有文件随机 IOPS |
| `bench_mt_simple.c` | 基准测试 | 轻量多线程读 IOPS |
| `bench_seq_write.c` | 基准测试 | 顺序写吞吐量 |
| `bench_seq_rw.c` | 基准测试 | 顺序写+回读+数据完整性校验 |
| `test_stat.c` | 单元测试 | 59 项 stat/fstat 检查 |
| `test_fsync.c` | 单元测试 | 32 项 fsync/fdatasync 检查 |
| `test_dir.c` | 单元测试 | 22 项目录操作检查（ROOT_INO fix, 并发 lookup） |
| `test_tools.c` | 集成测试 | 24 项（创建目录树 + stat + read + 目录枚举） |
| `test_direct_io.c` | 单元测试 | 35 项 O_DIRECT 检查（整块/部分块/跨模式一致性） |
| `test_barrier_sleep.c` | 兼容性测试 | pthread_barrier + sleep() 在 Junction 中的正确性 |
| `myls.c` | 工具 | 列出 shaofs 目录内容 |
| `mystat.c` | 工具 | 显示文件元数据 |
| `mycat.c` | 工具 | 显示文件内容（文本/hex dump） |
| `mytree.c` | 工具 | 递归显示目录树 |
| `run_iops_bench.sh` | 自动化脚本 | 28 组配置的完整 IOPS 测试套件 |
| `shaofs_preempt_latency.c` | 抢占机制测试 | 构造 CPU-bound uthread 干扰单次 O_DIRECT read，观察 I/O completion preemption 是否降低尾延迟 |
| `shaofs_preempt_iops.c` | 抢占机制测试 | 构造长 CPU-bound uthread 干扰连续 O_DIRECT reads，观察抢占对 IOPS 和最大延迟的影响 |
| `shaofs_iopreempt_bench.c` | 抢占机制测试 | 早期/通用抢占 benchmark，保留作参考 |
| `shaofs_storage_st.config` | 运行配置 | 单 runtime kthread + storage enabled 的 Junction config，适合验证 IO_PREEMPT 机制 |

---

## 第四章：核心数据结构精确定义

### 4.1 磁盘数据结构（`fs.h`）

```cpp
// SuperBlock — 位于 LBA 0，描述整个文件系统布局
typedef struct {
    uint32_t magic_number;          // 0x0517
    uint32_t block_size;            // 4096
    uint64_t total_blocknum;
    uint32_t inode_size;            // 256 (sizeof(DInode))
    uint32_t inode_num;             // 32768
    BlockID  imap_blockstart;       // inode bitmap 起始 LBA
    uint64_t imap_blocknum;
    BlockID  itable_blockstart;     // inode table 起始 LBA
    uint64_t itable_blocknum;
    BlockID  indirect_block_start;  // 第一个 indirect extent block
    uint64_t indirect_block_num;    // = inode_num
    uint32_t group_num;
    BlockID  gdt_blockstart;
    uint64_t gdt_blocknum;
    BlockID  group_blockstart;      // 第一个 data group 的起始 LBA
    uint32_t root_inode;            // 0
} SuperBlock;

// DInode — 256 字节，磁盘上的 inode
typedef struct {
    int         idx;                // inode 编号
    uint8_t     used;               // 是否在使用
    uint8_t     major, minor;       // 设备号（未使用）
    file_type_t type;               // UNKNOWN=0, REGULAR=1, DIRECTORY=2, SYMLINK=3
    uint32_t    nlink;              // 硬链接数
    uint64_t    file_size;          // 文件大小（字节）
    iExtent     direct_extents[6];  // 6 个直接 extent
    uint64_t    indirect_extent_block; // 间接 extent block 的 LBA
    uint64_t    ctime, mtime, atime;
    uint32_t    valid_extent_count;
    char        pad[52];
} DInode;  // sizeof = 256

// iExtent — 24 字节，内存中的 extent 表示
typedef struct {
    BlockID  logical_start;   // 文件内逻辑起始块号
    BlockID  physical_start;  // 磁盘物理起始块号
    uint64_t block_count;     // 连续块数
} iExtent;
// EXTENTS_PER_BLOCK = 4096 / 24 = 170

// Dirent — 512 字节，目录项
typedef struct {
    int         inum;           // inode 编号（0 + name[0]=='\0' 表示空 slot）
    file_type_t filetype;
    char        name[255];
    char        pad[249];
} Dirent;  // 每个 4KB block 容纳 8 个 Dirent
```

### 4.2 内存数据结构

```cpp
// MInode — 内存中的 inode（继承 DInode，添加运行时字段）
struct MInode : public DInode {
    mutable rwmutex_t dir_mtx;      // 目录读写锁（DirReadGuard/DirWriteGuard 使用）
    mutable iExtent   extent_hint;  // 上次 extent 查找缓存（顺序访问 O(1)）
};

// GroupDescExt — 内存中的 group 描述符（64 字节对齐，避免 false sharing）
struct alignas(64) GroupDescExt {
    spinlock_t lock;
    uint32_t   free_blocks_count;
    uint32_t   next_free_hint;      // 环形搜索起点
    uint32_t   flags;
    BlockID    bitmap_lba;
    BlockID    data_start_lba;
    uint32_t   group_id;
};

// 全局状态
extern SuperBlock sb;               // 超级块（init_meta 时从磁盘加载）
extern bitmap_ptr_t imap;           // inode 位图（内存中）
extern bitmap_ptr_t gmap;           // group 占用位图（per-core group affinity）
extern int core_to_group[NCPU];     // 每个 CPU 核心当前绑定的 group
extern GroupDescExt* group_info;    // 所有 group 的内存描述符数组
```

### 4.3 关键常量速查表

| 常量 | 值 | 含义 |
|------|-----|------|
| `BLOCK_SIZE` | 4096 | 块大小 |
| `INODENUM` | 32768 | 最大 inode 数 |
| `INODENUM_PER_BLOCK` | 16 | 每块容纳的 inode 数 |
| `DIRECT_EXTENT_NUM` | 6 | 每个 inode 的直接 extent 数 |
| `EXTENTS_PER_BLOCK` | 170 | 间接块中的 extent 数 (4096/24) |
| **最大 extent 数/文件** | **176** | 6 direct + 170 indirect |
| `DATABLOCKS_PERGROUP` | 32768 | 每个 group 的数据块数 |
| `TOTALBLOCKS_PERGROUP` | 32769 | 1 bitmap + 32768 data |
| `ROOT_INO` | 0 | 根目录 inode 编号 |
| `MYPREFIX` | `"FSHAO"` | 当前 shaofs 识别前缀；`SHAOFS_REALPATH()` 同时接受 `FSHAO/path` 和 `FSHAO:/path`，会把 `FSHAO`、`FSHAO:` 映射为 `/`，并拒绝 `FSHAOabc` 这类伪前缀。FIO 命令仍建议优先用 `FSHAO/` 规避未转义冒号分隔问题 |
| `IO_PREEMPT` | 默认 0 | 是否启用 IOKernel 检查 SPDK completion 并抢占目标 Runtime core；可由 CMake `SHAOFS_IO_PREEMPT=ON` 定义为 1 |
| `NAMESIZ` | 255 | 文件名最大长度 |
| `NCPU` | 256 | Caladan 最大 CPU 数 |
| `RUNTIME_STACK_SIZE` | 512KB | uthread 栈大小 |

---

## 第五章：核心算法与设计模式

### 5.1 Block 分配算法（`group.cc:alloc_block()`）

**设计要点**：Per-core group affinity + snapshot-verify 模式

```
while (true):
    ① kguard k;                          // preempt_disable
       snapshot_coreid = k->curr_cpu
       snapshot_gid = core_to_group[coreid]
       if group 无空闲 → set_newgroup()   // 环形搜索下一个可用 group
       记录 bitmap_lba, data_startlba
    ~kguard                               // preempt_enable

    ② handle = bc_get_handle(bitmap_lba)  // 可能 yield（cache miss → 磁盘 I/O）

    ③ acc = handle.write_access()         // 获取 bitmap 块写锁（可能 yield）
       bmap = acc->data

    ④ kguard k;                           // 再次 preempt_disable
       验证 core/gid 未变 → 否则 continue
       spin_lock(group_info[gid].lock)
       从 bitmap 中分配一个 bit
       更新 free_blocks_count, next_free_hint
       spin_unlock
    ~kguard

    ⑤ return allocated_blk
```

**⚠️ 关键约束**：步骤 ② 和 ③ **必须在 kguard 之外**执行，因为它们可能导致 uthread yield。在 preempt_disable 状态下 yield 会触发 Caladan 调度器的 `preempt_cnt` 断言失败。

### 5.2 Extent 查找与 Hint 优化（`extent.cc:inode_bmap_locked()`）

```
inode_bmap_locked(inode_ptr, inum, logical_blk, allocate, is_new):
    ① Hint Fast Path:
       if extent_hint 覆盖 logical_blk → 直接返回 physical_blk  // O(1)

    ② Direct Extents:
       binary_search(direct_extents[0..5], logical_blk)
       if 命中 → 更新 hint, 返回

    ③ Indirect Extents (仅当 direct 全满):
       bc_get_handle(indirect_extent_block)
       binary_search(indirect_extents[0..169], logical_blk)
       if 命中 → 更新 hint, 返回

    ④ 如果 allocate=false → 返回 INVALID (稀疏文件空洞)

    ⑤ 分配新块:
       new_phys = alloc_block()
       收集所有 extents + 新 extent → compact_extents_inplace()
       写回 direct + indirect 区域
       返回 new_phys
```

### 5.3 file_write 锁范围优化

**优化前**（inode 写锁覆盖整个块操作）：
```
inode_write_lock {
    bmap → bc_get_handle → memcpy → mark_dirty → update file_size
}
```

**优化后**（拆分为两阶段）：
```
Phase 1: inode_write_lock {
    bmap + update file_size + mark_dirty
}  // 释放 inode 锁

Phase 2: (无 inode 锁)
    bc_get_handle → block_write_lock { memcpy + mark_dirty }
```

**效果**：同一文件的并发写入不再被 inode 锁完全串行化，block cache 自身的 per-entry 锁提供足够的保护。

### 5.4 O_DIRECT I/O 路径

**`file_read_direct`**：绕过 Block Cache，使用 `storage_read_obj()` 直接从磁盘读取。读取前调用 `bc_flush_block(phys_blk)` 确保 cache 中的脏数据已落盘（cached write → direct read 一致性）。

**`file_write_direct`**：绕过 Block Cache，使用 `storage_write_obj()` / `storage_write_obj_no_rmw()` 直接写盘。新块无需 Read-Modify-Write。

**注意**：O_DIRECT 路径内部仍有一次 memcpy（`storage_read/write` 使用 SPDK DMA buffer 中转），因为用户 buffer 不是 DMA-capable 的（Junction 用户内存来自普通 mmap，非 hugepage）。

### 5.5 fsync 精确刷写

```
my_fsync(inum):
    ① 获取 inode 读锁
    ② 遍历 direct_extents → bc_flush_block(每个物理块)
    ③ 遍历 indirect_extents → bc_flush_block(间接块自身 + 每个物理块)
    ④ 释放 inode 读锁
    ⑤ ic_flush_inode(inum) → flush inode cache entry + inode table block
```

### 5.6 目录操作的读写锁模型

```
dir_lookup()      → DirReadGuard  (rwmutex_rdlock)  → 允许并发 lookup
dir_is_empty()    → DirReadGuard  (rwmutex_rdlock)
dir_add_entry()   → DirWriteGuard (rwmutex_wrlock)  → 排他增删
dir_delete_entry()→ DirWriteGuard (rwmutex_wrlock)
```

`dir_foreach_locked()` 是 static 模板函数，调用前 caller 必须已持有锁。

### 5.7 I/O completion driven preemption（`IO_PREEMPT`）

**设计目标**：降低“SPDK I/O 已完成但 Runtime 正在执行 CPU-bound uthread，导致 completion 无人处理”的延迟。该机制特别适合少量 kthread 上混合运行 I/O uthread 和长时间计算 uthread 的实验场景。

**开关与共享状态**：

- CMake option：`SHAOFS_IO_PREEMPT`，定义在 `junction/CMakeLists.txt`。默认 OFF；当前 `build/CMakeCache.txt` 中为 ON。
- 编译宏：`IO_PREEMPT`，默认在 `junction/fs/shaofs/fs.h` 中为 0。
- Runtime/IOKernel 共享字段：`runtime_info->spdk_uipi`，定义在 `lib/caladan/inc/iokernel/control.h`。
- ShaOFS `init_meta()` 在 `IO_PREEMPT=1` 时设置 `spdk_uipi=1`；`final_flush()` 清零。

**IOKernel 策略**：

- `lib/caladan/iokernel/main.c:check_spdk_and_preempt()` 在 dataplane loop 中执行。
- 遍历 `dp.clients`，跳过未启用 storage、缺少 `runtime_info` 或 `spdk_uipi=0` 的 Runtime。
- 对每个 active kthread 读取 `th->storage_hwq.consumer_idx`，用 `hwq_busy()` 判断是否有未处理 SPDK completion。
- 通过 `last_storage_cons_idx` 和 `storage_was_busy` 避免对同一批 completion 重复发送。
- 成功触发至少一个 yield 后统一调用 `ksched_send_intrs()`。

**Runtime 策略**：

- UIPI/signal 入口只决定是否需要中断当前 uthread，不直接处理 SPDK completion。
- `junction/kernel/signal.cc:InterruptNeeded()` 当前会检查 `storage_available_completions(k)`。
- `thread_yield()` 进入调度前会先调用 `softirq_run()`。
- `softirq_run_locked()` 发现 storage completion pending 后，将 `k->storage_softirq` 用 `thread_ready_head_locked()` 放到 runqueue 头。
- `storage_softirq()` 调用 `spdk_nvme_qpair_process_completions()`。
- `seq_complete()` / `vectorIO_complete()` 在 `spdk_uipi` 开启时用 `thread_ready_head()` 唤醒等待 I/O 的 uthread；关闭时仍使用普通 `thread_ready()`。

**关键修复**：

`sched_yield_on_core()` 必须读取 live `th->q_ptrs->rcu_gen`，不能使用 `th->metrics.rcu_gen`。后者依赖 IOKernel 调度统计刷新，在连续 completion 场景下可能是旧值，导致后续 yield 被误判为重复请求，从而出现“第一批抢占有效，持续 I/O 又被 CPU-bound uthread 卡住”的问题。

**性能适用场景**：

- Runtime kthread 数量少，尤其是单 kthread。
- 同一 kthread 上有 I/O uthread 和 CPU-bound uthread。
- CPU-bound uthread 不频繁主动 yield。
- 小块随机读写或 latency-sensitive I/O。
- NVMe completion 已经到达，但 Runtime 需要被外部提示去 poll。

**收益不明显或需谨慎的场景**：

- Runtime 本来就在频繁 poll storage，没有 CPU-bound 干扰。
- core/kthread 充足，I/O uthread 总能及时运行。
- 大块顺序吞吐已受 SSD 带宽、ShaOFS 数据路径 memcpy、cache flush 或 direct path 限制。
- 当前 uthread 长时间处于 `preempt_disable()`、runtime stack 或不适合被中断的状态，UIPI 会被延迟处理。
- completion rate 极高时，需要评估 UIPI/coalescing 开销，避免 IOKernel 自身成为瓶颈。

---

## 第六章：环境配置与运行指令

### 6.1 构建

```bash
cd /home/syh/MyProj1/junction
scripts/build.sh          # Release → build/
scripts/build.sh -d       # Debug → build-debug/
```

启用/关闭 ShaOFS I/O completion preemption：

```bash
cd /home/syh/MyProj1/junction
cmake -S . -B build -DSHAOFS_IO_PREEMPT=ON
cmake --build build --target junction_run -- -j$(nproc)

# 如需回到普通模式：
cmake -S . -B build -DSHAOFS_IO_PREEMPT=OFF
cmake --build build --target junction_run -- -j$(nproc)
```

注意：CMake option 默认值是 OFF；当前会话结束时 `build/CMakeCache.txt` 中为 `SHAOFS_IO_PREEMPT:BOOL=ON`。

### 6.2 格式化磁盘

```bash
# 必须从 mkfs 目录执行（脚本内使用 ./mkfs 相对路径）
# 必须先 kill iokernel（它持有 NVMe 设备）
sudo pkill -9 iokerneld
cd /home/syh/mkfs && sudo bash mkfs.sh
```

### 6.3 启动 IOKernel + 运行测试

```bash
# 启动 IOKernel（独立进程，需 root）
sudo lib/caladan/iokerneld ias &
sleep 5

# 运行程序
cd build/junction
sudo timeout 30s ./junction_run caladan_test.config -- mytest/<program> [args]

# 编译测试程序
gcc -O2 junction/fs/mytest/<source.c> -o build/junction/mytest/<binary> -lpthread

# 结束后清理
sudo pkill -9 iokerneld
```

### 6.4 编译并运行 `IO_PREEMPT` 针对性测试

编译：

```bash
cd /home/syh/MyProj1/junction
mkdir -p build/junction/mytest
gcc -O2 junction/fs/mytest/shaofs_preempt_latency.c -o build/junction/mytest/shaofs_preempt_latency -lpthread
gcc -O2 junction/fs/mytest/shaofs_preempt_iops.c -o build/junction/mytest/shaofs_preempt_iops -lpthread
gcc -O2 junction/fs/mytest/test_direct_io.c -o build/junction/mytest/test_direct_io -lpthread
```

推荐使用单 kthread storage config：

```bash
junction/fs/mytest/shaofs_storage_st.config
```

运行 latency benchmark 示例：

```bash
# shell 1
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias

# shell 2
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 20s ./junction_run ../../junction/fs/mytest/shaofs_storage_st.config -- \
  mytest/shaofs_preempt_latency 1 20 50000 16777216 4096 1 FSHAO:/preempt_latency

# 结束后
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld
```

运行 continuous IOPS benchmark 示例：

```bash
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 20s ./junction_run ../../junction/fs/mytest/shaofs_storage_st.config -- \
  mytest/shaofs_preempt_iops 1 2000 1000000 16777216 4096 1 FSHAO:/preempt_iops
```

正确性回归：

```bash
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 20s ./junction_run ../../junction/fs/mytest/shaofs_storage_st.config -- mytest/test_direct_io
```

### 6.5 sudo 密码

```
syh2syh
```

### 6.6 在 Junction 中运行 FIO（2026-05-06 当前流程）

FIO 源码位于 `junction/fs/mytest/benchmark/fio`，这是一个独立 git 仓库。为了让它能在 Junction 中启动，当前采用“FIO 内部降级 + 构建配置”的方式，不修改 Junction 源码。

相关文件：

| Path | Purpose |
|------|---------|
| `junction/fs/mytest/benchmark/fio` | FIO 源码和构建产物目录 |
| `junction/fs/mytest/benchmark/patch/fio_changes.patch` | 当前 FIO 适配补丁，120 行，只修改 `filesetup.c` 和 `helper_thread.c` |
| `junction/fs/mytest/benchmark/patch/toggle_fio.sh` | 补丁 apply/revert + 自动重新 `configure`/`make` 的管理脚本 |

将 FIO 切换并构建为 Junction 适配版：

```bash
cd /home/syh/MyProj1/junction
junction/fs/mytest/benchmark/patch/toggle_fio.sh apply
```

`apply` 会执行：

```bash
patch -p1 -d junction/fs/mytest/benchmark/fio < junction/fs/mytest/benchmark/patch/fio_changes.patch
cd junction/fs/mytest/benchmark/fio
./configure --disable-shm
make -j $(nproc)
```

将 FIO 恢复为普通源码状态并重新构建普通版：

```bash
cd /home/syh/MyProj1/junction
junction/fs/mytest/benchmark/patch/toggle_fio.sh revert
```

`revert` 会执行反向 patch，然后运行默认 `./configure && make -j $(nproc)`。

当前 FIO 适配补丁做了三件事：

1. `filesetup.c`：识别 `FSHAO/` 和 `FSHAO:/` 路径，避免 FIO 把 `FSHAO` 或 `FSHAO:` 当作普通目录去 `mkdir`。注意：FIO 自身的 `options.c:get_next_str()` 会把未转义的 `:` 当作 filename/directory 列表分隔符；当前 ShaOFS core 的 `MYPREFIX` 是 `"FSHAO"`，实际 FIO 命令建议优先使用 `FSHAO/` 规避这个问题。
2. `filesetup.c`：当 ShaOFS 的 `ftruncate` 返回 `EINVAL` 或 `ENOSYS` 时，不让 FIO prepare 阶段直接失败，而是继续通过写入铺文件。
3. `helper_thread.c`：`timerfd_create()` / `timerfd_settime()` 在 Junction 中不可用或失败时不再 `assert` 崩溃，而是回退到原有 select timeout 路径。

运行 FIO 前建议重新格式化 ShaOFS 测试盘：

```bash
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld 2>/dev/null || true
cd /home/syh/mkfs
printf 'syh2syh\n' | sudo -S bash mkfs.sh
```

启动 IOKernel：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias
```

在另一个 shell 运行 FIO：

```bash
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 60s ./junction_run caladan_test.config -- \
  /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/fio/fio \
  --name=global \
  --ioengine=psync \
  --iodepth=1 \
  --thread=1 \
  --numjobs=4 \
  --directory=FSHAO/ \
  --group_reporting=1 \
  --filesize=1M \
  --nrfiles=1 \
  --filename_format='testfile_t$jobnum.dat' \
  --randrepeat=0 \
  --name=prepare \
  --rw=write \
  --bs=1M \
  --direct=0 \
  --create_serialize=1 \
  --create_fsync=1 \
  --end_fsync=1 \
  --name=bench \
  --stonewall \
  --rw=randrw \
  --rwmixread=100 \
  --size=1M \
  --number_ios=100000 \
  --direct=0 \
  --bs=4096 \
  --io_size=409600000 \
  --norandommap \
  --allow_file_create=0
```

测试结束后清理 IOKernel：

```bash
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld
```

---

## 第七章：踩坑记录与高危警告（最重要）

### 7.1 GOTCHA 1：kguard 内禁止 yield 操作 ⚠️⚠️⚠️

**严重程度**：致命（FATAL assertion crash）

**症状**：`FATAL: runtime/sched.c:545 ASSERTION ... preempt_cnt ... FAILED IN 'enter_schedule'`

**根因**：`kguard` 调用 `getk()` 禁用抢占。如果在 kguard 作用域内执行任何可能导致 uthread yield 的操作（`rwmutex_*lock`、`mutex_lock`、`bc_get_handle` cache miss、`storage_read/write`、`file_read/write`），Caladan 调度器断言失败。

**铁律**：**kguard 内只能做纯 CPU 操作**（spinlock、atomic、内存读写已缓存数据）。所有可能 yield 的操作必须在 kguard 之外。

**已修复位置**：`group.cc:alloc_block()` — `handle.write_access()` 移到 kguard 之前。

**审计方法**：搜索所有 `kguard` 使用点，检查作用域内是否有 `rwmutex`、`mutex`、`bc_get_handle`、`storage_*`、`file_*` 调用。

### 7.2 GOTCHA 2：ROOT_INO=0 与空 slot 冲突

**严重程度**：高（数据损坏）

**根因**：`ROOT_INO` 是 0，旧代码用 `entry.inum == 0` 判断空 slot，导致根目录子目录的 `..` 条目被覆盖。

**修复**：引入 `dirent_is_empty()` 检查 `inum == 0 && name[0] == '\0'`。

**铁律**：**永远不要直接用 `entry.inum == 0` 判断空 slot**，必须用 `dirent_is_empty(&entry)`。

### 7.3 GOTCHA 3：pthread_barrier / sleep 曾不工作 — 已修复

**根因**：Caladan `softirq.c:softirq_run_locked()` 中 timer soft interrupt 处理代码被注释掉了。

**修复**：用户恢复了该代码。现在 `pthread_barrier` 和 `sleep()` 在 Junction 中完全正常工作。已通过 `test_barrier_sleep.c` 和 `DRBL.c` 验证。

### 7.4 GOTCHA 4：并发文件创建（未充分验证）

**状态**：恢复 softirq 后 preempt assertion 不再触发，但多线程并发 `open(O_CREAT)` 的正确性尚未专门回归测试。当前所有测试中文件创建均在主线程顺序完成。

### 7.5 GOTCHA 5：Extent 数组溢出

**触发条件**：文件 > ~700KB 且块分配碎片化（非连续）时，extent 数超过 176 上限。

**规避**：控制文件大小或确保顺序分配产生连续 extent。

**根治**：实现批量块分配 `alloc_blocks(N)`。

### 7.6 GOTCHA 6：IOKernel 必须在 mkfs 前 kill

`mkfs.sh` 需要将 NVMe 设备从 SPDK 解绑到内核驱动。IOKernel 运行时持有设备，解绑会静默失败。

### 7.7 GOTCHA 7：热路径禁止 log_info

`alloc_block`、`free_block`、`file_read`、`file_write` 等每次 I/O 都调用的函数中，`log_info` 会导致 **10-100 倍性能下降**。所有热路径日志已注释掉。

### 7.8 脆弱代码 — 修改前必须理解

| 代码位置 | 风险说明 |
|----------|----------|
| **`group.cc:alloc_block()`** | 锁顺序 `write_access → kguard → spinlock` 是精心设计的，任何调整都可能引入死锁或 preempt 违规 |
| **`cache.h:evict_locked()`** | 驱逐期间临时释放 shard_lock 做后端写回，重新获取后的 double-check 模式不可省略 |
| **`cache_entry.h:_freelist_hook`** | offset 0 的牺牲字段，ObjectPool 的 freelist 指针会覆写此处，不可在其前添加字段 |
| **`core.cc:usys_openat()` SHAOFS 路径** | 创建 "dummy" DirectoryEntry 是 shim，不是真正的 dentry |
| **`utili.h:RuntimeFSBaseGuard`** | 切换 x86 FS-base 寄存器到 Caladan runtime 上下文，缺少此 guard 时任何 Caladan API 调用都会崩溃 |

### 7.9 GOTCHA 8：FIO 运行依赖 `--disable-shm` 和补丁状态

FIO 默认会使用 SysV shared memory（`shmget()` 等），而 Junction 当前无法完整支持这条路径。本项目当前不修改 Junction，而是在 FIO 侧处理：

- `toggle_fio.sh apply` 会应用 `fio_changes.patch` 并执行 `./configure --disable-shm && make -j $(nproc)`。
- `toggle_fio.sh revert` 会撤销补丁并执行默认 `./configure && make -j $(nproc)`。
- 如果只应用源码补丁但没有重新 `./configure --disable-shm`，`config-host.h` 可能仍然允许 FIO 编译进 shm 路径，导致 Junction 内运行失败。
- 当前已确认 `config-host.h` / `config-host.mak` 中存在 `CONFIG_NO_SHM` 时，FIO 二进制版本输出为 `fio-3.42-22-g7215-dirty`。

### 7.10 GOTCHA 9：`FSHAO` / `FSHAO:` 前缀与 FIO 冒号分隔

当前 `junction/fs/shaofs/fs.h` 定义 `MYPREFIX` 为 `"FSHAO"`，不是历史常用的 `"FSHAO:"`。这不是因为 ShaOFS 内部必须如此，而是为了绕开 FIO 对冒号的特殊处理。

已从 FIO 当前源码确认：

- `junction/fs/mytest/benchmark/fio/options.c:get_next_str()` 的注释和实现明确说明：filename/directory 列表用 `:` 分隔。
- 未转义的 `:` 会被当作分隔符；只有写成反斜杠转义形式时，该冒号才属于文件名。
- 因此 `--directory=FSHAO:/` 或 `--filename=FSHAO:/file` 在 FIO 参数解析层仍有风险，可能被拆成 `FSHAO` 和 `/...` 两段。
- 当前推荐 FIO 命令使用 `FSHAO/`，例如 `--directory=FSHAO/`，这样 `core.cc` 剥离 `FSHAO` 后内部路径仍以 `/` 开头。

如果后续想恢复更清晰的 `MYPREFIX="FSHAO:"`，需要同时处理并验证以下事项：

- FIO 命令里的冒号必须转义，例如 shell 参数应传成类似 `--directory='FSHAO\:/'` 或 `--filename='FSHAO\:/file'`，具体转义是否能穿过 shell、FIO option parser 和 Junction syscall，需要实测确认。
- 修改 `junction/fs/shaofs/fs.h` 中 `MYPREFIX`，并同步检查 `junction/fs/core.cc`、`junction/fs/file.cc`、测试程序和工具程序。
- 更新 `junction/fs/mytest/benchmark/patch/fio_changes.patch`，因为当前 FIO patch 同时识别 `FSHAO/` 和 `FSHAO:/`，但它不能改变 FIO 更早的 option parser 分隔语义。
- 重新运行 FIO prepare + bench 和 ShaOFS 基础测试，确认 `FSHAO:/` 不再被 FIO 或 ShaOFS 内部路径解析破坏。

### 7.11 GOTCHA 10：`IO_PREEMPT` 依赖 live `rcu_gen` 和双重队头插入

当前抢占机制里有两个容易误判的点：

- `thread_yield()` 原本就会调用 `softirq_run()`，并且 `softirq_run_locked()` 原本就会把 `storage_softirq` 放到 runqueue 头部。因此“收到 UIPI 后优先执行 storage softirq”不是唯一缺失点。
- 真正等待 I/O 的业务 uthread 在 SPDK completion callback 中也必须优先进入 runqueue。否则 storage softirq 可以先运行，但 I/O uthread 仍可能被普通 tail insertion 排到 CPU-bound uthread 后面。
- `sched_yield_on_core()` 必须读取 `th->q_ptrs->rcu_gen` 的实时值。使用 `th->metrics.rcu_gen` 会依赖 IOKernel 统计刷新，在连续 I/O 场景中可能导致后续 yield 请求被错误去重。

修改这条路径时应同时检查：

- `lib/caladan/iokernel/main.c:check_spdk_and_preempt()`
- `lib/caladan/iokernel/sched.c:sched_yield_on_core()`
- `lib/caladan/runtime/storage.c:seq_complete()` 和 `vectorIO_complete()`
- `lib/caladan/runtime/softirq.c:softirq_run_locked()`
- `junction/kernel/signal.cc:InterruptNeeded()` / `uintr_entry()`

### 7.12 GOTCHA 11：`IO_PREEMPT` benchmark 证明的是调度机制，不是最终 NVMe 满带宽

2026-05-09 的测试中 Junction 输出仍包含 `DIRECTPATH DISABLED` 警告，因此 `shaofs_preempt_latency` / `shaofs_preempt_iops` 的结果应解释为“completion-driven preemption 在 CPU-bound 干扰下显著降低延迟、提升可观测 IOPS”，不要直接写成 ShaOFS 已达到最终 NVMe 带宽上限。

另外，ShaOFS direct I/O 路径目前仍存在 SPDK DMA buffer 与用户 buffer 之间的 memcpy；为了保证 cached/direct 一致性，direct read/write 还会涉及 cache flush/invalidate。这些都会影响最终吞吐。

---

## 第八章：当前代码状态与测试结果

### 8.1 历史测试覆盖项（需重新验证）

以下表格只保留历史上记录过的测试项名称，表示这些测试曾被用作回归覆盖面。历史结果可能不可靠，本次 2026-05-06 交接整理未重新完整运行这些 ShaOFS 测试套件；下一次正式交接或论文实验应重新运行并保存原始输出。

| 测试套件 | 覆盖重点 |
|----------|----------|
| `test_stat` | stat/fstat/newfstatat 元数据 |
| `test_fsync` | fsync/fdatasync 刷写路径 |
| `test_dir` | 目录项、ROOT_INO 空 slot、并发 lookup |
| `test_tools` | 工具程序和目录树集成场景 |
| `test_direct_io` | O_DIRECT 与 cached I/O 一致性 |
| `test_barrier_sleep` | pthread barrier 和 sleep 兼容性 |
| `testwholepath` | 基础全路径 open/read/write |
| `DRBL` | 历史私有目录/读负载基准 |
| `4KB_iops` | 参数化 4KB IOPS 基准 |

### 8.2 已实现的 syscall

| syscall | shaofs 函数 | dispatch 位置 |
|---------|------------|---------------|
| `openat` | `my_open` | `core.cc:usys_openat` |
| `mkdir` | `my_mkdir` | `core.cc:usys_mkdir` |
| `read` | `my_read` | `file.cc:usys_read` |
| `write` | `my_write` | `file.cc:usys_write` |
| `pread64` | `my_read` | `file.cc:usys_pread64` |
| `pwrite64` | `my_write` | `file.cc:usys_pwrite64` |
| `lseek` | `my_lseek` | `file.cc:usys_lseek` |
| `fstat` | `my_fstat` | `file.cc:usys_fstat` |
| `newfstatat` | `my_newfstatat` | `file.cc:usys_newfstatat` |
| `fsync/fdatasync` | `my_fsync` | `file.cc:usys_fsync` |

### 8.3 已实现的功能特性

- **三级缓存**：Block Cache (write-back) + Inode Cache (write-back) + Dentry Cache (write-through)
- **O_DIRECT**：绕过 Block Cache，通过 `storage_read_obj/write_obj` 直接访盘
- **O_TRUNC**：`truncate_inode()` 释放所有数据块并重置 file_size
- **O_APPEND**：在 write dispatch 时 `lseek(SEEK_END)` 后写入
- **fsync**：per-file 精确刷写（遍历 extent → `bc_flush_block` + `ic_flush_inode`）
- **stat/fstat**：完整填充 `struct stat`（含 indirect extent 块数统计）
- **Extent hint**：顺序访问 O(1) 块映射
- **Per-core group affinity**：减少块分配锁竞争
- **目录读写锁**：`dir_lookup` 并发读，`dir_add/delete` 排他写
- **I/O completion driven preemption**：`IO_PREEMPT` 开启时，IOKernel 检查 SPDK completion 并触发目标 Runtime core yield；Runtime 优先运行 storage softirq 和完成 I/O 的 uthread
- **ShaOFS 前缀解析**：`SHAOFS_REALPATH()` 同时支持 `FSHAO/path` 与 `FSHAO:/path`，并拒绝 `FSHAOabc` 伪前缀

### 8.4 2026-05-09 `IO_PREEMPT` 验证结果

本次验证使用 `junction/fs/mytest/shaofs_storage_st.config`：单 runtime kthread、storage enabled。测试前重新执行过 `/home/syh/mkfs/mkfs.sh`。以下结果用于证明抢占机制在目标学术场景下有效；由于 Junction 运行时仍提示 `DIRECTPATH DISABLED`，不应将其解释为最终 NVMe 极限带宽结果。

**正确性回归**：

```text
test_direct_io:
35 passed, 0 failed
```

**Latency benchmark**：

命令形态：

```bash
mytest/shaofs_preempt_latency 1 20 50000 16777216 4096 1 FSHAO:/preempt_latency
```

结果：

```text
IO_PREEMPT=OFF:
lat_us p50=50002 p90=50002 p99=50003 max=50008

IO_PREEMPT=ON:
lat_us p50=68 p90=69 p99=70 max=73
```

解释：单 kthread 上 CPU-bound uthread 占用约 50ms 时，关闭抢占会让 I/O completion 等到 CPU-bound uthread 结束；开启抢占后 I/O uthread 在几十微秒级恢复。

**Continuous IOPS benchmark**：

命令形态：

```bash
mytest/shaofs_preempt_iops 1 2000 1000000 16777216 4096 1 FSHAO:/preempt_iops
```

结果：

```text
IO_PREEMPT=OFF:
elapsed=1.016810 ops=2000 iops=1966.94 throughput_MBps=7.68
lat_us p50=8 p90=8 p99=12 max=1000021

IO_PREEMPT=ON:
elapsed=0.018952 ops=2000 iops=105530.36 throughput_MBps=412.23
lat_us p50=9 p90=9 p99=13 max=102
```

解释：开启抢占后，在一个 1s CPU-bound uthread 干扰下，连续 4KB O_DIRECT reads 不再被卡住，IOPS 从约 1.97K 提升到约 105.5K，最大延迟从约 1s 降到约 102us。

### 8.5 2026-05-06 当前 FIO 状态

- `junction/fs/mytest/benchmark/fio` 是独立 FIO git 仓库；当前已执行 `toggle_fio.sh apply`，所以子仓库 `git status --short` 显示 `M filesetup.c` 和 `M helper_thread.c` 是预期状态。
- `junction/fs/mytest/benchmark/patch/fio_changes.patch` 已保存这两处源码修改，可用 `toggle_fio.sh revert` 恢复 FIO tracked 源码。
- `config-host.h` 和 `config-host.mak` 当前包含 `CONFIG_NO_SHM`，表示已用 `./configure --disable-shm` 构建 Junction 适配版。
- 当前已构建的 FIO 二进制可执行，`fio --version` 输出 `fio-3.42-22-g7215-dirty`。
- 本次会话中曾验证过一个接近目标参数的 FIO 命令可在 Junction 中跑完并输出报告。历史性能数字不写入本文档；后续正式 benchmark 必须重新运行并保存原始输出。

---

## 第九章：已知缺陷与待办事项

### 9.1 已知缺陷

1. **并发文件创建未充分测试**：多线程同时 `open(O_CREAT)` 在同一目录下创建不同文件的正确性未验证
2. **Extent 数组溢出**：文件 > ~700KB 且分配碎片化时可能溢出 176 extent 上限
3. **高线程混合负载表现需要重新确认**：历史上曾关注 cache shard 竞争，但具体性能结论不再写入本文档，后续应重新 benchmark
4. **无 `unlink` / `rmdir`**：`dir_delete_entry` 存在但未接入 VFS dispatch
5. **无 `getdents64`**：目录列表需要直接读取 Dirent 结构
6. **时间戳全为 0**：DInode 有 atime/mtime/ctime 字段但无代码设置
7. **O_DIRECT 非零拷贝**：`storage_read/write` 内部仍有 SPDK buffer → 用户 buffer 的 memcpy
8. **FIO 适配是源码补丁而非上游通用修复**：当前只保证本项目常用参数（特别是 `--thread=1`、psync、ShaOFS 路径）可跑通；不要默认它覆盖 FIO 全部 job 组合。
9. **`MYPREFIX` 与测试路径书写存在历史不一致**：当前代码定义为 `"FSHAO"`，`SHAOFS_REALPATH()` 已兼容 `FSHAO/...` 与 `FSHAO:/...`，但 FIO 当前源码仍将未转义 `:` 作为 filename/directory 分隔符，因此 FIO 命令建议写 `FSHAO/`；若恢复 `FSHAO:`，必须设计并验证 FIO 参数转义方案。
10. **`IO_PREEMPT` 只在特定场景下显著收益**：它主要解决 CPU-bound uthread 阻塞 SPDK completion poll 的问题；没有 CPU-bound 干扰、kthread 充足或大块顺序吞吐场景下，收益可能较小甚至需要评估额外 UIPI 开销。
11. **当前构建缓存开启了 `SHAOFS_IO_PREEMPT`**：`build/CMakeCache.txt` 当前为 ON，但 CMake 默认值仍为 OFF。做性能对比时必须明确重新 configure，避免把 ON/OFF 结果混淆。

### 9.2 优先待办任务

**Task 1: 批量块分配 `alloc_blocks(N)`**
- 消除 extent 溢出，并减少顺序写时的分配和 extent 管理开销
- 在 bitmap 中搜索连续 N 个空闲 bit，一次锁内全部分配

**Task 2: 接入 `unlink` / `rmdir`**
- `my_unlink(path)`: nameiparent → dir_delete_entry → ic_free_inode
- `my_rmdir(path)`: dir_is_empty 检查 → 同 unlink + nlink 更新
- 在 `core.cc:usys_unlinkat` 中添加 FSHAO: dispatch

**Task 3: 并发文件创建压力测试**
- 编写 `test_concurrent_create.c`：N 线程同时在同一目录创建不同文件
- 验证无 segfault、double-free、目录项丢失

**Task 4: FIO 正式基准回归**
- 用 `toggle_fio.sh apply` 确认 FIO 处于 `CONFIG_NO_SHM` 状态。
- 在重新 `mkfs` 后运行目标 FIO 命令，至少记录完整 stdout、退出码、IOKernel 日志和 `timeout` 是否触发。
- 对比 `FSHAO/` 与转义后的 `FSHAO\:/` 两种路径写法，确认哪一种应作为论文脚本标准写法。

**Task 5: 前缀语义统一**
- 当前 `fs.h` 的 `MYPREFIX` 是 `"FSHAO"`，历史文档曾写 `"FSHAO:"`。
- 如果恢复 `FSHAO:`，必须同时解决 FIO 未转义冒号分隔问题；建议先写一个最小 FIO 参数验证用例，再同步修改 `fs.h`、`core.cc`/`file.cc` 判断、测试程序、工具程序和 FIO 适配补丁。

**Task 6: 将 `IO_PREEMPT` 纳入正式论文 benchmark**
- 固化 ON/OFF 两套构建脚本，避免手动 CMake cache 状态污染结果。
- 保存完整 stdout、退出码、IOKernel 日志、Junction `DIRECTPATH` 状态和机器负载。
- 在不同 `busy_us`、kthread 数、CPU-bound uthread 数、I/O 深度下扫参数，找出机制收益边界。

**Task 7: 优化 direct I/O 数据路径**
- 当前 direct I/O 仍有用户 buffer 与 SPDK DMA buffer 之间的 memcpy。
- cached/direct 一致性修复使用 `bc_flush_block()` 和 `bc_invalidate_block()`，正确但可能增加 direct path CPU 开销。
- 后续可考虑仅在 cache entry dirty 时 flush，或在 direct read 命中 clean cache 时直接从 cache 返回。

**Task 8: 评估 IOKernel completion 检查开销**
- 当前 `check_spdk_and_preempt()` 在 dataplane loop 中遍历 runtime/kthread。
- 对少量 Runtime 的学术实验可接受；若扩展到更多 Runtime，应评估 bitmap/event/coalescing，避免 IOKernel 忙等扫描成为瓶颈。

---

## 第十章：文件修改历史总览

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `shaofs/group.cc` | Bug fix + perf | 移除热路径 log_info；`alloc_block` 中 write_access 移到 kguard 之前 |
| `shaofs/file.cc` | Perf + feature | 拆分 file_write 锁范围；新增 truncate_inode + free_inode_data_blocks；新增 file_read/write_direct |
| `shaofs/file.h` | Feature | 新增 file_read/write_direct, truncate_inode 声明 |
| `shaofs/inode.h` | Perf + refactor | MInode 新增 extent_hint；dir_mtx 从 mutex_t 升级为 rwmutex_t |
| `shaofs/extent.cc` | Perf | inode_bmap_locked 增加 hint fast path + hint 更新 |
| `shaofs/inodeCache.cc` | Bug fix + feature | ic_free_inode 实现完整块释放；新增 ic_flush_inode |
| `shaofs/inodeCache.h` | Feature | 新增 ic_flush_inode 声明 |
| `shaofs/dir.h` | Refactor | 新增 dirent_is_empty()；移除未实现的 _locked 声明 |
| `shaofs/dir.cc` | Refactor + bug fix | 全面重写：DirReadGuard/DirWriteGuard + rwmutex；dir_foreach_locked；dirent_is_empty |
| `shaofs/syscall.h` | Feature | 新增 my_fstat, my_newfstatat, my_fsync |
| `shaofs/syscall.cc` | Feature + perf | 实现 fstat/newfstatat/fsync；移除热路径日志；my_read/write 支持 direct 分派 |
| `shaofs/blockCache.h` | Feature + fix | NVMeSSD 后端改用 DMA_read/write_block；新增 bc_flush_block |
| `shaofs/blockCache.cc` | Feature | 新增 bc_flush_block；新增 bc_invalidate_block 用于 direct write 后失效旧 cache entry |
| `generic_cache/cache.h` | Feature | 新增 flush_entry(key) |
| `generic_cache/sharded_cache.h` | Feature | 转发 flush_entry |
| `junction/fs/file.cc` | Feature | usys_read/write/pread64/pwrite64 传递 O_DIRECT flag；usys_fstat/newfstatat/fsync 添加 SHAOFS dispatch；newfstatat 使用 SHAOFS_REALPATH |
| `junction/fs/core.cc` | Feature + path fix | openat/mkdir 使用 SHAOFS_REALPATH，兼容 `FSHAO/path` 与 `FSHAO:/path` |
| `junction/CMakeLists.txt` | Build | 新增 `SHAOFS_IO_PREEMPT` option，开启时定义 `IO_PREEMPT=1` |
| `lib/caladan/runtime/softirq.c` | User fix | 恢复 timer soft interrupt 处理（修复 sleep/barrier）；storage softirq pending 时使用 runqueue head insertion |
| `fs/mytest/*.c` | New | 15+ 测试/工具/基准测试程序 |
| `junction/fs/shaofs/dsa.cc` | Perf + fallback | 当前实现使用 DML 硬件路径、Caladan `runtime_async_park` 和 per-thread tcache；硬件/提交失败时回退 CPU memcpy |
| `junction/fs/shaofs/dsa.h` | Feature | 暴露 `dsa_init`、`dsa_copy`、`dsa_copyv` 和 `ShaofsDsaOptions`；`dsa_batch_task_num=32` |
| `junction/fs/CMakeLists.txt` | Build | 当前查找静态 `libdml.a` 和 `dml/dml.h`，找不到会 FATAL |
| `junction/fs/mytest/benchmark/fio/filesetup.c` | FIO adapter | 补丁后识别 `FSHAO/`、`FSHAO:/`，并容忍 ShaOFS 上 `ftruncate` 不支持 |
| `junction/fs/mytest/benchmark/fio/helper_thread.c` | FIO adapter | 补丁后 `timerfd_create/settime` 失败不再 assert，回退 select timeout |
| `junction/fs/mytest/benchmark/patch/fio_changes.patch` | Handover artifact | 保存 FIO 适配源码补丁 |
| `junction/fs/mytest/benchmark/patch/toggle_fio.sh` | Tooling | 一键 apply/revert FIO 补丁，并自动重新 configure/make |
| `lib/caladan/iokernel/main.c` | IO_PREEMPT | dataplane 中启用 `check_spdk_and_preempt()`，检查 SPDK completion 并批量发送 yield/UIPI |
| `lib/caladan/iokernel/sched.c` | IO_PREEMPT bug fix | `sched_yield_on_core()` 改读 live `q_ptrs->rcu_gen`，修复持续 I/O 场景下 yield 去重错误 |
| `lib/caladan/iokernel/ksched.h` | Perf | 移除发送 UIPI 热路径日志 |
| `lib/caladan/runtime/preempt.c` | Perf | 移除 preempt 热路径日志 |
| `lib/caladan/runtime/storage.c` | IO_PREEMPT | `seq_complete()` / `vectorIO_complete()` 在 `spdk_uipi` 开启时用 `thread_ready_head()` 唤醒 I/O uthread |
| `junction/kernel/signal.cc` | IO_PREEMPT | UINTR 判断路径包含 storage completion pending 检查，并移除热路径日志 |
| `junction/fs/mytest/shaofs_preempt_latency.c` | New test | CPU-bound 干扰下的单次 I/O 延迟测试 |
| `junction/fs/mytest/shaofs_preempt_iops.c` | New test | CPU-bound 干扰下的连续 O_DIRECT read IOPS 测试 |
| `junction/fs/mytest/shaofs_storage_st.config` | Test config | 单 kthread storage runtime 配置，用于 IO_PREEMPT 实验 |

---

## 第十一章：2026-05-06 交接整理验证记录

### 11.1 本次实际检查过的内容

```bash
sed -n '1,320p' HANDOVER.md
git status --short
find junction/fs/shaofs -maxdepth 2 -type f | sort
sed -n '1,140p' junction/fs/shaofs/fs.h
sed -n '390,555p' junction/fs/core.cc
sed -n '560,710p' junction/fs/file.cc
sed -n '1,380p' junction/fs/shaofs/syscall.cc
sed -n '1,260p' junction/fs/shaofs/namei.cc
sed -n '1,240p' junction/fs/mytest/benchmark/patch/toggle_fio.sh
grep -n 'CONFIG_NO_SHM\|CONFIG_HAVE_SHM' \
  junction/fs/mytest/benchmark/fio/config-host.h \
  junction/fs/mytest/benchmark/fio/config-host.mak
git -C junction/fs/mytest/benchmark/fio status --short
grep -n '^diff --git' junction/fs/mytest/benchmark/patch/fio_changes.patch
wc -l junction/fs/mytest/benchmark/patch/fio_changes.patch
junction/fs/mytest/benchmark/fio/fio --version
bash -n junction/fs/mytest/benchmark/patch/toggle_fio.sh
```

### 11.2 本次确认的客观状态

- 顶层 git 工作区当前有大量既有未提交/未跟踪文件；本次交接整理只修改了 `HANDOVER.md`。
- `junction/fs/shaofs/dml_utili.h` 当前不存在，旧文档中对它的引用已移除。
- `junction/fs/shaofs/fs.h` 当前 `MYPREFIX` 是 `"FSHAO"`。
- FIO 适配补丁文件存在，大小为 120 行，覆盖 `filesetup.c` 和 `helper_thread.c`。
- 当前 FIO 子仓库处于补丁已应用状态，`git -C junction/fs/mytest/benchmark/fio status --short` 显示两个预期 modified 文件。
- 当前 FIO 构建配置包含 `CONFIG_NO_SHM`，用于绕过 Junction 不支持的 SysV shm 路径。
- `toggle_fio.sh` 通过 `bash -n` 语法检查。

### 11.3 本次未重新验证的内容

- 未重新运行完整 `scripts/build.sh`。
- 未重新启动 IOKernel。
- 未重新运行 ShaOFS 单元测试套件。
- 未重新运行 FIO target benchmark。
- 未验证 `FSHAO/` 与转义后的 `FSHAO\:/` 在所有 syscall 和 FIO job 组合下是否完全等价。

---

## 第十二章：2026-05-09 `IO_PREEMPT` 交接整理验证记录

### 12.1 本次实际检查过的内容

```bash
sed -n '1,220p' HANDOVER.md
sed -n '221,440p' HANDOVER.md
sed -n '441,700p' HANDOVER.md
sed -n '701,980p' HANDOVER.md
git -C /home/syh/MyProj1/junction status --short
git -C /home/syh/MyProj1/junction/lib/caladan status --short
rg -n 'SHAOFS_IO_PREEMPT' build/CMakeCache.txt junction/CMakeLists.txt junction/fs/shaofs/fs.h
rg -n 'MYPREFIX|SHAOFS_REALPATH|USE_SHAOFS' junction/fs/shaofs/fs.h junction/fs/core.cc junction/fs/file.cc
sed -n '1,140p' junction/fs/shaofs/fs.h
sed -n '120,190p;200,218p' lib/caladan/iokernel/main.c
sed -n '332,358p' lib/caladan/iokernel/sched.c
sed -n '44,60p;632,646p' lib/caladan/runtime/storage.c
sed -n '33,74p;83,101p' lib/caladan/runtime/softirq.c
sed -n '824,842p' lib/caladan/runtime/sched.c
sed -n '316,366p' junction/kernel/signal.cc
```

### 12.2 本次实际运行过的构建和测试

本轮开发中运行过以下关键命令和测试。这里记录的是本轮会话中的实际结果，便于下一位接手者复现实验：

```bash
cmake -S . -B build -DSHAOFS_IO_PREEMPT=OFF
cmake --build build --target junction_run -- -j$(nproc)
cmake -S . -B build -DSHAOFS_IO_PREEMPT=ON
cmake --build build --target junction_run -- -j$(nproc)

gcc -O2 junction/fs/mytest/shaofs_preempt_latency.c -o build/junction/mytest/shaofs_preempt_latency -lpthread
gcc -O2 junction/fs/mytest/shaofs_preempt_iops.c -o build/junction/mytest/shaofs_preempt_iops -lpthread
gcc -O2 junction/fs/mytest/test_direct_io.c -o build/junction/mytest/test_direct_io -lpthread

cd /home/syh/mkfs && printf 'syh2syh\n' | sudo -S bash ./mkfs.sh
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias
```

已验证：

- `IO_PREEMPT=ON` 最终构建成功，`build/CMakeCache.txt` 当前为 `SHAOFS_IO_PREEMPT:BOOL=ON`。
- `test_direct_io` 在开启抢占后通过：`35 passed, 0 failed`。
- `shaofs_preempt_latency` 在单 kthread + CPU-bound 干扰场景下，p99 从约 `50003us` 降到约 `70us`。
- `shaofs_preempt_iops` 在单 kthread + 1s CPU-bound 干扰场景下，IOPS 从约 `1966.94` 提升到约 `105530.36`，最大延迟从约 `1000021us` 降到约 `102us`。
- 测试结束后已执行 `pkill -9 iokerneld`，并用 `pgrep -a iokerneld` / `pgrep -a junction_run` 确认没有残留进程。

### 12.3 本次确认的客观状态

- `HANDOVER.md` 在顶层 git 中仍是 untracked 文件；本次直接在该文件上增量更新。
- 顶层工作区和 `lib/caladan` 子仓库均存在既有 dirty/untracked 状态。不要假设所有 dirty 文件都是当前 Agent 创建的。
- `junction/fs/mytest/iops.c`、`junction/fs/mytest/testwholepath.c` 在本次整理前已是 modified，未在本次文档整理中修改。
- 本次新增/依赖的抢占测试文件包括 `shaofs_preempt_latency.c`、`shaofs_preempt_iops.c`、`shaofs_iopreempt_bench.c` 和 `shaofs_storage_st.config`。
- IOKernel 与 Runtime 的 `spdk_uipi` 共享字段已存在于 `lib/caladan/inc/iokernel/control.h`；该结构版本号已有对应更新，具体兼容性仍应以当前 Caladan/Junction 同步构建为准。
- 当前 `SHAOFS_REALPATH()` 已同时支持 `FSHAO/path` 与 `FSHAO:/path`。FIO 命令仍建议优先使用 `FSHAO/`，因为 FIO option parser 对未转义 `:` 有特殊分隔语义。

### 12.4 本次未重新验证的内容

- 未重新运行完整 `scripts/build.sh`，只用 CMake build 了 `junction_run` target。
- 未重新运行完整 ShaOFS 单元测试套件，只回归了 `test_direct_io`。
- 未重新运行 FIO target benchmark。
- 未重新跑 ext4 对比测试。
- 未验证 `IO_PREEMPT` 在多 kthread、多 Runtime、高 completion rate 或真实 FIO 混合负载下的收益边界。
- 未验证 DSA/DML 硬件路径是否实际启用；测试输出中曾出现 DSA 执行失败并回退 CPU memcpy 的提示。
- 未确认 `DIRECTPATH DISABLED` 的根因；因此当前抢占 benchmark 不能作为最终带宽数字使用。

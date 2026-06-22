# Project Handover / ShaoFS 全局项目交接与 AI 上下文恢复文档
> **目的**: 使任何 AI Code Agent 读取本文档后，能瞬间加载全部项目上下文，无缝继续开发。

---

## 第一章：项目概述与核心目标

### 1.1 一句话定位

shaoFS 是一个构建在 **Junction LibOS + Caladan uthread runtime** 之上的**用户态文件系统**，使用 **SPDK** 直接访问 NVMe SSD，通过绕过传统内核文件系统路径和利用 uthread 协作式调度（I/O 等待时快速切换线程），目标是在特定学术测试场景下显著提升 IOPS 和带宽利用率。

### 1.2 学术背景

这是一个**学术项目**，目标是在论文中证明：用户态文件系统 + 内核旁路 + 协作式 uthread 调度可以在现代 NVMe SSD 上大幅超越传统内核文件系统。为了极致性能，允许合理简化文件系统逻辑（扬长避短），但必须保证测试场景下的绝对正确性。

### 1.3 核心技术栈

| 组件 | 详情 |
|------|------|
| **主语言** | C++23（GCC 13，`-march=native -muintr -mxsavec -O3 -flto`） |
| **测试程序** | 纯 C（`gcc -O2 -lpthread`），运行在 Junction 容器内 |
| **底层 runtime** | [Caladan](https://github.com/shenango/caladan) — uthread 调度、SPDK 存储、内核旁路网络 |
| **LibOS** | [Junction](https://github.com/JunctionOS/junction) — 用户态内核，拦截 syscall |
| **存储后端** | SPDK NVMe 驱动（直接设备访问，无内核参与） |
| **I/O 抢占** | ShaoFS 可选开启 `IO_PREEMPT`：IOKernel 观察 Runtime SPDK completion queue，发现 I/O 完成后向对应 kthread/core 发送 yield/UIPI，使 Runtime 优先执行 storage softirq 和刚完成 I/O 的 uthread |
| **Crash consistency** | shaoFS 可选开启 `CRASH_CONSISTENCY`：metadata-only redo journal，journal 区位于盘尾，异常退出后执行 committed transaction replay 和 dirty mount repair |
| **硬件加速** | Intel DSA/DML（运行时硬件路径可选，当前代码通过 `dsa_init()` 初始化；硬件不可用时回退到 CPU memcpy。构建期目前要求能找到静态 `libdml.a` 和 `dml/dml.h`） |
| **Direct user-buffer DMA** | `O_DIRECT` 路径在严格对齐约束下可直接使用用户 buffer 作为 SPDK NVMe payload，避免 SPDK bounce buffer 与 user buffer 之间的 memcpy |
| **显式 batch read** | shaoFS direct `readv/preadv` 可把多个 iovec 聚合为 `storage_read_aligned_batch()`，一次提交多个 NVMe read 后只 park 当前 uthread 一次；当前不包含透明底层 doorbell batching |
| **构建系统** | CMake + Make，封装脚本 `scripts/build.sh` |
| **磁盘格式化** | 自定义 mkfs 工具（`/home/syh/mkfs/mkfs.sh`） |

### 1.4 2026-06-02 历史交接快照

- 当前阶段：ShaoFS 的 Filebench 导向机制优化、debug 清理和四项 Filebench 自动化对比已经完成一轮。核心目标已经从“跑通”推进到“在 fileserver/webserver/varmail/webproxy 四项中对 ext4 形成稳定领先”的状态。
- 当前 `build/CMakeCache.txt` 中 `SHAOFS_IO_PREEMPT=ON`、`SHAOFS_CRASH_CONSISTENCY=ON`。CMake 默认值仍分别是 `IO_PREEMPT=OFF`、`CRASH_CONSISTENCY=ON`，正式实验前必须显式记录 cache 状态。
- 当前 shaoFS 的关键机制包括：metadata-only redo journal、batched metadata group commit、异步 home-block checkpoint、data block 后台 writeback、fsync dirty-range 快路径、EOF append 预分配/batch copy、目录内存索引、simple extent tree。
- 最新一次四项 Filebench 自动化结果位于 `junction/fs/mytest/scripts/results/filebench_compare_20260528_130532`，8 个子测试退出码均为 0。ShaoFS 相对 ext4 的 ops/s：fileserver +14.6%、webserver +55.9%、varmail +36.6%、webproxy +148.6%。详见 `8.10`。
- 当前新增的统一脚本为 `junction/fs/mytest/scripts/run_filebench_compare.sh`：参数 `0` 只测 shaoFS，`1` 只测 ext4，`2` 先测 shaoFS 后测 ext4，默认 `2`。
- 最近一次 cleanup 已删除临时 shaoFS profiling 模块和相关热路径统计输出；当前源码树中不再存在 `junction/fs/shaofs/profile.h` / `profile.cc`，`junction/fs/CMakeLists.txt` 也不再包含 `SHAOFS_PROFILE_COMPILED` 或 `shaofs/profile.cc`。
- 2026-06-01 本轮重点转向 4KB random read/write IOPS 上限与 core/QD 扩展性诊断。已新增 Caladan raw storage fixed-QD benchmark：`lib/caladan/tests/test_storage_async_iops.c` 和 `lib/caladan/tests/run_storage_async_iops.sh`，用于绕过 shaoFS/FIO 层直接验证 Caladan runtime + SPDK storage API 的上限。
- 本轮诊断确认：PM9A3 4KB randread 同时受 random range、总 outstanding 和 LBA 状态影响。`blkdiscard` 后读取 deallocated/unwritten LBA 可达到约 1.16M-1.17M IOPS；读真实写过的数据 LBA 或全盘顺序写满后，raw randread 会降到约 583K IOPS。2 个 core、每 core QD64 未达到约 1.03M 的旧现象主要是总 outstanding=128 不足；但后续 ShaoFS/ext4 FIO 结果必须以“数据准备后、同一盘状态下”的 raw baseline 为准。详见 `8.11` 和 `8.12`。
- 本轮 cleanup 已删除探索阶段临时接口 `storage_read2/storage_write2`、回退 `test_storage_iops.c` 到原始写 IOPS 测试，并移除早期 `test_storage_randread_iops` 探索测试及其生成物。保留的 storage async API 是最终 raw benchmark 所需，不属于 debug 代码。
- 2026-06-01/02 已完成 `numjobs=256` 的 ShaoFS/ext4 4KB random read `O_DIRECT` 对比。ShaoFS 通过 `runtime_kthreads` 控 core，ext4 通过 `taskset` 控 Linux CPU affinity。当前近满写入/全盘已写状态下 raw 上限约 583K IOPS；ShaoFS 1 core 即达到约 583K，ext4 约需 4 cores 达到同水平。详见 `8.12`。
- 最近一次裸盘状态复现实验结束后，目标盘 PCI `0000:5b:00.0` 已恢复到 Linux `nvme` 驱动，`/dev/nvme2n1` 当前没有文件系统签名，并且已经被 SPDK 顺序写满过。继续跑 ShaoFS 前需要重新执行 `/home/syh/mkfs/mkfs.sh`；继续跑 ext4 前需要重新 `mkfs.ext4`/挂载。不要假设盘仍保留上一轮 ShaoFS 或 ext4 数据。

### 1.5 2026-06-03 历史交接快照

- 当时阶段：完成两轮以可读性和机制一致性为主的 ShaoFS 重构审查。第一轮当时记录在 `plan.md`，第二轮当时记录在 `plan2.md`。注意：截至 2026-06-07，仓库根目录 `plan.md` 已被本轮 Junction 多进程验证计划覆盖，当前工作区未发现 `plan2.md`；本节和 `8.13` 保留的是 2026-06-03 重构阶段的历史摘要。
- 当时 tracked ShaoFS 源码 diff 仍较大：`10 files changed, 974 insertions(+), 724 deletions(-)`。这不是净删代码式重构；本轮主要价值在于把大函数拆成更清楚的顶层流程，并封装重复的 direct/cached write、fsync、journal recovery 和 block flush 机制。
- 主要重构点：`syscall.cc` 中 `my_open()` / `my_mkdir()` / `my_fsync()` 拆分为命名 helper；`file.cc` 中 cached write 参数改为 `CachedWriteOp`，append/EOF extension 和 direct write helper 统一；`blockCache.cc` 中 `bc_flush_blocks_contiguous()` 改为显式 `CachedFlushRun`；`journal.cc` 中 repair buffer 改为 `CFreeBuffer<T>`，recovery slot classification/replay/repair 拆分为独立 helper。
- 第二轮最终正确性回归目录：`/tmp/shaofs_plan2_phase6_correctness_20260603`。状态 `0`：`test_shaofs_syscall_correctness`、`test_shaofs_direct_correctness`、`test_shaofs_concurrent_correctness`、`test_shaofs_append_prealloc`、`test_shaofs_concurrent_append`、`test_shaofs_direct_concurrent_append`、`test_shaofs_many_extents`、`test_shaofs_mt_full_extents 4 256 2 write-verify 8`、`test_shaofs_fsync_direct_verify`。`journal_recovery_prepare --crash` 退出 `124` 是预期 timeout 崩溃模拟，独立重启后的 `journal_recovery_check` 退出 `0`。
- 第二轮最终 ShaoFS-only Filebench 对比的是 Phase 0 baseline，而不是 2026-05-28 ext4 对比。首轮目录 `junction/fs/mytest/scripts/results/filebench_compare_plan2_final_20260603_015704`，fileserver/varmail 因首轮波动超过 10% 阈值已各复跑一次。最终采用值：fileserver `62460.787 ops/s` (`-0.70%` vs baseline)，webserver `483673.175 ops/s` (`-0.08%`)，varmail `173705.862 ops/s` (`-2.30%`)，webproxy `612508.355 ops/s` (`+57.21%`)。无确认的 >10% 性能回归。
- 当前 `build/CMakeCache.txt` 仍显示 `SHAOFS_IO_PREEMPT:BOOL=ON`、`SHAOFS_CRASH_CONSISTENCY:BOOL=ON`；当前 `build/junction/caladan_test.config` 为 `runtime_kthreads=1`、`runtime_spinning_kthreads=0`、`runtime_quantum_us=100`、`enable_storage=1`。正式实验前必须重新记录这些构建/cache/config 状态。
- 当时没有残留 `iokerneld` / `junction_run` / Filebench 进程。当前工作区仍有大量 untracked benchmark、script、result、`plan.md`、`.cache/`、`logs/` 和 generated build artifact；历史交接曾提到 `plan2.md`，但截至 2026-06-07 当前工作区未发现该文件。不要盲目删除用户可能仍需要的实验资产。

### 1.6 2026-06-07 当前交接快照

- 当前阶段：完成了 **Junction 单容器多进程运行能力验证**，目标是验证 README 中“一个 `junction_run` 容器可以运行多个应用/进程”的声明。该工作只新增测试程序、计划和报告，没有修改 Junction/ShaoFS/Caladan 实现代码。
- 新增测试程序：`junction/fs/mytest/junction_multiproc_vfork.c`。它是一个纯 C 双模式程序：controller 模式显式调用 `vfork()`，child 立即 `execv()` 同一二进制的 `--worker` 模式，parent 用 `waitpid()` 回收所有 child；worker 打印 `getpid/gettid/getppid`、记录起止时间，并写入 ShaoFS 私有文件。
- 新增报告：`junction_multiproc_report.md`。当前根目录 `plan.md` 是本轮多进程验证的 checklist，不再是旧的 ShaoFS 重构/ablation 计划。
- 原始日志目录：`/tmp/junction_multiproc_20260607_162050`。`mkfs.status=0`、`vfork.status=0`、`fish.status=0`、`hostps_vfork.status=0`。
- vfork/exec 路线：`sudo timeout 30s ./junction_run caladan_test.config -- mytest/junction_multiproc_vfork 4 20 50000 FSHAO:/junction_multiproc_vfork` 成功。容器内 controller PID/TID 为 `1/1`，worker PID/TID 为 `2/2`、`3/3`、`4/4`、`5/5`，所有 worker `ppid=1`，parent 成功 `waitpid()`，并验证 4 个 ShaoFS worker 文件，最终输出 `MULTIPROC_VFORK_OK workers=4`。
- fish/`posix_spawn()` 路线：`sudo timeout 30s ./junction_run caladan_test.config -- /usr/bin/fish -c 'for i in (seq 0 3); mytest/junction_multiproc_vfork --worker $i 20 50000 FSHAO:/junction_multiproc_fish &; end; wait'` 成功。worker PID/TID 为 `4/4`、`5/5`、`6/6`、`7/7`，验证 README 推荐的 fish 后台任务方式可以在单个 Junction 容器内启动多个任务。
- host 侧证据：约 6 秒长运行期间，Linux `pgrep -a junction_run` 只看到一个 host `junction_run` 进程；`ps -T -p <junction_run_pid>` 只显示 `junction_run` 主线程和 DPDK 辅助线程 `dpdk-intr`、`dpdk-mp-msg`。容器内多个 Junction PID 没有对应为多个 host Linux 子进程，符合“Junction process 由 LibOS/Caladan uthread 承载”的预期。
- 重要边界：本轮验证的是 `vfork()` + `execve()` 和 fish/`posix_spawn()` 路线，不等价于证明通用 Linux `fork()` 语义完整支持。`vfork()` child 在 exec 前共享 parent 地址空间且 parent 被暂停，所以 child 在 exec 前不能做复杂工作。
- 当前 `build/CMakeCache.txt` 仍显示 `SHAOFS_IO_PREEMPT:BOOL=ON`、`SHAOFS_CRASH_CONSISTENCY:BOOL=ON`；`build/junction/caladan_test.config` 为 `runtime_kthreads 1`、`runtime_spinning_kthreads 0`、`runtime_guaranteed_kthreads 0`、`runtime_quantum_us 100`、`enable_storage 1`。
- 本轮运行前执行过 `/home/syh/mkfs/mkfs.sh`，随后 vfork/fish/hostps 三轮测试都正常退出并触发 final flush。测试结束后已确认没有残留 `iokerneld` / `junction_run` 进程。若后续实验需要干净磁盘状态，仍建议重新执行 mkfs。
- 当前工作区仍有大量未跟踪测试、benchmark、报告和结果文件；`git status --short` 还显示 `junction/CMakeLists.txt`、`junction/fs/file.cc`、`junction/fs/shaofs/file.cc`、`junction/fs/shaofs/syscall.cc` 为已修改状态，这些不是本轮多进程验证新增修改，接手时不要误删或回退。

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
    │   │        调用 my_write(inum, buf, &offset, len, direct, append) [shaofs/syscall.cc]
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
    │   │            └─ direct=true → file_write_direct() / file_write_direct_append() [shaofs/file.cc]
    │   │                ├─ 校验 user buffer/offset/length 是否满足 DMA 合约
    │   │                ├─ storage_prepare_user_dma()    → mlock + spdk_mem_register + vtophys 验证
    │   │                ├─ inode_bmap_locked(allocate=true) → 同上
    │   │                ├─ storage_write_user_dma()      → 绕过 Block Cache，user buffer 直接作为 SPDK payload
    │   │                └─ 更新 file_size；O_APPEND 时在 inode 写锁内读取 EOF 并写入
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
│  16384 entries, 256 shards                              │
│  Key: (parent_inum, filename) → Value: (inum, type)     │
│  Policy: LRU, write-through (不负责写回磁盘)             │
│  Backend: dir_lookup() 从磁盘读取目录项                   │
└─────────────────────┬───────────────────────────────────┘
                      │ namei() 路径解析时查询
                      ▼
┌─────────────────────────────────────────────────────────┐
│                    Inode Cache                            │
│  8192 entries, 256 shards                               │
│  Key: inum (int) → Value: MInode (extends DInode)        │
│  Policy: LRU, write-back                                │
│  Backend: 通过 Block Cache 读写 inode table blocks        │
└─────────────────────┬───────────────────────────────────┘
                      │ ic_get_inode() / ic_alloc_inode()
                      ▼
┌─────────────────────────────────────────────────────────┐
│                    Block Cache                            │
│  65536 entries (256MB), 256 shards                      │
│  Key: BlockID (uint64_t) → Value: BlockData (4KB)        │
│  Policy: Metadata-aware LRU, write-back                  │
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
Block N+1...J-1:  Data Groups (repeating: 1 bitmap block + 32768 data blocks)
Block J...end:    Journal Area (default 4096 blocks, 16MB, placed at disk tail)
                  J = sb.journal_blockstart
```

mkfs 侧实现位于 `/home/syh/mkfs/fs.h` 和 `/home/syh/mkfs/mkfs.c`。当前 `DEFAULT_JOURNAL_BLOCKS=4096`，`build_superblock()` 会把 `journal_blockstart` 设置为 `total_blocknum - journal_blocknum`，再只用 journal 之前的空间计算 data group 数。`init_data_groups()` 会清空 journal 区。

shaoFS journal 区内部约定：

```
sb.journal_blockstart ... mount_state_lba-1:
    固定大小 journal slots，每个 slot 66 blocks (= 2 + kMaxJournalEntries)
    slot_base + 0: transaction header
    slot_base + 1: transaction entry table
    slot_base + 2 ... slot_base + 65: logged metadata block images

sb.journal_blockstart + journal_blocknum-1:
    mount dirty marker
```

当前 `kMaxJournalEntries=64`、`kJournalSlotBlocks=66`、`kMaxJournalSlots=32`。可用 slot 数由 `journal_slot_capacity()` 根据 journal 区大小计算，并保留最后一个 journal block 作为 mount dirty marker。恢复时 `journal_recover()` 会扫描所有 slot，校验 header/entry/image checksum，按 transaction `seq` 排序 replay committed slot，然后再根据 dirty marker 决定是否执行 dirty repair。

### 2.4 I/O completion driven preemption 路径

当前 `IO_PREEMPT` 机制跨越 shaoFS、Caladan Runtime 和 IOKernel：

```
shaoFS 初始化
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

### 3.1 shaoFS 源码树

```
junction/fs/shaofs/                    ← 我们的项目代码
├── fs.h                               ← 全局常量 + 磁盘数据结构
│   SuperBlock, DInode(256B), iExtent(24B), BlockData(4KB),
│   Dirent 相关常量, BLOCK_SIZE=4096, INODENUM=32768,
│   ROOT_INO=0, MYPREFIX="FSHAO", DIRECT_EXTENT_NUM=6,
│   IO_PREEMPT 默认值, SHAOFS_REALPATH()/USE_SHAOFS()
│
├── inode.h                            ← MInode = DInode + 目录锁/目录索引/extent hint/dirty range/metadata seq
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
│                                        CRASH_CONSISTENCY=1 时 metadata block 写回走 journal
│                                        data block 支持固定环形队列后台 writeback、dirty generation 校验
├── inodeCache.h/cc                    ← InodeBackend (通过 Block Cache 读写 inode table)
│                                        ic_get_inode, ic_alloc_inode, ic_free_inode, ic_flush_inode
├── dentryCache.h/cc                   ← DentryBackend (调用 dir_lookup 从磁盘读)
│                                        DentryKey(parent_inum, name), DentryValue(inum, type)
│
├── extent.h/cc                        ← inode_bmap_locked (extent 查找 + hint 优化 + 分配)
│                                        legacy direct/indirect extents + simple extent tree
│                                        inode_for_each_extent, inode_free_extent_metadata
├── group.h/cc                         ← alloc_block (per-core group affinity + preempt-safe)
│                                        free_block, init_group, set_newgroup, sync_all_gdt
├── dir.h/cc                           ← Dirent(512B), dirent_is_empty()
│                                        DirReadGuard/DirWriteGuard (rwmutex)
│                                        目录运行时 hash/index + free slot 链表
│                                        dir_lookup, dir_add_entry, dir_delete_entry, dir_is_empty
├── namei.h/cc                         ← namei() / nameiparent() 路径解析 (仅绝对路径)
├── file.h/cc                          ← file_read/write (cached), file_read/write_direct (O_DIRECT)
│                                        buffered O_APPEND/EOF extension batch write, truncate_inode,
│                                        free_inode_data_blocks, final_flush
├── syscall.h/cc                       ← my_open/read/write/mkdir/unlink/lseek/fstat/newfstatat/fsync
│                                        my_write 当前带 append 参数
│                                        fill_stat_from_inode
├── utili.h                            ← SpinGuard, ReadGuard, WriteGuard, RuntimeFSBaseGuard,
│                                        SpinGuardNP, kguard, atomic_read/write/inc/dec
├── dsa.h/cc                           ← Intel DSA/DML 初始化、direction-aware 异步 copy/copyv，
│                                        硬件不可用或策略不满足时回退 CPU memcpy
└── journal.h/cc                       ← metadata-only redo journal + dirty mount repair
                                         sync commit API + batched group commit + async home-block checkpoint + recovery scan
                                         journal_init/recover/mark_dirty/mark_clean,
                                         journal_write_metadata, journal_commit_single,
                                         journal_build_metadata_map
```

注：早期交接内容曾把 `OPTIMIZATION_REPORT.md` 和 `IOPS_BENCHMARK_REPORT.md` 列为 `junction/fs/shaofs` 下的文件；2026-06-03 复查该目录时未发现这两个文件。2026-06-03 ShaoFS 重构阶段的历史摘要见本文 `1.5` / `8.13` 和 `junction/fs/mytest/scripts/results/` 下的结果目录；截至 2026-06-07，仓库根目录 `plan.md` 已被 Junction 多进程验证 checklist 覆盖，当前工作区未发现 `plan2.md`，不要再把当前 `plan.md` 当作旧 ShaoFS 重构计划。

### 3.2 VFS 集成层（Junction 侧）

| 文件 | 职责 |
|------|------|
| **`junction/fs/file.cc`** | syscall dispatch: `usys_read/write/readv/pread64/preadv/pwrite64/fstat/fsync/lseek` 中检查 `SHAOFS` 模式并转发到 `my_*` 或 shaoFS direct readv fast path |
| **`junction/fs/core.cc`** | `usys_openat` 和 `usys_mkdir` 中用 `SHAOFS_REALPATH()` 识别 `FSHAO/path`、`FSHAO:/path` 并转发；O_DIRECT 打开 shaoFS 文件时构建 `DirectReadHint` |
| **`junction/fs/file.h`** | `kFlagDirect=O_DIRECT`, `kFlagTruncate=O_TRUNC`, `FromFlags()`, `File` 类定义；当前 `File` 内嵌 shaoFS `DirectReadHint` |
| **`junction/syscall/seccomp.cc`** | 安装 seccomp BPF；当前 allowlist 包含 Caladan `mlock` wrapper，并按 ioctl request 放行 `VFIO_IOMMU_MAP_DMA` / `VFIO_IOMMU_UNMAP_DMA`，用于 user-buffer DMA 注册 |
| **`lib/caladan/inc/base/syscall.h` / `lib/caladan/base/syscall.S`** | Caladan 受控 Linux syscall wrapper；syscall 指令位于 `[base_syscall_start, base_syscall_end)`，供 Junction seccomp 按 IP 范围放行 |
| **`lib/caladan/runtime/storage.c`** | SPDK submit/completion、storage softirq、user-buffer DMA 注册与 `storage_read_aligned()` / `storage_write_user_dma()` / `storage_read_aligned_batch()` |
| **`junction/kernel/signal.cc`** | Junction UINTR 入口；`InterruptNeeded()` 当前同时检查 preempt cede/yield 和 `storage_available_completions(k)` |
| **`lib/caladan/inc/runtime/thread.h`** | `thread_t` 定义；当前新增 `runtime_fsbase_depth`，用于区分用户 FS base 与 runtime FS base 区域 |
| **`lib/caladan/runtime/sched.c`** | uthread 调度与 FS base 保存/恢复；`thread_save_fsbase()` / `thread_fsbase_to_run()` 避免 shaoFS guard 内 park 时污染用户 TLS |
| **`lib/caladan/iokernel/main.c`** | dataplane 中的 `check_spdk_and_preempt()`，负责跨 Runtime 检查 SPDK completion 并触发 yield |
| **`lib/caladan/iokernel/sched.c`** | `sched_yield_on_core()`；当前读取 live `q_ptrs->rcu_gen`，避免 stale metrics 导致持续抢占失效 |
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
| `test_direct_io.c` | 单元测试 | 严格 O_DIRECT 合约检查：2MB 对齐 arena、4KB 对齐 buffer/len/offset、invalid request 返回 `EINVAL`、direct/cached 跨模式一致性 |
| `shaofs_direct_io_example.c` | 示例程序 | 最小 shaoFS O_DIRECT 示例：分配 2MB arena，传入 4KB 对齐子区间执行 pwrite/pread |
| `test_user_dma_direct.c` | 回归测试 | 验证 2MB 对齐 arena、4KB 子区间、尾部 4KB 子区间、非 4KB 对齐拒绝等 Direct DMA 合约 |
| `batch_direct_read_bench.c` | 对照 benchmark | 使用 O_DIRECT `pread()` 和 `preadv()` 对比 scalar direct read 与显式 batch direct read；默认创建 2MB 对齐 arena，适合验证 `file_readv_direct()` / `storage_read_aligned_batch()` 路径 |
| `test_many_inodes.c` | 回归测试 | 顺序创建大量小文件，用于验证 inode bitmap 分配能越过 inode cache 容量 |
| `test_many_inodes_read_threads.c` | 回归测试 | 创建大量 16KB 文件后用 3 个 pthread 反复整文件读取并校验内容，用于覆盖 Filebench 类似读负载 |
| `test_barrier_sleep.c` | 兼容性测试 | pthread_barrier + sleep() 在 Junction 中的正确性 |
| `junction_multiproc_vfork.c` | Junction 多进程验证 | 双模式 C 测试：controller 显式 `vfork()`，child 立即 `execv()` 同一二进制的 `--worker` 模式；worker 打印 PID/TID/PPID 并写 ShaoFS 文件，parent `waitpid()` 回收并校验输出文件；2026-06-07 用于验证单个 `junction_run` 容器可承载多个 Junction process |
| `myls.c` | 工具 | 列出 shaofs 目录内容 |
| `mystat.c` | 工具 | 显示文件元数据 |
| `mycat.c` | 工具 | 显示文件内容（文本/hex dump） |
| `mytree.c` | 工具 | 递归显示目录树 |
| `run_iops_bench.sh` | 自动化脚本 | 28 组配置的完整 IOPS 测试套件 |
| `shaofs_preempt_latency.c` | 抢占机制测试 | 构造 CPU-bound uthread 干扰单次 O_DIRECT read，观察 I/O completion preemption 是否降低尾延迟 |
| `shaofs_preempt_iops.c` | 抢占机制测试 | 构造长 CPU-bound uthread 干扰连续 O_DIRECT reads，观察抢占对 IOPS 和最大延迟的影响 |
| `shaofs_iopreempt_bench.c` | 抢占机制测试 | 早期/通用抢占 benchmark，保留作参考 |
| `shaofs_storage_st.config` | 运行配置 | 单 runtime kthread + storage enabled 的 Junction config，适合验证 IO_PREEMPT 机制 |
| `journal_layout_probe.c` | Crash consistency 测试 | 打开 shaoFS 根路径，确认带 journal 字段的新 superblock 可以正常 mount |
| `journal_recovery_prepare.c` | Crash consistency 测试 | 创建目录和文件，`--crash` 模式下写入/flush 后循环等待，供外部 `timeout -s KILL` 模拟崩溃 |
| `journal_recovery_check.c` | Crash consistency 测试 | 下一次 mount 后检查崩溃前创建的目录、文件大小和数据内容是否恢复一致 |
| `test_shaofs_unlink.c` | 回归测试 | 覆盖 shaoFS `unlink` 的目录项删除、inode/data 回收和错误码行为 |
| `test_shaofs_dir_index.c` | 回归测试 | 覆盖目录运行时 hash/index 的 lookup/add/delete/free slot 行为 |
| `test_shaofs_append_prealloc.c` | 回归测试 | 覆盖 EOF append 预分配和 extent/数据一致性 |
| `test_shaofs_concurrent_append.c` | 回归测试 | 8 pthread 同时 `O_APPEND` 写同一文件，覆盖 64KB batch path 和 512B scalar tail，验证无 EOF reservation 重叠、无 torn/lost/duplicate records |
| `test_shaofs_many_extents.c` | 回归测试 | 构造单文件稀疏写，多于 legacy 176 extents，验证 simple extent tree 的读回、空洞补 0、fsync/reopen 行为 |
| `test_shaofs_mt_full_extents.c` | 压力回归测试 | 多 pthread 分别写私有文件，可默认写满当前 extent tree 上限，验证每文件大量 extents、并发写、fsync、读回和可选 unlink |
| `test_shaofs_varmail_bottleneck.c` | 诊断 benchmark | 构造 varmail 类 append+fsync/open/read/delete 混合负载，用于定位 fsync/journal 开销 |
| `test_shaofs_fsync_direct_verify.c` | 回归测试 | 验证 `fsync` 后 direct read 能看到 buffered write 的持久化数据 |
| `shaofs_prepare_large_files.c` | 数据准备工具 | 在 ShaoFS 中顺序创建 `FSHAO:/fio128_large.<id>` 大文件；2026-06-01 用于准备 256 × 3575MiB near-full random-read 数据集 |
| `shaofs_async_randread_iops.c` | 历史诊断 benchmark | 曾通过 `SHAOFS_IOC_ASYNC_RANDREAD` ioctl 直接进入 ShaoFS 内部 fixed-QD async randread benchmark；2026-06-02 清理后核心 ioctl dispatch 和实现已删除，当前该未跟踪测试源文件仅作为参考保留，不能直接代表现有功能 |

### 3.4 Benchmark 与补丁工作区文件

| Path | Purpose |
|------|---------|
| `junction/fs/mytest/benchmark/fio` | FIO 源码子仓库；当前适配通过 `patch/fio_changes.patch` 管理 |
| `junction/fs/mytest/benchmark/filebench` | Filebench 源码目录；当前适配通过 `patch/filebench_changes.patch` 管理 |
| `junction/fs/mytest/benchmark/fxmark` | FxMark 源码目录；当前适配通过 `patch/fxmark_changes.patch` 管理 |
| `junction/fs/mytest/benchmark/patch/toggle_fio.sh` | 应用/撤销 FIO 适配补丁并重新 configure/make |
| `junction/fs/mytest/benchmark/patch/toggle_filebench.sh` | 应用/撤销 Filebench 适配补丁并重新 configure/make |
| `junction/fs/mytest/benchmark/patch/toggle_fxmark.sh` | 应用/撤销 FxMark 适配补丁并重新 `make -j $(nproc)` |
| `junction/fs/mytest/benchmark/patch/fxmark_changes.patch` | FxMark Junction 适配补丁；当前覆盖 `Makefile`、`src/bench.c`、`src/DRBL.c`、`src/util.c` |
| `junction/fs/mytest/benchmark/fio_test/directio.fio` | shaoFS FIO direct I/O 配置：`FSHAO/`、16 jobs、4KB O_DIRECT random read、60s |
| `junction/fs/mytest/benchmark/fio_test/psync_{64,128,256,512}job_randread_sweep.fio` | shaoFS FIO 高并发 O_DIRECT psync random read 扫描配置；用于更突出单 runtime kthread + 多 uthread 的调度优势 |
| `junction/fs/mytest/benchmark/fio_test/psync_128job_randread_large.fio` | shaoFS FIO 大工作集 O_DIRECT random read 配置：128 jobs × 4GiB private files，避免 64GiB 小随机范围低估 PM9A3 randread IOPS |
| `junction/fs/mytest/benchmark/fio_test/psync_256job_randread_full_direct.fio` | shaoFS FIO near-full O_DIRECT random read 配置：256 jobs × 3575MiB private files，`psync`/`iodepth=1`/`direct=1`，用于 2026-06-01 core sweep |
| `junction/fs/mytest/benchmark/fio_test/ext4_prepare_256x3500m.fio` | ext4 对比数据准备配置：在 `/mnt/ext4_cmp` 创建 256 × 3500MiB private files；3575MiB 在 ext4 上因 metadata/journal 开销曾触发 ENOSPC |
| `junction/fs/mytest/benchmark/fio_test/ext4_psync_256job_randread_3500m_direct.fio` | ext4 对比 O_DIRECT random read 配置：256 jobs × 3500MiB private files，`psync`/`iodepth=1`/`direct=1`，与 ShaoFS near-full jobfile 形态对应 |
| `junction/fs/mytest/scripts/cg_run.sh` | 通用 cgroup v2 runner：创建 cpuset/memory cgroup，运行目标命令，收集 `cpu.stat` / `memory.events` / `memory.peak` 并清理 |
| `junction/fs/mytest/scripts/run_ext4_fio.sh` | ext4 FIO 主脚本：revert FIO patch、reset ext4、drop cache、通过 `cg_run.sh` 跑 FIO 并保存 log/cgroup stats |
| `junction/fs/mytest/scripts/fio_test/ext4_directio.fio` | ext4 版 16-job direct I/O FIO 配置，和 shaoFS `directio.fio` 对应 |
| `junction/fs/mytest/scripts/fio_test/psync_128job_randread.fio` | ext4 版 128-job psync random read 配置，和 shaoFS `psync_128job_randread_sweep.fio` 形态对应 |
| `junction/fs/mytest/scripts/fio_test/psync_128job_randread_large.fio` | ext4 版大工作集 128-job psync random read 配置，目录为 `/mnt/nvme/ext4_bench` |
| `junction/fs/mytest/benchmark/filebench_wml/fileserver.f` | 当前用于 shaoFS 的 Filebench fileserver workload：`FSHAO:`、10000 files、50 threads、60s runtime |
| `junction/fs/mytest/benchmark/filebench_wml/webserver.f` | 当前用于 shaoFS 的 Filebench webserver workload：`FSHAO:`、10000 files、100 threads、60s runtime |
| `junction/fs/mytest/benchmark/filebench_wml/varmail.f` | 当前用于 shaoFS 的 Filebench varmail workload：`FSHAO:`、5000 files、16 threads、60s runtime；用于 fsync/journal 优化验证 |
| `junction/fs/mytest/benchmark/filebench_wml/webproxy.f` | 当前用于 shaoFS 的 Filebench webproxy workload：`FSHAO:`、10000 files、100 threads、60s runtime |
| `junction/fs/mytest/scripts/run_ext4_filebench.sh` | ext4 Filebench 主脚本：revert Filebench patch、reset ext4、drop cache，并通过 `cg_run.sh` 按指定 WML/cgroup 参数运行原生 Filebench |
| `junction/fs/mytest/scripts/run_filebench_compare.sh` | ShaoFS/ext4 Filebench 四项统一测试脚本；参数 `0/1/2` 分别表示只测 shaoFS、只测 ext4、两者都测 |
| `junction/fs/mytest/scripts/filebench_test/ext4_fileserver.f` | ext4 版 fileserver workload，默认由 `run_ext4_filebench.sh --wml` 使用 |
| `junction/fs/mytest/scripts/filebench_test/ext4_webserver.f` | ext4 版 webserver workload |
| `junction/fs/mytest/scripts/filebench_test/ext4_varmail.f` | ext4 版 varmail workload；与 shaoFS `varmail.f` 参数保持对应，只替换测试目录 |
| `/home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh` | repo 外部 ext4 对比脚本；会 reset ext4、设置 cgroup v2 CPU/内存限制并运行 Filebench randomread |
| `/home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh` | repo 外部 ext4 `fileserver.f` 对比脚本；会 reset ext4、生成只替换 `$dir` 的临时 WML、设置 cgroup v2 CPU/内存限制并运行 Filebench |
| `/home/syh/fs_test/results/` | repo 外部 ext4 benchmark 输出目录 |

### 3.5 Caladan raw storage benchmark 文件

本轮新增的 Caladan raw storage benchmark 位于 `lib/caladan/tests/`，目的是绕过 shaoFS、Junction syscall dispatch 和 FIO 适配层，直接测 Caladan runtime + SPDK storage API 是否能把 PM9A3 压到硬件上限。

| Path | Purpose |
|------|---------|
| `lib/caladan/inc/runtime/storage.h` | 新增 `struct storage_async_req`、`storage_async_read()`、`storage_async_write()`、`storage_async_poll()`；这是 fixed-QD benchmark 的最小 async storage API |
| `lib/caladan/runtime/storage.c` | 实现 async read/write/poll；同时把 `SAMSUNG MZQL2960HCJR` 加入 `known_devices[]`，使当前 PM9A3 被识别为低延迟 NVMe |
| `lib/caladan/tests/test_storage_async_iops.c` | fixed-QD raw benchmark；每个 poller 维护固定 outstanding depth，completion callback 只把完成 slot 放入本 poller CQ，由 poll loop 继续重提交 |
| `lib/caladan/tests/run_storage_async_iops.sh` | 自动启动/清理 iokernel、运行 `test_storage_async_iops`、收集 `test.log`/`iokernel.log`/`summary.txt`；默认 `RANGE_MB=0` 表示使用全 namespace |
| `lib/caladan/tests/storage_{1c,2c,4c,8c}_q0.config` | raw benchmark 专用 runtime config：`runtime_kthreads` 与 `runtime_spinning_kthreads` 分别为 1/2/4/8，`runtime_quantum_us=0`，`enable_storage=1`，`storage_quota_enabled=false` |
| `lib/caladan/tests/storage.config` | 用户此前创建的手动 storage config，当前为 untracked；包含 `runtime_kthreads 10`、`runtime_spinning_kthreads 10`、`enable_storage 1`、`quota_enabled false` |

注意：`test_storage_iops.c` 当前已恢复为 Caladan 原始写 IOPS smoke benchmark，不再承载本轮 randread/QD 诊断。早期探索用 `test_storage_randread_iops` 已删除，后续不要基于它继续分析。

---

## 第四章：核心数据结构精确定义

具体内容可能已更新，请阅读相关源代码来确定。

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
    BlockID  journal_blockstart;    // Journal 区域起始 LBA（盘尾预留）
    uint64_t journal_blocknum;      // Journal 区域块数
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
    DirIndex*         dir_index;    // 目录运行时 hash/index，仅驻留内存，不写盘
    mutable iExtent   extent_hint;  // 上次 extent 查找缓存（顺序访问 O(1)）
    spinlock_t        hint_lock;    // 保护读锁持有期间对 extent_hint 的并发更新
    volatile int      has_dirty_data_cache; // 标记该 inode 是否存在脏的 buffered data cache
    spinlock_t        dirty_lock;   // 保护 dirty byte range
    uint64_t          dirty_data_start, dirty_data_end, dirty_data_seq;
    uint64_t          inode_dirty_seq; // inode 盘上元数据变更序号
    uint64_t          inode_fsync_seq; // 最近一次 fsync 已持久化的 inode_dirty_seq
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
| `LEGACY_MAX_EXTENT_NUM` | 176 | legacy 布局上限：6 direct + 170 flat indirect extents |
| `EXTENT_TREE_ROOT_REFS` | 169 | simple extent tree root 中可保存的 leaf 引用数 |
| `EXTENT_TREE_LEAF_EXTENTS` | 169 | simple extent tree 每个 leaf 可保存的 extents 数 |
| **最大 extent 数/文件** | **28567** | 当前 simple extent tree 上限：6 direct + 169 * 169 leaf extents |
| `DATABLOCKS_PERGROUP` | 32768 | 每个 group 的数据块数 |
| `TOTALBLOCKS_PERGROUP` | 32769 | 1 bitmap + 32768 data |
| `DEFAULT_JOURNAL_BLOCKS` | 4096 | 默认 journal 区大小，位于盘尾，约 16MB |
| `ROOT_INO` | 0 | 根目录 inode 编号 |
| `MYPREFIX` | `"FSHAO"` | 当前 shaofs 识别前缀；`SHAOFS_REALPATH()` 同时接受 `FSHAO/path` 和 `FSHAO:/path`，会把 `FSHAO`、`FSHAO:` 映射为 `/`，并拒绝 `FSHAOabc` 这类伪前缀。FIO 命令仍建议优先用 `FSHAO/` 规避未转义冒号分隔问题 |
| `IO_PREEMPT` | 默认 0 | 是否启用 IOKernel 检查 SPDK completion 并抢占目标 Runtime core；可由 CMake `SHAOFS_IO_PREEMPT=ON` 定义为 1 |
| `CRASH_CONSISTENCY` | 默认 1 | 是否启用 shaoFS metadata journal；可由 CMake `SHAOFS_CRASH_CONSISTENCY=OFF` 定义为 0 |
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

### 5.2 Extent 查找、Hint 与 Simple Extent Tree（`extent.cc:inode_bmap_locked()`）

```
inode_bmap_locked(inode_ptr, logical_blk, allocate, is_new):
    ① Hint Fast Path:
       if extent_hint 覆盖 logical_blk → 直接返回 physical_blk  // O(1)

    ② Direct Extents:
       binary_search(direct_extents[0..5], logical_blk)
       if 命中 → 更新 hint, 返回

    ③ Indirect Extents:
       if valid_extent_count <= 176:
           indirect_extent_block 保存 flat iExtent[170]
           binary_search(indirect_extents[0..169], logical_blk)
       else:
           indirect_extent_block 保存 ExtentTreeHeader + ExtentLeafRef[]
           root refs 二分定位 leaf → leaf extents 二分定位 logical_blk
       if 命中 → 更新 hint, 返回

    ④ 如果 allocate=false → 返回 INVALID (稀疏文件空洞)

    ⑤ 分配新块:
       EOF append fast path 先尝试合并/追加最后一个 extent
       普通文件大 append 可批量预分配并合并物理 runs
       slow path 收集所有 extents + 新 extent → compact_extents_inplace()
       ext_count <= 176 写回 legacy 布局，否则写回 simple extent tree
       返回 new_phys
```

**simple extent tree**：

2026-05-20 的 `ea98931` 已把“单 inode 最多 176 个 extents”的旧限制扩展为固定深度 simple extent tree。这个设计刻意很轻量，目标是保证常见小文件和顺序/append workload 仍走原来的快路径，同时让稀疏写、随机碎片写或强压力测试可以支撑大量 extents。

- `DInode::direct_extents[6]` 不变，前 6 个 extents 仍直接放在 inode 内。
- 当 `valid_extent_count <= LEGACY_MAX_EXTENT_NUM` 时，`indirect_extent_block` 仍保存 legacy flat `iExtent[170]`，这避免小文件/少 extent 文件为 tree 付出额外 leaf lookup 成本。
- 当 `valid_extent_count > LEGACY_MAX_EXTENT_NUM` 时，`uses_extent_tree()` 为 true，`indirect_extent_block` 改为 tree root block：`ExtentTreeHeader + ExtentLeafRef[]`；每个 leaf block 保存 `ExtentLeafHeader + iExtent[]`。
- 当前 root 最多 169 个 leaf refs，每个 leaf 最多 169 个 extents，因此单文件最大 extent 数为 `6 + 169 * 169 = 28567`。这不是无限 extent 结构，超过该上限仍会失败。
- `bmap_lookup_tree()` 先在 root refs 中二分找到 leaf，再在 leaf 内二分查找 extent；命中后更新 `extent_hint`。
- append 场景优先走 `bmap_try_append_extent()`：可与最后一个 extent 合并，或直接追加到最后一个 leaf；只有乱序/碎片插入才进入收集、排序、合并、重写 tree 的 slow path。
- `inode_for_each_extent()` / `inode_for_each_extent_metadata_block()` / `inode_flush_extent_metadata()` / `inode_free_extent_metadata()` 是 tree-aware helper。`unlink`、`truncate`、`ic_free_inode()`、`stat.st_blocks`、fsync metadata flush、journal repair/recovery 都应通过这些 helper 或等价 tree-aware 逻辑处理 extents。

**append 预分配**：

当前 `extent.cc` 已不再对普通文件 EOF append 一律单块分配。`inode_bmap_locked()` 在 `allocate=true` 且未命中现有 extent 时，会先判断是否满足：

```cpp
inode->type == REGULAR &&
inode->file_size >= 256KB &&
logical_blk == ceil(file_size / BLOCK_SIZE)
```

若满足，则进入 `bmap_prealloc_append()`：

1. 根据当前文件大小选择预分配块数：小于 1MB 时 64 blocks，更大时 128 blocks。当前代码保留了 16-block 常量，但由于触发阈值已经是 256KB，正常路径不会选到 16 blocks。
2. 调用 `alloc_blocks(blocks, want)` 批量获取物理块。
3. 将返回的物理块按物理连续性切分成一个或多个 `iExtent` run。
4. 调用 `bmap_insert_compact_extents()` 把这些 run 与已有 extent 一起排序、合并、写回 legacy 或 tree extent 元数据。
5. 如果插入失败，调用 `bc_invalidate_block()` 丢弃相关 cache entry，并用 `free_extent()` 释放已分配物理块。

这个改动的目标是减少 Filebench `fileserver.f` 中 `appendfilerand` 长时间运行时的 extent 碎片和元数据更新次数。当前实现只对普通文件 EOF append 生效，目录、稀疏远距离写和非 append 场景仍走单块分配 fallback；这些场景现在主要依赖 simple extent tree 承载大量 extents。

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

### 5.4 O_DIRECT user-buffer DMA 路径

当前 `O_DIRECT` 路径的目标是绕过 Block Cache，并在满足严格约束时把用户传入的 buffer 直接作为 SPDK NVMe payload，避免“NVMe → SPDK bounce buffer → user buffer”的额外 memcpy。

**用户 buffer 合约**：

- 用户传入的 `buf` 必须 4KB 对齐。
- 用户传入的 `len` 必须是 4KB 的倍数。
- `offset` 必须非负且 4KB 对齐。
- direct read/write 内部还要求每次提交给 NVMe 的片段是整 4KB 块；当前不支持 direct partial-block I/O。
- 不满足上述条件时，`file_read_direct()` / `file_write_direct()` 返回错误；当前实现故意不回退到 bounce buffer + memcpy。
- 底层 DMA 注册仍按覆盖用户子区间的 2MB 范围进行。因此实际使用时推荐通过 `posix_memalign(..., 2MB, 2MB 或更大)` 分配 arena，再传入 arena 内部 4KB 对齐的子区间；只传一个孤立的 4KB malloc 小块通常无法保证覆盖 2MB 注册区间都属于可安全 pin/register 的用户映射。

**注册和验证流程**：

```
file_read_direct/file_write_direct
    ├─ user_dma_request_ok(buf, offset, len)
    ├─ storage_prepare_user_dma(buf, len)
    │   ├─ 4KB 对齐检查
    │   ├─ 计算覆盖 [buf, buf+len) 的 2MB 注册区间 [reg_base, reg_end)
    │   ├─ 查 user_dma_pages[] 2MB 页粒度 cache
    │   ├─ syscall_mlock(reg_base, reg_end-reg_base)          // pin 覆盖用户子区间的 2MB 注册区
    │   ├─ spdk_mem_register(reg_base, reg_end-reg_base)      // 建立 SPDK/DPDK/VFIO DMA 映射
    │   ├─ rc == -EBUSY 时按 2MB 页逐段注册
    │   ├─ spdk_vtophys() 遍历验证实际 [buf, buf+len) 可转 IOVA
    │   └─ 成功后写入 user_dma_pages[] cache
    └─ 进入实际 direct I/O
```

**direct read**：

- `file_read_direct()` 通过 inode extent 查找目标物理块。
- 如果逻辑块是 sparse hole，直接 `memset()` 用户 buffer 为 0，不下发磁盘 I/O。
- 如果 `MInode::has_dirty_data_cache` 置位，读取前对目标物理块调用 `bc_flush_block()`，保证 buffered write 后 direct read 能读到最新盘上数据。
- 调用 `storage_read_aligned(dst, phys_blk, block_count)`；该函数直接把 `dst` 传给 `spdk_nvme_ns_cmd_read()`。

**direct write**：

- `file_write_direct()` 对已分配且不扩展文件大小的块走读锁 fast path；需要分配新块或扩展文件时走 inode 写锁 slow path。
- 写盘前调用 `bc_flush_block(phys_blk)`，随后 `storage_write_user_dma(src, phys_blk, block_count)` 直接把用户 buffer 传给 `spdk_nvme_ns_cmd_write()`。
- 写盘后调用 `bc_invalidate_block(phys_blk)`，避免 Block Cache 中旧副本覆盖 direct write 的新数据。

**DirectReadHint fast path**：

- shaoFS O_DIRECT open 时，`core.cc:usys_openat()` 会调用 `file_prepare_direct_read_hint()`，把当前文件 extents、file size 和 `has_dirty_data_cache` 指针缓存进 `File::shaofs_direct_read_hint_`。
- `usys_read()` / `usys_pread64()` 在 hint valid 时直接调用 `file_read_direct_hint()`，减少每次 read 的 inode cache 查找和 extent 读取开销。
- `usys_write()` 会把 hint 标记为 invalid，避免 direct write 或 buffered write 后继续使用旧 extent/size 快照。

**seccomp/syscall 前提**：

- `storage_prepare_user_dma()` 调用 `syscall_mlock()`，需要 `lib/caladan/base/syscall.S` 中的 wrapper 且 `junction/syscall/seccomp.cc` 中有 `ALLOW_CALADAN_SYSCALL(mlock)`。
- `spdk_mem_register()` 内部需要 VFIO DMA map/unmap ioctl；当前 seccomp 通过 `ALLOW_IOCTL_REQUEST(VFIO_IOMMU_MAP_DMA)` 和 `ALLOW_IOCTL_REQUEST(VFIO_IOMMU_UNMAP_DMA)` 按 ioctl request 放行。
- 这些放行只服务于 DMA 注册；不要把它理解为 Junction 内可以任意调用 Linux syscall。

### 5.5 shaoFS direct `readv/preadv` 显式批量读路径

当前代码已经实现 shaoFS direct fd 的 `readv()` / `preadv()` dispatch。它的目标是给“应用显式一次提交多个 4KB 读”的场景提供对照路径，减少每 4KB 一次 syscall/uthread park 的开销；它不是透明地把所有普通 `pread()` 自动合并，也没有实现底层 NVMe doorbell delayed-submit。

调用链：

```
用户程序 preadv(fd, iov, iovcnt, off) / readv(fd, iov, iovcnt)
    │
    ▼
junction/fs/file.cc::usys_preadv() / usys_readv()
    ├─ 非 shaoFS 或非 O_DIRECT → 原 Junction VFS / scalar fallback
    └─ shaoFS + O_DIRECT → file_readv_direct(inum, iov, iovcnt, offset)
        ├─ RuntimeFSBaseGuard
        ├─ ic_get_inode() + inode read lock
        ├─ 检查每个 iovec 的 4KB 对齐和 EOF 边界
        ├─ inode_bmap_locked(allocate=false) 查物理块
        ├─ 生成 small_vector<storage_batch_read, 64>
        └─ storage_read_aligned_batch(reqs.data(), reqs.size())
            ├─ 为每个请求确认/注册 user DMA
            ├─ 在同一个 storage qpair lock 下提交多个 spdk_nvme_ns_cmd_read()
            ├─ completion.remaining 归零后唤醒当前 uthread
            └─ 当前 uthread 只 park 一次
```

当前边界条件：

- 只覆盖 **shaoFS + O_DIRECT + readv/preadv**；`writev/pwritev` 仍走 Junction `File::Writev()`，未接入 shaoFS direct writev。
- 每个 iovec 必须满足 shaoFS direct DMA 合约：`iov_base` 4KB 对齐，`iov_len` 为 4KB 倍数，整体 offset 4KB 对齐。
- 如果 inode 有 dirty buffered cache，或遇到 sparse hole / 非整块 EOF 片段，代码会走 `file_readv_direct_scalar()` fallback。
- `storage_read_aligned_batch()` 只是“一次提交多个 read 后一次 park”；当前每个 `spdk_nvme_ns_cmd_read()` 仍按 SPDK 默认路径提交，未证明能减少 NVMe SQ doorbell/MMIO 次数。
- 对照测试程序是 `junction/fs/mytest/batch_direct_read_bench.c`。它会先用 O_DIRECT 准备文件，再分别测 scalar `pread()` 与 batch `preadv()`。

### 5.6 fsync dirty range + metadata sequence 快路径

```
my_fsync(inum):
    ① 获取 inode 读锁，读取 inode_dirty_seq / inode_fsync_seq
    ② 在 dirty_lock 下读取 buffered write 产生的 dirty byte range 和 dirty_data_seq
    ③ 如果存在 dirty data，只遍历 [dirty_data_start, dirty_data_end) 覆盖的逻辑块
       └─ inode_bmap_locked(allocate=false) 找物理块，并把连续物理块合并为 run
       └─ 对每个 run 调用 bc_flush_blocks_contiguous(run_start, run_count)
    ④ 仅当 inode_dirty_seq != inode_fsync_seq 时调用 inode_flush_extent_metadata()
    ⑤ 若无 dirty data 且 inode_dirty_seq == inode_fsync_seq，直接返回
    ⑥ 若 dirty_data_seq 未变化，清空 dirty byte range 并递增 dirty_data_seq
    ⑦ 若 inode 元数据序号需要持久化，调用 ic_flush_inode(inum)，成功后更新 inode_fsync_seq
```

这条路径替代了旧的“每次 fsync 都遍历文件所有 extents 并刷所有数据块”的实现。`mark_inode_metadata_dirty()` 当前在文件大小增长、extent 新增/合并/压缩、inode alloc/free、目录 add/delete entry、`mkdir` nlink 更新等路径调用，使 `fsync` 可以区分“只有 clean repeated fsync”和“确实有 inode/extent 元数据需要落盘”的情况。2026-05-23 当前实现已经把 extent metadata flush 收敛到 `need_inode_flush` 为真时执行，不再在 clean fsync 或仅数据 dirty 的路径上无条件 flush extent metadata。

重要边界：dirty range 只描述 buffered write 放进 Block Cache 的数据块。Direct write 绕过 Block Cache 并在写后 invalidate 对应 cache block；direct read 在看到 `has_dirty_data_cache` 时会先 flush 目标物理块，保证 cached/direct 一致性。

### 5.7 目录操作的读写锁模型

```
dir_lookup()      → DirReadGuard  (rwmutex_rdlock)  → 允许并发 lookup
dir_is_empty()    → DirReadGuard  (rwmutex_rdlock)
dir_add_entry()   → DirWriteGuard (rwmutex_wrlock)  → 排他增删
dir_delete_entry()→ DirWriteGuard (rwmutex_wrlock)
```

`dir_foreach_locked()` 是 static 模板函数，调用前 caller 必须已持有锁。

当前代码还为每个目录 inode 增加了内存态 `DirIndex`。第一次 `dir_add_entry()` 或需要写入目录时会在持有目录写锁的情况下扫描目录块，构建 hash bucket、free slot 链表和 live child 计数；之后 `dir_lookup()` 命中已存在索引时可以直接按文件名 hash 查找，不再线性扫描目录文件。`dir_add_entry()` 优先复用 free slot，否则追加到目录尾部；`dir_delete_entry()` 将目录项写成空 slot，并把对应 `DirIndexNode` 放回 free slot 链表。该索引不写入磁盘，inode eviction/destruction 时通过 `MInode::drop_dir_index()` 释放。

`DirIndex` 与 `dentryCache` 有交叉但不是同一个东西：

- `dentryCache` 是全局 sharded LRU，key 为 `(parent_inum, name)`，value 为 `(inum, type)`；`namei()` 每解析一级路径都会先查它，miss 后由 `DentryBackend::read()` 调用 `dir_lookup()`。
- `DirIndex` 是某一个目录 inode 内部的运行时索引，除了 `(name -> inum/type)`，还保存目录项 `offset`、`free_slots` 和 `live_children`。这些字段用于目录文件内部管理，`dentryCache` 无法替代。
- 因此当前调用层次是：`namei()` → `dentryCache` → miss 时 `dir_lookup()` → 已建 `DirIndex` 则 hash 查找，否则扫描目录文件。
- 若要简化架构，可以评估弱化或删除 `dentryCache`，让 `namei()` 直接依赖 `dir_lookup()`/`DirIndex`；但不能简单删除 `DirIndex`，因为它承担 free slot 复用和 `dir_is_empty()` 快路径。

内存风险：`DirIndexNode` 在当前 x86_64 布局约 `296B`，`DirIndexChunk` 固定包含 512 个 node，约 `148KB`。因此一个很小的目录只要触发 `dir_ensure_index_locked()`，最低也会分配约 148KB 的节点 chunk。少量热点目录可接受，但如果大量小目录都发生 add/delete，会有明显内存放大。后续优化方向是小目录延迟建索引、降低 chunk size，或超过目录项阈值后再建完整 hash index。

### 5.8 I/O completion driven preemption（`IO_PREEMPT`）

**设计目标**：降低“SPDK I/O 已完成但 Runtime 正在执行 CPU-bound uthread，导致 completion 无人处理”的延迟。该机制特别适合少量 kthread 上混合运行 I/O uthread 和长时间计算 uthread 的实验场景。

**开关与共享状态**：

- CMake option：`SHAOFS_IO_PREEMPT`，定义在 `junction/CMakeLists.txt`。默认 OFF；当前 `build/CMakeCache.txt` 中为 ON。
- 编译宏：`IO_PREEMPT`，默认在 `junction/fs/shaofs/fs.h` 中为 0。
- Runtime/IOKernel 共享字段：`runtime_info->spdk_uipi`，定义在 `lib/caladan/inc/iokernel/control.h`。
- shaoFS `init_meta()` 在 `IO_PREEMPT=1` 时设置 `spdk_uipi=1`；`final_flush()` 清零。

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

`sched_yield_on_core()` 应读取 live `th->q_ptrs->rcu_gen`，不能使用 `th->metrics.rcu_gen`。后者依赖 IOKernel 调度统计刷新，在连续 completion 场景下可能是旧值，导致后续 yield 被误判为重复请求，从而出现“第一批抢占有效，持续 I/O 又被 CPU-bound uthread 卡住”的问题。

**性能适用场景**：

- Runtime kthread 数量少，尤其是单 kthread。
- 同一 kthread 上有 I/O uthread 和 CPU-bound uthread。
- CPU-bound uthread 不频繁主动 yield。
- 小块随机读写或 latency-sensitive I/O。
- NVMe completion 已经到达，但 Runtime 需要被外部提示去 poll。

**收益不明显或需谨慎的场景**：

- Runtime 本来就在频繁 poll storage，没有 CPU-bound 干扰。
- core/kthread 充足，I/O uthread 总能及时运行。
- 大块顺序吞吐已受 SSD 带宽、shaoFS 数据路径 memcpy、cache flush 或 direct path 限制。
- 当前 uthread 长时间处于 `preempt_disable()`、runtime stack 或不适合被中断的状态，UIPI 会被延迟处理。
- completion rate 极高时，需要评估 UIPI/coalescing 开销，避免 IOKernel 自身成为瓶颈。

### 5.8.1 Block Cache 后台 data writeback

当前 `blockCache.cc` 增加了专门面向普通文件数据块的后台写回机制，目标是把 Filebench `fileserver/varmail` 中大量 buffered data flush 从前台 fsync/open-close 路径移走，同时不把 metadata checkpoint 语义混在 data path 里。

核心结构：

- `WritebackQueue` 是固定大小环形队列，当前 `kWritebackQueueSize=131072`，不在热路径动态分配内存。
- 队列项只有 `{ BlockID block; uint64_t gen; }`，其中 `dirty_gen` 用来防止旧队列项把后来再次写脏的 cache entry 错误清 clean。
- 当前有 `kWritebackWorkerCount=4` 个后台 worker，worker 从队列取 block 后按物理连续性聚合，最多一次处理 `kWritebackBatchMax=32` 个块。
- `CacheEntry` 新增 `dirty_gen` 和 `writeback_queued`。`bc_mark_data_block_dirty()` 设置 dirty/generation 并入队；`bc_mark_block_dirty()` 只标脏，不入队。
- `bc_clean_block_if_unchanged(block, checkpoint_image)` 只在 cache 中当前内容仍等于已经落盘的 checkpoint image 时清 dirty，避免并发覆盖。

当前使用边界：

- EOF extension / buffered append 的 full-block batch path 和 existing-block batch path 会调用 `bc_mark_data_block_dirty()`，让大块数据写回尽早后台化。
- 小块/标量写仍主要调用 `bc_mark_block_dirty()`，避免 webserver 这类大量小文件/读主导 workload 因过度后台写回而增加队列、锁和 I/O 干扰。
- metadata block 不走 data writeback 队列；metadata dirty 的持久化由 journal commit/checkpoint 机制负责。
- `shaofs_sync_all()` 调用 `bc_drain_writeback()`，`final_flush()` 调用 `bc_stop_writeback_and_drain()`，之后再 drain journal checkpoint、写 imap/GDT/inode cache/block cache，并在 clean shutdown 时清 dirty marker。

### 5.9 Crash consistency：metadata-only redo journal + dirty repair

**设计目标**：当前 shaoFS 没有实现完整 POSIX 级事务语义，也不 journal 普通文件数据块。本机制的目标是在学术测试场景下，以较低 CPU/IO 开销保证异常退出后盘上元数据回到合法、自洽状态，避免 inode bitmap、group bitmap、GDT、inode table 和目录项互相矛盾。

**构建开关**：

- CMake option：`SHAOFS_CRASH_CONSISTENCY`，定义在 `junction/fs/CMakeLists.txt`，默认 ON。
- 编译宏：`CRASH_CONSISTENCY`，默认在 `junction/fs/shaofs/fs.h` 中为 1。
- 关闭时 `journal.h` 中相关 API 退化为 no-op 或原始 `storage_write_obj()`，系统行为尽量接近原始实现。

**journal transaction/slot 格式**：

- `JournalHeader`：magic/version/state/entry_count/seq/checksum。
- `JournalEntry`：home block、journal image block、image checksum。
- 单个事务最多 `kMaxJournalEntries=64` 个 metadata block。
- 每个 slot 固定 66 blocks：header block、entry table block、最多 64 个 image blocks。
- 当前最多使用 `kMaxJournalSlots=32` 个 slot；最后一个 journal block 始终保留为 mount dirty marker。
- checksum 使用 FNV-1a，用于发现 torn header、torn entry 或 image 损坏。

**metadata 写入流程**：

```
journal_commit_blocks_impl(blocks, images, count, checkpoint_async)
    │
    ├─ 校验目标块属于 metadata block
    ├─ 获取 journal_commit_lock
    ├─ checkpoint_async=true 时确保 checkpoint worker 已启动，并等待目标 slot 空闲
    ├─ 将每个 4KB metadata 新镜像写到 journal image block
    ├─ 写 entry table
    ├─ 写 state=COMMITTED 的 transaction header
    ├─ checkpoint_async=false:
    │   └─ 当前线程同步 checkpoint_home_blocks(): 写回 home blocks，并清空 transaction header 后返回
    └─ checkpoint_async=true:
        ├─ 复制 home block、entry 和 4KB image 到预分配 CheckpointTask
        ├─ 把 task 放入固定 checkpoint queue
        └─ 后台 checkpoint_worker 写回 home blocks，随后清 slot header
```

这里采用 redo journal：崩溃恢复时，如果看到 checksum 正确且 `COMMITTED` 的 header，就把 journal image 重新写回 home block。普通数据块不进入 journal，仍直接走 SPDK/DMA 写盘路径。

当前有两条 commit API：

- `journal_commit_blocks()` / `journal_commit_single()` 是同步 checkpoint 路径。generic cache backend write、dirty repair、`journal_write_metadata()` 等仍使用这条路径，调用返回时 home block 已经写回。
- `journal_commit_single_batched()` 进入 `journal_commit_single_grouped()`：多个并发单块 metadata commit 会在 `group_lock` 下组成 group，并把同一 home block 的重复请求去重到最后一个镜像；group 最终调用 `journal_commit_blocks_async_checkpoint()`，即 header 持久化后把 home-block checkpoint 交给后台 worker。当前 `kGroupCommitWindowUs=0`，不主动等待窗口，只利用已经排队的自然并发。

`bc_flush_block_batched()` 对 metadata block 的处理非常关键：它调用 `journal_commit_single_batched()` 后直接返回 true，但不清除 cache entry 的 dirty bit。后台 checkpoint worker 在写回 home block 后调用 `bc_clean_block_if_unchanged()`，只有当 cache 中内容仍与 checkpoint image 一致时才清 dirty。这个 dirty generation/image compare 约束用于防止旧事务 checkpoint 覆盖或清理新事务产生的 dirty 状态。

**metadata block 判定**：

- `block < sb.group_blockstart`：superblock、imap、inode table、indirect extent blocks、GDT 等固定元数据。
- 每个 data group 的 bitmap block。
- 已登记的目录数据块。
- journal 区自身。

目录块比较特殊，因为它们物理上位于 data group 内。当前通过 `journal_build_metadata_map()` 在 mount 时扫描目录 inode 的 extents，并通过 `journal_register_metadata_block()` 在新目录块分配后登记。

**mount / shutdown 流程**：

```
init_meta()
    ├─ 读取 superblock
    ├─ journal_init()
    ├─ journal_recover()
    ├─ journal_mark_dirty()
    ├─ 读取 imap / gmap
    ├─ journal_build_metadata_map()
    ├─ init_group()
    ├─ init_file_io()
    └─ bc_start_writeback()

final_flush()
    ├─ bc_stop_writeback_and_drain()
    ├─ journal_drain_checkpoint()
    ├─ journal_write_metadata(imap)
    ├─ sync_all_gdt() → journal_write_metadata(GDT)
    ├─ ic_flush_all()
    ├─ bc_flush_all()
    ├─ journal_drain_checkpoint()
    └─ journal_mark_clean()
```

`journal_mark_dirty()` 写在 journal 区最后一个块。正常退出时 `journal_mark_clean()` 清掉它；如果进程被 `SIGKILL`、崩溃或 timeout 强杀，下一次 mount 会看到 dirty marker。

**恢复流程**：

`journal_recover()` 会扫描所有 slot：

- header 全 0：跳过。
- header 无效或版本不认识：清该 slot header，进入 repair。
- entry_count 不合法：清该 slot header，进入 repair。
- checksum 不匹配：认为事务 torn/incomplete，清该 slot header，进入 repair。
- `state=COMMITTED` 且 checksum 正确：收集该 transaction，稍后按 `seq` 排序 replay。
- 其他非空状态：清该 slot header，进入 repair。

replay 阶段会把所有 committed transaction 按 `seq` 从小到大写回 home blocks，并清空各 slot header。恢复发生在 checkpoint worker 启动前；恢复和 repair 自身使用同步 journal 写回路径，避免恢复期间再产生后台 checkpoint 交错。

随后如果 dirty marker 存在，或 transaction 检查阶段判断需要修复，则执行 `repair_filesystem_state()`：

1. 读取整张 inode table。
2. 清理明显非法 inode：`UNKNOWN`、extent 数超界、extent 指向非法 data block 的 inode 会被置空。
3. 从 live inode 重新构造 inode bitmap。
4. 根据 live inode 的 direct/indirect extents 重新构造所有 group bitmap。
5. 重新计算 GDT 的 `free_blocks_count` 和 `next_free_hint`。
6. 扫描目录块，删除指向无效 inode 或 filetype 不匹配的目录项。
7. 通过 journal 写回 imap、group bitmap、GDT、inode table。
8. 清 dirty marker。

**语义边界**：

- 当前机制保证 crash 后文件系统元数据合法、自洽，不保证每个 syscall 具有完整原子持久化语义。
- 普通文件数据不 journal；崩溃后数据内容取决于崩溃前实际写盘情况。
- 某些不完整创建可能在 repair 中被清理为“目录项不存在”或“inode 不再 live”，这是当前简化设计的预期行为。
- dirty repair 会扫描 inode table 和 group bitmap，异常退出后的第一次 mount 会比 clean mount 慢；正常 clean shutdown 不走 repair。

### 5.10 FS base 保存/恢复策略

**背景问题**：shaoFS 调用 Caladan runtime、SPDK、DML 或 runtime libc 相关路径时，需要把 x86 `%fs` 切到 Caladan runtime TLS。用户程序自己的 `%fs` 则指向用户 libc/TLS 区域，里面包括 stack canary、`errno`、pthread TLS 等。如果 shaoFS 在 runtime FS base 下发生 uthread park/yield，而调度器把当前 `%fs` 直接保存到 `thread_t::fsbase`，就会把用户 TLS 状态污染成 runtime TLS，后续回到用户程序可能出现 stack smashing、SIGSEGV 或随机 TLS 错乱。

**当前状态字段**：

- `thread_t::fsbase`：保存用户线程自己的 FS base。对 Junction 用户线程来说，它应代表用户 libc/TLS，而不是临时 runtime TLS。
- `perthread runtime_fsbase`：每个 runtime kthread 的 Caladan/Junction runtime FS base，定义在 `lib/caladan/runtime/sched.c`。
- `thread_t::runtime_fsbase_depth`：当前 uthread 是否处在“主动切换到 runtime FS base 的区域”内；用 depth 支持嵌套 guard。

**普通用户代码执行时**：

```
runtime_fsbase_depth == 0
park/yield 时 thread_save_fsbase() 保存当前 %fs 到 thread_t::fsbase
resume 时 thread_fsbase_to_run() 返回 thread_t::fsbase
```

**进入 shaoFS runtime-FS 区域时**：

`junction/fs/shaofs/utili.h:RuntimeFSBaseGuard` 构造函数只在很短的切换窗口内 `preempt_disable()`：

```
preempt_disable()
prev_fs_base_ = _readfsbase_u64()
thread_self()->runtime_fsbase_depth++
_writefsbase_u64(perthread_read(runtime_fsbase))
preempt_enable()
```

注意：它不会在整个 shaoFS syscall 生命周期内保持 preempt disabled，因为 shaoFS 内部可能等待 SPDK I/O、获取 `rwmutex`、cache miss 后 park 或 yield。长时间禁用抢占会触发 Caladan 调度器对 `preempt_cnt` 的断言。

**shaoFS guard 内发生 park/yield 时**：

`thread_park_and_unlock_np()` 和 `thread_park_and_preempt_enable()` 当前调用 `thread_save_fsbase(curth)`。其逻辑是：

```
fsbase = _readfsbase_u64()
if (th->runtime_fsbase_depth && fsbase == perthread_read(runtime_fsbase))
    return;              // 不覆盖 thread_t::fsbase
th->fsbase = fsbase;
```

因此线程在 shaoFS guard 内 park 时，调度器不会把 runtime FS base 写进 `thread_t::fsbase`。用户 TLS 状态得以保留。

**恢复一个 park 在 shaoFS guard 内的线程时**：

`jmp_thread()` / `jmp_thread_direct()` 不再直接 `set_fsbase(th->fsbase)`，而是调用 `thread_fsbase_to_run(th)`：

- `runtime_fsbase_depth > 0`：恢复到当前 kthread 的 `runtime_fsbase`，继续执行 shaoFS/runtime 代码。
- `runtime_fsbase_depth == 0`：恢复到 `thread_t::fsbase`，回到用户 TLS。
- `has_fsbase == false` 的 runtime-only thread 会默认使用 `runtime_fsbase` 初始化 `th->fsbase`。

**退出 shaoFS runtime-FS 区域时**：

`RuntimeFSBaseGuard` 析构函数再次只在短窗口内关闭抢占，恢复构造时保存的 `prev_fs_base_`，然后递减 `runtime_fsbase_depth`。嵌套 guard 可以正确工作：内层退出后仍保持 runtime FS base，最外层退出才恢复用户 FS base。

**和 Junction 原有 `RuntimeLibcGuard` 的区别**：

`junction/bindings/runtime.h:RuntimeLibcGuard` 会在整个 guard 生命周期内关闭抢占并切换到 runtime FS base，这适合很短、不会 yield 的 runtime libc 调用。shaoFS 的 `RuntimeFSBaseGuard` 是 depth-aware 且允许 guard 内 yield 的版本，专门用于文件系统 I/O 路径。

**后续维护铁律**：

如果新增代码路径满足“切换到 runtime FS base，并且期间可能 park/yield”，必须使用或复用当前 `runtime_fsbase_depth` 策略。否则会重新引入用户 TLS 被 runtime TLS 污染的问题。

### 5.11 inode 分配

shaoFS 的 inode 上限来自 `INODENUM=32768` 和一块 inode bitmap；inode cache 容量 `DEFAULT_INODECACHE_CAPACITY=8192` 只是内存缓存容量，不应限制文件系统可创建 inode 数。

当前 `junction/fs/shaofs/inode.cc:alloc_inum()` 逻辑为：

```
static volatile unsigned int cursor;
start = atomic_fetch_add(&cursor, 1);
for i in [0, INODENUM):
    idx = (start + i) % INODENUM;
    if (!bitmap_atomic_test_and_set(imap, idx)) return idx;
return -1;
```

这表示分配器会按 `INODENUM` 全范围环形扫描 inode bitmap，可以越过 8192 cache capacity。`ic_alloc_inode()` 获取 inode cache entry 失败时会释放刚分配的 inum，避免 bitmap 泄漏。

### 5.12 Caladan/Junction syscall 包装与拦截机制

这个项目里必须区分三类 syscall：

1. **用户程序 syscall**：例如 Filebench 调用 `open/read/pread/write`。这些 syscall 应被 Junction 拦截并分发到 `usys_*`，shaoFS 路径再转到 `my_*`。
2. **Junction runtime 真实 Linux syscall**：Junction 自己加载程序、管理 host file、映射内存时需要少量真实 Linux syscall，必须走 `junction/kernel/ksys.S` / `junction/kernel/ksys.h`。
3. **Caladan/runtime/SPDK 真实 Linux syscall**：Caladan 或 SPDK 路径需要 `mmap/mlock/ioctl/writev/exit_group` 等，必须走 `lib/caladan/base/syscall.S` / `lib/caladan/inc/base/syscall.h` 中的 wrapper。

**Caladan wrapper 的意义**：

- `base_syscall_start` 到 `base_syscall_end` 之间放置少量 raw `syscall` 指令。
- Junction seccomp filter 使用 syscall instruction pointer 判断来源；只有 syscall 号在 allowlist 且 IP 落在该范围内，才允许真正进入 Linux kernel。
- wrapper 还负责修正 x86_64 syscall ABI 中第 4 个参数从 `%rcx` 到 `%r10` 的传递差异。
- wrapper 返回 raw syscall 结果，失败通常是负数 `-errno`，不会自动设置 libc `errno`。

**Junction ksys 的意义**：

- `junction/kernel/ksys.S` 里有 `ksys_start` 到 `ksys_end` 范围。
- `ALLOW_JUNCTION_SYSCALL(name)` 同样按 syscall 号和 IP 范围放行少量 Linux syscall。
- Junction 内部访问 host kernel 时应使用 `ksys_*` 或 `ksyscall()`，不要直接调用 glibc `syscall()`。

**用户 syscall 拦截路径**：

```
用户程序执行 syscall instruction
    ├─ seccomp BPF 检查
    ├─ 不在 Caladan/Junction 真实 syscall allowlist 中
    ├─ SECCOMP_RET_TRAP → Linux 发送 SIGSYS
    ├─ syscall_trap_handler()
    ├─ 保存用户寄存器和 trapframe，切到 Junction syscall stack
    └─ entry.S 根据 syscall number 调用 sys_tbl[sysnr] → usys_*
```

如果启用 zpoline，Junction 还可能把用户程序中的 syscall 指令热补丁到更快入口，减少反复 SIGSYS trap 的开销；语义上仍是进入 Junction 的 syscall table。

**编程影响**：

- 用户程序可以写普通 Linux API，但只能依赖 Junction 已实现或已适配的 syscall 语义。
- shaoFS/Junction/Caladan 内部不能任意调用 Linux syscall。需要真实 syscall 时，必须走受控 wrapper，并确认 `seccomp.cc` allowlist 已放行。
- glibc/libc 函数可能隐式 syscall，例如 `personality()`、SysV semaphore、`timerfd`、`fork/wait`、`brk`、部分 pthread/timer 路径。Filebench/FIO 适配的大量工作就是绕开或降级这些 Junction 不支持的机制。
- 如果 syscall 来自 runtime/libc 内部且不在允许路径中，常见表现是 `Trapped a Junction libc internal syscall`、`Unexpected syscall from Caladan`、`blocked syscall` 或直接退出。

---

## 第六章：环境配置与运行指令

### 6.1 构建

```bash
cd /home/syh/MyProj1/junction
scripts/build.sh          # Release → build/
scripts/build.sh -d       # Debug → build-debug/
```

启用/关闭 shaoFS I/O completion preemption：

```bash
cd /home/syh/MyProj1/junction
cmake -S . -B build -DSHAOFS_IO_PREEMPT=ON
cmake --build build --target junction_run -- -j$(nproc)

# 如需回到普通模式：
cmake -S . -B build -DSHAOFS_IO_PREEMPT=OFF
cmake --build build --target junction_run -- -j$(nproc)
```

注意：CMake option 默认值是 OFF；当前会话结束时 `build/CMakeCache.txt` 中为 `SHAOFS_IO_PREEMPT:BOOL=ON`。

启用/关闭 shaoFS crash consistency：

```bash
cd /home/syh/MyProj1/junction
cmake -S . -B build -DSHAOFS_CRASH_CONSISTENCY=ON
cmake --build build --target junction_run -- -j$(nproc)

# 如需回到无 journal 的旧行为：
cmake -S . -B build -DSHAOFS_CRASH_CONSISTENCY=OFF
cmake --build build --target junction_run -- -j$(nproc)
```

注意：CMake option 默认值是 ON；当前会话结束时 `build/CMakeCache.txt` 中为 `SHAOFS_CRASH_CONSISTENCY:BOOL=ON`。

### 6.2 格式化磁盘

```bash
# 必须先 kill iokernel（它持有 NVMe 设备）
sudo pkill -9 iokerneld
sudo bash /home/syh/mkfs/mkfs.sh
```

### 6.3 启动 IOKernel + 运行测试

```bash
# 启动 IOKernel（独立进程，需 root）
sudo lib/caladan/iokerneld ias &
sleep 5      # 给一段时间让 IOKernel 能够运行起来

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

### 6.5 编译并运行 sync syscall smoke test

当前 Junction syscall table 已包含 `sync`。最小测试程序为：

```bash
cd /home/syh/MyProj1/junction
gcc junction/fs/mytest/test_sync_syscall.c -o build/junction/mytest/test_sync_syscall -lpthread
```

运行方式：

```bash
# shell 1
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias

# shell 2
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 20s ./junction_run caladan_test.config -- \
  mytest/test_sync_syscall

# 结束后
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld
```

本轮已观察到输出：

```text
sync ret=0 errno=0 (Success)
```

### 6.5.1 编译并运行 Junction 单容器多进程验证

本测试用于验证 README 提到的“一个 `junction_run` 容器中运行多个应用/进程”能力。测试程序不修改 Junction/ShaoFS 实现，只通过用户程序显式触发 `vfork()` + `execv()`，并用 ShaoFS 文件写入和 `waitpid()` 证明多个 Junction PID 能在同一个容器中运行。

编译：

```bash
cd /home/syh/MyProj1/junction
mkdir -p build/junction/mytest
gcc -O2 -Wall -Wextra junction/fs/mytest/junction_multiproc_vfork.c \
  -o build/junction/mytest/junction_multiproc_vfork -lpthread
```

重新格式化并启动 IOKernel：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld 2>/dev/null || true
cd /home/syh/mkfs && printf 'syh2syh\n' | sudo -S bash ./mkfs.sh

cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias
```

在另一个 shell 运行 `vfork()` + `execv()` 路线：

```bash
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 30s ./junction_run caladan_test.config -- \
  mytest/junction_multiproc_vfork 4 20 50000 FSHAO:/junction_multiproc_vfork
```

成功时应看到 controller PID/TID 为 `1/1`，4 个 worker 使用不同 Junction PID/TID，并最终输出：

```text
MULTIPROC_VFORK_OK workers=4
```

也可以验证 README 中 fish 后台任务路线：

```bash
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 30s ./junction_run caladan_test.config -- \
  /usr/bin/fish -c 'for i in (seq 0 3); mytest/junction_multiproc_vfork --worker $i 20 50000 FSHAO:/junction_multiproc_fish &; end; wait'
```

需要确认 host 侧只有一个 `junction_run` 进程时，可以把 worker 运行时间调长，然后在第三个 shell 观察：

```bash
pgrep -a junction_run
ps -T -p <junction_run_pid>
```

2026-06-07 实测中，host 侧只看到一个 `junction_run` 进程及 DPDK 辅助线程；容器内 worker PID/TID 分别为 `2/2`、`3/3`、`4/4`、`5/5`。这说明多个 Junction process 没有被实现成多个 Linux child process。测试结束后清理：

```bash
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld
```

### 6.6 编译并运行 crash consistency 测试

编译：

```bash
cd /home/syh/MyProj1/junction
mkdir -p build/junction/mytest
gcc -O2 junction/fs/mytest/journal_layout_probe.c -o build/junction/mytest/journal_layout_probe -lpthread
gcc -O2 junction/fs/mytest/journal_recovery_prepare.c -o build/junction/mytest/journal_recovery_prepare -lpthread
gcc -O2 junction/fs/mytest/journal_recovery_check.c -o build/junction/mytest/journal_recovery_check -lpthread
```

干净格式化并启动 IOKernel：

```bash
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld 2>/dev/null || true
cd /home/syh/mkfs && printf 'syh2syh\n' | sudo -S bash ./mkfs.sh

cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias
```

在另一个 shell 模拟崩溃并检查恢复：

```bash
cd /home/syh/MyProj1/junction/build/junction

# 先制造一个非 clean shutdown。退出码 137 是 SIGKILL 预期结果。
printf 'syh2syh\n' | sudo -S timeout -s KILL 2s ./junction_run caladan_test.config -- \
  mytest/journal_recovery_prepare --crash

# 下一次 mount 应输出 "[journal] previous mount was dirty, repairing metadata state"，
# 并且检查程序应报告 journal recovery check failures=0。
printf 'syh2syh\n' | sudo -S timeout 60s ./junction_run caladan_test.config -- \
  mytest/journal_recovery_check

printf 'syh2syh\n' | sudo -S pkill -9 iokerneld
```

### 6.7 sudo 密码

```
syh2syh
```

### 6.8 在 Junction 中运行 FIO

FIO 源码位于 `junction/fs/mytest/benchmark/fio`，这是一个独立 git 仓库。为了让它能在 Junction 中启动，当前采用“FIO 内部降级 + 构建配置”的方式，不修改 Junction 源码。

相关文件：

| Path | Purpose |
|------|---------|
| `junction/fs/mytest/benchmark/fio` | FIO 源码和构建产物目录 |
| `junction/fs/mytest/benchmark/patch/fio_changes.patch` | 当前 FIO 适配补丁，158 行，修改 `filesetup.c`、`helper_thread.c`、`memory.c` |
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

1. `filesetup.c`：识别 `FSHAO/` 和 `FSHAO:/` 路径，避免 FIO 把 `FSHAO` 或 `FSHAO:` 当作普通目录去 `mkdir`。注意：FIO 自身的 `options.c:get_next_str()` 会把未转义的 `:` 当作 filename/directory 列表分隔符；当前 shaoFS core 的 `MYPREFIX` 是 `"FSHAO"`，实际 FIO 命令建议优先使用 `FSHAO/` 规避这个问题。
2. `filesetup.c`：当 shaoFS 的 `ftruncate` 返回 `EINVAL` 或 `ENOSYS` 时，不让 FIO prepare 阶段直接失败，而是继续通过写入铺文件。
3. `helper_thread.c`：`timerfd_create()` / `timerfd_settime()` 在 Junction 中不可用或失败时不再 `assert` 崩溃，而是回退到原有 select timeout 路径。
4. `memory.c`：当 job 使用 `direct=1` 且 FIO 走 malloc 内存模式时，使用 `posix_memalign()` 分配 2MB 对齐、按 2MB 向上取整的 buffer，保证 shaoFS 当前 O_DIRECT user-buffer DMA 能对覆盖区间执行 2MB 注册。

运行 FIO 前建议重新格式化 shaoFS 测试盘：

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

### 6.9 cgroup v2 runner 与 ext4 FIO 主脚本

通用 cgroup runner：

```bash
junction/fs/mytest/scripts/cg_run.sh
```

当前行为：

- 需要 root，要求 `/sys/fs/cgroup` 是 cgroup v2。
- 默认创建 `/sys/fs/cgroup/shaofs_bench/<name>_<pid>`。
- 配置 `cpuset.cpus`、`cpuset.mems`、`memory.max` 和 `memory.swap.max=0`。
- 可选用 `timeout --kill-after=5s` 包裹目标命令。
- 命令结束后收集 `cpu.stat usage_usec`、`memory.events high/max/oom` 和 `memory.peak`，以 `key=value` 写入 `--stats-file`。
- cleanup 会 kill 残留在该 cgroup 中的任务，并尝试删除本次 cgroup 和空的 base cgroup。

使用示例：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S junction/fs/mytest/scripts/cg_run.sh \
  --cpus 2 \
  --mems 0 \
  --memory-mb 1024 \
  --timeout 60s \
  --name ext4_fio \
  --stats-file /tmp/ext4_fio.cgroup \
  -- /path/to/command arg1 arg2
```

ext4 FIO 主脚本：

```bash
junction/fs/mytest/scripts/run_ext4_fio.sh
```

当前行为：

1. 默认调用 `junction/fs/mytest/benchmark/patch/toggle_fio.sh revert`，把 FIO 恢复为普通 Linux/ext4 版本并重新构建。
2. 默认执行 `/home/syh/mkfs/reset_ext4.sh`，把测试 SSD 重置为 ext4 并挂载到 `/mnt/nvme/ext4_bench`。
3. 执行 `sync` 并写 `/proc/sys/vm/drop_caches`。
4. 通过 `cg_run.sh` 运行 FIO jobfile，默认 jobfile 为 `junction/fs/mytest/scripts/fio_test/ext4_directio.fio`。
5. 将 FIO stdout/stderr 写入 `junction/fs/mytest/scripts/results/ext4_fio_<timestamp>.log`，将 cgroup stats 写入同名 `.cgroup` 文件。

运行示例：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S junction/fs/mytest/scripts/run_ext4_fio.sh \
  --cpus 2 \
  --mems 0 \
  --memory-mb 1024 \
  --timeout 300 \
  --fio-job /home/syh/MyProj1/junction/junction/fs/mytest/scripts/fio_test/psync_128job_randread.fio
```

注意：对 ext4 运行前应确保没有 IOKernel 持有 NVMe 设备。`run_ext4_fio.sh` 会重建 FIO 为普通版；回到 shaoFS/Junction FIO 测试前，必须重新执行 `toggle_fio.sh apply`，让 FIO 回到 `--disable-shm` + shaoFS O_DIRECT buffer 适配状态。

### 6.10 运行 Caladan raw storage fixed-QD benchmark

这个 benchmark 不经过 shaoFS/Junction/FIO，只用于确认 Caladan runtime + SPDK storage API 自身能达到的 4KB IOPS 上限。Caladan 顶层 `Makefile` 会把 `tests/*.c` 自动纳入 test targets，因此新增 `test_storage_async_iops.c` 后可直接按目标名构建：

```bash
cd /home/syh/MyProj1/junction/lib/caladan
make -j 64 tests/test_storage_async_iops
```

推荐通过脚本运行，脚本会启动并清理 iokernel：

```bash
cd /home/syh/MyProj1/junction/lib/caladan/tests
SUDO_PASSWORD=syh2syh CONFIG=storage_2c_q0.config POLLERS=2 QD=128 RUN_SECS=20 \
  OP=read PATTERN=rand RANGE_MB=0 BASE_LBA=1048576 \
  ./run_storage_async_iops.sh
```

关键参数：

- `CONFIG`：选择 `storage_1c_q0.config`、`storage_2c_q0.config`、`storage_4c_q0.config` 或 `storage_8c_q0.config`，控制 runtime kthread/core 数。
- `POLLERS`：benchmark 内 poller uthread 数，通常应与 runtime kthread 数一致，用于让每个 kthread/core 有独立 qpair outstanding。
- `QD`：每个 poller 维持的 outstanding I/O 数。总 outstanding 约为 `POLLERS * QD`。
- `RANGE_MB=0`：使用 `BASE_LBA` 之后的全 namespace。对 PM9A3 randread 很重要；64GiB 左右小随机范围会明显低估 IOPS。
- `BASE_LBA=1048576`：跳过盘前部约 512MiB，避免覆盖文件系统元数据区域；raw benchmark 会直接读写 LBA，写测试尤其要先确认可覆盖范围。
- `POLL_BATCH=0`：传给 `spdk_nvme_qpair_process_completions()` 的 `max_completions=0`，即 SPDK 默认尽可能处理可用 completion。

不要把 `lib/caladan/tests/storage.config` 当成唯一标准配置。它是用户手动创建的旧配置，当前包含 `runtime_kthreads 10`、`runtime_spinning_kthreads 10`、`quota_enabled false`，可用于手工实验，但正式 core-sweep 建议使用 `storage_{1c,2c,4c,8c}_q0.config` 并记录参数。

### 6.11 在 Junction 中运行 Filebench

Filebench 源码位于 `junction/fs/mytest/benchmark/filebench`。当前策略是只修改 Filebench 内部并用 patch 管理，不修改 Junction 源码。

相关文件：

| Path | Purpose |
|------|---------|
| `junction/fs/mytest/benchmark/filebench` | Filebench 源码和构建产物目录 |
| `junction/fs/mytest/benchmark/patch/filebench_changes.patch` | Junction 适配补丁，覆盖 `aslr.c`、`fb_cvar.c`、`fb_localfs.c`、`fileset.c`、`flag.h`、`flowop_library.c`、`ipc.c`、`misc.c`、`procflow.c` |
| `junction/fs/mytest/benchmark/patch/toggle_filebench.sh` | 补丁 apply/revert + 自动 `configure`/`make` 的管理脚本 |
| `junction/fs/mytest/benchmark/filebench_wml/fileserver.f` | 当前用于 shaoFS 的 Filebench fileserver workload：`FSHAO:`、10000 files、50 threads、60s runtime |
| `junction/fs/mytest/benchmark/filebench_wml/webserver.f` | 当前用于 shaoFS 的 Filebench webserver workload：`FSHAO:`、10000 files、100 threads、60s runtime |
| `junction/fs/mytest/benchmark/filebench_wml/varmail.f` | 当前用于 shaoFS 的 Filebench varmail workload：`FSHAO:`、5000 files、16 threads、60s runtime |
| `junction/fs/mytest/benchmark/filebench_wml/webproxy.f` | 当前用于 shaoFS 的 Filebench webproxy workload：`FSHAO:`、10000 files、100 threads、60s runtime |

将 Filebench 切换并构建为 Junction 适配版：

```bash
cd /home/syh/MyProj1/junction
junction/fs/mytest/benchmark/patch/toggle_filebench.sh apply
```

`apply` 会执行补丁应用，并用以下 configure cache 变量禁用 SysV semaphore 相关探测结果：

```bash
ac_cv_func_ftok=no \
ac_cv_func_semget=no \
ac_cv_func_semop=no \
ac_cv_func_semtimedop=no \
./configure
make -j "$(nproc)"
```

恢复 Filebench 普通源码状态并按默认配置重建：

```bash
cd /home/syh/MyProj1/junction
junction/fs/mytest/benchmark/patch/toggle_filebench.sh revert
```

当前 Filebench 适配补丁的核心变化：

1. `aslr.c`：`linux_disable_aslr()` 直接返回。当前测试环境已全局关闭 ASLR，且 Junction 不实现 `personality()`。
2. `ipc.c`：在 configure 禁用 `ftok/semget/semop/semtimedop` 后，Filebench 不再创建 SysV semaphore；`shm_semkey` 置 0。Filebench 仍使用自己的共享内存结构，但在 Junction 内主要以单进程多线程模型运行。
3. `procflow.c` / `procflow.h`：不再 `fork()` / `exec()` / `waitpid()` worker procflow；每个 procflow monitor 改为 `pthread_create()` 在当前进程内运行，然后由它创建配置中的 Filebench worker threads。用户曾提示 Junction 可能支持 `vfork()`，但当前实际补丁选择 pthread 降级，避免引入 exec/shm 地址传递和 wait 语义问题。
4. `fb_localfs.c`：`fb_lfs_recur_rm()` 遇到 `FSHAO:/` 或 `FSHAO/` 路径直接返回，避免通过 `system("rm -rf ...")` 清理 shaoFS 路径。
5. `fb_cvar.c`：cvar 目录不可用时降级为 verbose log；如果默认目录没有加载到 cvar 插件，会再根据 Filebench 可执行文件路径尝试 `cvars/.libs` build 目录，保证 `webserver.f` 中的 `cvar-gamma` 可用。
6. `fileset.c` / `ipc.c`：修复若干 `strncpy` 未保证 NUL 结尾的问题，避免 Junction/Filebench 长路径下字符串截断或未终止。
7. `fileset.c`：`fileset_mkdir()` 的 `dirs[65536]` 栈数组改为动态数组，避免 Junction uthread 512KB 栈被大栈对象压垮。
8. `misc.c`：`filebench_log()` 的 128KB 栈上缓冲改为全局缓冲并加 pthread mutex，避免日志路径消耗过大 uthread 栈；同时改用 `vsnprintf()`。
9. `flowop_library.c`：修正 debug log 打印 `threadflow->tf_fd[fd]` 结构体的问题，改为打印 `fd_num`。
10. `flag.h`：`wait_flag()` 的纯 busy-wait 改为循环中 `sched_yield()`，降低 Filebench 进程内 pthread 降级模式下的 CPU 空转和终止等待问题。

推荐优先使用四项统一脚本，而不是手工逐个运行：

```bash
cd /home/syh/MyProj1/junction

# 默认 mode=2：先测 shaoFS，再测 ext4
printf 'syh2syh\n' | sudo -S junction/fs/mytest/scripts/run_filebench_compare.sh

# 只测 shaoFS
printf 'syh2syh\n' | sudo -S junction/fs/mytest/scripts/run_filebench_compare.sh 0

# 只测 ext4
printf 'syh2syh\n' | sudo -S junction/fs/mytest/scripts/run_filebench_compare.sh 1

# 已经构建过 Junction 时可跳过 build；TIMEOUT_SEC 是每个 workload 的 timeout。
printf 'syh2syh\n' | sudo -S env BUILD_JUNCTION=0 TIMEOUT_SEC=240s \
  junction/fs/mytest/scripts/run_filebench_compare.sh 2
```

`run_filebench_compare.sh` 当前行为：

- shaoFS 阶段会执行 `toggle_filebench.sh apply`，每个 workload 前重新运行 `/home/syh/mkfs/mkfs.sh`，启动 IOKernel，使用 `timeout` 包裹 `junction_run`，结束后清理 IOKernel。
- ext4 阶段会调用 `run_ext4_filebench.sh`。第一个 workload 默认 revert Filebench patch 并重建原生 Filebench，后续 workload 使用 `--no-rebuild`。
- ext4 cgroup memory 默认值：fileserver 800MiB、webserver 1300MiB、varmail 500MiB、webproxy 1300MiB。可通过 `EXT4_FILESERVER_MEM_MB`、`EXT4_WEBSERVER_MEM_MB`、`EXT4_VARMAIL_MEM_MB`、`EXT4_WEBPROXY_MEM_MB` 覆盖。
- 结果默认保存到 `junction/fs/mytest/scripts/results/filebench_compare_<timestamp>/`，包含每个 workload 的 stdout/stderr、IOKernel log、mkfs log、driver log 和 `.status` 文件。

推荐运行方式：

```bash
# 先确保没有旧 IOKernel 持有设备，然后重新格式化
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld 2>/dev/null || true
cd /home/syh/mkfs && printf 'syh2syh\n' | sudo -S bash ./mkfs.sh

# shell 1: 启动 IOKernel
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias

# shell 2: 运行 Filebench，务必使用 timeout 防死锁
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 90s ./junction_run caladan_test.config -- \
  /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench/filebench \
  -f /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/patch/example.f

# 结束后清理
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld
```


运行当前 `fileserver.f` workload：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld 2>/dev/null || true
cd /home/syh/mkfs && printf 'syh2syh\n' | sudo -S bash ./mkfs.sh

cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias

cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 120s ./junction_run caladan_test.config -- \
  /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench/filebench \
  -f /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench_wml/fileserver.f

printf 'syh2syh\n' | sudo -S pkill -9 iokerneld
```

运行当前 `webserver.f` workload：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld 2>/dev/null || true
cd /home/syh/mkfs && printf 'syh2syh\n' | sudo -S bash ./mkfs.sh

cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias

cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 90s ./junction_run caladan_test.config -- \
  /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench/filebench \
  -f /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench_wml/webserver.f

printf 'syh2syh\n' | sudo -S pkill -9 iokerneld
```

运行当前 `varmail.f` workload：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld 2>/dev/null || true
cd /home/syh/mkfs && printf 'syh2syh\n' | sudo -S bash ./mkfs.sh

cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias
# IOKernel 需要几秒完成初始化；脚本中应等待几秒后再启动 junction_run。

cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 120s ./junction_run caladan_test.config -- \
  /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench/filebench \
  -f /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench_wml/varmail.f

printf 'syh2syh\n' | sudo -S pkill -9 iokerneld
```

运行 ext4 Filebench 对比：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S junction/fs/mytest/scripts/run_ext4_filebench.sh \
  --cpus 2 \
  --mems 0 \
  --memory-mb 300 \
  --timeout 180s \
  --wml /home/syh/MyProj1/junction/junction/fs/mytest/scripts/filebench_test/ext4_varmail.f
```

`run_ext4_filebench.sh` 会先 revert Filebench Junction patch 并重建原生 Filebench，再调用 `/home/syh/mkfs/reset_ext4.sh` 重置并挂载 ext4，最后通过 `cg_run.sh` 在 cgroup v2 CPU/memory 限制下运行指定 WML。跑完 ext4 后，如果要回到 shaoFS/Junction Filebench 测试，必须重新执行 `junction/fs/mytest/benchmark/patch/toggle_filebench.sh apply`。

### 6.12 在 Junction 中运行 FxMark

FxMark 源码位于：

```bash
/home/syh/MyProj1/junction/junction/fs/mytest/benchmark/fxmark
```

当前策略是只修改 FxMark 内部并用 patch 管理，不修改 Junction 或 shaoFS 源码。相关文件：

| Path | Purpose |
|------|---------|
| `junction/fs/mytest/benchmark/fxmark` | FxMark 源码和构建产物目录 |
| `junction/fs/mytest/benchmark/patch/fxmark_changes.patch` | Junction 适配补丁，覆盖 `Makefile`、`src/bench.c`、`src/DRBL.c`、`src/util.c` |
| `junction/fs/mytest/benchmark/patch/toggle_fxmark.sh` | 补丁 apply/revert + 自动 `make -j "$(nproc)"` 的管理脚本 |

将 FxMark 切换并构建为 Junction 适配版：

```bash
cd /home/syh/MyProj1/junction
junction/fs/mytest/benchmark/patch/toggle_fxmark.sh apply
```

恢复 FxMark 普通源码状态并重建：

```bash
cd /home/syh/MyProj1/junction
junction/fs/mytest/benchmark/patch/toggle_fxmark.sh revert
```

当前 FxMark 适配补丁的核心变化：

1. `src/bench.c`：把 worker 创建从 `fork()` 改为 `pthread_create()`。Junction 当前没有通用 `fork()`，原 FxMark 在 `--ncore > 1` 时会因 worker 创建失败而卡在 ready/start 屏障；`vfork()` 也不适合这里，因为 Junction 的 `vfork()` 会暂停 parent 到 child exit/exec，而 FxMark worker 必须并发运行。
2. `src/bench.c`：启动/结束屏障从纯 `pause` busy-wait 改为 `sched_yield()`，避免 `runtime_quantum_us=0` 且只有一个 runtime kthread 时，某个等待 worker 自旋霸占唯一运行权。
3. `src/bench.c` / `Makefile`：新增 pthread worker wrapper，并用 `-pthread` 构建。
4. `src/util.c`：把 `system("mkdir -p ...")` 改为进程内递归 `mkdir()`，并把 `FSHAO` / `FSHAO:` 当作 shaoFS 根别名处理，避免通过 shell 清理或创建 shaoFS 路径。
5. `src/DRBL.c`：DRBL 主循环每 4096 次迭代检查一次 wall-clock deadline，使 worker 自己按 `bench->duration` 结束；这绕开了 Junction/Caladan tight loop 中 `SIGALRM` 递送不及时的问题。核心 I/O 操作仍是原 DRBL 的 `pread(fd, page, PAGE_SIZE, 0)`。

推荐运行方式：

```bash
# shell 1: 启动 IOKernel
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias

# shell 2: 运行 FxMark，务必使用 timeout
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 45s ./junction_run caladan_test.config -- \
  /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/fxmark/bin/fxmark \
  --type DRBL --ncore 8 --nbg 0 --duration 5 --directio 0 --root FSHAO/demo

# 结束后清理
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld
```

---

## 第七章：踩坑记录与高危警告（重要）

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

### 7.5 GOTCHA 5：Extent 数量上限与 fixed-depth tree

**历史触发条件**：单个文件的物理块分配高度碎片化时，extent 数超过旧布局 `DIRECT_EXTENT_NUM + EXTENTS_PER_BLOCK = 176` 上限。历史上 Filebench `fileserver.f` 的 append-heavy 模式曾触发该问题。

**当前状态**：2026-05-13 已在普通文件 EOF append 路径加入批量预分配，2026-05-20 的 `ea98931` 又加入 simple extent tree；2026-05-21 当前代码仍保留该实现。现在 `valid_extent_count <= 176` 的文件继续使用 legacy flat indirect block；超过 176 后会把 `indirect_extent_block` 解释为 tree root，并通过 leaf metadata blocks 保存大量 indirect extents。当前代码上限是 `DIRECT_EXTENT_NUM + EXTENT_TREE_ROOT_REFS * EXTENT_TREE_LEAF_EXTENTS = 28567` extents。

**仍需注意**：simple extent tree 是固定深度轻量结构，不是通用 B+tree。超过 28567 extents 仍会失败并打印类似 `[extent] extent tree root overflow ...` / `[extent] Extent tree overflow ...`。乱序或极端碎片插入会走 `collect_all_extents()` + sort/compact + rewrite tree 的 slow path，适合测试正确性，不适合作为随机写极致性能路径。后续若要继续扩大容量，应优先设计多级 tree 或进一步增强连续块分配策略。

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

当前 `junction/fs/shaofs/fs.h` 定义 `MYPREFIX` 为 `"FSHAO"`，不是历史常用的 `"FSHAO:"`。这不是因为 shaoFS 内部必须如此，而是为了绕开 FIO 对冒号的特殊处理。

已从 FIO 当前源码确认：

- `junction/fs/mytest/benchmark/fio/options.c:get_next_str()` 的注释和实现明确说明：filename/directory 列表用 `:` 分隔。
- 未转义的 `:` 会被当作分隔符；只有写成反斜杠转义形式时，该冒号才属于文件名。
- 因此 `--directory=FSHAO:/` 或 `--filename=FSHAO:/file` 在 FIO 参数解析层仍有风险，可能被拆成 `FSHAO` 和 `/...` 两段。
- 当前推荐 FIO 命令使用 `FSHAO/`，例如 `--directory=FSHAO/`，这样 `core.cc` 剥离 `FSHAO` 后内部路径仍以 `/` 开头。

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

2026-05-09 的测试中 Junction 输出仍包含 `DIRECTPATH DISABLED` 警告，因此 `shaofs_preempt_latency` / `shaofs_preempt_iops` 的结果应解释为“completion-driven preemption 在 CPU-bound 干扰下显著降低延迟、提升可观测 IOPS”，不要直接写成 shaoFS 已达到最终 NVMe 带宽上限。

另外，shaoFS direct I/O 当前已有严格约束下的 user-buffer DMA 路径，但为了保证 cached/direct 一致性，direct read/write 仍会涉及 `bc_flush_block()` / `bc_invalidate_block()`；不满足 4KB 对齐请求合约、或底层覆盖 2MB 区间无法 `mlock/spdk_mem_register/vtophys` 的请求会失败。正式解释性能时需要明确 workload 是否真的走到了 user-buffer DMA fast path。

### 7.13 GOTCHA 12：Crash consistency 当前是 metadata-only，不是完整事务文件系统

当前 `CRASH_CONSISTENCY` 机制只 journal 元数据块，并通过 dirty mount repair 保证元数据自洽。不要把它描述成完整 POSIX crash consistency 或 ext4 data=journal 等价实现。

需要特别注意：

- 普通文件数据块不 journal；崩溃后数据内容只取决于已完成的底层写。
- 多块元数据更新没有合并成一个高层语义事务；每个 metadata block 通常单独 redo，崩溃后靠 repair 兜底。
- `repair_filesystem_state()` 可能清除指向无效 inode 的目录项，这表示不完整操作被回滚到“合法但可能丢失该新文件/目录项”的状态。
- journal 区复用由单个 `journal_lock` 串行化；这是为了正确性和实现简单，元数据密集负载下可能成为瓶颈。
- dirty repair 会扫描整张 inode table 和所有 group bitmap；非 clean shutdown 后第一次 mount 会明显慢于 clean mount。
- 修改目录块写入路径时，必须继续维护 `journal_register_metadata_block()` / `journal_build_metadata_map()`，否则目录块可能被当作普通数据块绕过 journal。

### 7.14 GOTCHA 13：FS base guard 内允许 yield，但不能污染用户 TLS

shaoFS 的 `RuntimeFSBaseGuard` 与 Junction 原有 `RuntimeLibcGuard` 不同。`RuntimeLibcGuard` 在整个 guard 生命周期内关闭抢占，只适合短小且不会 yield 的 libc/runtime 调用；shaoFS guard 只在切换 `%fs` 和更新 `runtime_fsbase_depth` 时短暂关闭抢占，随后允许 I/O、锁等待和 uthread park。

修改相关代码时必须同时理解：

- `junction/fs/shaofs/utili.h:RuntimeFSBaseGuard`
- `lib/caladan/inc/runtime/thread.h:thread::runtime_fsbase_depth`
- `lib/caladan/runtime/sched.c:thread_save_fsbase()`
- `lib/caladan/runtime/sched.c:thread_fsbase_to_run()`
- `lib/caladan/runtime/sched.c:thread_park_and_unlock_np()` / `thread_park_and_preempt_enable()`
- `lib/caladan/runtime/sched.c:jmp_thread()` / `jmp_thread_direct()`

不要把 shaoFS guard 改回“整个作用域 `preempt_disable()`”；这会在 shaoFS 内部 park/yield 时触发 Caladan 调度器 preempt count 断言。也不要在 `runtime_fsbase_depth > 0` 且当前 `%fs == runtime_fsbase` 时保存到 `thread_t::fsbase`；这会污染用户程序 TLS，典型表现是 stack smashing 或 Filebench 并发读崩溃。

### 7.15 GOTCHA 14：inode cache 容量不是 inode 总量

`DEFAULT_INODECACHE_CAPACITY=8192` 只是 inode cache 的可驻留 entry 数。shaoFS 盘上 inode 总数是 `INODENUM=32768`，由 inode bitmap 决定。任何 inode 分配逻辑都不能用 cache capacity 作为扫描上限。

当前 `alloc_inum()` 已按 `INODENUM` 全范围环形扫描；如果后续重构 inode cache 或 allocator，必须保留这个语义。否则 Filebench `entries=10000` 这类测试会在约 8192 inode 附近再次失败。

### 7.16 GOTCHA 15：Filebench 当前是“单进程多线程降级版”

当前 Filebench patch 为了适配 Junction，改变了 Filebench 的 procflow 执行模型：不再 fork worker process，而是在同一进程内用 pthread 运行 procflow monitor 和 worker threads。需要同时注意：Junction 会拦截用户程序的 `clone3()`，因此这些 pthread 在 Junction/shaoFS 内实际由 Junction uthread 承载，并不是普通 Linux kernel pthread。这足以跑通当前 shaoFS 学术负载，但不是 Filebench 上游多进程语义的完整等价实现。

使用 Filebench 结果时需要明确：

- `process name=...,instances=N` 当前会变成同一 Junction 进程内的 N 个 procflow monitor pthread，而不是 N 个 OS process。
- 当前补丁没有修改核心 flowop I/O 操作，例如 open/readwholefile/closefile 的实际文件 I/O 逻辑仍走 Filebench 原有 flowop。
- 涉及多进程隔离、进程级资源统计、真实 fork/exec 行为的 Filebench workload 不应直接拿当前 patch 的结果做结论。
- `toggle_filebench.sh apply` 后 Filebench 子仓库处于 modified/dirty 状态是预期的；源码修改应通过 `filebench_changes.patch` 管理，不要直接手改子仓库后忘记更新 patch。

### 7.16.1 GOTCHA 15A：已验证 `vfork()+exec` 多进程路线，但不要推断完整 `fork()` 语义

2026-06-07 已通过 `junction_multiproc_vfork.c` 和 fish 后台任务验证：单个 `junction_run` 容器可以承载多个 Junction process，容器内可观察到多个 Junction PID/TID，host 侧仍只有一个 Linux `junction_run` 进程。

这个结论不推翻 Filebench/FxMark 当前的 pthread 降级策略。原因是本轮验证的核心路径是：

- `vfork()` child 立即 `execv()`，parent 在 child `exec/exit` 前被暂停。
- fish 后台任务通常走 `posix_spawn()` / fork-exec 类路线，实测可以启动多个独立任务。
- 这不能证明“parent 和 child 在 `fork()` 后、`exec()` 前共享一份用户地址空间快照并并发执行”的完整 Linux fork 语义。

因此，像 FxMark worker 或 Filebench procflow 这类需要 worker 与 parent 并发运行、共享或传递复杂运行时状态的 benchmark，仍不能只因为 `vfork()+exec` 成功就恢复为原始 process/fork 模型。若未来要恢复原始模型，必须针对目标 workload 单独验证 `fork`、`exec`、`wait*`、信号、文件描述符继承和共享内存/IPC 行为。

### 7.17 GOTCHA 16：`O_DIRECT` user-buffer DMA 合约非常严格

当前 shaoFS O_DIRECT 不再为不合规请求自动回退到 bounce buffer。测试程序或 benchmark 如果要验证“NVMe SSD ↔ user buffer”直通路径，必须保证：

- 用户传给 read/write 的 buffer 地址 4KB 对齐。
- I/O 长度是 4KB 的倍数。
- 文件 offset 4KB 对齐。
- 读写范围落在整 4KB 块上。
- `storage_prepare_user_dma()` 能对覆盖用户子区间的 2MB 注册范围成功完成 `mlock`、`spdk_mem_register`，并对实际请求子区间完成 `spdk_vtophys` 验证。

因此推荐测试程序分配 2MB 对齐、至少 2MB 大小的 arena，再传入其中 4KB 对齐的子区间。孤立的小 4KB malloc buffer 即使地址碰巧 4KB 对齐，也可能因为底层需要注册覆盖它的完整 2MB 区间而失败。当前 FIO patch 已在 `memory.c` 中为 `direct=1` 的 malloc buffer 做 2MB 对齐/向上取整；Filebench `directio=1` 是否满足该模式仍需单独确认。

### 7.18 GOTCHA 17：Caladan/Junction 内不能任意调用 Linux syscall

当前真实 Linux syscall 由 seccomp 按“syscall number + syscall instruction pointer 地址范围”控制：

- Caladan wrapper 必须位于 `[base_syscall_start, base_syscall_end)`。
- Junction `ksys_*` wrapper 必须位于 `[ksys_start, ksys_end)`。
- 额外的 VFIO ioctl 只按 request 放行，用于 SPDK DMA map/unmap。

因此不要在 shaoFS、Caladan runtime 或 Junction runtime 中直接调用 glibc `syscall()`、`open()`、`ioctl()` 等。需要新增真实 syscall 时，必须同时增加 wrapper、头文件声明和 seccomp allowlist，并确认调用点不会破坏 Junction 对用户 syscall 的拦截语义。

### 7.19 GOTCHA 18：uthread 路径里的短 spinlock 应禁用抢占，但不要全局无脑替换

Caladan 的 `spin_lock()` 只是 raw busy-wait，不会自动禁止 uthread 抢占。如果 uthread A 已经持有某把 spinlock 后被 Caladan 抢占，而同一个 kthread 上的 uthread B 又去拿同一把锁，B 会在用户态 busy-spin，导致 A 没机会恢复执行并释放锁，表现为死锁式等待或 timeout。

`spin_lock_np()` 的语义是 `preempt_disable(); spin_lock();`，对应 `spin_unlock_np()` 在解锁后 `preempt_enable()`。它适合 **短、纯内存、不会 yield/阻塞/IO** 的元数据临界区。

2026-05-15 已完成的低风险修复：

- `generic_cache/cache.h` 的 shard metadata lock 使用 `_np`，但 backend read/write 仍在 shard lock 外执行。
- `lib/caladan/runtime/storage.c` 的 `user_dma_lock` 使用 `_np`；`mlock()`、`spdk_mem_register()`、`spdk_vtophys()` 不在该锁内。
- `utili.h` 新增 `SpinGuardNP`。
- `blockCache.h::BlockPool::alloc/free` 改为 `SpinGuardNP`。
- `group.cc` 的 group bitmap/free counter 短临界区改为 `SpinGuardNP`。
- `inode.h` / `extent.cc` 的 `extent_hint` try-lock 改为 `spin_try_lock_np()` / `spin_unlock_np()`。
- `dsa.cc` 的 DSA request pool 初始化锁改为 `_np`。

不要全局替换所有 `spin_lock()`：

- `journal.cc::journal_lock` 临界区会调用 `storage_write()`，不能直接换成 `_np`，否则会在不可抢占状态下做 IO。
- `journal.cc::metadata_lock` 当前保护固定大小 `metadata_ranges[]`，已经不是 `std::vector` 动态扩容路径；但如果未来在该锁下重新引入动态分配、条件变量等待或 I/O，不能改成 `_np`。
- `lib/caladan/runtime/storage.c` 的 `q->lock` 多数路径在 `getk()` 后运行，而 `getk()` 已经 `preempt_disable()`；`thread_park_and_unlock_np()` 也要求进入时 preemption 已关闭。这类 runtime 锁需要按 Caladan 约定审计，不能按 shaoFS 元数据锁简单处理。

判断原则：只有在确认临界区不会执行 `rwmutex`/`mutex` 等可能阻塞的等待、不会访问磁盘/提交或等待 I/O、不会调用可能 yield 的函数时，才使用 `spin_lock_np()` 或 `SpinGuardNP`。

### 7.20 GOTCHA 19：PM9A3 randread IOPS 对工作集大小和总 outstanding 很敏感

2026-06-01 的 raw storage 诊断表明，当前 Samsung PM9A3 在 4KB random read 下不能只用“小随机范围 + 增加 core 数”解释硬件上限：

- 同样是 SPDK/Caladan 路径，小随机范围（例如 64GiB）会明显低估 4KB randread IOPS；`test_storage_async_iops` 已在 `effective_range_mb < 512GiB` 时打印 warning。
- 2 个 core、每 core QD64 的总 outstanding 只有 128，尚不足以稳定达到约 1.03M IOPS；2 个 core、每 core QD128（总 outstanding 256）或 1 个 core QD256 才接近本轮观测到的硬件 randread 上限。
- 因此“1 个 core QD64 已约 80 万 IOPS，所以 2 个 core QD64 必然线性到硬件上限”这个推断在当前盘上不成立。扩展性实验必须同时记录 core 数、每 core QD、总 outstanding、random range 和使用的 `spdk_nvme_perf` 二进制路径。
- 系统中至少存在三个 `spdk_nvme_perf`：`/usr/local/bin/spdk_nvme_perf`、`/home/syh/spdk/build/bin/spdk_nvme_perf`、`/home/syh/MyProj1/junction/lib/caladan/spdk/build/bin/spdk_nvme_perf`。本轮发现 `/home/syh/spdk/build/bin/spdk_nvme_perf` 曾给出异常低的 4-core randread 结果约 22K IOPS；后续硬件口径应优先使用 `/home/syh/MyProj1/junction/lib/caladan/spdk/build/bin/spdk_nvme_perf`，并在实验日志中写明完整路径。

对 ShaoFS/FIO IOPS 实验的直接影响：旧的 128-job × 128MiB 配置总数据集约 16GiB，不能作为“超过硬件/cache 并能代表全盘 random read”的正式证据；新加的 128-job × 4GiB large jobfile 才更适合作为后续 ShaoFS/ext4 random-read 对比起点。

### 7.21 GOTCHA 20：PM9A3 deallocated/unwritten LBA 与已写 LBA 的 randread IOPS 不同

2026-06-01/02 的裸盘复现实验进一步确认：这块 PM9A3 的 4KB random read IOPS 会被 LBA 状态显著影响，不能把 `blkdiscard` 后的全盘 randread 结果直接当成“读真实文件数据”的上限。

关键现象：

- `blkdiscard` 后，读取 deallocated/unwritten LBA 时，控制器可按 NVMe deallocate 语义返回零或走较轻路径；本机 `spdk_nvme_perf -q64 -o4096 -w randread -t20 -c0xF` 观测到约 `1.17M IOPS`。
- `blkdiscard` 后只顺序写前 256GiB，再用 SPDK 全盘 randread，仍约 `1.16M IOPS`，因为随机请求大部分落在未写区域，这个结果会被 unwritten LBA 稀释。
- 同一状态下，用 Linux raw block FIO 只读已写 `0-256GiB` 区间约 `583K IOPS`；只读未写 `512-768GiB` 区间约 `1.166M IOPS`。
- 用 SPDK 顺序写满整个 namespace 后，再测 SPDK 全盘 randread，结果降到约 `583.5K IOPS`。

正式 ShaoFS/ext4 random-read 实验必须在数据准备之后重新测 raw baseline，且说明 baseline 对应的是“已写数据 LBA”还是“discard 后 unwritten LBA”。如果目标是证明文件系统读真实文件数据能打满硬件，应使用全盘/近满写入后的 raw baseline；如果目标是展示厂商标称或最优 raw 能力，应单独说明这是 deallocated/unwritten 状态或经过 sanitize/format/discard 后的状态。

本轮可复现日志位于：

```text
/tmp/ssd_state_repro_20260601_163852
```

实验结束现场状态：目标盘已恢复 Linux `nvme` 驱动，`/dev/nvme2n1` 没有文件系统签名，并已被 SPDK 顺序写满过。后续文件系统实验必须重新格式化。

---

## 第八章：当前代码状态与测试结果

### 8.1 历史测试覆盖项（需重新验证）

以下表格只保留历史上记录过的测试项名称，表示这些测试曾被用作回归覆盖面。历史结果可能不可靠，正式交接或论文实验应重新运行并保存原始输出。一些普通场景下的测试，还是应尽量使用 FIO、Filebench、FXMARK 等业界常用 Benchmark 以提高数据的说服力。

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
| `readv` | `file_readv_direct` / scalar `my_read` loop | `file.cc:usys_readv` |
| `write` | `my_write` | `file.cc:usys_write` |
| `pread64` | `my_read` | `file.cc:usys_pread64` |
| `preadv` | `file_readv_direct` / scalar `my_read` loop | `file.cc:usys_preadv` |
| `pwrite64` | `my_write` | `file.cc:usys_pwrite64` |
| `lseek` | `my_lseek` | `file.cc:usys_lseek` |
| `fstat` | `my_fstat` | `file.cc:usys_fstat` |
| `newfstatat` | `my_newfstatat` | `file.cc:usys_newfstatat` |
| `fsync/fdatasync` | `my_fsync` | `file.cc:usys_fsync` |
| `sync` | `shaofs_sync_all` | `file.cc:usys_sync` |
| `unlink` | `my_unlink` | `core.cc:usys_unlink` |
| `unlinkat` | `my_unlink` when not `AT_REMOVEDIR` | `core.cc:usys_unlinkat` |

### 8.3 已实现的功能特性

- **三级缓存**：Block Cache (write-back) + Inode Cache (write-back) + Dentry Cache (write-through)
- **O_DIRECT user-buffer DMA**：满足 4KB 对齐请求合约且底层覆盖 2MB 注册成功时绕过 Block Cache，直接用用户 buffer 作为 SPDK NVMe read/write payload；不满足时返回错误
- **Direct `readv/preadv` 显式批量读**：shaoFS O_DIRECT fd 的 `readv/preadv` 会聚合多个整块 iovec，通过 `storage_read_aligned_batch()` 一次提交多个 NVMe read 并只 park 一次；普通 `pread/read` 不透明合并
- **O_TRUNC**：`truncate_inode()` 释放所有数据块并重置 file_size
- **O_APPEND**：shaoFS buffered write 路径已把 append 语义下推到 `my_write(..., append=true)` / `file_write_append()`，在 inode 写锁内读取 EOF 并写入，避免并发 append 抢同一个 EOF；direct `O_APPEND` 当前通过 `file_write_direct_append()` 在 inode 写锁内读取 EOF、校验 direct DMA 对齐约束并写入，dispatch 层仍会先更新 fd offset 作为兼容处理，但真正的 direct append EOF reservation 在 `file_write_direct_append()` 内完成
- **fsync**：dirty byte range 精确刷写 + inode metadata sequence 快路径；clean repeated fsync 可以直接返回
- **sync**：全局刷写 shaoFS 内存脏状态（imap、GDT、inode cache、block cache），用于 FxMark/FIO/Filebench 等会调用 `sync()` 的 benchmark；不会执行 clean unmount 语义或清 dirty marker
- **stat/fstat**：完整填充 `struct stat`（通过 tree-aware extent 遍历统计 allocated blocks）
- **Extent hint**：顺序访问 O(1) 块映射
- **Simple extent tree**：单文件 extent 容量从 legacy 176 扩展到当前 28567，少 extent 文件仍保持 legacy flat indirect 布局
- **EOF append 预分配与 batch write**：`extent.cc` 仍在普通文件 EOF append 且文件至少 256KB 时预分配 64/128 blocks；当前工作区还在 `file.cc` 增加了 regular-file EOF extension / buffered O_APPEND 的 full-block batch copy 路径，满足 64KB 以上、EOF 且 block-aligned 时可用 `dsa_copyv_ex(..., SHAOFS_DSA_WRITE_FROM_USER)` 批量从用户 buffer 写入 block cache
- **Per-core group affinity**：减少块分配锁竞争
- **目录读写锁**：`dir_lookup` 并发读，`dir_add/delete` 排他写
- **目录运行时索引**：目录 inode 内存态 `DirIndex` 按文件名 hash 加速 lookup/add/delete，并维护 free slot 链表和 live child 计数
- **unlink**：`my_unlink()` 已接入 shaoFS 普通文件删除；目录删除仍应走 `rmdir` 语义，当前 shaoFS 尚未实现 `my_rmdir`
- **I/O completion driven preemption**：`IO_PREEMPT` 开启时，IOKernel 检查 SPDK completion 并触发目标 Runtime core yield；Runtime 优先运行 storage softirq 和完成 I/O 的 uthread
- **shaoFS 前缀解析**：`SHAOFS_REALPATH()` 同时支持 `FSHAO/path` 与 `FSHAO:/path`，并拒绝 `FSHAOabc` 伪前缀
- **Block Cache 后台 data writeback**：large buffered write path 可通过固定环形队列把普通数据块交给后台 worker 聚合写回，使用 `dirty_gen` / `writeback_queued` 防止旧队列项清理新脏数据
- **Crash consistency**：`CRASH_CONSISTENCY=1` 时启用 metadata-only redo journal；当前 journal 支持同步 commit API、batched metadata group commit、异步 home-block checkpoint、clean shutdown 清 dirty marker，异常退出后扫描/replay/repair

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

### 8.5 2026-05-15 当前 FIO / Direct IO 状态

- `junction/fs/mytest/benchmark/fio` 是独立 FIO git 仓库；当前已执行 `toggle_fio.sh apply`，所以子仓库 `git status --short` 显示 `M filesetup.c`、`M helper_thread.c`、`M memory.c` 是预期状态。
- `junction/fs/mytest/benchmark/patch/fio_changes.patch` 已保存这三处源码修改，可用 `toggle_fio.sh revert` 恢复 FIO tracked 源码。
- `config-host.h` 和 `config-host.mak` 当前包含 `CONFIG_NO_SHM`，表示已用 `./configure --disable-shm` 构建 Junction 适配版。
- 当前已构建的 FIO 二进制可执行，`fio --version` 输出 `fio-3.42-22-g7215-dirty`。
- 当前 `junction/fs/mytest/benchmark/fio_test/directio.fio` 配置为 `ioengine=psync`、`thread=1`、`numjobs=16`、`directory=FSHAO/`、`rw=randread`、`bs=4k`、`direct=1`、`runtime=60`、`size=2G`。
- 2026-05-15 本轮在 spinlock 修复后复跑该配置，命令形态为：

```bash
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 100s ./junction_run caladan_test.config -- \
  ../../junction/fs/mytest/benchmark/fio/fio \
  ../../junction/fs/mytest/benchmark/fio_test/directio.fio
```

### 8.6 2026-05-17 FIO/cgroup 对比与底层 batching 实验状态

当前工作区新增/保留了一组更适合展示 shaoFS 单 runtime kthread + 多 uthread 优势的 FIO 配置：

- shaoFS：`junction/fs/mytest/benchmark/fio_test/psync_128job_randread_sweep.fio`
  - `ioengine=psync`
  - `thread=1`
  - `numjobs=128`
  - `directory=FSHAO/`
  - `iodepth=1`
  - `rw=randread`
  - `bs=4k`
  - `direct=1`
  - `runtime=30`
  - `size=128M`
  - `group_reporting` 未开启。注意：在单核且 `runtime_quantum_us=0` 时，uthread 实际串行运行，开启 `group_reporting=1` 会让 FIO 聚合口径产生误导。
- ext4：`junction/fs/mytest/scripts/fio_test/psync_128job_randread.fio`
  - 同样是 `psync`、128 jobs、4KB O_DIRECT random read。
  - 目录为 `/mnt/nvme/ext4_bench`。
  - `runtime=60`、`ramp_time=5`，其余形态和 shaoFS 版一致。

已核对到的 ext4 成功输出位于：

```text
junction/fs/mytest/scripts/results/ext4_fio_20260515_154054.log
```

该次脚本通过 `cg_run.sh` 限制在 `cpuset.cpus=2`、`memory.max=1GiB` 下运行 `psync_128job_randread.fio`，退出码 0，cgroup stats 显示 `timed_out=0`。FIO 聚合摘要：

```text
Run status group 0 (all jobs):
   READ: bw=853MiB/s (894MB/s), io=50.0GiB (53.7GB), run=60000-60002msec
Disk stats: nvme2n1 util=100.00%
```

对应约 `218k 4KB IOPS`。同目录下 `ext4_fio_20260515_154007.log` 是一次失败尝试，FIO 报 `Bad option <eta=never>`，退出码 1，不应用作性能结果。

2026-05-17 曾用 shaoFS 同形态 128-job FIO 做透明底层 read pending-submit batching 实验。已从 `/tmp/shaofs_fio_nobatch.json` 和 `/tmp/shaofs_fio_batch.json` 结构化输出核对：

```text
baseline:              sum(read.iops)=547070.987716, sum(read.bw)=2188225 KiB/s
pending-submit batch:  sum(read.iops)=547072.229839, sum(read.bw)=2188228 KiB/s
```

这两组结果实际上没有性能差异。batch stats 日志显示平均 batch size 约 `1.6`，大部分 flush 来自 softirq/age，而不是队列满。进一步尝试在 batching 模式下启用 SPDK `delay_cmd_submit` 以真正减少 doorbell/MMIO，但 `junction_run` 在 shaoFS init 附近 timeout，未能进入可用状态。基于这些结果，透明底层 pending-submit batching 相关代码已回退；当前代码库不应再出现 `SHAOFS_STORAGE_READ_BATCH`、`storage_pending_submissions`、`storage_batch_state` 或 SPDK `delay_cmd_submit` 实验逻辑。

当前保留的 batching 相关实现只有显式 direct `readv/preadv` 路径：

- `lib/caladan/inc/runtime/storage.h::storage_batch_read`
- `lib/caladan/runtime/storage.c::storage_read_aligned_batch()`
- `junction/fs/shaofs/file.cc::file_readv_direct()`
- `junction/fs/mytest/batch_direct_read_bench.c`

后续如果继续做真正的底层 NVMe submit batching，不应恢复简单 pending list。更合理的方向是先确认 SPDK qpair delayed-submit 与 Caladan iokernel completion/shadow CQ 机制如何安全协作，再设计明确的 flush 条件：队列深度达到阈值、等待时间超过 `age_us`、当前 kthread 即将 idle/park、或 completion softirq 前必须 kick。时间阈值是必要的，否则低并发和尾部请求可能等待过久。

### 8.7 2026-05-13 当前 Filebench 状态

- `junction/fs/mytest/benchmark/filebench` 是 Filebench 源码目录；当前通过 `junction/fs/mytest/benchmark/patch/filebench_changes.patch` 管理 Junction 适配修改。
- `toggle_filebench.sh apply` 会应用补丁，并用 `ac_cv_func_ftok=no ac_cv_func_semget=no ac_cv_func_semop=no ac_cv_func_semtimedop=no ./configure` 重新配置，然后执行 `make -j $(nproc)`。
- 当前补丁覆盖 9 个 Filebench 源文件：`aslr.c`、`fb_cvar.c`、`fb_localfs.c`、`fileset.c`、`flag.h`、`flowop_library.c`、`ipc.c`、`misc.c`、`procflow.c`。
- 适配后的 Filebench 不再依赖 `personality()` 关闭 ASLR，不再创建 SysV semaphore，不再 fork/exec worker process，也避免了 Filebench 日志和 mkdir 路径中的大栈对象；`fb_cvar.c` 还会从可执行文件所在 build tree 的 `cvars/.libs` 查找 cvar 插件，使 `webserver.f` 的 `cvar-gamma` 能在 Junction 内加载。
- 早期调试中曾使用 `example.f` 在 Junction 上跑通 60s Filebench 读 whole-file workload：10000 个 16KB 文件，2 个 process instances，每个 3 个 reader threads。记录到的输出约为 `79454650 ops`、`1324058 ops/s`、`6.9GB/s`、`0.0ms/op`、`0.660ms` latency；这些是历史调试验证数字，不是当前四项正式 benchmark。
- 2026-05-13 当时 `fileserver.f` 曾缩小为 shaoFS smoke workload：`set $dir=FSHAO:`、40 files、1 thread、4KB file/io、`run 2`。当时在 Junction/shaoFS 上完整跑通，输出约 `1979495 ops`、`989422.475 ops/s`、`579.4mb/s`。当前 WML 已恢复为完整 workload：10000 files、50 threads、60s runtime，最新结果见 `8.10`。
- repo 外部脚本 `/home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh` 是历史 smoke 对比脚本。当前四项对比以 repo 内 `run_filebench_compare.sh` / `run_ext4_filebench.sh` 为准。
- `webserver.f` 当前为 `set $dir=FSHAO:`、10000 files、100 threads、`run 60`。历史 1000 files 结果只作为早期跑通记录，不代表当前配置。
- 之后又派生了 Filebench `randomread.f` workload 到 `junction/fs/mytest/benchmark/filebench_wml/`，并编写了 repo 外部 ext4 cgroup 对比脚本 `/home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh`。本次交接整理未重新运行这些 randomread benchmark，无法从当前上下文确认最新 shaoFS/ext4 对比数字。
- 早期 Filebench 结果曾在 `DIRECTPATH DISABLED` 环境下得到，不能直接解释为最终 NVMe 极限带宽。2026-05-28 四项结果的完整环境信息应以后续正式实验日志为准。

**2026-05-17/2026-05-28 varmail 状态**：

- 2026-05-17/2026-05-19 当时 `junction/fs/mytest/benchmark/filebench_wml/shaofs_varmail.f` 与 `junction/fs/mytest/scripts/filebench_test/ext4_varmail.f` 均为 1000 files、16 threads、16KB mean append、`run 60`，主要差异是 `$dir` 分别指向 `FSHAO:` 和 `/mnt/nvme/ext4_bench`。当前 shaoFS WML 文件名为 `varmail.f`，配置为 5000 files、16 threads、60s runtime。
- shaoFS 初始 varmail 瓶颈主要在 `fsyncfile2/fsyncfile3`：`/tmp/shaofs_varmail_confirm.log` 显示 `IO Summary: 3693489 ops 61557.127 ops/s 222.3mb/s`，`fsyncfile2 2.367ms/op`、`fsyncfile3 0.970ms/op`。
- 只加入 dirty range / inode sequence fsync 快路径后，`/tmp/shaofs_varmail_fastpath.log` 仍为 `61372.790 ops/s`，说明 varmail 的 fsync 基本都紧跟 append，真正主瓶颈不是 clean repeated fsync，而是每次 dirty fsync 的 journal/home-block 持久化成本。
- 2026-05-19 当时多槽 async checkpoint + group commit 后，`/tmp/shaofs_varmail_ring.log` 显示 `IO Summary: 10404113 ops 173399.294 ops/s 625.1mb/s`，`fsyncfile2 0.727ms/op`、`fsyncfile3 0.399ms/op`；后续曾为了 correctness 暂时退回同步 checkpoint，之后又在 dirty generation / clean-if-unchanged 约束下恢复 batched metadata lane 的 async checkpoint。
- ext4 同 WML、`cpus=2`、`memory=300MiB` 的日志为 `junction/fs/mytest/scripts/results/ext4_filebench_20260517_155402.log`，显示 `IO Summary: 6645666 ops 110748.997 ops/s 399.4mb/s`，`fsyncfile2 0.447ms/op`、`fsyncfile3 0.398ms/op`。
- ext4 同 WML、`memory=2048MiB` 的日志为 `junction/fs/mytest/scripts/results/ext4_filebench_20260517_154912.log`，显示 `96831.694 ops/s`、`350.6mb/s`。本轮只做了单次观测，不能据此得出“内存越大越慢”的通用结论；它只能说明当前 300MiB 限制下 ext4 并没有因内存更小而明显劣化。
- 2026-05-28 清理阶段已移除临时 journal/profile 统计代码和临时 `test_shaofs_fsync_fastpath.c` 源文件；清理后已重新构建，并通过 smoke 与完整四项 Filebench 对比。最新 varmail 结果见 `8.10`。

### 8.8 2026-05-15 当前 FxMark 状态

- `junction/fs/mytest/benchmark/fxmark` 是 FxMark 源码目录；当前通过 `junction/fs/mytest/benchmark/patch/fxmark_changes.patch` 管理 Junction 适配修改。
- `toggle_fxmark.sh apply` 会应用补丁并执行 `make -j "$(nproc)"`；`toggle_fxmark.sh revert` 会反向应用补丁并重新构建。2026-05-15 已实际验证 `revert` 和 `apply` 都能干净执行并构建通过。
- 当前补丁覆盖 4 个文件：`Makefile`、`src/bench.c`、`src/DRBL.c`、`src/util.c`。当前 FxMark 工作区另外显示 `bin/install-fs-tools.sh` modified（`btrfs-tools` 改为 `btrfs-progs`），但该文件不在 `fxmark_changes.patch` 中；接手者应确认这是用户环境修正还是需要纳入 patch。
- 适配后的 FxMark 不再使用 `fork()` 创建 worker，而是在同一 Junction 进程内使用 pthread worker；启动/结束屏障中的纯 busy-wait 改为 `sched_yield()`，避免 `runtime_quantum_us=0` + 单 runtime kthread 下等待线程自旋霸占 CPU。
- `src/util.c` 中的 `mkdir_p()` 已改为进程内递归 `mkdir()`，避免 `system("mkdir -p")` 在 Junction/shaoFS 路径上触发额外 shell/未支持机制。
- `src/DRBL.c` 当前用 wall-clock deadline 让 worker 自己结束；这样不依赖 `SIGALRM` 在 tight loop 中及时投递。这个改动只覆盖 DRBL workload，其他 FxMark workload 是否也需要类似处理尚未验证。
- 2026-05-15 验证 FxMark 时的 `build/junction/caladan_test.config` 为 `runtime_kthreads=1`、`runtime_spinning_kthreads=1`、`runtime_quantum_us=0`。2026-05-19 当时文件已改为 `runtime_kthreads=10`、`runtime_spinning_kthreads=0`、`runtime_quantum_us=100`；2026-05-20 当时文件又改为 `runtime_kthreads=1`、`runtime_spinning_kthreads=1`。当前 `build/junction/caladan_test.config` 为 `runtime_kthreads=1`、`runtime_spinning_kthreads=0`、`runtime_quantum_us=100`。以下历史结果主要证明当时 FxMark 多 worker 在 Junction/shaoFS 上能稳定跑完，不代表当前配置或真实多核扩展性。

已验证命令形态：

```bash
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 45s ./junction_run caladan_test.config -- \
  /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/fxmark/bin/fxmark \
  --type DRBL --ncore 8 --nbg 0 --duration 5 --directio 0 --root FSHAO/demo
```

本轮观察到的 DRBL smoke 结果：

```text
--ncore 1: 1 5.000181 30830592.000000 6165895.194594
--ncore 2: 2 2.500116 30744576.000000 12297259.807145
--ncore 4: 4 1.250023 30773248.000000 24618135.579051
--ncore 8: 8 0.625031 30765056.000000 49221658.050092
```

注意：这些是跑通/烟测数字，不是论文最终 benchmark。正式实验前应重新固定 Junction config、构建开关、timeout、FxMark patch 状态和完整 stdout/stderr。

### 8.9 2026-05-09 crash consistency 验证结果

本次验证使用默认 `SHAOFS_CRASH_CONSISTENCY=ON` 构建；同时确认 `SHAOFS_CRASH_CONSISTENCY=OFF` 可以成功编译。测试前重新执行过 `/home/syh/mkfs/mkfs.sh`，mkfs 输出包含：

```text
Journal: start=234419030 blocks=4096
```

已验证：

```text
journal_layout_probe:
shaofs mount ok

journal_recovery_prepare --crash:
由 timeout -s KILL 2s 强杀，退出码 137，模拟非 clean shutdown

journal_recovery_check:
[journal] previous mount was dirty, repairing metadata state
journal recovery check failures=0
```

最终二进制上还回归了：

```text
test_fsync: 32 passed, 0 failed
test_dir:   22 passed, 0 failed
```

性能 sanity check：

```text
baseline before journal:
bench_seq_rw 32: Write 0.4981s, 64.24 MB/s, 16445 IOPS; Read 0.0037s, 8679.14 MB/s, 2221861 IOPS
4KB_iops:        9,455,148.78 IOPS, 36,934.17 MB/s

after CRASH_CONSISTENCY=ON:
bench_seq_rw 32: Write 0.4978s, 64.29 MB/s, 16457 IOPS; Read 0.0034s, 9467.46 MB/s, 2423669 IOPS
4KB_iops:        9,438,393.29 IOPS, 36,868.72 MB/s
```

这些数字说明当前 metadata-only journal 对该组读密集/顺序小规模 sanity benchmark 没有明显断崖式性能下降。正式论文性能数据仍应重新跑完整 benchmark 并保存原始输出。

### 8.10 2026-05-28 Filebench 四项对比结果

本次使用统一脚本运行：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S env BUILD_JUNCTION=0 TIMEOUT_SEC=240s \
  junction/fs/mytest/scripts/run_filebench_compare.sh 2
```

结果目录：

```text
junction/fs/mytest/scripts/results/filebench_compare_20260528_130532
```

所有 8 个 `.status` 文件均为 `0`。测试结束后已确认没有残留 `iokerneld` / `junction_run` 进程。该轮结果是在当前工作区源码、当前 Filebench patch 和当前 WML 下得到的单次运行结果，正式论文图表仍建议做多轮重复并记录均值/方差。

| Workload | ShaoFS ops/s | ext4 ops/s | ShaoFS 提升 | ShaoFS bandwidth | ext4 bandwidth |
|----------|--------------|------------|-------------|------------------|----------------|
| fileserver | 56,487.827 | 49,290.432 | +14.6% | 1353.7 MB/s | 1181.7 MB/s |
| webserver | 485,956.349 | 311,723.678 | +55.9% | 2555.1 MB/s | 1639.0 MB/s |
| varmail | 126,901.212 | 92,871.076 | +36.6% | 457.8 MB/s | 334.2 MB/s |
| webproxy | 612,979.535 | 246,596.963 | +148.6% | 1516.0 MB/s | 613.3 MB/s |

原始 `IO Summary`：

```text
ShaoFS fileserver: 3389677 ops 56487.827 ops/s 5135/10271 rd/wr 1353.7mb/s 0.870ms/op
ShaoFS webserver:  29158589 ops 485956.349 ops/s 156760/15677 rd/wr 2555.1mb/s 0.204ms/op
ShaoFS varmail:    7614351 ops 126901.212 ops/s 19523/19523 rd/wr 457.8mb/s 0.123ms/op
ShaoFS webproxy:   36779683 ops 612979.535 ops/s 161310/32262 rd/wr 1516.0mb/s 0.162ms/op

ext4 fileserver:   2959679 ops 49290.432 ops/s 4481/8962 rd/wr 1181.7mb/s 0.996ms/op
ext4 webserver:    18723241 ops 311723.678 ops/s 100555/10057 rd/wr 1639.0mb/s 0.197ms/op
ext4 varmail:      5572902 ops 92871.076 ops/s 14288/14288 rd/wr 334.2mb/s 0.169ms/op
ext4 webproxy:     14817833 ops 246596.963 ops/s 64894/12979 rd/wr 613.3mb/s 0.335ms/op
```

本轮结果对应的主要机制贡献：

- fileserver：EOF append 预分配、large full-block batch copy、data block 后台 writeback、目录索引和 simple extent tree 降低了 append-heavy 场景的元数据/flush 压力。
- webserver/webproxy：目录索引、inode/dentry/cache 命中、Junction syscall bypass 和 uthread 调度优势是主要贡献；小写路径未强行进入 data writeback 队列，避免读主导场景被后台写回干扰。
- varmail：fsync dirty range、metadata group commit、async home-block checkpoint 和 data writeback 解耦降低了 append+fsync 的前台延迟。

### 8.11 2026-06-01 Caladan raw storage IOPS 诊断结果

本轮目标是解释为什么提高 `runtime_kthreads` 后 randread IOPS 没有线性扩展，以及确认 Caladan runtime + storage API 是否能接近 PM9A3 硬件上限。当前保留的正式诊断工具是 `lib/caladan/tests/test_storage_async_iops.c`。

**硬件口径参考**：

- 用户手动使用 `/home/syh/MyProj1/junction/lib/caladan/spdk/build/bin/spdk_nvme_perf -q 64 -o 4096 -w randread -t 60 -c 0xF` 测得约 `1,028,269 IOPS`。
- 用户手动使用同一路径 `spdk_nvme_perf -q 64 -o 4096 -w randwrite -t 60 -c 0xF` 测得约 `364,374 IOPS`。
- `/usr/local/bin/spdk_nvme_perf` 与 `lib/caladan/spdk/build/bin/spdk_nvme_perf` 在 4-core randread 下均可达到约 1.1M IOPS 量级；`/home/syh/spdk/build/bin/spdk_nvme_perf` 曾在同命令下只输出约 22K IOPS，不应混用为硬件基准。

**Caladan raw async benchmark 关键结论**：

- 使用全 namespace random range（`RANGE_MB=0`）时，Caladan raw async benchmark 可以接近硬件口径：
  - 1c QD64：约 `809K IOPS`
  - 2c QD64：约 `960K IOPS`
  - 4c QD64：约 `1.03M IOPS`
  - 8c QD64：约 `1.03M IOPS`
- 使用 64GiB random range 时 IOPS 明显偏低，曾观测约 1c `497K IOPS`、4c `614K IOPS`。这不是 runtime core 扩展性的主要问题，而是当前 PM9A3 在小随机工作集下的设备/FTL 行为会低估全盘 random read 上限。
- “2 个 core QD64 未达到约 1.03M”主要是总 outstanding 不足。2c QD64 的总 outstanding 只有 128；提高到 2c QD128（总 outstanding 256）后可达到约 `1.029M IOPS`。1c QD256 也可达到约 `1.014M IOPS`。SPDK perf 在相同 QD 变化下表现一致，因此该现象不是 Caladan runtime 调度 bug。
- 当前未发现 `runtime_kthreads` 本身导致未达硬件上限的线性扩展 bug；真正需要控制的是每 core QD、总 outstanding、random range 和 benchmark 是否真的绕过了文件系统额外开销。

**已清理的探索代码**：

- 临时 `storage_read2()` / `storage_write2()` 声明和实现已删除。
- `test_storage_iops.c` 已恢复为 Caladan 原始写 IOPS smoke benchmark。
- 早期探索用 `test_storage_randread_iops.c` 及生成物已删除。

**保留的代码/文件**：

- `storage_async_read()` / `storage_async_write()` / `storage_async_poll()`：`test_storage_async_iops.c` 需要 fixed-QD async API 来维持可控 outstanding；这些接口不改变 shaoFS 正常 hot path。
- `SAMSUNG MZQL2960HCJR` known device 条目：让当前 PM9A3 被 Caladan 识别为低延迟 NVMe，不是 debug 代码。
- `storage_{1c,2c,4c,8c}_q0.config` 和 `run_storage_async_iops.sh`：用于复现 core/QD sweep。
- `psync_128job_randread_large.fio`：用于后续 ShaoFS/ext4 FIO random-read 对比时避免小数据集误导。

**本轮清理后的验证**：

```bash
cd /home/syh/MyProj1/junction/lib/caladan
make -j 64 tests/test_storage_async_iops tests/test_storage_iops
git -C /home/syh/MyProj1/junction/lib/caladan diff --check
```

构建通过，`diff --check` 通过。构建期间 `runtime/storage.c` 仍有若干既有 style warning（例如 misleading indentation、const discard），但不是本轮 cleanup 新增的错误。清理后已确认没有残留 `iokerneld`、`test_storage_async_iops`、`timeout` 或 `sudo` 进程。

### 8.12 2026-06-01/02 ShaoFS/ext4 FIO 256-job O_DIRECT 对比与 SSD 状态复现

本轮用户要求把 FIO `numjobs` 提高到 256，并确认 ShaoFS 是否能在少量 core 下接近 NVMe 4KB random read 上限。实验只完成了 `O_DIRECT` 场景；buffered I/O 尚未重跑。

**核心控制方式**：

- ShaoFS：通过 Junction runtime config 中的 `runtime_kthreads` 控制 core budget；FIO `numjobs=256` 不变。配置文件位于 `/tmp/shaofs_full_20260601_124815/configs/shaofs_{1,2,4,8,16,32}c.config`。
- ext4：通过 Linux `taskset` 限制 FIO 进程 CPU affinity；FIO `numjobs=256` 不变。
- 两者均使用 `psync`、`iodepth=1`、`bs=4k`、`direct=1`。虽然单 job iodepth 为 1，但 256 个 job 合计可形成约 256 outstanding I/O。

**数据准备**：

- ShaoFS：先执行 `/home/syh/mkfs/mkfs.sh`，再用 `shaofs_prepare_large_files` 创建 256 个 private file，每个 3575MiB，总计约 915200MiB。日志：`/tmp/shaofs_full_20260601_124815/logs/prepare_256x3575m.log`。
- ext4：测试盘曾被格式化为 ext4 并挂载 `/mnt/ext4_cmp`，mount options 为 `rw,noatime,nodiratime,stripe=32`。3575MiB/file 因 ext4 metadata/journal 开销触发 ENOSPC，正式使用 3500MiB/file，总计约 875GiB。日志：`/tmp/shaofs_full_20260601_124815/logs/fio_ext4_prepare_256x3500m.log`。

**FIO jobfiles**：

- ShaoFS：`junction/fs/mytest/benchmark/fio_test/psync_256job_randread_full_direct.fio`
- ext4 prepare：`junction/fs/mytest/benchmark/fio_test/ext4_prepare_256x3500m.fio`
- ext4 read：`junction/fs/mytest/benchmark/fio_test/ext4_psync_256job_randread_3500m_direct.fio`

**ShaoFS/ext4 core sweep 结果**：

| cores | ShaoFS MiB/s | ShaoFS IOPS | ext4 MiB/s | ext4 IOPS |
|------:|-------------:|------------:|-----------:|----------:|
| 1 | 2278 | 583168 | 965 | 247040 |
| 2 | 2278 | 583168 | 1501 | 384256 |
| 4 | 2277 | 582912 | 2278 | 583168 |
| 8 | 2277 | 582912 | 2278 | 583168 |
| 16 | 2277 | 582912 | 2279 | 583424 |
| 32 | 2277 | 582912 | 2279 | 583424 |

对应日志：

```text
/tmp/shaofs_full_20260601_124815/logs/fio_shaofs_{1,2,4,8,16,32}c_256job_full_direct.log
/tmp/shaofs_full_20260601_124815/logs/fio_ext4_{1,2,4,8,16,32}c_256job_3500m_direct.log
```

解释：在本轮“near-full/已写数据”盘状态下，ShaoFS 1 core 已达到当前 raw device baseline；ext4 约需 4 cores 达到同水平。因此这轮数据支持 ShaoFS 的 CPU efficiency 优势，但不能用来声称达到 `1M+ IOPS`，因为此时读真实写过数据 LBA 的 raw baseline 自身约为 583K IOPS。

**SSD 状态复现实验**：

为确认“盘状态影响 IOPS”不是 ShaoFS/FIO 引入的假象，本轮只使用裸盘工具复现。日志目录：

```text
/tmp/ssd_state_repro_20260601_163852
```

关键结果：

| 状态 | 工具 | 范围 | 结果 |
|------|------|------|------|
| `blkdiscard` 后 | SPDK `spdk_nvme_perf randread` | 全盘 | `1,173,943 IOPS` |
| 只顺序写 256GiB 后 | SPDK `spdk_nvme_perf randread` | 全盘 | `1,159,557 IOPS` |
| 只顺序写 256GiB 后 | Linux raw FIO randread | 已写 `0-256GiB` | `583K IOPS` |
| 只顺序写 256GiB 后 | Linux raw FIO randread | 未写 `512-768GiB` | `1,166K IOPS` |
| SPDK 顺序写满全盘后 | SPDK `spdk_nvme_perf randread` | 全盘 | `583,542 IOPS` |

可复现命令骨架：

```bash
# 清空签名并 discard
sudo umount /mnt/ext4_cmp 2>/dev/null || true
sudo wipefs -a /dev/nvme2n1
sudo blkdiscard -f /dev/nvme2n1

# 绑定给 SPDK，测 discard 后全盘 randread
sudo PCI_ALLOWED="0000:5b:00.0" /home/syh/MyProj1/junction/lib/caladan/spdk/scripts/setup.sh
sudo /home/syh/MyProj1/junction/lib/caladan/spdk/build/bin/spdk_nvme_perf \
  -q 64 -o 4096 -w randread -t 20 -c 0xF \
  -r 'trtype:PCIe traddr:0000:5b:00.0'

# 顺序写 256GiB：67108864 * 4KiB
sudo /home/syh/MyProj1/junction/lib/caladan/spdk/build/bin/spdk_nvme_perf \
  -q 64 -o 4096 -w write -d 67108864 -t 3600 -c 0x1 \
  -r 'trtype:PCIe traddr:0000:5b:00.0'

# 绑回 Linux 后用 raw block FIO 分别读已写/未写区间
sudo PCI_ALLOWED="0000:5b:00.0" /home/syh/MyProj1/junction/lib/caladan/spdk/scripts/setup.sh reset
FIO=/home/syh/MyProj1/junction/junction/fs/mytest/benchmark/fio/fio

sudo $FIO --name=written_0_256g --filename=/dev/nvme2n1 \
  --offset=0 --size=256G --rw=randread --bs=4k --direct=1 \
  --ioengine=libaio --iodepth=64 --numjobs=4 --runtime=20 \
  --time_based=1 --group_reporting=1 --norandommap=1 --randrepeat=0

sudo $FIO --name=dealloc_512_768g --filename=/dev/nvme2n1 \
  --offset=512G --size=256G --rw=randread --bs=4k --direct=1 \
  --ioengine=libaio --iodepth=64 --numjobs=4 --runtime=20 \
  --time_based=1 --group_reporting=1 --norandommap=1 --randrepeat=0
```

SPDK-only 写满复现：

```bash
sudo PCI_ALLOWED="0000:5b:00.0" /home/syh/MyProj1/junction/lib/caladan/spdk/scripts/setup.sh

# 234441648 * 4KiB ~= 894.3GiB
sudo /home/syh/MyProj1/junction/lib/caladan/spdk/build/bin/spdk_nvme_perf \
  -q 64 -o 4096 -w write -d 234441648 -t 7200 -c 0x1 \
  -r 'trtype:PCIe traddr:0000:5b:00.0'

sudo /home/syh/MyProj1/junction/lib/caladan/spdk/build/bin/spdk_nvme_perf \
  -q 64 -o 4096 -w randread -t 20 -c 0xF \
  -r 'trtype:PCIe traddr:0000:5b:00.0'
```

现场状态：本复现实验结束后已执行 SPDK setup reset，`/dev/nvme2n1` 在 Linux `nvme` 驱动下，无文件系统签名，且全盘已写过。后续 ShaoFS/ext4 实验必须重新格式化。

### 8.13 2026-06-03 ShaoFS 重构阶段结果

本轮用户目标是对 `junction/fs/shaofs` 做深度 code review 和可读性重构。第一次计划当时记录在仓库根目录 `plan.md`；第二次、也是当时更重要的执行计划记录在 `plan2.md`。截至 2026-06-07，当前根目录 `plan.md` 已被 Junction 单容器多进程验证 checklist 覆盖，当前工作区未发现 `plan2.md`；本节保留的是 2026-06-03 阶段的历史摘要，不应再从当前 `plan.md` 恢复旧 ShaoFS 重构计划。目录子系统阶段在分析后选择跳过大改，原因是 `DirIndex` 不是 `dentryCache` 的简单重复，贸然删除会破坏目录 free slot 复用和 `dir_is_empty()` 快路径。

当时 tracked ShaoFS 代码 diff 规模为 `10 files changed, 974 insertions(+), 724 deletions(-)`。这不是“净删除大量代码”的重构；主要价值在于把大函数中的隐式状态、重复分支和补丁式路径拆成更明确的 helper 和小结构体，降低后续维护成本。主要涉及：

- `shaofs/syscall.cc`：把 `my_open()` / `my_mkdir()` / `my_fsync()` 中的路径准备、inode 初始化、目录创建、dirty-range fsync 等逻辑拆成命名 helper；`my_mkdir()` 的错误返回现在更明确地保持负 errno 语义。
- `shaofs/file.cc` / `file.h`：引入 `CachedWriteOp`，把 cached write 的参数组收敛；拆出 `write_cached_existing_block()`、`write_cached_allocated_block_locked()`、`file_write_extend_locked()`、`direct_write_one_block_locked()`；direct `O_APPEND` 由 `file_write_direct_append()` 在 inode 写锁内完成 EOF reservation。
- `shaofs/blockCache.cc`：`bc_flush_blocks_contiguous()` 改用 `CachedFlushRun` 明确描述 contiguous flush run，并拆出 writeback/metadata flush 的判断与提交 helper。
- `shaofs/journal.cc`：repair/recovery 路径使用 `CFreeBuffer<T>` 管理 C 分配内存，并拆出 journal slot 分类、slot 收集、replay、clear 和 dirty mount repair helper。
- `shaofs/extent.h/cc`：补充给 recovery/repair 使用的 disk inode extent 遍历 helper，避免 journal 修复路径继续手写 extent 扫描细节。

重构后正确性检查结果目录：

```text
/tmp/shaofs_plan2_phase6_correctness_20260603
```

该阶段以下测试退出码均为 `0`：`test_shaofs_syscall_correctness`、`test_shaofs_direct_correctness`、`test_shaofs_concurrent_correctness`、`test_shaofs_append_prealloc`、`test_shaofs_concurrent_append`、`test_shaofs_direct_concurrent_append`、`test_shaofs_many_extents`、`test_shaofs_mt_full_extents 4 256 2 write-verify 8`、`test_shaofs_fsync_direct_verify`、`journal_recovery_check`。`journal_recovery_prepare --crash` 由 `timeout` 杀死，退出码 `124` 是该 crash-recovery 测试的预期行为。

Filebench 四项性能对比使用 2026-06-03 Phase0 baseline 和 plan2 final 结果；结果目录如下：

```text
junction/fs/mytest/scripts/results/filebench_compare_20260603_010941
junction/fs/mytest/scripts/results/filebench_compare_plan2_final_20260603_015704
junction/fs/mytest/scripts/results/filebench_compare_plan2_final_fileserver_rerun_20260603_020235
junction/fs/mytest/scripts/results/filebench_compare_plan2_final_varmail_rerun_20260603_020408
```

| Workload | Phase0 baseline ShaoFS ops/s | Final ShaoFS ops/s | 变化 |
|----------|------------------------------:|-------------------:|-----:|
| fileserver | 62,900.199 | 62,460.787 | -0.70% |
| webserver | 484,067.536 | 483,673.175 | -0.08% |
| varmail | 177,792.079 | 173,705.862 | -2.30% |
| webproxy | 389,611.111 | 612,508.355 | +57.21% |

`fileserver` 和 `varmail` 的 final 首轮曾低于 baseline，随后单项重跑后恢复到上表数值；因此当前没有确认到超过 10% 的性能回退。正式论文数据仍应多轮重复并记录均值/方差，不能只引用单轮结果。

本轮测试结束后已清理测试进程；接手时仍建议先执行 `pgrep -a iokerneld` / `pgrep -a junction_run` 确认现场干净。当前构建缓存中 `SHAOFS_CRASH_CONSISTENCY=ON`、`SHAOFS_IO_PREEMPT=ON`；`build/junction/caladan_test.config` 为 `runtime_kthreads 1`、`runtime_spinning_kthreads 0`、`runtime_guaranteed_kthreads 0`、`runtime_quantum_us 100`、`enable_storage 1`。

### 8.14 2026-06-07 Junction 单容器多进程验证结果

本轮目标是验证 Junction README 中提到的单个 `junction_run` 容器运行多个进程/任务的能力。实现方式是新增用户态测试程序和报告，没有修改 Junction/ShaoFS/Caladan 实现代码。

新增文件和记录：

- 测试程序：`junction/fs/mytest/junction_multiproc_vfork.c`
- 实践报告：`junction_multiproc_report.md`
- 当前计划文件：`plan.md`，内容为 Junction single-container multi-process verification checklist
- 原始日志目录：`/tmp/junction_multiproc_20260607_162050`

编译命令：

```bash
cd /home/syh/MyProj1/junction
gcc -O2 -Wall -Wextra junction/fs/mytest/junction_multiproc_vfork.c \
  -o build/junction/mytest/junction_multiproc_vfork -lpthread
```

测试前执行过：

```bash
printf 'syh2syh\n' | sudo -S bash /home/syh/mkfs/mkfs.sh
```

`vfork()` + `execv()` 路线：

```bash
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 30s ./junction_run caladan_test.config -- \
  mytest/junction_multiproc_vfork 4 20 50000 FSHAO:/junction_multiproc_vfork
```

结果状态 `vfork.status=0`。关键输出：

```text
CONTROLLER_START pid=1 tid=1 workers=4
PARENT_AFTER_VFORK worker=0 child_pid=2
PARENT_AFTER_VFORK worker=1 child_pid=3
PARENT_AFTER_VFORK worker=2 child_pid=4
PARENT_AFTER_VFORK worker=3 child_pid=5
WORKER_START id=0 pid=2 tid=2 ppid=1
WORKER_START id=1 pid=3 tid=3 ppid=1
WORKER_START id=2 pid=4 tid=4 ppid=1
WORKER_START id=3 pid=5 tid=5 ppid=1
WAIT_OK worker=0 child_pid=2
WAIT_OK worker=1 child_pid=3
WAIT_OK worker=2 child_pid=4
WAIT_OK worker=3 child_pid=5
VERIFY_OK worker=0
VERIFY_OK worker=1
VERIFY_OK worker=2
VERIFY_OK worker=3
MULTIPROC_VFORK_OK workers=4
```

fish / `posix_spawn()` 路线：

```bash
cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 30s ./junction_run caladan_test.config -- \
  /usr/bin/fish -c 'for i in (seq 0 3); mytest/junction_multiproc_vfork --worker $i 20 50000 FSHAO:/junction_multiproc_fish &; end; wait'
```

结果状态 `fish.status=0`。关键输出显示 worker PID/TID 为 `4/4`、`5/5`、`6/6`、`7/7`，所有 worker `ppid=1`，均正常完成。该路线验证 README 推荐的 fish 后台任务方式可以在单个 Junction 容器内启动多个任务。

host 侧证据：

- 长运行版测试状态 `hostps_vfork.status=0`。
- 测试运行期间 `pgrep -a junction_run` 只看到一个 host `junction_run` 进程。
- `ps -T -p <junction_run_pid>` 只显示 `junction_run`、`dpdk-intr`、`dpdk-mp-msg` 等线程，没有为每个 Junction child 创建一个 Linux 子进程。
- 与此同时，容器 stdout 中仍能看到 controller PID `1` 和 worker PID `2`、`3`、`4`、`5`。

结论：当前 Junction 在本项目环境中可以在单个 `junction_run` 容器内运行多个 Junction process；这些 process 由 LibOS/Caladan runtime 管理，host 侧不表现为多个 Linux child process。

边界：本轮只证明 `vfork()` + `execve()`、fish/`posix_spawn()` 这两条路线可用。它不能作为通用 `fork()` 语义完整支持的证据，也不能直接用于推翻 Filebench/FxMark 当前为了适配 Junction 而采用的 pthread worker 模型。

本轮结束后已确认没有残留 `iokerneld` / `junction_run`。当前构建缓存仍为 `SHAOFS_CRASH_CONSISTENCY=ON`、`SHAOFS_IO_PREEMPT=ON`；`build/junction/caladan_test.config` 为 `runtime_kthreads 1`、`runtime_spinning_kthreads 0`、`runtime_guaranteed_kthreads 0`、`runtime_quantum_us 100`、`enable_storage 1`。

---

## 第九章：已知缺陷与待办事项

### 9.1 已知缺陷

1. **并发文件创建未充分测试**：多线程同时 `open(O_CREAT)` 在同一目录下创建不同文件的正确性未验证
2. **Extent tree 仍是固定深度**：旧的 176 extent 上限已通过 simple extent tree 扩展到 28567 extents，但它不是无限结构；超过 root/leaf 容量或极端乱序碎片写仍会失败或落入较重的收集/排序/重写 slow path
3. **高线程混合负载表现需要重新确认**：历史上曾关注 cache shard 竞争，但具体性能结论不再写入本文档，后续应重新 benchmark
4. **`rmdir` 尚未实现 shaoFS 专用路径**：`unlink` / `unlinkat` 普通文件删除已接入 `my_unlink()`；`unlinkat(..., AT_REMOVEDIR)` / `rmdir` 仍走 Junction 原生 VFS 路径，shaoFS 目录删除语义尚未实现
5. **无 `getdents64`**：目录列表需要直接读取 Dirent 结构
6. **时间戳全为 0**：DInode 有 atime/mtime/ctime 字段但无代码设置
7. **O_DIRECT user-buffer DMA 适用范围有限**：当前已实现严格约束下的 direct user-buffer DMA；请求层要求 `buf`/`len`/`offset` 4KB 对齐，但底层仍会对覆盖区间按 2MB 粒度注册。FIO 已做 2MB arena 适配；Filebench 等通用 benchmark 不一定天然满足这些约束，需专门确认
8. **FIO 适配是源码补丁而非上游通用修复**：当前只保证本项目常用参数（特别是 `--thread=1`、psync、shaoFS 路径）可跑通；不要默认它覆盖 FIO 全部 job 组合。
9. **`MYPREFIX` 与测试路径书写存在历史不一致**：当前代码定义为 `"FSHAO"`，`SHAOFS_REALPATH()` 已兼容 `FSHAO/...` 与 `FSHAO:/...`，但 FIO 当前源码仍将未转义 `:` 作为 filename/directory 分隔符，因此 FIO 命令建议写 `FSHAO/`；若恢复 `FSHAO:`，必须设计并验证 FIO 参数转义方案。
10. **`IO_PREEMPT` 只在特定场景下显著收益**：它主要解决 CPU-bound uthread 阻塞 SPDK completion poll 的问题；没有 CPU-bound 干扰、kthread 充足或大块顺序吞吐场景下，收益可能较小甚至需要评估额外 UIPI 开销。
11. **当前构建缓存开启了 `SHAOFS_IO_PREEMPT`**：`build/CMakeCache.txt` 当前为 ON，但 CMake 默认值仍为 OFF。做性能对比时必须明确重新 configure，避免把 ON/OFF 结果混淆。
12. **Crash consistency 不是完整事务语义**：当前只保证异常退出后元数据合法、自洽；普通数据块不 journal，不完整创建/写入可能被 repair 清理或留下已落盘的数据内容。
13. **Journal metadata map 依赖目录 extent 登记**：如果后续新增目录扩容、rename、rmdir 或新的目录写路径，必须确保新目录块被 `journal_register_metadata_block()` 登记，否则该目录块写回可能绕过 journal。
14. **FS base 策略是针对 shaoFS 的混合修复，不是 Junction 全局 TLS 架构终局**：当前已经覆盖 shaoFS guard 内 park/yield 的场景，但其他隐式进入 runtime libc 且可能 yield 的路径仍需单独审计。
15. **Filebench patch 改变 procflow 执行模型**：当前 Filebench 适配版用于跑通 Junction/shaoFS 学术负载；它不是对上游 Filebench 多进程语义的完整兼容。
16. **32768 inode 边界尚未完整耗尽验证**：当前已修复约 8192 inode 附近失败的问题，并验证过 10000 文件级别场景；完整创建到接近 `INODENUM=32768` 后的行为仍应补充压力测试。
17. **当前工作区存在未跟踪 benchmark/patch/script/result 文件**：`junction/fs/mytest/benchmark/`、`junction/fs/mytest/scripts/`、`junction/fs/mytest/scripts/results/`、`plan.md`、`junction_multiproc_report.md`、`junction/fs/mytest/junction_multiproc_vfork.c` 等当前在主仓库中显示为 untracked 或包含大量未跟踪结果；接手前应确认哪些需要纳入版本控制。早期文档曾提到的 `junction/fs/shaofs/OPTIMIZATION_REPORT.md` / `IOPS_BENCHMARK_REPORT.md` 当前目录下未发现；历史交接曾提到的 `plan2.md` 当前工作区未发现。
18. **FxMark patch 是 Junction 适配版，不是上游语义完整等价实现**：当前把 FxMark worker 从 process/fork 模型改成同进程 pthread 模型，只验证了 DRBL 跑通。涉及进程隔离、真实多进程扩展性或其他 FxMark workload 的结论需要单独验证。
19. **FxMark 多 worker smoke 不是多核扩展性结果**：2026-05-15 跑 FxMark 时的 `build/junction/caladan_test.config` 只有 `runtime_kthreads=1` / `runtime_spinning_kthreads=1`，所以 `--ncore 8` 并不表示 Junction/shaoFS 使用了 8 个 runtime kthreads。2026-05-19 当时 config 曾改为 `runtime_kthreads=10` / `runtime_spinning_kthreads=0` / `runtime_quantum_us=100`；2026-05-20 当前 config 又已改为 `runtime_kthreads=1` / `runtime_spinning_kthreads=1`。正式多核实验必须重新记录并验证当前 config。
20. **`sync()` 是全局 flush，不是 clean unmount**：`usys_sync()` 当前调用 `shaofs_sync_all()` 刷写脏状态，但不会调用 `journal_mark_clean()`，也不会清除 `runtime_info->spdk_uipi`。如果测试依赖 clean shutdown 语义，仍应让 `junction_run` 正常退出走 `final_flush()`。
21. **透明底层 read submit batching 实验已回退**：2026-05-17 的 pending-submit batching 在 128-job FIO 上没有提升，`delay_cmd_submit` 又导致初始化阶段 timeout。当前保留的是显式 `readv/preadv` batch read；不要把已回退的 `SHAOFS_STORAGE_READ_BATCH` 环境变量或 pending-list 设计当作现有功能。
22. **`writev/pwritev` 尚未接入 shaoFS direct 路径**：当前 `readv/preadv` 对 shaoFS direct fd 有专门 dispatch；`writev/pwritev/pwritev2` 仍调用 Junction `File::Writev()`，没有走 `my_write()` / `file_write_direct()`。如果 FIO 改用 writev/pwritev engine 或混合写场景，需要先实现并验证 shaoFS dispatch。
23. **Journal checkpoint 正确性约束不能破坏**：当前 `journal_commit_blocks()` 仍是同步 home-block checkpoint；但 `journal_commit_single_batched()` / `bc_flush_block_batched()` 已使用 async checkpoint lane。维护时必须保留 slot-busy 等待、checkpoint task 的 image copy、`bc_clean_block_if_unchanged()` 和 final/sync 中的 `journal_drain_checkpoint()`，保证同一 metadata block 不会被旧事务镜像乱序覆盖新事务镜像，也不能在 home checkpoint 完成前把 cache entry 误当作 clean。
24. **`fsync` dirty range 依赖所有元数据变更正确递增 `inode_dirty_seq`**：新增会影响 inode 盘上元数据的路径时必须调用 `mark_inode_metadata_dirty()`；否则 clean fsync 快路径可能误判 inode 不需要刷写。
25. **后台 data writeback 只应覆盖适合的 data path**：当前 large EOF/batch write path 会调用 `bc_mark_data_block_dirty()` 入队，小块标量写主要只调用 `bc_mark_block_dirty()`。不要为了“统一接口”把所有小写都强制入队；这会增加队列锁、后台 I/O 和读主导 Filebench 场景的干扰。
26. **FIO random-read 小数据集会误导 PM9A3 上限判断**：旧 `psync_128job_randread_sweep.fio` 的 128-job × 128MiB 规模只有约 16GiB，不适合证明全盘 4KB random read 硬件上限。后续 random-read IOPS 对比应优先使用 `psync_128job_randread_large.fio` 或明确记录每轮 `size/numjobs/range`。
27. **PM9A3 raw baseline 必须匹配 LBA 状态**：2026-06-01/02 已确认 discard/unwritten LBA 可测到约 1.17M IOPS，而已写数据 LBA 约 583K IOPS。ShaoFS/ext4 读文件数据时应和“已写数据 LBA”baseline 对比；不要拿 discard 后全盘 randread 的 1.17M 直接判断文件系统没有打满硬件。
28. **2026-06-01/02 只完成了 O_DIRECT 256-job 对比，buffered I/O 尚未重跑**：当前 `8.12` 表格不能代表 buffered read 场景，也不能代表 page cache/block cache 命中场景。
29. **当前目标盘现场状态不是文件系统可用状态**：最后一次裸盘复现实验后，`/dev/nvme2n1` 无文件系统签名并已被 SPDK 写满；下一轮 ShaoFS 或 ext4 实验必须显式重新格式化，不能直接复用当前盘。
30. **`DirIndex` 有小目录内存放大风险**：当前 `DirIndexChunk` 固定包含 512 个 `DirIndexNode`，在 x86_64 上约 148KB；一个小目录只要触发 `dir_ensure_index_locked()` 就会分配一整个 chunk。少量热点目录可接受，但大量小目录 create/delete 负载可能放大内存占用。
31. **`dentryCache` 与 `DirIndex` 的取舍尚未通过实验定论**：二者职责不同，`dentryCache` 是全局路径解析 LRU，`DirIndex` 是单目录内部索引并维护 free slot/live child；不能直接删除 `DirIndex`。是否弱化或删除 `dentryCache` 需要先做 path lookup、Filebench webserver/webproxy 和多目录压力测试。
32. **Junction 多进程验证不等于完整 `fork()` 支持**：2026-06-07 已验证单个 `junction_run` 容器内可通过 `vfork()+execve()` 和 fish/`posix_spawn()` 启动多个 Junction process；但这不能证明通用 Linux `fork()` 后 parent/child 在 exec 前并发执行的完整语义。Filebench/FxMark 的 pthread 降级仍应保留，除非目标 workload 另行完成 fork/exec/wait/IPC/信号验证。

### 9.2 优先待办任务

**Task 1: 完善 append 预分配与 extent 压力测试**
- 当前 `alloc_blocks()` 已用于普通文件 EOF append 预分配，但只覆盖 append 到 EOF 的常见路径；当前 2026-05-20 代码在文件至少 256KB 后触发，实际使用 64/128 blocks。
- 当前 simple extent tree 已把单文件 extent 上限扩展到 28567，但仍需把 `test_shaofs_many_extents.c` 和 `test_shaofs_mt_full_extents.c` 固化为 regression，覆盖随机写、稀疏写、跨 legacy/tree 转换、预分配失败回滚、unlink 回收和崩溃恢复后的 extent 校验。
- 根据正式 Filebench/fileserver 结果继续调优预分配阈值，确认额外预分配、dirty cache 和 tree metadata 不会在目标负载下引入明显 CPU/写放大。

**Task 2: 完成目录删除和 unlink 回归扩展**
- `my_unlink(path)` 当前已接入普通文件删除：nameiparent → dir_lookup → dir_delete_entry → ic_free_inode。
- 继续实现 `my_rmdir(path)`：拒绝非空目录，更新父目录/子目录 nlink，删除目录项并释放目录 inode/data blocks。
- 在 `core.cc:usys_rmdir` 和 `core.cc:usys_unlinkat(..., AT_REMOVEDIR)` 中添加 FSHAO dispatch。
- 扩展 `test_shaofs_unlink.c`：覆盖 open fd 后 unlink、删除后重建同名文件、错误码、目录 unlink 应返回 `EISDIR`、多线程 delete/create 混合。

**Task 3: 并发文件创建压力测试**
- 编写 `test_concurrent_create.c`：N 线程同时在同一目录创建不同文件
- 验证无 segfault、double-free、目录项丢失

**Task 4: FIO 正式基准回归**
- 用 `toggle_fio.sh apply` 确认 FIO 处于 `CONFIG_NO_SHM` 状态。
- 在重新 `mkfs` 后运行目标 FIO 命令，至少记录完整 stdout、退出码、IOKernel 日志和 `timeout` 是否触发。
- 对比 `FSHAO/` 与转义后的 `FSHAO\:/` 两种路径写法，确认哪一种应作为论文脚本标准写法。
- 对 4KB O_DIRECT random read，优先使用 `psync_128job_randread_large.fio` 这类大工作集 jobfile，并同时记录 core 数、Junction config、numjobs、每 job size、总 outstanding 和是否触发 O_DIRECT user-buffer DMA fast path。
- 对 near-full 256-job 对比，参考 `8.12` 的 `psync_256job_randread_full_direct.fio` / `ext4_psync_256job_randread_3500m_direct.fio`。每轮都应在数据准备后重新测 SPDK raw baseline，并说明 baseline 是 discard/unwritten 状态还是已写数据状态。
- buffered I/O 对比尚未完成。下一轮若补 buffered，需要分别说明 ShaoFS BlockCache、Linux page cache、drop_caches、工作集大小和 warm/cold cache 状态，不能和 O_DIRECT 表格混报。

**Task 4A: 固化 Caladan raw storage core/QD sweep**
- 使用 `lib/caladan/tests/run_storage_async_iops.sh` 跑 1/2/4/8 core 与 QD64/QD128/QD256 组合，`RANGE_MB=0`，保存每轮 `summary.txt`、`test.log`、`iokernel.log`。
- 同时用 `/home/syh/MyProj1/junction/lib/caladan/spdk/build/bin/spdk_nvme_perf` 跑相同 core mask/QD/op，记录完整命令和输出，避免混用另一个异常低性能的 SPDK perf 二进制。
- 把 raw storage 结果作为 ShaoFS/FIO 优化的下界诊断工具：如果 raw path 达上限而 ShaoFS/FIO 达不到，再分析文件系统路径；如果 raw path 自身受限，先检查 QD、工作集、设备绑定和 runtime config。
- 增加 LBA 状态维度：至少保留 `blkdiscard` 后、部分顺序写后已写 range、部分顺序写后未写 range、全盘顺序写后四种状态的 raw baseline。已有复现实验日志在 `/tmp/ssd_state_repro_20260601_163852`。

**Task 4B: uFS 对比尚未完成**
- `/home/syh/uFS/README.md` 已确认 uFS 是 SOSP'21 filesystem semi-microkernel，源码位于 `/home/syh/uFS`，但本轮尚未完成构建、运行和 FIO/uFS 对比。
- 下一位接手者需要先按 `/home/syh/uFS/README.md` 和其 artifact 文档确认依赖、SPDK/pinned memory 初始化、benchmark 入口和是否可使用当前 PM9A3 设备，再设计与 ShaoFS/ext4 对齐的 4KB random read/write 实验。
- 不要把 uFS 结果写入报告，除非已有完整 stdout/stderr、退出码、配置、设备绑定状态和重复实验记录。

**Task 5: 前缀语义统一**
- 当前 `fs.h` 的 `MYPREFIX` 是 `"FSHAO"`，历史文档曾写 `"FSHAO:"`。
- 如果恢复 `FSHAO:`，必须同时解决 FIO 未转义冒号分隔问题；建议先写一个最小 FIO 参数验证用例，再同步修改 `fs.h`、`core.cc`/`file.cc` 判断、测试程序、工具程序和 FIO 适配补丁。

**Task 6: 将 `IO_PREEMPT` 纳入正式论文 benchmark**
- 固化 ON/OFF 两套构建脚本，避免手动 CMake cache 状态污染结果。
- 保存完整 stdout、退出码、IOKernel 日志、Junction `DIRECTPATH` 状态和机器负载。
- 在不同 `busy_us`、kthread 数、CPU-bound uthread 数、I/O 深度下扫参数，找出机制收益边界。

**Task 7: 验证并优化 O_DIRECT user-buffer DMA**
- 用 `shaofs_direct_io_example.c` / `test_user_dma_direct.c` 或新的最小 C 测试程序显式分配 2MB 对齐 arena，并传入 4KB 对齐子区间，确认 `storage: enabled direct DMA into user buffers` 日志和 `NVMe SSD ↔ user buffer` 数据正确性。
- 确认 Filebench `directio=1` 是否满足当前 4KB 请求对齐 + 2MB 覆盖注册合约；若不满足，需要在 Filebench 适配层控制 buffer 对齐/arena 大小，或明确该 workload 不用于验证 user-buffer DMA。
- cached/direct 一致性当前使用 `bc_flush_block()` 和 `bc_invalidate_block()`，正确但可能增加 direct path CPU 开销。后续可考虑仅在 cache entry dirty 时 flush，或在 direct read 命中 clean cache 时直接从 cache 返回。

**Task 8: 评估 IOKernel completion 检查开销**
- 当前 `check_spdk_and_preempt()` 在 dataplane loop 中遍历 runtime/kthread。
- 对少量 Runtime 的学术实验可接受；若扩展到更多 Runtime，应评估 bitmap/event/coalescing，避免 IOKernel 忙等扫描成为瓶颈。

**Task 9: 重新设计真正的底层 NVMe submit batching**
- 2026-05-17 的简单 pending-submit batching 没有收益，因为不使用 SPDK delayed-submit 时并不能减少每个 `spdk_nvme_ns_cmd_read()` 的提交/doorbell 成本；只是在 Runtime 内多了一层队列和 flush 逻辑。
- 如果继续做，应围绕 SPDK qpair delayed-submit / doorbell kick 设计，而不是恢复简单 pending list。
- flush 条件至少包含：batch size 达到阈值、首个 pending 请求等待超过 `age_us`、当前 kthread 即将 idle/park、storage softirq 处理 completion 前、以及任何需要等待 completion 的路径进入 park 前。
- 必须先做小 smoke，确认 shaoFS mount/init 期间的同步读不会因 delayed-submit 死锁；此前启用 `delay_cmd_submit` 时 `junction_run` 在 shaoFS init 附近 timeout。
- 指标应同时记录 IOPS、p99 latency、平均 batch size、flush reason 分布和真实 doorbell/MMIO 次数；没有 doorbell 计数时，batch size 不能证明提交成本下降。

**Task 10: 完善 crash consistency 语义测试**
- 当前已有 SIGKILL dirty-mount 恢复测试，但还没有覆盖 torn transaction header、坏 checksum、目录块事务 replay、inode table 单块 replay 等更细粒度场景。
- 建议编写离线磁盘破坏工具或 Junction 内部测试 hook，构造 journal header/image/home block 的不同崩溃点。

**Task 11: 评估 journal 粒度和批量事务**
- 当前 `journal_commit_single_batched()` 已有自然并发 group commit，并会把同一 home block 的重复请求去重；同步 `journal_commit_single()` 仍保留给 repair、generic backend 和非 batched 调用点。
- 后续可把一个 syscall 的多个相关元数据块合并为更明确的小事务，减少不完整操作被 repair 清理的概率，并减少多次 header 写。
- 必须补充 slot exhaustion、checkpoint worker error、同一 metadata block 多事务乱序、recovery scan 多 committed slot 等回归测试。

**Task 12: 优化 dirty repair 的 mount 成本**
- 当前 repair 会扫描完整 inode table 和所有 group bitmap。
- 学术测试中只要避免非 clean shutdown，正常路径不受影响；如果要频繁 crash/recover 实验，需要记录 repair 耗时并考虑按需扫描或 checkpoint。

**Task 13: 维护 Filebench 正式 benchmark 脚本与结果记录**
- `junction/fs/mytest/scripts/run_filebench_compare.sh` 已经固化 `toggle_filebench.sh apply`、重新 mkfs、启动/清理 IOKernel、`timeout` 运行 shaoFS、调用 ext4 cgroup 脚本和结果收集流程。参数 `0/1/2` 分别表示只测 shaoFS、只测 ext4、两者都测。
- 脚本当前会记录 Filebench stdout/stderr、退出码、mkfs log、IOKernel log、ext4 driver log 和 cgroup stats。后续应补充更结构化的 CSV/JSON 汇总，并把 `SHAOFS_IO_PREEMPT` / `SHAOFS_CRASH_CONSISTENCY` 构建开关、Filebench patch 状态、Junction `DIRECTPATH` 状态自动写入结果目录。
- 明确论文中如何解释 Filebench `process` 被降级为 pthread 的限制。
- `fileserver.f`、`webserver.f`、`varmail.f`、`webproxy.f` 已纳入同一脚本体系。后续若继续使用 randomread，应明确它是否属于正式四项之外的补充测试。
- 正式论文实验仍应做多轮重复，固定 timeout/runtime/线程数/文件数/cgroup memory，并保留每轮原始日志。

**Task 14: 固化 ext4 对比脚本与参数**
- 当前主要 ext4 Filebench 路径是 repo 内 `junction/fs/mytest/scripts/run_ext4_filebench.sh`，由 `run_filebench_compare.sh` 调用；它负责 revert Filebench patch、reset ext4、drop cache、通过 `cg_run.sh` 运行指定 WML。
- `run_filebench_compare.sh` 的 ext4 memory 默认值为 fileserver 800MiB、webserver 1300MiB、varmail 500MiB、webproxy 1300MiB。这个选择是为了给 Filebench 自身留出可运行内存，同时尽量限制 ext4 可用 page cache；后续若调整，必须记录原因和探测过程。
- `/home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh`、`/home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh` 是历史 repo 外脚本，可作为参考，但当前四项正式流程应优先使用 repo 内脚本。
- 需要把 shaoFS WML 参数、ext4 WML 参数、cgroup CPU/memory、Junction config 的 core 数固定到同一实验记录中。
- 每次 ext4 测试前确认 `/home/syh/mkfs/reset_ext4.sh` 成功格式化并挂载目标盘，避免拿旧数据或 page cache 结果做对比。

**Task 15: 清理或纳入未跟踪工作区文件**
- 当前 benchmark patch、Filebench WML、`plan.md`、`junction_multiproc_report.md`、`junction/fs/mytest/junction_multiproc_vfork.c` 和 `junction/fs/mytest/scripts/results/` 下的实验结果大量处于 untracked 状态；历史交接曾提到 `plan2.md`，但当前工作区未发现该文件。
- 接手者应先决定哪些是正式资产，哪些只是临时实验输出，再统一加入版本控制或清理；不要盲目删除用户可能仍需要的实验文件。

**Task 16: 完整验证 inode 上限**
- 扩展 `test_many_inodes.c` 或新增测试，创建接近 `INODENUM=32768` 的 inode，记录成功数量和耗尽时错误码。
- 覆盖 clean shutdown 后 remount，再随机读取这些文件，确认 inode bitmap、inode table、dentry/path lookup 与 journal repair 不引入不一致。

**Task 17: 继续审计 FS base / TLS 入口**
- 当前 shaoFS `RuntimeFSBaseGuard` 已处理 guard 内 yield 的问题。
- 仍需检查 Junction 其他 runtime libc 调用点、lazy binding、signal trampoline、interrupt/syscall entry 等是否存在未 guard 或 guard 后可能 yield 的路径。
- 如果新增可 yield 的 runtime-FS 区域，应复用 `runtime_fsbase_depth`，而不是使用会长时间禁用抢占的 `RuntimeLibcGuard`。

**Task 18: 固化 FxMark 正式 benchmark 脚本**
- 当前只手动验证了 DRBL `--ncore 1/2/4/8` 能跑完；建议把 `toggle_fxmark.sh apply`、重新 mkfs、启动 IOKernel、`timeout` 运行 FxMark、清理 IOKernel 的流程写成脚本。
- 正式实验应同时记录 `caladan_test.config` 中的 `runtime_kthreads` / `runtime_spinning_kthreads`、`runtime_quantum_us`、shaoFS 构建开关、FxMark patch 状态、完整 stdout/stderr 和退出码。
- 如果要报告多核扩展性，必须先提供多 runtime kthread 配置，并确认 FxMark pthread worker 真正分布到多个 Caladan kthread/core 上。

**Task 19: 扩展 FxMark workload 兼容性验证**
- 当前 patch 中只有 DRBL 被改成 self-timed loop；其他 FxMark workload 仍可能依赖 `SIGALRM` 或遇到 Junction 不支持的 syscall。
- 后续可逐个验证常用 FxMark 类型，遇到失败时优先在 FxMark 适配层绕过不支持机制，不要直接修改 Junction，除非确认是 Junction bug。
- 需要注意 FxMark 当前 pthread 降级模型对原始 process-based 语义的影响，尤其是共享地址空间、共享全局变量和资源统计。

**Task 20: 为 fsync/journal 优化补齐正式回归**
- 把 `test_shaofs_varmail_bottleneck.c`、`test_shaofs_fsync_direct_verify.c`、`journal_recovery_prepare/check` 和 Filebench varmail 固化为一组 regression。
- 覆盖 clean repeated fsync、dirty append+fsync、direct read after buffered fsync、同步 checkpoint 后 clean shutdown、async checkpoint 未完成时 clean shutdown、强杀后 recovery。
- 2026-05-28 清理后已重新构建，并完整跑通 Filebench 四项 shaoFS/ext4 对比；下一步应补更多 targeted crash/checkpoint 回归和多轮 benchmark 重复。

**Task 21: 评估目录索引与 dentryCache 简化方案**
- 先写一个大量小目录 create/delete/lookup 压力测试，记录 `DirIndex` chunk 数、目录数量、RSS 和 lookup/add/delete 吞吐，确认 148KB/chunk 是否会成为目标场景瓶颈。
- 如果内存放大明显，优先考虑小目录不建完整索引、降低 `kDirIndexNodeChunkEntries`，或超过目录项阈值后再建 hash index；修改后必须覆盖 `dir_add_entry()` free slot 复用、`dir_delete_entry()`、`dir_is_empty()` 和 remount 后扫描行为。
- 评估删除或弱化 `dentryCache` 时，只能把 `namei()` miss/backend 路径改为直接依赖 `dir_lookup()`/`DirIndex`；不能用 `dentryCache` 替代 `DirIndex`，因为前者不保存目录项 offset/free slot/live child。该实验需要和当前 Filebench 四项 baseline 对比，确认 path lookup 性能没有显著下降。

**Task 22: 按需扩展 Junction 多进程语义验证**
- 当前 `junction_multiproc_vfork.c` 已覆盖 `vfork()+execv()`、`waitpid()`、worker 文件 I/O 和 fish 后台任务路线；如果后续 benchmark 要恢复原始多进程模型，需要继续验证通用 `fork()`、`execve()` 多目标程序、`wait4/waitid`、信号、文件描述符继承、pipe/stdout/stderr 和共享内存/IPC。
- 不要因为 2026-06-07 的验证通过就直接撤销 Filebench/FxMark 的 pthread worker 适配；二者当前依赖的是与 `vfork()+exec` 不同的执行语义。
- 若决定把本轮测试纳入正式回归，应将 `junction/fs/mytest/junction_multiproc_vfork.c`、`junction_multiproc_report.md` 和当前 `plan.md` 的取舍一起整理，避免长期保留未跟踪但关键的交接资产。

---

## 第十章：文件修改历史总览

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `plan.md` | Handover artifact | 2026-06-07 当前内容为 Junction single-container multi-process verification checklist；旧 ShaoFS plan 内容已被覆盖，2026-06-03 ShaoFS 重构阶段摘要见本文 `1.5` / `8.13` |
| `junction_multiproc_report.md` | Report | 2026-06-07 Junction 单容器多进程技术实践报告，记录 README 机制理解、`vfork()+execv()` / fish 复现命令、日志路径、host 侧证据和语义边界 |
| `junction/fs/mytest/junction_multiproc_vfork.c` | New test | 2026-06-07 双模式 C 测试：controller 显式 `vfork()` 后 child 立即 `execv()` 同一二进制的 `--worker` 模式；worker 写 `FSHAO` 文件并打印 PID/TID/PPID，parent `waitpid()` 并校验 worker 文件 |
| `shaofs/syscall.cc` | Refactor | 2026-06-03 plan2：拆分 `my_open()` / `my_mkdir()` / `my_fsync()` 的路径准备、inode 初始化、目录插入和 dirty-range fsync helper；目标是降低大函数认知负担，行为由 plan2 Phase6 correctness 和 Filebench final 验证 |
| `shaofs/file.cc` / `shaofs/file.h` | Refactor + correctness | 2026-06-03 plan2：引入 `CachedWriteOp`，拆出 cached existing-block/new-block/EOF extension helper 和 direct 单块写 helper；direct `O_APPEND` 由 `file_write_direct_append()` 在 inode 写锁内完成 EOF reservation |
| `shaofs/blockCache.cc` | Refactor | 2026-06-03 plan2：`bc_flush_blocks_contiguous()` 改用 `CachedFlushRun` 表达连续 flush run，并拆出 data/metadata flush 判断与提交 helper |
| `shaofs/journal.cc` | Refactor | 2026-06-03 plan2：repair/recovery 使用 `CFreeBuffer<T>` 管理 C buffer，并把 slot 分类、收集、replay、clear、dirty mount repair 拆成小 helper |
| `shaofs/extent.h/cc` | Refactor | 2026-06-03 plan2：补充 disk inode extent 遍历 helper，供 journal repair/recovery 复用，减少修复路径手写 extent 扫描 |
| `shaofs/dir.cc` | Design note | 2026-06-03 review：确认 `DirIndex` 是目录 inode 内部运行时索引，和全局 `dentryCache` 不等价；plan2 未删除该机制，但记录了小目录内存放大风险 |
| `shaofs/group.cc` | Bug fix + perf | 移除热路径 log_info；`alloc_block` 中 write_access 移到 kguard 之前 |
| `shaofs/file.cc` | Perf + feature | 拆分 file_write 锁范围；新增 truncate_inode + free_inode_data_blocks；新增 file_read/write_direct；当前 O_DIRECT 请求层要求 `buf`/`len`/`offset` 4KB 对齐，底层按覆盖 2MB 区间注册 |
| `shaofs/file.cc` | Feature | 新增 `file_readv_direct()`：shaoFS O_DIRECT `readv/preadv` 聚合多个整块 iovec，调用 `storage_read_aligned_batch()` 一次提交多个 read 并只 park 一次；dirty cache、sparse hole 或非整块场景 fallback 到 scalar direct read |
| `shaofs/file.cc` | Perf + correctness | 当前工作区新增 `file_write_scalar_locked()`、`file_write_batch_new_blocks_locked()`、`file_write_append()`、`file_write_eof_extension()`；buffered `O_APPEND` 在 inode 写锁内原子获取 EOF，regular-file EOF extension 可在 full-block/64KB+ 场景用 DSA batch copy 写入新 block cache entries |
| `shaofs/file.h` | Feature | 新增 file_read/write_direct, file_readv_direct, truncate_inode, file_write_append 声明 |
| `shaofs/inode.h` | Perf + refactor | MInode 新增 extent_hint、hint_lock 和 has_dirty_data_cache；dir_mtx 从 mutex_t 升级为 rwmutex_t；extent_hint try-lock 当前使用 `spin_try_lock_np()` |
| `shaofs/inode.cc` | Bug fix | `alloc_inum()` 按 `INODENUM=32768` 全范围环形扫描 inode bitmap，避免把 inode cache 容量 8192 误当作 inode 上限 |
| `shaofs/fs.h` | Feature | 新增 `LEGACY_MAX_EXTENT_NUM`、`ExtentTreeHeader`、`ExtentLeafRef`、`ExtentLeafHeader` 和 simple extent tree 容量常量；DInode 大小仍保持 256B |
| `shaofs/extent.h/cc` | Perf + feature | inode_bmap_locked 增加 hint fast path；保留 legacy direct/flat indirect 布局；超过 176 extents 后使用 simple extent tree，把单文件上限扩展到 28567 extents；普通文件 EOF append 路径加入 64/128 blocks 批量预分配和失败回滚；新增 tree-aware extent 遍历、metadata flush/free helper |
| `shaofs/inodeCache.cc` | Bug fix + feature | ic_free_inode 实现完整块释放；新增 ic_flush_inode |
| `shaofs/inodeCache.h` | Feature | 新增 ic_flush_inode 声明 |
| `shaofs/dir.h` | Refactor | 新增 dirent_is_empty()；移除未实现的 _locked 声明 |
| `shaofs/dir.cc` | Refactor + bug fix | 全面重写：DirReadGuard/DirWriteGuard + rwmutex；dir_foreach_locked；dirent_is_empty |
| `shaofs/syscall.h` | Feature | 新增 my_fstat, my_newfstatat, my_fsync；当前工作区 `my_write` 签名增加 `append` 参数 |
| `shaofs/syscall.cc` | Feature + perf | 实现 fstat/newfstatat/fsync；移除热路径日志；my_read/write 支持 direct 分派；当前工作区在 `append && !direct` 时调用 `file_write_append()` |
| `shaofs/blockCache.h` | Feature + fix | NVMeSSD 后端改用 DMA_read/write_block；新增 bc_flush_block/bc_flush_block_batched、bc_mark_data_block_dirty、writeback drain/stop API；BlockPool freelist 短临界区使用 `SpinGuardNP` |
| `shaofs/blockCache.cc` | Feature + perf | 新增 bc_flush_block；新增 bc_invalidate_block 用于 direct write 后失效旧 cache entry；CRASH_CONSISTENCY=1 时 metadata block 写回走 journal；新增固定环形队列 data writeback worker、contiguous-run batch write、dirty generation 校验和 `bc_clean_block_if_unchanged()` |
| `generic_cache/cache_entry.h` | Feature + correctness | CacheEntry 新增 `dirty_gen` 和 `writeback_queued`，支持后台 data writeback 与 async checkpoint 在并发覆盖时安全清 dirty |
| `generic_cache/cache.h` | Feature + concurrency | 新增 flush_entry(key)；shard metadata lock 使用 `spin_lock_np()`，backend I/O 保持在 shard lock 外 |
| `generic_cache/sharded_cache.h` | Feature | 转发 flush_entry |
| `junction/fs/file.cc` | Feature | usys_read/write/readv/pread64/preadv/pwrite64 传递或处理 O_DIRECT flag；shaoFS direct `readv/preadv` 分派到 `file_readv_direct()`；当前工作区中 `usys_write()` 把 buffered `O_APPEND` 语义作为 `append` 参数传给 `my_write()`，`pwrite64` 显式传 `append=false`；usys_fstat/newfstatat/fsync 添加 SHAOFS dispatch；newfstatat 使用 SHAOFS_REALPATH |
| `junction/fs/file.cc` | Feature | 新增 `usys_sync()`，调用 `shaofs_sync_all()` 全局刷写 shaoFS 脏状态 |
| `junction/fs/core.cc` | Feature + path fix | openat/mkdir 使用 SHAOFS_REALPATH，兼容 `FSHAO/path` 与 `FSHAO:/path` |
| `junction/CMakeLists.txt` | Build | 新增 `SHAOFS_IO_PREEMPT` option，开启时定义 `IO_PREEMPT=1` |
| `lib/caladan/runtime/softirq.c` | User fix | 恢复 timer soft interrupt 处理（修复 sleep/barrier）；storage softirq pending 时使用 runqueue head insertion |
| `fs/mytest/*.c` | New | 15+ 测试/工具/基准测试程序 |
| `junction/fs/shaofs/dsa.cc` | Perf + fallback | 当前工作区实现 direction-aware DSA policy/stats：read/write/internal copy 使用不同 batch threshold，`SHAOFS_DSA_*` 环境变量可调；使用 DML 硬件路径、Caladan `runtime_async_park` 和 per-thread tcache，硬件/提交失败或策略不满足时回退 CPU memcpy |
| `junction/fs/shaofs/dsa.h` | Feature | 暴露 `dsa_init`、`dsa_copy`、`dsa_copy_ex`、`dsa_copyv`、`dsa_copyv_ex`、`dsa_dump_stats`、`ShaofsDsaOptions` 和 `ShaofsDsaCopyKind`；`dsa_batch_task_num=32` |
| `junction/fs/CMakeLists.txt` | Build | 当前查找静态 `libdml.a` 和 `dml/dml.h`，找不到会 FATAL |
| `junction/fs/CMakeLists.txt` | Build | 新增 `SHAOFS_CRASH_CONSISTENCY` option，默认 ON，并把 `shaofs/journal.cc` 纳入 fs library |
| `lib/CMakeLists.txt` | Build fix | Caladan `shared.mk` 查询改用 `make --no-print-directory` 并 strip trailing whitespace，避免 nested make 输出污染 linker flags |
| `shaofs/fs.h` | Crash consistency | SuperBlock 新增 `journal_blockstart` / `journal_blocknum`；新增 `CRASH_CONSISTENCY` 和 `DEFAULT_JOURNAL_BLOCKS` |
| `shaofs/fs.cc` | Crash consistency + perf | mount 时执行 `journal_init()` / `journal_recover()` / `journal_mark_dirty()`，扫描目录 extents 建立 metadata map，并启动 block cache 后台 writeback |
| `shaofs/journal.h` | New | journal API 和 `CRASH_CONSISTENCY=0` no-op fallback |
| `shaofs/journal.cc` | New + perf | metadata-only redo journal、dirty mount marker、transaction replay、dirty repair；当前包含同步 commit API、batched metadata group commit、async home-block checkpoint worker 和固定 metadata range 表 |
| `shaofs/file.cc` | Crash consistency + perf | final_flush 通过 journal 写 imap，clean shutdown 清 dirty marker；目录块分配后登记为 metadata block；`flush_all_dirty_state()` 会 drain/stop data writeback 并等待 journal checkpoint |
| `shaofs/file.cc` / `shaofs/file.h` | Feature | 新增 `shaofs_sync_all()`；与 `final_flush()` 共用 `flush_all_dirty_state()`，但 runtime `sync()` 不清 dirty marker；`sync` drain writeback/checkpoint，`final_flush` stop writeback 后 clean shutdown |
| `shaofs/file.cc` / `shaofs/file.h` / `junction/fs/file.cc` | Cleanup | 2026-06-02 清理：移除此前用于探索的 `SHAOFS_IOC_ASYNC_RANDREAD` dispatch、`shaofs_direct_randread_async_bench()` 和 `SHAOFS_DIRECT_READ_STATS` direct-read 统计，避免把内部 benchmark/诊断计数留在最终热路径；未跟踪测试源 `shaofs_async_randread_iops.c` 仍保留为参考 |
| `junction/fs/mytest/shaofs_prepare_large_files.c` | New benchmark tool | 顺序准备 256 个 `FSHAO:/fio128_large.<id>` 大文件；2026-06-01 用于 near-full FIO random-read 数据集 |
| `junction/fs/mytest/shaofs_async_randread_iops.c` | Historical benchmark tool | 曾通过 ioctl 触发 shaoFS 内部 async randread benchmark；2026-06-02 已移除核心 ioctl dispatch 和实现，当前该未跟踪源文件仅作为历史参考，不能直接运行代表当前功能 |
| `junction/fs/mytest/benchmark/fio_test/psync_256job_randread_full_direct.fio` | Benchmark config | ShaoFS 256-job near-full 4KB O_DIRECT random read 配置，`psync`/`iodepth=1`/`size=3575M` |
| `junction/fs/mytest/benchmark/fio_test/ext4_prepare_256x3500m.fio` | Benchmark config | ext4 256-file 数据准备配置，`size=3500M`，用于避免 3575MiB/file 在 ext4 上 ENOSPC |
| `junction/fs/mytest/benchmark/fio_test/ext4_psync_256job_randread_3500m_direct.fio` | Benchmark config | ext4 256-job 4KB O_DIRECT random read 配置，`psync`/`iodepth=1`/`size=3500M` |
| `shaofs/group.cc` | Crash consistency + concurrency | GDT sync 改为 `journal_write_metadata()`；group bitmap/free counter 短临界区使用 `SpinGuardNP` |
| `/home/syh/mkfs/fs.h` | Crash consistency | mkfs 侧 SuperBlock 同步新增 journal 字段和 `DEFAULT_JOURNAL_BLOCKS` |
| `/home/syh/mkfs/mkfs.c` | Crash consistency | mkfs 在盘尾预留并清空 journal 区，data group 只使用 journal 前空间 |
| `junction/fs/mytest/journal_layout_probe.c` | New test | 验证 journal superblock 布局可 mount |
| `junction/fs/mytest/journal_recovery_prepare.c` | New test | 构造崩溃前文件/目录状态，`--crash` 配合 SIGKILL |
| `junction/fs/mytest/journal_recovery_check.c` | New test | 验证 dirty mount repair 后目录、文件大小和数据内容 |
| `junction/fs/mytest/benchmark/fio/filesetup.c` | FIO adapter | 补丁后识别 `FSHAO/`、`FSHAO:/`，并容忍 shaoFS 上 `ftruncate` 不支持 |
| `junction/fs/mytest/benchmark/fio/helper_thread.c` | FIO adapter | 补丁后 `timerfd_create/settime` 失败不再 assert，回退 select timeout |
| `junction/fs/mytest/benchmark/fio/memory.c` | FIO adapter | 补丁后 `direct=1` 且 malloc 内存模式下使用 2MB 对齐、按 2MB 向上取整的 buffer，满足 shaoFS 底层 2MB 注册要求 |
| `junction/fs/mytest/benchmark/patch/fio_changes.patch` | Handover artifact | 保存 FIO 适配源码补丁 |
| `junction/fs/mytest/benchmark/patch/toggle_fio.sh` | Tooling | 一键 apply/revert FIO 补丁，并自动重新 configure/make |
| `junction/fs/mytest/benchmark/filebench/aslr.c` | Filebench adapter | 补丁后不再调用 `personality()` 禁用 ASLR，要求宿主环境全局关闭 ASLR |
| `junction/fs/mytest/benchmark/filebench/ipc.c` | Filebench adapter | 补丁后可在 configure 禁用 SysV semaphore 的状态下运行；修复字符串复制 NUL 结尾问题 |
| `junction/fs/mytest/benchmark/filebench/procflow.c` | Filebench adapter | 补丁后用 pthread 在进程内运行 procflow monitor，绕过 Junction 不支持 fork/exec/wait worker 进程的问题 |
| `junction/fs/mytest/benchmark/filebench/misc.c` | Filebench adapter | `filebench_log()` 大栈缓冲改为全局互斥缓冲并使用 `vsnprintf()` |
| `junction/fs/mytest/benchmark/filebench/fileset.c` | Filebench adapter | 动态分配 mkdir 路径栈，避免 512KB uthread 栈被大数组压垮 |
| `junction/fs/mytest/benchmark/filebench/fb_localfs.c` | Filebench adapter | 跳过对 `FSHAO:/` / `FSHAO/` 的 `system("rm -rf ...")` 清理 |
| `junction/fs/mytest/benchmark/filebench/fb_cvar.c` | Filebench adapter | cvar 目录不可用时降级为 verbose log，并从可执行文件 build tree 的 `cvars/.libs` fallback 加载 cvar 插件 |
| `junction/fs/mytest/benchmark/filebench/flag.h` | Filebench adapter | `wait_flag()` busy-wait 中调用 `sched_yield()`，降低进程内 pthread 模型下的空转 |
| `junction/fs/mytest/benchmark/patch/filebench_changes.patch` | Handover artifact | 保存 Filebench 适配源码补丁 |
| `junction/fs/mytest/benchmark/patch/toggle_filebench.sh` | Tooling | 一键 apply/revert Filebench 补丁，并自动重新 configure/make |
| `junction/fs/mytest/benchmark/patch/example.f` | Benchmark config | 当前用于 Junction/shaoFS 的 Filebench 示例 workload |
| `junction/fs/mytest/benchmark/fxmark/Makefile` | FxMark adapter | 补丁后增加 `-pthread`，支持 pthread worker 模型 |
| `junction/fs/mytest/benchmark/fxmark/src/bench.c` | FxMark adapter | 补丁后用 `pthread_create()` 代替 `fork()` 创建 worker；屏障等待使用 `sched_yield()` |
| `junction/fs/mytest/benchmark/fxmark/src/DRBL.c` | FxMark adapter | 补丁后 DRBL worker 自己按 wall-clock deadline 结束，避免依赖 `SIGALRM` 及时投递 |
| `junction/fs/mytest/benchmark/fxmark/src/util.c` | FxMark adapter | 补丁后 `mkdir_p()` 使用进程内递归 `mkdir()`，并兼容 `FSHAO` / `FSHAO:` 根别名 |
| `junction/fs/mytest/benchmark/patch/fxmark_changes.patch` | Handover artifact | 保存 FxMark Junction 适配源码补丁 |
| `junction/fs/mytest/benchmark/patch/toggle_fxmark.sh` | Tooling | 一键 apply/revert FxMark 补丁，并自动 `make -j $(nproc)` |
| `junction/fs/mytest/test_sync_syscall.c` | New test | 最小 `sync()` syscall smoke test，写入 shaoFS 文件后调用 `syscall(SYS_sync)` 并检查返回值 |
| `junction/fs/mytest/benchmark/filebench_wml/shaofs_randomread*.f` | Benchmark config | 从 Filebench `workloads/randomread.f` 派生的 shaoFS randomread workload，当前为 untracked 工作区文件 |
| `junction/fs/mytest/benchmark/filebench_wml/fileserver.f` | Benchmark config | 当前用于 shaoFS 的 Filebench fileserver workload：`FSHAO:`、10000 files、50 threads、60s runtime；2026-05-13 的 40 files smoke 对比是历史状态 |
| `junction/fs/mytest/benchmark/filebench_wml/webserver.f` | Benchmark config | 当前用于 shaoFS 的 Filebench webserver workload：`FSHAO:`、10000 files、100 threads、60s runtime；2026-05-13 的 1000 files 结果是历史状态 |
| `junction/fs/mytest/benchmark/filebench_wml/varmail.f` | Benchmark config | 当前用于 shaoFS 的 Filebench varmail workload：`FSHAO:`、5000 files、16 threads、60s runtime |
| `junction/fs/mytest/benchmark/filebench_wml/webproxy.f` | Benchmark config | 当前用于 shaoFS 的 Filebench webproxy workload：`FSHAO:`、10000 files、100 threads、60s runtime |
| `junction/fs/mytest/scripts/cg_run.sh` | Benchmark tooling | 通用 cgroup v2 runner，配置 cpuset/memory，运行目标命令并收集 cpu/memory stats |
| `junction/fs/mytest/scripts/run_ext4_fio.sh` | Benchmark tooling | ext4 FIO 主脚本：revert FIO、reset ext4、drop cache、通过 `cg_run.sh` 运行 jobfile并保存结果 |
| `junction/fs/mytest/scripts/run_ext4_filebench.sh` | Benchmark tooling | ext4 Filebench 主脚本：revert 原生 Filebench、reset ext4、drop cache、通过 `cg_run.sh` 跑指定 WML 并保存 log/cgroup stats |
| `junction/fs/mytest/scripts/run_filebench_compare.sh` | Benchmark tooling | ShaoFS/ext4 四项 Filebench 统一脚本；支持 mode `0/1/2`，自动处理 patch、mkfs、IOKernel、timeout、ext4 cgroup memory 和结果汇总 |
| `junction/fs/mytest/benchmark/fio_test/psync_128job_randread_sweep.fio` | Benchmark config | shaoFS 128-job 4KB O_DIRECT psync random read 配置，不启用 `group_reporting` |
| `junction/fs/mytest/benchmark/fio_test/psync_128job_randread_large.fio` | Benchmark config | shaoFS 128-job × 4GiB 大工作集 4KB O_DIRECT psync random read 配置，用于避免小随机范围低估 PM9A3 randread IOPS |
| `junction/fs/mytest/scripts/fio_test/psync_128job_randread.fio` | Benchmark config | ext4 对应 128-job 4KB O_DIRECT psync random read 配置 |
| `junction/fs/mytest/scripts/fio_test/psync_128job_randread_large.fio` | Benchmark config | ext4 对应 128-job × 4GiB 大工作集 psync random read 配置 |
| `/home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh` | Benchmark tooling | repo 外部 ext4 randomread 对比脚本，包含 ext4 reset、cgroup v2 CPU/memory 限制和 Filebench 运行 |
| `/home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh` | Benchmark tooling | repo 外部 ext4 fileserver 对比脚本，包含 ext4 reset、临时 WML `$dir` 替换、cgroup v2 CPU/memory 限制、log/CSV 输出 |
| `lib/caladan/inc/base/syscall.h` / `lib/caladan/base/syscall.S` | User DMA support | Caladan wrapper 当前包含 `syscall_mlock()`，供 `storage_prepare_user_dma()` pin 用户页 |
| `junction/syscall/seccomp.cc` | User DMA support | seccomp allowlist 当前包含 Caladan `mlock`，并按 request 放行 VFIO DMA map/unmap ioctl |
| `lib/caladan/runtime/storage.c` | User DMA support + concurrency | 新增 user DMA registration cache、`storage_prepare_user_dma()`、`storage_read_aligned()`、`storage_write_user_dma()`；当前按用户 4KB 子区间计算覆盖 2MB 注册范围，`user_dma_lock` 使用 `spin_lock_np()` |
| `lib/caladan/runtime/storage.c` | Explicit batch read | 新增 `storage_read_aligned_batch()`：校验/注册每个 user DMA buffer，在同一 qpair 上提交多个 `spdk_nvme_ns_cmd_read()`，completion 计数归零后唤醒等待 uthread；当前不是透明底层 doorbell batching |
| `lib/caladan/runtime/storage.c` | Raw benchmark support | 新增 `storage_async_read()` / `storage_async_write()` / `storage_async_poll()`，供 fixed-QD raw benchmark 维持可控 outstanding；同时加入 `SAMSUNG MZQL2960HCJR` known device 条目 |
| `lib/caladan/inc/runtime/storage.h` | User DMA support + batch read | 声明 user-buffer DMA 相关 storage API、`struct storage_batch_read` 和 `storage_read_aligned_batch()`；注释说明用户传入 4KB 对齐子区间，底层按覆盖的 2MB 区间注册 |
| `lib/caladan/inc/runtime/storage.h` | Raw benchmark support | 声明 `struct storage_async_req` 和 async storage API；临时 `storage_read2/storage_write2` 已删除 |
| `lib/caladan/tests/test_storage_async_iops.c` | New benchmark | Caladan raw storage fixed-QD 4KB read/write benchmark，可扫 pollers、QD、op、pattern、range、base LBA、request size 和 completion batch |
| `lib/caladan/tests/run_storage_async_iops.sh` | Benchmark tooling | 自动启动/清理 iokernel 并运行 `test_storage_async_iops`，保存 summary/test/iokernel log；默认 `RANGE_MB=0` 使用全 namespace |
| `lib/caladan/tests/storage_{1c,2c,4c,8c}_q0.config` | Benchmark config | raw storage core-sweep runtime config，`runtime_quantum_us=0`、`enable_storage=1`、`storage_quota_enabled=false` |
| `lib/caladan/tests/.gitignore` | Build hygiene | 忽略新增测试二进制 `test_storage_async_iops` |
| `junction/fs/file.h` | User DMA support | `File` 内嵌 `DirectReadHint`，供 shaoFS O_DIRECT read fast path 使用 |
| `junction/fs/core.cc` | User DMA support | shaoFS O_DIRECT open 时调用 `file_prepare_direct_read_hint()` |
| `junction/fs/file.cc` | User DMA support | shaoFS O_DIRECT read/pread 优先使用 `file_read_direct_hint()`；write 时使 hint invalid |
| `lib/caladan/iokernel/main.c` | IO_PREEMPT | dataplane 中启用 `check_spdk_and_preempt()`，检查 SPDK completion 并批量发送 yield/UIPI |
| `lib/caladan/iokernel/sched.c` | IO_PREEMPT bug fix | `sched_yield_on_core()` 改读 live `q_ptrs->rcu_gen`，修复持续 I/O 场景下 yield 去重错误 |
| `lib/caladan/iokernel/ksched.h` | Perf | 移除发送 UIPI 热路径日志 |
| `lib/caladan/runtime/preempt.c` | Perf | 移除 preempt 热路径日志 |
| `lib/caladan/runtime/storage.c` | IO_PREEMPT | `seq_complete()` / `vectorIO_complete()` 在 `spdk_uipi` 开启时用 `thread_ready_head()` 唤醒 I/O uthread |
| `junction/kernel/signal.cc` | IO_PREEMPT | UINTR 判断路径包含 storage completion pending 检查，并移除热路径日志 |
| `junction/fs/mytest/shaofs_preempt_latency.c` | New test | CPU-bound 干扰下的单次 I/O 延迟测试 |
| `junction/fs/mytest/shaofs_preempt_iops.c` | New test | CPU-bound 干扰下的连续 O_DIRECT read IOPS 测试 |
| `junction/fs/mytest/shaofs_storage_st.config` | Test config | 单 kthread storage runtime 配置，用于 IO_PREEMPT 实验 |
| `lib/caladan/inc/runtime/thread.h` | FS base fix | `thread_t` 新增 `runtime_fsbase_depth`，表示 uthread 处于 runtime FS base 区域的嵌套深度 |
| `lib/caladan/runtime/sched.c` | FS base fix | 调度器用 `thread_save_fsbase()` / `thread_fsbase_to_run()` 区分用户 FS base 与 runtime FS base，修复 shaoFS guard 内 park 污染 TLS |
| `junction/fs/shaofs/utili.h` | FS base fix | `RuntimeFSBaseGuard` 更新 `runtime_fsbase_depth`，只在切换窗口短暂禁用抢占，guard 内允许 yield |
| `junction/fs/mytest/test_many_inodes.c` | New test | 验证创建大量 inode 能越过 inode cache 容量 |
| `junction/fs/mytest/test_many_inodes_read_threads.c` | New test | 大量小文件创建后多 pthread 读整文件并校验数据 |
| `junction/fs/mytest/shaofs_direct_io_example.c` | New example | 最小 shaoFS O_DIRECT 示例：2MB arena 内 4KB 子区间 pwrite/pread |
| `junction/fs/mytest/test_user_dma_direct.c` | New test | 验证 2MB arena、4KB 子区间和非 4KB 对齐拒绝等 Direct user-buffer DMA 合约 |
| `junction/fs/mytest/batch_direct_read_bench.c` | New benchmark | 对比 O_DIRECT scalar `pread()` 与 batch `preadv()`，用于验证 `file_readv_direct()` / `storage_read_aligned_batch()` |

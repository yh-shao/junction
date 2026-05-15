# Project Handover / ShaOFS 全局项目交接与 AI 上下文恢复文档

> **文档版本**: v4.5 | **最后更新**: 2026-05-15
> **目的**: 使任何 AI Code Agent 读取本文档后，能瞬间加载全部项目上下文，无缝继续开发。

> **2026-05-06 补充说明**: 本文档保留了 2026-04-10 之前关于 ShaOFS 架构、测试和优化的历史沉淀。本次交接修正了与当前代码明显不一致的事实，并追加了 FIO-on-Junction 适配、补丁管理脚本和当前验证状态。历史性能测试结果可能不可靠，已从本文档移除；正式性能数据应以重新跑出的 benchmark 原始输出为准。

> **2026-05-09 补充说明**: 本次交接追加了 ShaOFS 的 I/O completion driven preemption（`IO_PREEMPT`）机制、相关 Caladan/IOKernel 改动、针对性 benchmark 和最新验证结果。当前 `build/CMakeCache.txt` 中 `SHAOFS_IO_PREEMPT=ON`，但 CMake option 默认值仍为 OFF，后续实验应在报告中明确构建开关状态。

> **2026-05-09 晚些时候补充说明**: 本次交接追加了 ShaOFS 的简单 crash consistency 支持。当前实现是 **metadata-only redo journal + dirty mount repair**：mkfs 在盘尾预留 journal 区，ShaOFS 在 `CRASH_CONSISTENCY=1` 时对元数据块写入做 redo logging，并在异常退出后的下一次 mount 中 replay/repair。当前 `build/CMakeCache.txt` 中 `SHAOFS_CRASH_CONSISTENCY=ON`，`junction/fs/CMakeLists.txt` 的默认值也是 ON。

> **2026-05-10 补充说明**: 本次交接追加了 Filebench-on-Junction 适配、`alloc_inum()` 越过 8192 inode 的修复，以及 x86 FS base 保存/恢复策略。当前 Filebench 适配仅修改 Filebench 源码并通过 patch 管理，不修改 Junction；它把 Filebench procflow 从 fork/exec/wait 模型降级为进程内 pthread 模型，并绕过 Junction 当前不支持或不稳定的 `personality()`、SysV semaphore、部分清理命令和日志栈缓冲路径。当前 `alloc_inum()` 已按 `INODENUM=32768` 全范围扫描 inode bitmap；本轮验证越过了 8192 inode，但尚未完整验证 32768 inode 耗尽边界。

> **2026-05-12 补充说明**: 本次交接修正了 O_DIRECT 路径的描述。当前 ShaOFS 已实现严格约束下的 user-buffer DMA：用户 buffer 必须 2MB 对齐、长度必须是 2MB 的倍数，文件 offset 必须 4KB 对齐；不满足时直接返回错误，不再回退到 bounce buffer。`storage_prepare_user_dma()` 会 `mlock()` 用户页、调用 `spdk_mem_register()` 并用 `spdk_vtophys()` 验证 IOVA，随后 `storage_read_aligned()` / `storage_write_user_dma()` 直接把用户 buffer 作为 SPDK NVMe payload。为支持这条路径，当前 seccomp 放行了 Caladan `mlock` wrapper 以及 VFIO DMA map/unmap ioctl request。另补充了 Filebench `randomread.f` 派生 workload、ext4 cgroup 对比脚本和 Caladan/Junction syscall 包装/拦截机制。

> **2026-05-13 补充说明**: 本次交接追加了 Filebench `fileserver.f` / `webserver.f` 在 Junction/ShaOFS 上的当前验证状态、ext4 `fileserver.f` cgroup 对比脚本和最新 smoke 对比数据。为跑通 `fileserver.f` 的 append-heavy 模式，`junction/fs/shaofs/extent.cc` 已在普通文件 EOF append 路径加入批量预分配：按文件大小选择 16/64/128 blocks，通过 `alloc_blocks()` 获取物理块，合并成 extent 后一次写回 inode extent 元数据；预分配块会立即在 Block Cache 中清零并标脏，失败路径会 invalidate cache entry 并释放物理块。当前 `fileserver.f` 是一个缩小版 smoke workload（40 files、1 thread、2s runtime），ShaOFS 结果约 `989k ops/s`，ext4 同资源 cgroup 结果约 `627k ops/s`。`webserver.f` 已改为 `set $dir=FSHAO:` 并能完整跑完 60s，ShaOFS 结果约 `680k ops/s`、`3412.3MB/s`。这些是本轮功能验证/初步对比数据，不是最终论文 benchmark；正式实验仍需重新固定构建开关、WML、cgroup/Junction config 并保存完整原始输出。

> **2026-05-15 补充说明**: 本次交接追加了 Junction `sync()` syscall 和 FxMark-on-Junction 适配状态。`sync()` 已加入 `usys.txt` / `usys.h` / `junction/fs/file.cc`，当前语义是调用 `shaofs_sync_all()` 刷写 ShaOFS 的 imap、GDT、inode cache 和 block cache；它不会像 `final_flush()` 那样清除 crash-consistency dirty marker，也不会 disable `IO_PREEMPT`。为避免运行时 `sync()` 与并发 cache 操作死锁，`generic_cache/cache.h::flush_all()` 当前先在 shard lock 下收集 dirty entry handle，再释放 shard lock 后逐个刷写。FxMark 源码位于 `junction/fs/mytest/benchmark/fxmark`，适配通过 `junction/fs/mytest/benchmark/patch/fxmark_changes.patch` 和 `toggle_fxmark.sh` 管理；当前 patch 把 FxMark worker 从 `fork()` 改为 `pthread_create()`，把启动/结束屏障的纯 busy-wait 改为 `sched_yield()`，把 `mkdir -p` 改为进程内递归 `mkdir()`，并让 DRBL worker 自己按 wall-clock 控制 duration。已验证 DRBL `--ncore 2/4/8` 能在 Junction/ShaOFS 上跑完，但当前 `build/junction/caladan_test.config` 仍是 `runtime_kthreads=1`、`runtime_spinning_kthreads=1`、`runtime_quantum_us=0`，这些结果只能说明多 worker 适配已跑通，不是多核扩展性结论。

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
| **Crash consistency** | ShaOFS 可选开启 `CRASH_CONSISTENCY`：metadata-only redo journal，journal 区位于盘尾，异常退出后执行 committed transaction replay 和 dirty mount repair |
| **硬件加速** | Intel DSA/DML（运行时硬件路径可选，当前代码通过 `dsa_init()` 初始化；硬件不可用时回退到 CPU memcpy。构建期目前要求能找到静态 `libdml.a` 和 `dml/dml.h`） |
| **Direct user-buffer DMA** | O_DIRECT 路径在严格对齐约束下可直接使用用户 buffer 作为 SPDK NVMe payload，避免 SPDK bounce buffer 与 user buffer 之间的 memcpy |
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
    │   │                ├─ 校验 user buffer/offset/length 是否满足 DMA 合约
    │   │                ├─ storage_prepare_user_dma()    → mlock + spdk_mem_register + vtophys 验证
    │   │                ├─ inode_bmap_locked(allocate=true) → 同上
    │   │                ├─ storage_write_user_dma()      → 绕过 Block Cache，user buffer 直接作为 SPDK payload
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
Block N+1...J-1:  Data Groups (repeating: 1 bitmap block + 32768 data blocks)
Block J...end:    Journal Area (default 4096 blocks, 16MB, placed at disk tail)
                  J = sb.journal_blockstart
```

mkfs 侧实现位于 `/home/syh/mkfs/fs.h` 和 `/home/syh/mkfs/mkfs.c`。当前 `DEFAULT_JOURNAL_BLOCKS=4096`，`build_superblock()` 会把 `journal_blockstart` 设置为 `total_blocknum - journal_blocknum`，再只用 journal 之前的空间计算 data group 数。`init_data_groups()` 会清空 journal 区。

ShaOFS journal 区内部约定：

```
sb.journal_blockstart:                     transaction header
sb.journal_blockstart + 1:                 transaction entry table
sb.journal_blockstart + 2 ...:             logged metadata block images
sb.journal_blockstart + journal_blocknum-1 mount dirty marker
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
│                                        CRASH_CONSISTENCY=1 时 metadata block 写回走 journal_commit_single()
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
├── journal.h/cc                       ← metadata-only redo journal + dirty mount repair
│                                        journal_init/recover/mark_dirty/mark_clean,
│                                        journal_write_metadata, journal_commit_single,
│                                        journal_build_metadata_map
├── OPTIMIZATION_REPORT.md             ← 优化报告
└── IOPS_BENCHMARK_REPORT.md           ← IOPS 测试报告
```

### 3.2 VFS 集成层（Junction 侧）

| 文件 | 职责 |
|------|------|
| **`junction/fs/file.cc`** | syscall dispatch: `usys_read/write/pread64/pwrite64/fstat/fsync/lseek` 中检查 `SHAOFS` 模式并转发到 `my_*` 函数 |
| **`junction/fs/core.cc`** | `usys_openat` 和 `usys_mkdir` 中用 `SHAOFS_REALPATH()` 识别 `FSHAO/path`、`FSHAO:/path` 并转发；O_DIRECT 打开 ShaOFS 文件时构建 `DirectReadHint` |
| **`junction/fs/file.h`** | `kFlagDirect=O_DIRECT`, `kFlagTruncate=O_TRUNC`, `FromFlags()`, `File` 类定义；当前 `File` 内嵌 ShaOFS `DirectReadHint` |
| **`junction/syscall/seccomp.cc`** | 安装 seccomp BPF；当前 allowlist 包含 Caladan `mlock` wrapper，并按 ioctl request 放行 `VFIO_IOMMU_MAP_DMA` / `VFIO_IOMMU_UNMAP_DMA`，用于 user-buffer DMA 注册 |
| **`lib/caladan/inc/base/syscall.h` / `lib/caladan/base/syscall.S`** | Caladan 受控 Linux syscall wrapper；syscall 指令位于 `[base_syscall_start, base_syscall_end)`，供 Junction seccomp 按 IP 范围放行 |
| **`lib/caladan/runtime/storage.c`** | SPDK submit/completion、storage softirq、user-buffer DMA 注册与 `storage_read_aligned()` / `storage_write_user_dma()` |
| **`junction/kernel/signal.cc`** | Junction UINTR 入口；`InterruptNeeded()` 当前同时检查 preempt cede/yield 和 `storage_available_completions(k)` |
| **`lib/caladan/inc/runtime/thread.h`** | `thread_t` 定义；当前新增 `runtime_fsbase_depth`，用于区分用户 FS base 与 runtime FS base 区域 |
| **`lib/caladan/runtime/sched.c`** | uthread 调度与 FS base 保存/恢复；`thread_save_fsbase()` / `thread_fsbase_to_run()` 避免 ShaOFS guard 内 park 时污染用户 TLS |
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
| `test_direct_io.c` | 单元测试 | 35 项 O_DIRECT 检查（整块/部分块/跨模式一致性） |
| `test_many_inodes.c` | 回归测试 | 顺序创建大量小文件，用于验证 inode bitmap 分配能越过 inode cache 容量 |
| `test_many_inodes_read_threads.c` | 回归测试 | 创建大量 16KB 文件后用 3 个 pthread 反复整文件读取并校验内容，用于覆盖 Filebench 类似读负载 |
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
| `journal_layout_probe.c` | Crash consistency 测试 | 打开 ShaOFS 根路径，确认带 journal 字段的新 superblock 可以正常 mount |
| `journal_recovery_prepare.c` | Crash consistency 测试 | 创建目录和文件，`--crash` 模式下写入/flush 后循环等待，供外部 `timeout -s KILL` 模拟崩溃 |
| `journal_recovery_check.c` | Crash consistency 测试 | 下一次 mount 后检查崩溃前创建的目录、文件大小和数据内容是否恢复一致 |

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
| `junction/fs/mytest/benchmark/patch/example.f` | Filebench whole-file read 示例 workload |
| `junction/fs/mytest/benchmark/filebench_wml/shaofs_randomread*.f` | 从 Filebench `workloads/randomread.f` 派生的 ShaOFS randomread workload；当前这些文件在工作区中是 untracked |
| `junction/fs/mytest/benchmark/filebench_wml/fileserver.f` | 当前用于 ShaOFS 的 Filebench fileserver smoke workload：`FSHAO:`、40 files、1 thread、2s runtime；已在 Junction/ShaOFS 上跑通 |
| `junction/fs/mytest/benchmark/filebench_wml/webserver.f` | 当前用于 ShaOFS 的 Filebench webserver workload：`FSHAO:`、1000 files、100 threads、60s runtime；已在 Junction/ShaOFS 上跑通 |
| `/home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh` | repo 外部 ext4 对比脚本；会 reset ext4、设置 cgroup v2 CPU/内存限制并运行 Filebench randomread |
| `/home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh` | repo 外部 ext4 `fileserver.f` 对比脚本；会 reset ext4、生成只替换 `$dir` 的临时 WML、设置 cgroup v2 CPU/内存限制并运行 Filebench |
| `/home/syh/fs_test/results/` | repo 外部 ext4 benchmark 输出目录 |
| `junction/fs/shaofs/OPTIMIZATION_REPORT.md` | 当前工作区中的 ShaOFS 优化报告，未跟踪 |
| `junction/fs/shaofs/IOPS_BENCHMARK_REPORT.md` | 当前工作区中的历史 4KB IOPS 报告，未跟踪；其中数字需按正式实验重新验证 |

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
    mutable iExtent   extent_hint;  // 上次 extent 查找缓存（顺序访问 O(1)）
    spinlock_t        hint_lock;    // 保护读锁持有期间对 extent_hint 的并发更新
    volatile int      has_dirty_data_cache; // 标记该 inode 是否存在脏的 buffered data cache
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
| `DEFAULT_JOURNAL_BLOCKS` | 4096 | 默认 journal 区大小，位于盘尾，约 16MB |
| `ROOT_INO` | 0 | 根目录 inode 编号 |
| `MYPREFIX` | `"FSHAO"` | 当前 shaofs 识别前缀；`SHAOFS_REALPATH()` 同时接受 `FSHAO/path` 和 `FSHAO:/path`，会把 `FSHAO`、`FSHAO:` 映射为 `/`，并拒绝 `FSHAOabc` 这类伪前缀。FIO 命令仍建议优先用 `FSHAO/` 规避未转义冒号分隔问题 |
| `IO_PREEMPT` | 默认 0 | 是否启用 IOKernel 检查 SPDK completion 并抢占目标 Runtime core；可由 CMake `SHAOFS_IO_PREEMPT=ON` 定义为 1 |
| `CRASH_CONSISTENCY` | 默认 1 | 是否启用 ShaOFS metadata journal；可由 CMake `SHAOFS_CRASH_CONSISTENCY=OFF` 定义为 0 |
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

**2026-05-13 append 预分配现状**：

当前 `extent.cc` 已不再对普通文件 EOF append 一律单块分配。`inode_bmap_locked()` 在 `allocate=true` 且未命中现有 extent 时，会先判断是否满足：

```cpp
inode->type == REGULAR &&
inode->file_size >= BLOCK_SIZE &&
logical_blk == ceil(file_size / BLOCK_SIZE)
```

若满足，则进入 `bmap_prealloc_append()`：

1. 根据当前文件大小选择预分配块数：小于 64KB 时 16 blocks，小于 1MB 时 64 blocks，更大时 128 blocks。
2. 调用 `alloc_blocks(blocks, want)` 批量获取物理块。
3. 将返回的物理块按物理连续性切分成一个或多个 `iExtent` run。
4. 对所有新物理块调用 `get_block_cache().getHandle(block, false)`，写入全 0、标记 dirty，并设置 cache entry valid。
5. 调用 `bmap_insert_compact_extents()` 把这些 run 与已有 extent 一起排序、合并、写回 direct/indirect extent 元数据。
6. 如果插入失败，调用 `bc_invalidate_block()` 丢弃刚清零的 cache entry，并用 `free_extent()` 释放已分配物理块。

这个改动的目标是解决 Filebench `fileserver.f` 中 `appendfilerand` 长时间运行时单块分配导致 inode extent 数超过 `6 + 170` 上限的问题。预分配块提前清零是为了保证未来对预分配但尚未写满的块执行部分写或读时，不会暴露旧盘数据。当前实现只对普通文件 EOF append 生效，目录、稀疏远距离写和非 append 场景仍走原有单块分配 fallback。

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

### 5.4 O_DIRECT user-buffer DMA 路径（2026-05-12）

当前 O_DIRECT 路径的目标是绕过 Block Cache，并在满足严格约束时把用户传入的 buffer 直接作为 SPDK NVMe payload，避免“NVMe → SPDK bounce buffer → user buffer”的额外 memcpy。

**用户 buffer 合约**：

- `buf` 必须按 2MB 对齐。
- `len` 必须是 2MB 的倍数。
- `offset` 必须非负且 4KB 对齐。
- direct read/write 内部还要求每次提交给 NVMe 的片段是整 4KB 块；当前不支持 direct partial-block I/O。
- 不满足上述条件时，`file_read_direct()` / `file_write_direct()` 返回错误；当前实现故意不回退到 bounce buffer + memcpy。

**注册和验证流程**：

```
file_read_direct/file_write_direct
    ├─ user_dma_request_ok(buf, offset, len)
    ├─ storage_prepare_user_dma(buf, len)
    │   ├─ 2MB 对齐检查
    │   ├─ 查 user_dma_pages[] 2MB 页粒度 cache
    │   ├─ syscall_mlock(buf, len)                 // pin 用户页
    │   ├─ spdk_mem_register(buf, len)             // 建立 SPDK/DPDK/VFIO DMA 映射
    │   ├─ rc == -EBUSY 时按 2MB 页逐段注册
    │   ├─ spdk_vtophys() 遍历验证每段可转 IOVA
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

- ShaOFS O_DIRECT open 时，`core.cc:usys_openat()` 会调用 `file_prepare_direct_read_hint()`，把当前文件 extents、file size 和 `has_dirty_data_cache` 指针缓存进 `File::shaofs_direct_read_hint_`。
- `usys_read()` / `usys_pread64()` 在 hint valid 时直接调用 `file_read_direct_hint()`，减少每次 read 的 inode cache 查找和 extent 读取开销。
- `usys_write()` 会把 hint 标记为 invalid，避免 direct write 或 buffered write 后继续使用旧 extent/size 快照。

**seccomp/syscall 前提**：

- `storage_prepare_user_dma()` 调用 `syscall_mlock()`，需要 `lib/caladan/base/syscall.S` 中的 wrapper 且 `junction/syscall/seccomp.cc` 中有 `ALLOW_CALADAN_SYSCALL(mlock)`。
- `spdk_mem_register()` 内部需要 VFIO DMA map/unmap ioctl；当前 seccomp 通过 `ALLOW_IOCTL_REQUEST(VFIO_IOMMU_MAP_DMA)` 和 `ALLOW_IOCTL_REQUEST(VFIO_IOMMU_UNMAP_DMA)` 按 ioctl request 放行。
- 这些放行只服务于 DMA 注册；不要把它理解为 Junction 内可以任意调用 Linux syscall。

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

### 5.8 Crash consistency：metadata-only redo journal + dirty repair

**设计目标**：当前 ShaOFS 没有实现完整 POSIX 级事务语义，也不 journal 普通文件数据块。本机制的目标是在学术测试场景下，以较低 CPU/IO 开销保证异常退出后盘上元数据回到合法、自洽状态，避免 inode bitmap、group bitmap、GDT、inode table 和目录项互相矛盾。

**构建开关**：

- CMake option：`SHAOFS_CRASH_CONSISTENCY`，定义在 `junction/fs/CMakeLists.txt`，默认 ON。
- 编译宏：`CRASH_CONSISTENCY`，默认在 `junction/fs/shaofs/fs.h` 中为 1。
- 关闭时 `journal.h` 中相关 API 退化为 no-op 或原始 `storage_write_obj()`，系统行为尽量接近原始实现。

**journal transaction 格式**：

- `JournalHeader`：magic/version/state/entry_count/seq/checksum。
- `JournalEntry`：home block、journal image block、image checksum。
- 单个事务最多 `kMaxJournalEntries=64` 个 metadata block；当前主要使用 `journal_commit_single()` 做单块事务。
- checksum 使用 FNV-1a，用于发现 torn header、torn entry 或 image 损坏。

**正常 metadata 写入流程**：

```
journal_commit_blocks(blocks, images, count)
    │
    ├─ 获取 journal_lock，串行化 journal 区复用
    ├─ 校验目标块属于 metadata block
    ├─ 将每个 4KB metadata 新镜像写到 journal image block
    ├─ 写 entry table
    ├─ 写 state=COMMITTED 的 transaction header
    ├─ 将镜像写回各自 home block
    └─ 清空 transaction header
```

这里采用 redo journal：崩溃恢复时，如果看到 checksum 正确且 `COMMITTED` 的 header，就把 journal image 重新写回 home block。普通数据块不进入 journal，仍直接走 SPDK/DMA 写盘路径。

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
    └─ init_group()

final_flush()
    ├─ journal_write_metadata(imap)
    ├─ sync_all_gdt() → journal_write_metadata(GDT)
    ├─ ic_flush_all()
    ├─ bc_flush_all()
    └─ journal_mark_clean()
```

`journal_mark_dirty()` 写在 journal 区最后一个块。正常退出时 `journal_mark_clean()` 清掉它；如果进程被 `SIGKILL`、崩溃或 timeout 强杀，下一次 mount 会看到 dirty marker。

**恢复流程**：

`journal_recover()` 会先检查 transaction header：

- header 无效或版本不认识：清 header，进入 repair。
- entry_count 不合法：清 header，进入 repair。
- checksum 不匹配：认为事务 torn/incomplete，清 header，进入 repair。
- `state=COMMITTED` 且 checksum 正确：replay image blocks 到 home blocks，然后进入 repair。
- 其他非空状态：清 header，进入 repair。

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

### 5.9 FS base 保存/恢复策略（2026-05-10）

**背景问题**：ShaOFS 调用 Caladan runtime、SPDK、DML 或 runtime libc 相关路径时，需要把 x86 `%fs` 切到 Caladan runtime TLS。用户程序自己的 `%fs` 则指向用户 libc/TLS 区域，里面包括 stack canary、`errno`、pthread TLS 等。如果 ShaOFS 在 runtime FS base 下发生 uthread park/yield，而调度器把当前 `%fs` 直接保存到 `thread_t::fsbase`，就会把用户 TLS 状态污染成 runtime TLS，后续回到用户程序可能出现 stack smashing、SIGSEGV 或随机 TLS 错乱。

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

**进入 ShaOFS runtime-FS 区域时**：

`junction/fs/shaofs/utili.h:RuntimeFSBaseGuard` 构造函数只在很短的切换窗口内 `preempt_disable()`：

```
preempt_disable()
prev_fs_base_ = _readfsbase_u64()
thread_self()->runtime_fsbase_depth++
_writefsbase_u64(perthread_read(runtime_fsbase))
preempt_enable()
```

注意：它不会在整个 ShaOFS syscall 生命周期内保持 preempt disabled，因为 ShaOFS 内部可能等待 SPDK I/O、获取 `rwmutex`、cache miss 后 park 或 yield。长时间禁用抢占会触发 Caladan 调度器对 `preempt_cnt` 的断言。

**ShaOFS guard 内发生 park/yield 时**：

`thread_park_and_unlock_np()` 和 `thread_park_and_preempt_enable()` 当前调用 `thread_save_fsbase(curth)`。其逻辑是：

```
fsbase = _readfsbase_u64()
if (th->runtime_fsbase_depth && fsbase == perthread_read(runtime_fsbase))
    return;              // 不覆盖 thread_t::fsbase
th->fsbase = fsbase;
```

因此线程在 ShaOFS guard 内 park 时，调度器不会把 runtime FS base 写进 `thread_t::fsbase`。用户 TLS 状态得以保留。

**恢复一个 park 在 ShaOFS guard 内的线程时**：

`jmp_thread()` / `jmp_thread_direct()` 不再直接 `set_fsbase(th->fsbase)`，而是调用 `thread_fsbase_to_run(th)`：

- `runtime_fsbase_depth > 0`：恢复到当前 kthread 的 `runtime_fsbase`，继续执行 ShaOFS/runtime 代码。
- `runtime_fsbase_depth == 0`：恢复到 `thread_t::fsbase`，回到用户 TLS。
- `has_fsbase == false` 的 runtime-only thread 会默认使用 `runtime_fsbase` 初始化 `th->fsbase`。

**退出 ShaOFS runtime-FS 区域时**：

`RuntimeFSBaseGuard` 析构函数再次只在短窗口内关闭抢占，恢复构造时保存的 `prev_fs_base_`，然后递减 `runtime_fsbase_depth`。嵌套 guard 可以正确工作：内层退出后仍保持 runtime FS base，最外层退出才恢复用户 FS base。

**和 Junction 原有 `RuntimeLibcGuard` 的区别**：

`junction/bindings/runtime.h:RuntimeLibcGuard` 会在整个 guard 生命周期内关闭抢占并切换到 runtime FS base，这适合很短、不会 yield 的 runtime libc 调用。ShaOFS 的 `RuntimeFSBaseGuard` 是 depth-aware 且允许 guard 内 yield 的版本，专门用于文件系统 I/O 路径。

**后续维护铁律**：

如果新增代码路径满足“切换到 runtime FS base，并且期间可能 park/yield”，必须使用或复用当前 `runtime_fsbase_depth` 策略。否则会重新引入用户 TLS 被 runtime TLS 污染的问题。

### 5.10 inode 分配与 32768 inode 支持（2026-05-10）

ShaOFS 的 inode 上限来自 `INODENUM=32768` 和一块 inode bitmap；inode cache 容量 `DEFAULT_INODECACHE_CAPACITY=8192` 只是内存缓存容量，不应限制文件系统可创建 inode 数。

本轮曾在约 8192 inode 附近遇到 inode 分配失败。根因是 allocator 逻辑把可分配范围错误限制在缓存容量附近，混淆了“inode cache entry 数量”和“盘上 inode bitmap 容量”。当前 `junction/fs/shaofs/inode.cc:alloc_inum()` 已改为：

```
static volatile unsigned int cursor;
start = atomic_fetch_add(&cursor, 1);
for i in [0, INODENUM):
    idx = (start + i) % INODENUM;
    if (!bitmap_atomic_test_and_set(imap, idx)) return idx;
return -1;
```

这表示分配器会按 `INODENUM` 全范围环形扫描 inode bitmap，可以越过 8192 cache capacity。`ic_alloc_inode()` 获取 inode cache entry 失败时会释放刚分配的 inum，避免 bitmap 泄漏。

当前已通过 `test_many_inodes` 和 `test_many_inodes_read_threads` 覆盖 10000 文件级别场景；完整创建接近 32768 个 inode 的耗尽边界尚未在本次交接整理中重新验证。

### 5.11 Caladan/Junction syscall 包装与拦截机制（2026-05-12）

这个项目里必须区分三类 syscall：

1. **用户程序 syscall**：例如 Filebench 调用 `open/read/pread/write`。这些 syscall 应被 Junction 拦截并分发到 `usys_*`，ShaOFS 路径再转到 `my_*`。
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
- ShaOFS/Junction/Caladan 内部不能任意调用 Linux syscall。需要真实 syscall 时，必须走受控 wrapper，并确认 `seccomp.cc` allowlist 已放行。
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

启用/关闭 ShaOFS crash consistency：

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

### 6.5 编译并运行 sync syscall smoke test（2026-05-15）

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

### 6.8 在 Junction 中运行 FIO（2026-05-06 当前流程）

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

### 6.9 在 Junction 中运行 Filebench（2026-05-13 当前流程）

Filebench 源码位于 `junction/fs/mytest/benchmark/filebench`。当前策略是只修改 Filebench 内部并用 patch 管理，不修改 Junction 源码。

相关文件：

| Path | Purpose |
|------|---------|
| `junction/fs/mytest/benchmark/filebench` | Filebench 源码和构建产物目录 |
| `junction/fs/mytest/benchmark/patch/filebench_changes.patch` | Junction 适配补丁，覆盖 `aslr.c`、`fb_cvar.c`、`fb_localfs.c`、`fileset.c`、`flag.h`、`flowop_library.c`、`ipc.c`、`misc.c`、`procflow.c` |
| `junction/fs/mytest/benchmark/patch/toggle_filebench.sh` | 补丁 apply/revert + 自动 `configure`/`make` 的管理脚本 |
| `junction/fs/mytest/benchmark/patch/example.f` | 当前 Filebench 示例 workload：10000 个 16KB 文件，2 个 process instances，每个 3 个 reader threads，`readwholefile` 跑 60s |
| `junction/fs/mytest/benchmark/filebench_wml/fileserver.f` | 当前 ShaOFS fileserver smoke workload：40 files、1 thread、4KB file/io、2s runtime |
| `junction/fs/mytest/benchmark/filebench_wml/webserver.f` | 当前 ShaOFS webserver workload：1000 files、100 reader threads、10 次 readwholefile + appendlog、60s runtime |

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
4. `fb_localfs.c`：`fb_lfs_recur_rm()` 遇到 `FSHAO:/` 或 `FSHAO/` 路径直接返回，避免通过 `system("rm -rf ...")` 清理 ShaOFS 路径。
5. `fb_cvar.c`：cvar 目录不可用时降级为 verbose log；如果默认目录没有加载到 cvar 插件，会再根据 Filebench 可执行文件路径尝试 `cvars/.libs` build 目录，保证 `webserver.f` 中的 `cvar-gamma` 可用。
6. `fileset.c` / `ipc.c`：修复若干 `strncpy` 未保证 NUL 结尾的问题，避免 Junction/Filebench 长路径下字符串截断或未终止。
7. `fileset.c`：`fileset_mkdir()` 的 `dirs[65536]` 栈数组改为动态数组，避免 Junction uthread 512KB 栈被大栈对象压垮。
8. `misc.c`：`filebench_log()` 的 128KB 栈上缓冲改为全局缓冲并加 pthread mutex，避免日志路径消耗过大 uthread 栈；同时改用 `vsnprintf()`。
9. `flowop_library.c`：修正 debug log 打印 `threadflow->tf_fd[fd]` 结构体的问题，改为打印 `fd_num`。
10. `flag.h`：`wait_flag()` 的纯 busy-wait 改为循环中 `sched_yield()`，降低 Filebench 进程内 pthread 降级模式下的 CPU 空转和终止等待问题。

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

当前 `example.f` 使用 `path="FSHAO:"`。ShaOFS 的 `SHAOFS_REALPATH()` 同时支持 `FSHAO:`、`FSHAO:/...` 和 `FSHAO/...`；但其他工具如 FIO 对冒号有特殊解析，Filebench 是否会在所有 workload 语法中同样安全使用冒号仍建议按 workload 实测确认。

运行当前 `fileserver.f` smoke workload：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld 2>/dev/null || true
cd /home/syh/mkfs && printf 'syh2syh\n' | sudo -S bash ./mkfs.sh

cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias

cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 20s ./junction_run caladan_test.config -- \
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

注意：`webserver.f` 原始 upstream workload 默认 `set $dir=/tmp`，那会绕过 ShaOFS。当前工作区版本已改为 `set $dir=FSHAO:`。`webserver.f` 内部写的是 `run 60`，所以用 `timeout 20s` 运行会得到退出码 124，这只是 timeout 太短，不表示 workload 或 ShaOFS 失败；完整验证建议 timeout 至少 90s。

### 6.10 在 Junction 中运行 FxMark（2026-05-15 当前流程）

FxMark 源码位于：

```bash
/home/syh/MyProj1/junction/junction/fs/mytest/benchmark/fxmark
```

当前策略是只修改 FxMark 内部并用 patch 管理，不修改 Junction 或 ShaOFS 源码。相关文件：

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
4. `src/util.c`：把 `system("mkdir -p ...")` 改为进程内递归 `mkdir()`，并把 `FSHAO` / `FSHAO:` 当作 ShaOFS 根别名处理，避免通过 shell 清理或创建 ShaOFS 路径。
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

当前 `build/junction/caladan_test.config` 已确认包含：

```text
runtime_kthreads 1
runtime_spinning_kthreads 1
runtime_guaranteed_kthreads 0
runtime_quantum_us 0
enable_storage 1
```

因此 `--ncore` 只表示 FxMark 逻辑 worker 数，不代表 Junction 当前真的用了同等数量的 Caladan runtime kthreads。若要测试真实多核扩展性，需要另行调整 `caladan_test.config` 中的 runtime kthread/spinning kthread 配置并重新记录实验条件。

### 6.11 Filebench randomread workload 与 ext4 对比脚本（2026-05-12）

当前工作区中存在一组从 Filebench 官方 `workloads/randomread.f` 派生的 ShaOFS workload，目录为：

```bash
/home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench_wml/
```

已看到的文件包括：

```text
shaofs_randomread.f
shaofs_randomread_1t.f
shaofs_randomread_2t.f
shaofs_randomread_4t.f
shaofs_randomread_8t.f
shaofs_randomread_count_8t.f
shaofs_randomread_direct_16t.f
```

这些文件当前在主仓库状态中是 untracked。典型配置是：

- `set $dir=FSHAO:`
- `set $filesize=1g`
- `set $iosize=4k`
- `set $workingset=0`
- `set $directio=1`
- `shaofs_randomread_direct_16t.f` 使用 16 个 Filebench reader threads，`run 30`
- `shaofs_randomread_count_8t.f` 使用 `finishoncount`，避免完全依赖 Filebench 时间运行语义

ShaOFS randomread 运行示例：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld 2>/dev/null || true
cd /home/syh/mkfs && printf 'syh2syh\n' | sudo -S bash ./mkfs.sh

cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias

cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 120s ./junction_run caladan_test.config -- \
  /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench/filebench \
  -f /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench_wml/shaofs_randomread_direct_16t.f

printf 'syh2syh\n' | sudo -S pkill -9 iokerneld
```

ext4 对比脚本位于 repo 外部：

```bash
/home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh
```

脚本当前行为：

- 使用 `/home/syh/mkfs/reset_ext4.sh` 将盘重置为 ext4。
- 挂载点为 `/mnt/nvme/ext4_bench`。
- 使用 cgroup v2 限制 CPU、memory、swap。
- 默认 `MEM_LIMIT_MB=300`，默认 `CPU_LIMIT=1`，默认 `NTHREADS=1`，默认 `DIRECTIO=1`，默认 `RUNTIME=30`。
- 运行同样形态的 Filebench randomread WML，并将结果写入 `/home/syh/fs_test/results/`。

ext4 运行示例：

```bash
printf 'syh2syh\n' | sudo -S \
  CPU_LIMIT=1 MEM_LIMIT_MB=300 NTHREADS=16 DIRECTIO=1 RUNTIME=30 \
  /home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh
```

注意：本次交接整理没有重新运行 ShaOFS/ext4 randomread benchmark。正式对比时必须保存完整 Filebench stdout/stderr、退出码、WML 文件、构建开关、Junction `caladan_test.config` CPU 配置和 cgroup 参数。

### 6.12 ext4 fileserver.f cgroup 对比脚本（2026-05-13）

repo 外部新增了 ext4 `fileserver.f` 对比脚本：

```bash
/home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh
```

脚本当前行为：

- 使用 `/home/syh/mkfs/reset_ext4.sh` 将测试盘重置为 ext4。
- 默认挂载点为 `/mnt/nvme/ext4_bench`。
- 使用 cgroup v2 限制 Filebench 进程资源，默认 `CPU_LIMIT=2`、`NUMA_NODE=0`、`MEM_LIMIT_MB=300`、`TIMEOUT_SEC=30`。
- 默认读取 ShaOFS 当前 `fileserver.f`：`/home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench_wml/fileserver.f`。
- 在 `/home/syh/fs_test/results/` 生成临时 WML，只替换 `set $dir=` 为 ext4 挂载点，其他 workload 参数保持一致。
- 运行结束后保存 log 和 CSV，并输出 cgroup `cpu.stat` / `memory.events` / `memory.peak` 摘要。

运行示例：

```bash
printf 'syh2syh\n' | sudo -S /home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh
```

本轮实际观察到的 ext4 smoke 结果为：

```text
IO Summary: 1254505 ops 627211.104 ops/s 20907/439050 rd/wr 367.4mb/s 0.001ms/op
Cgroup: cpu_usage_usec_delta=1953362 mem_peak_bytes=314572800 mem.high_delta=0 mem.max_delta=694 mem.oom_delta=0
```

同一轮 ShaOFS `fileserver.f` smoke 结果为：

```text
IO Summary: 1979495 ops 989422.475 ops/s 32981/692595 rd/wr 579.4mb/s 0.001ms/op
```

这个对比对应当前缩小版 `fileserver.f`（40 files、1 thread、2s runtime），ShaOFS 约为 ext4 的 `1.58x` IOPS/throughput。注意该结果只是 smoke 对比，ext4 运行中 `memory.peak` 达到 `memory.max=300MB` 且 `memory.events max` 增加，虽然没有 OOM，但正式实验应明确记录这个资源限制状态。

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

**触发条件**：单个文件的物理块分配高度碎片化时，extent 数超过 `DIRECT_EXTENT_NUM + EXTENTS_PER_BLOCK = 176` 上限。历史上 Filebench `fileserver.f` 的 append-heavy 模式曾触发该问题。

**当前状态**：2026-05-13 已在普通文件 EOF append 路径加入批量预分配，使用 `alloc_blocks()` 一次获取 16/64/128 个块并合并成 extent，已经能跑通当前 `fileserver.f` smoke workload。

**仍需注意**：这不是全局根治。目录、稀疏远距离写、随机非 append 写、或极端碎片化情况下仍可能达到 176 extent 上限。后续如果看到 `[extent] Extent array overflow for inode ...`，应先确认 workload 是否走 EOF append 预分配路径，再考虑扩大 extent 存储结构、加入更强的连续块分配策略或为随机写 workload 做专门设计。

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

另外，ShaOFS direct I/O 当前已有严格约束下的 user-buffer DMA 路径，但为了保证 cached/direct 一致性，direct read/write 仍会涉及 `bc_flush_block()` / `bc_invalidate_block()`；不满足 2MB 对齐/长度合约的请求会直接失败。正式解释性能时需要明确 workload 是否真的走到了 user-buffer DMA fast path。

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

ShaOFS 的 `RuntimeFSBaseGuard` 与 Junction 原有 `RuntimeLibcGuard` 不同。`RuntimeLibcGuard` 在整个 guard 生命周期内关闭抢占，只适合短小且不会 yield 的 libc/runtime 调用；ShaOFS guard 只在切换 `%fs` 和更新 `runtime_fsbase_depth` 时短暂关闭抢占，随后允许 I/O、锁等待和 uthread park。

修改相关代码时必须同时理解：

- `junction/fs/shaofs/utili.h:RuntimeFSBaseGuard`
- `lib/caladan/inc/runtime/thread.h:thread::runtime_fsbase_depth`
- `lib/caladan/runtime/sched.c:thread_save_fsbase()`
- `lib/caladan/runtime/sched.c:thread_fsbase_to_run()`
- `lib/caladan/runtime/sched.c:thread_park_and_unlock_np()` / `thread_park_and_preempt_enable()`
- `lib/caladan/runtime/sched.c:jmp_thread()` / `jmp_thread_direct()`

不要把 ShaOFS guard 改回“整个作用域 `preempt_disable()`”；这会在 ShaOFS 内部 park/yield 时触发 Caladan 调度器 preempt count 断言。也不要在 `runtime_fsbase_depth > 0` 且当前 `%fs == runtime_fsbase` 时保存到 `thread_t::fsbase`；这会污染用户程序 TLS，典型表现是 stack smashing 或 Filebench 并发读崩溃。

### 7.15 GOTCHA 14：inode cache 容量不是 inode 总量

`DEFAULT_INODECACHE_CAPACITY=8192` 只是 inode cache 的可驻留 entry 数。ShaOFS 盘上 inode 总数是 `INODENUM=32768`，由 inode bitmap 决定。任何 inode 分配逻辑都不能用 cache capacity 作为扫描上限。

当前 `alloc_inum()` 已按 `INODENUM` 全范围环形扫描；如果后续重构 inode cache 或 allocator，必须保留这个语义。否则 Filebench `entries=10000` 这类测试会在约 8192 inode 附近再次失败。

### 7.16 GOTCHA 15：Filebench 当前是“单进程多线程降级版”

当前 Filebench patch 为了适配 Junction，改变了 Filebench 的 procflow 执行模型：不再 fork worker process，而是在同一进程内用 pthread 运行 procflow monitor 和 worker threads。这足以跑通当前 ShaOFS 学术读负载，但不是 Filebench 上游语义的完整等价实现。

使用 Filebench 结果时需要明确：

- `process name=...,instances=N` 当前会变成同一 Junction 进程内的 N 个 procflow monitor pthread，而不是 N 个 OS process。
- 当前补丁没有修改核心 flowop I/O 操作，例如 open/readwholefile/closefile 的实际文件 I/O 逻辑仍走 Filebench 原有 flowop。
- 涉及多进程隔离、进程级资源统计、真实 fork/exec 行为的 Filebench workload 不应直接拿当前 patch 的结果做结论。
- `toggle_filebench.sh apply` 后 Filebench 子仓库处于 modified/dirty 状态是预期的；源码修改应通过 `filebench_changes.patch` 管理，不要直接手改子仓库后忘记更新 patch。

### 7.17 GOTCHA 16：O_DIRECT user-buffer DMA 合约非常严格

当前 ShaOFS O_DIRECT 不再为不合规请求自动回退到 bounce buffer。测试程序或 benchmark 如果要验证“NVMe SSD ↔ user buffer”直通路径，必须保证：

- 用户 buffer 2MB 对齐。
- I/O 长度是 2MB 的倍数。
- 文件 offset 4KB 对齐。
- 读写范围落在整 4KB 块上。
- `storage_prepare_user_dma()` 成功完成 `mlock`、`spdk_mem_register` 和 `spdk_vtophys` 验证。

Filebench `directio=1` 不一定天然满足 ShaOFS 当前 2MB user-buffer 合约。若 Filebench 内部只按 4KB 分配/提交 buffer，ShaOFS O_DIRECT 可能返回错误或没有走到 user-buffer DMA fast path。正式 benchmark 前应通过最小测试程序或 Filebench 日志确认实际路径。

### 7.18 GOTCHA 17：Caladan/Junction 内不能任意调用 Linux syscall

当前真实 Linux syscall 由 seccomp 按“syscall number + syscall instruction pointer 地址范围”控制：

- Caladan wrapper 必须位于 `[base_syscall_start, base_syscall_end)`。
- Junction `ksys_*` wrapper 必须位于 `[ksys_start, ksys_end)`。
- 额外的 VFIO ioctl 只按 request 放行，用于 SPDK DMA map/unmap。

因此不要在 ShaOFS、Caladan runtime 或 Junction runtime 中直接调用 glibc `syscall()`、`open()`、`ioctl()` 等。需要新增真实 syscall 时，必须同时增加 wrapper、头文件声明和 seccomp allowlist，并确认调用点不会破坏 Junction 对用户 syscall 的拦截语义。

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
| `sync` | `shaofs_sync_all` | `file.cc:usys_sync` |

### 8.3 已实现的功能特性

- **三级缓存**：Block Cache (write-back) + Inode Cache (write-back) + Dentry Cache (write-through)
- **O_DIRECT user-buffer DMA**：满足 2MB 对齐/长度合约时绕过 Block Cache，直接用用户 buffer 作为 SPDK NVMe read/write payload；不满足时返回错误
- **O_TRUNC**：`truncate_inode()` 释放所有数据块并重置 file_size
- **O_APPEND**：在 write dispatch 时 `lseek(SEEK_END)` 后写入
- **fsync**：per-file 精确刷写（遍历 extent → `bc_flush_block` + `ic_flush_inode`）
- **sync**：全局刷写 ShaOFS 内存脏状态（imap、GDT、inode cache、block cache），用于 FxMark/FIO/Filebench 等会调用 `sync()` 的 benchmark；不会执行 clean unmount 语义或清 dirty marker
- **stat/fstat**：完整填充 `struct stat`（含 indirect extent 块数统计）
- **Extent hint**：顺序访问 O(1) 块映射
- **EOF append 预分配**：普通文件 append 到 EOF 时按文件大小批量预分配 16/64/128 blocks，减少 Filebench append-heavy 场景中的 extent 数和分配开销
- **Per-core group affinity**：减少块分配锁竞争
- **目录读写锁**：`dir_lookup` 并发读，`dir_add/delete` 排他写
- **I/O completion driven preemption**：`IO_PREEMPT` 开启时，IOKernel 检查 SPDK completion 并触发目标 Runtime core yield；Runtime 优先运行 storage softirq 和完成 I/O 的 uthread
- **ShaOFS 前缀解析**：`SHAOFS_REALPATH()` 同时支持 `FSHAO/path` 与 `FSHAO:/path`，并拒绝 `FSHAOabc` 伪前缀
- **Crash consistency**：`CRASH_CONSISTENCY=1` 时启用 metadata-only redo journal；clean shutdown 清 dirty marker，异常退出后 replay/repair

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

### 8.6 2026-05-13 当前 Filebench 状态

- `junction/fs/mytest/benchmark/filebench` 是 Filebench 源码目录；当前通过 `junction/fs/mytest/benchmark/patch/filebench_changes.patch` 管理 Junction 适配修改。
- `toggle_filebench.sh apply` 会应用补丁，并用 `ac_cv_func_ftok=no ac_cv_func_semget=no ac_cv_func_semop=no ac_cv_func_semtimedop=no ./configure` 重新配置，然后执行 `make -j $(nproc)`。
- 当前补丁覆盖 9 个 Filebench 源文件：`aslr.c`、`fb_cvar.c`、`fb_localfs.c`、`fileset.c`、`flag.h`、`flowop_library.c`、`ipc.c`、`misc.c`、`procflow.c`。
- 适配后的 Filebench 不再依赖 `personality()` 关闭 ASLR，不再创建 SysV semaphore，不再 fork/exec worker process，也避免了 Filebench 日志和 mkdir 路径中的大栈对象；`fb_cvar.c` 还会从可执行文件所在 build tree 的 `cvars/.libs` 查找 cvar 插件，使 `webserver.f` 的 `cvar-gamma` 能在 Junction 内加载。
- 本轮会话中曾使用 `example.f` 在 Junction 上跑通 60s Filebench 读 whole-file workload：10000 个 16KB 文件，2 个 process instances，每个 3 个 reader threads。记录到的输出约为 `79454650 ops`、`1324058 ops/s`、`6.9GB/s`、`0.0ms/op`、`0.660ms` latency；这些是本轮调试验证数字，不是正式论文 benchmark，后续必须重新运行并保存完整 stdout/stderr、退出码和构建开关状态。
- 当前 `fileserver.f` 已缩小为 ShaOFS smoke workload：`set $dir=FSHAO:`、40 files、1 thread、4KB file/io、`run 2`。2026-05-13 在 Junction/ShaOFS 上完整跑通，输出约 `1979495 ops`、`989422.475 ops/s`、`579.4mb/s`。
- repo 外部脚本 `/home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh` 使用同一 `fileserver.f` 参数对 ext4 做 cgroup v2 限制下的 smoke 对比，当前观察到 ext4 约 `1254505 ops`、`627211.104 ops/s`、`367.4mb/s`。该脚本会把 `$dir` 改成 ext4 挂载点，其他参数保持一致。
- 当前 `webserver.f` 已改为 `set $dir=FSHAO:`、1000 files、100 threads、`run 60`。用 `timeout 20s` 运行会因为 workload runtime 太长而得到退出码 124；用 `timeout 90s` 已在 Junction/ShaOFS 上完整跑通，输出约 `40842810 ops`、`680679.092 ops/s`、`3412.3mb/s`。
- 之后又派生了 Filebench `randomread.f` workload 到 `junction/fs/mytest/benchmark/filebench_wml/`，并编写了 repo 外部 ext4 cgroup 对比脚本 `/home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh`。本次交接整理未重新运行这些 randomread benchmark，无法从当前上下文确认最新 ShaOFS/ext4 对比数字。
- 当前 Filebench 结果是在 `DIRECTPATH DISABLED` 环境下得到的，不能直接解释为最终 NVMe 极限带宽。

### 8.7 2026-05-15 当前 FxMark 状态

- `junction/fs/mytest/benchmark/fxmark` 是 FxMark 源码目录；当前通过 `junction/fs/mytest/benchmark/patch/fxmark_changes.patch` 管理 Junction 适配修改。
- `toggle_fxmark.sh apply` 会应用补丁并执行 `make -j "$(nproc)"`；`toggle_fxmark.sh revert` 会反向应用补丁并重新构建。2026-05-15 已实际验证 `revert` 和 `apply` 都能干净执行并构建通过。
- 当前补丁覆盖 4 个文件：`Makefile`、`src/bench.c`、`src/DRBL.c`、`src/util.c`。
- 适配后的 FxMark 不再使用 `fork()` 创建 worker，而是在同一 Junction 进程内使用 pthread worker；启动/结束屏障中的纯 busy-wait 改为 `sched_yield()`，避免 `runtime_quantum_us=0` + 单 runtime kthread 下等待线程自旋霸占 CPU。
- `src/util.c` 中的 `mkdir_p()` 已改为进程内递归 `mkdir()`，避免 `system("mkdir -p")` 在 Junction/ShaOFS 路径上触发额外 shell/未支持机制。
- `src/DRBL.c` 当前用 wall-clock deadline 让 worker 自己结束；这样不依赖 `SIGALRM` 在 tight loop 中及时投递。这个改动只覆盖 DRBL workload，其他 FxMark workload 是否也需要类似处理尚未验证。
- 当前 `build/junction/caladan_test.config` 为 `runtime_kthreads=1`、`runtime_spinning_kthreads=1`、`runtime_quantum_us=0`。因此以下结果主要证明 FxMark 多 worker 在 Junction/ShaOFS 上能稳定跑完，不代表真实多核扩展性。

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

### 8.8 2026-05-09 crash consistency 验证结果

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

---

## 第九章：已知缺陷与待办事项

### 9.1 已知缺陷

1. **并发文件创建未充分测试**：多线程同时 `open(O_CREAT)` 在同一目录下创建不同文件的正确性未验证
2. **Extent 数组溢出未全局消除**：普通文件 EOF append 已加入 16/64/128 blocks 批量预分配并跑通当前 `fileserver.f` smoke workload，但随机非 append、稀疏写和极端碎片化场景仍可能超过 176 extent 上限
3. **高线程混合负载表现需要重新确认**：历史上曾关注 cache shard 竞争，但具体性能结论不再写入本文档，后续应重新 benchmark
4. **无 `unlink` / `rmdir`**：`dir_delete_entry` 存在但未接入 VFS dispatch
5. **无 `getdents64`**：目录列表需要直接读取 Dirent 结构
6. **时间戳全为 0**：DInode 有 atime/mtime/ctime 字段但无代码设置
7. **O_DIRECT user-buffer DMA 适用范围有限**：当前已实现严格约束下的 direct user-buffer DMA，但要求 buffer 2MB 对齐、长度 2MB 倍数、offset 4KB 对齐；Filebench 等通用 benchmark 不一定天然满足这些约束，需专门确认
8. **FIO 适配是源码补丁而非上游通用修复**：当前只保证本项目常用参数（特别是 `--thread=1`、psync、ShaOFS 路径）可跑通；不要默认它覆盖 FIO 全部 job 组合。
9. **`MYPREFIX` 与测试路径书写存在历史不一致**：当前代码定义为 `"FSHAO"`，`SHAOFS_REALPATH()` 已兼容 `FSHAO/...` 与 `FSHAO:/...`，但 FIO 当前源码仍将未转义 `:` 作为 filename/directory 分隔符，因此 FIO 命令建议写 `FSHAO/`；若恢复 `FSHAO:`，必须设计并验证 FIO 参数转义方案。
10. **`IO_PREEMPT` 只在特定场景下显著收益**：它主要解决 CPU-bound uthread 阻塞 SPDK completion poll 的问题；没有 CPU-bound 干扰、kthread 充足或大块顺序吞吐场景下，收益可能较小甚至需要评估额外 UIPI 开销。
11. **当前构建缓存开启了 `SHAOFS_IO_PREEMPT`**：`build/CMakeCache.txt` 当前为 ON，但 CMake 默认值仍为 OFF。做性能对比时必须明确重新 configure，避免把 ON/OFF 结果混淆。
12. **Crash consistency 不是完整事务语义**：当前只保证异常退出后元数据合法、自洽；普通数据块不 journal，不完整创建/写入可能被 repair 清理或留下已落盘的数据内容。
13. **Journal metadata map 依赖目录 extent 登记**：如果后续新增目录扩容、rename、unlink/rmdir 或新的目录写路径，必须确保新目录块被 `journal_register_metadata_block()` 登记，否则该目录块写回可能绕过 journal。
14. **FS base 策略是针对 ShaOFS 的混合修复，不是 Junction 全局 TLS 架构终局**：当前已经覆盖 ShaOFS guard 内 park/yield 的场景，但其他隐式进入 runtime libc 且可能 yield 的路径仍需单独审计。
15. **Filebench patch 改变 procflow 执行模型**：当前 Filebench 适配版用于跑通 Junction/ShaOFS 学术负载；它不是对上游 Filebench 多进程语义的完整兼容。
16. **32768 inode 边界尚未完整耗尽验证**：当前已修复约 8192 inode 附近失败的问题，并验证过 10000 文件级别场景；完整创建到接近 `INODENUM=32768` 后的行为仍应补充压力测试。
17. **当前工作区存在未跟踪 benchmark/patch/report 文件**：`junction/fs/mytest/benchmark/patch/*`、`junction/fs/mytest/benchmark/filebench_wml/*`、`junction/fs/shaofs/OPTIMIZATION_REPORT.md`、`junction/fs/shaofs/IOPS_BENCHMARK_REPORT.md` 当前在主仓库中显示为 untracked；接手前应确认哪些需要纳入版本控制
18. **FxMark patch 是 Junction 适配版，不是上游语义完整等价实现**：当前把 FxMark worker 从 process/fork 模型改成同进程 pthread 模型，只验证了 DRBL 跑通。涉及进程隔离、真实多进程扩展性或其他 FxMark workload 的结论需要单独验证。
19. **当前 FxMark 多 worker smoke 不是多核扩展性结果**：`build/junction/caladan_test.config` 当前只有 `runtime_kthreads=1` / `runtime_spinning_kthreads=1`，所以 `--ncore 8` 并不表示 Junction/ShaOFS 使用了 8 个 runtime kthreads。正式多核实验必须先调整并记录 config。
20. **`sync()` 是全局 flush，不是 clean unmount**：`usys_sync()` 当前调用 `shaofs_sync_all()` 刷写脏状态，但不会调用 `journal_mark_clean()`，也不会清除 `runtime_info->spdk_uipi`。如果测试依赖 clean shutdown 语义，仍应让 `junction_run` 正常退出走 `final_flush()`。

### 9.2 优先待办任务

**Task 1: 完善 append 预分配与 extent 压力测试**
- 当前 `alloc_blocks()` 已用于普通文件 EOF append 预分配，但只覆盖 append 到 EOF 的常见路径。
- 补充针对随机写、稀疏写、append 预分配跨 direct/indirect 边界、预分配失败回滚和崩溃恢复后的 extent 校验测试。
- 根据正式 Filebench/fileserver 结果调优 16/64/128 blocks 阈值，确认预清零和额外 dirty cache 不会在目标负载下引入明显 CPU/写放大。

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

**Task 7: 验证并优化 O_DIRECT user-buffer DMA**
- 用最小 C 测试程序显式分配 2MB 对齐 buffer、长度 2MB 倍数，确认 `storage: enabled direct DMA into user buffers` 日志和 `NVMe SSD ↔ user buffer` 数据正确性。
- 确认 Filebench `directio=1` 是否满足当前 2MB user-buffer 合约；若不满足，需要在 Filebench 适配层控制 buffer 对齐/长度，或明确该 workload 不用于验证 user-buffer DMA。
- cached/direct 一致性当前使用 `bc_flush_block()` 和 `bc_invalidate_block()`，正确但可能增加 direct path CPU 开销。后续可考虑仅在 cache entry dirty 时 flush，或在 direct read 命中 clean cache 时直接从 cache 返回。

**Task 8: 评估 IOKernel completion 检查开销**
- 当前 `check_spdk_and_preempt()` 在 dataplane loop 中遍历 runtime/kthread。
- 对少量 Runtime 的学术实验可接受；若扩展到更多 Runtime，应评估 bitmap/event/coalescing，避免 IOKernel 忙等扫描成为瓶颈。

**Task 9: 完善 crash consistency 语义测试**
- 当前已有 SIGKILL dirty-mount 恢复测试，但还没有覆盖 torn transaction header、坏 checksum、目录块事务 replay、inode table 单块 replay 等更细粒度场景。
- 建议编写离线磁盘破坏工具或 Junction 内部测试 hook，构造 journal header/image/home block 的不同崩溃点。

**Task 10: 评估 journal 粒度和批量事务**
- 当前主要是单 metadata block redo，正确性靠 dirty repair 兜底。
- 后续可把一个 syscall 的多个元数据块合并为小事务，减少不完整操作被 repair 清理的概率，并减少多次 header 写。

**Task 11: 优化 dirty repair 的 mount 成本**
- 当前 repair 会扫描完整 inode table 和所有 group bitmap。
- 学术测试中只要避免非 clean shutdown，正常路径不受影响；如果要频繁 crash/recover 实验，需要记录 repair 耗时并考虑按需扫描或 checkpoint。

**Task 12: 固化 Filebench 正式 benchmark 脚本**
- 基于 `toggle_filebench.sh apply`、重新 mkfs、启动 IOKernel、`timeout` 运行 Filebench、清理 IOKernel 的流程写自动化脚本。
- 每次记录 Filebench stdout/stderr、退出码、Junction `DIRECTPATH` 状态、`SHAOFS_IO_PREEMPT` / `SHAOFS_CRASH_CONSISTENCY` 构建开关和 Filebench patch 状态。
- 明确论文中如何解释 Filebench `process` 被降级为 pthread 的限制。
- 将 `fileserver.f`、`webserver.f` 和 randomread 的 WML、timeout、runtime、线程数、文件数固定到脚本和结果目录中，避免手工改 WML 后无法复现实验。

**Task 13: 固化 ext4 对比脚本与参数**
- `/home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh` 当前默认 `CPU_LIMIT=1`、`MEM_LIMIT_MB=300`，用于单核/300MB 内存限制的 ext4 randomread 对比。
- `/home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh` 当前默认 `CPU_LIMIT=2`、`MEM_LIMIT_MB=300`、`TIMEOUT_SEC=30`，用于当前缩小版 `fileserver.f` 的 ext4 smoke 对比。
- 需要把 ShaOFS WML 参数、ext4 WML 参数、cgroup CPU/memory、Junction config 的 core 数固定到同一实验记录中。
- 每次 ext4 测试前确认 `/home/syh/mkfs/reset_ext4.sh` 成功格式化并挂载目标盘，避免拿旧数据或 page cache 结果做对比。

**Task 14: 清理或纳入未跟踪工作区文件**
- 当前 benchmark patch、Filebench WML 和 ShaOFS 报告文件大量处于 untracked 状态。
- 接手者应先决定哪些是正式资产，哪些只是临时实验输出，再统一加入版本控制或清理；不要盲目删除用户可能仍需要的实验文件。

**Task 15: 完整验证 inode 上限**
- 扩展 `test_many_inodes.c` 或新增测试，创建接近 `INODENUM=32768` 的 inode，记录成功数量和耗尽时错误码。
- 覆盖 clean shutdown 后 remount，再随机读取这些文件，确认 inode bitmap、inode table、dentry/path lookup 与 journal repair 不引入不一致。

**Task 16: 继续审计 FS base / TLS 入口**
- 当前 ShaOFS `RuntimeFSBaseGuard` 已处理 guard 内 yield 的问题。
- 仍需检查 Junction 其他 runtime libc 调用点、lazy binding、signal trampoline、interrupt/syscall entry 等是否存在未 guard 或 guard 后可能 yield 的路径。
- 如果新增可 yield 的 runtime-FS 区域，应复用 `runtime_fsbase_depth`，而不是使用会长时间禁用抢占的 `RuntimeLibcGuard`。

**Task 17: 固化 FxMark 正式 benchmark 脚本**
- 当前只手动验证了 DRBL `--ncore 1/2/4/8` 能跑完；建议把 `toggle_fxmark.sh apply`、重新 mkfs、启动 IOKernel、`timeout` 运行 FxMark、清理 IOKernel 的流程写成脚本。
- 正式实验应同时记录 `caladan_test.config` 中的 `runtime_kthreads` / `runtime_spinning_kthreads`、`runtime_quantum_us`、ShaOFS 构建开关、FxMark patch 状态、完整 stdout/stderr 和退出码。
- 如果要报告多核扩展性，必须先提供多 runtime kthread 配置，并确认 FxMark pthread worker 真正分布到多个 Caladan kthread/core 上。

**Task 18: 扩展 FxMark workload 兼容性验证**
- 当前 patch 中只有 DRBL 被改成 self-timed loop；其他 FxMark workload 仍可能依赖 `SIGALRM` 或遇到 Junction 不支持的 syscall。
- 后续可逐个验证常用 FxMark 类型，遇到失败时优先在 FxMark 适配层绕过不支持机制，不要直接修改 Junction，除非确认是 Junction bug。
- 需要注意 FxMark 当前 pthread 降级模型对原始 process-based 语义的影响，尤其是共享地址空间、共享全局变量和资源统计。

---

## 第十章：文件修改历史总览

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `shaofs/group.cc` | Bug fix + perf | 移除热路径 log_info；`alloc_block` 中 write_access 移到 kguard 之前 |
| `shaofs/file.cc` | Perf + feature | 拆分 file_write 锁范围；新增 truncate_inode + free_inode_data_blocks；新增 file_read/write_direct |
| `shaofs/file.h` | Feature | 新增 file_read/write_direct, truncate_inode 声明 |
| `shaofs/inode.h` | Perf + refactor | MInode 新增 extent_hint、hint_lock 和 has_dirty_data_cache；dir_mtx 从 mutex_t 升级为 rwmutex_t |
| `shaofs/inode.cc` | Bug fix | `alloc_inum()` 按 `INODENUM=32768` 全范围环形扫描 inode bitmap，避免把 inode cache 容量 8192 误当作 inode 上限 |
| `shaofs/extent.cc` | Perf + bug fix | inode_bmap_locked 增加 hint fast path；普通文件 EOF append 路径加入 16/64/128 blocks 批量预分配、预清零和失败回滚，缓解 Filebench append-heavy extent 溢出 |
| `shaofs/inodeCache.cc` | Bug fix + feature | ic_free_inode 实现完整块释放；新增 ic_flush_inode |
| `shaofs/inodeCache.h` | Feature | 新增 ic_flush_inode 声明 |
| `shaofs/dir.h` | Refactor | 新增 dirent_is_empty()；移除未实现的 _locked 声明 |
| `shaofs/dir.cc` | Refactor + bug fix | 全面重写：DirReadGuard/DirWriteGuard + rwmutex；dir_foreach_locked；dirent_is_empty |
| `shaofs/syscall.h` | Feature | 新增 my_fstat, my_newfstatat, my_fsync |
| `shaofs/syscall.cc` | Feature + perf | 实现 fstat/newfstatat/fsync；移除热路径日志；my_read/write 支持 direct 分派 |
| `shaofs/blockCache.h` | Feature + fix | NVMeSSD 后端改用 DMA_read/write_block；新增 bc_flush_block |
| `shaofs/blockCache.cc` | Feature | 新增 bc_flush_block；新增 bc_invalidate_block 用于 direct write 后失效旧 cache entry；CRASH_CONSISTENCY=1 时 metadata block 写回走 journal |
| `generic_cache/cache.h` | Feature | 新增 flush_entry(key) |
| `generic_cache/sharded_cache.h` | Feature | 转发 flush_entry |
| `junction/fs/file.cc` | Feature | usys_read/write/pread64/pwrite64 传递 O_DIRECT flag；usys_fstat/newfstatat/fsync 添加 SHAOFS dispatch；newfstatat 使用 SHAOFS_REALPATH |
| `junction/fs/file.cc` | Feature | 新增 `usys_sync()`，调用 `shaofs_sync_all()` 全局刷写 ShaOFS 脏状态 |
| `junction/fs/core.cc` | Feature + path fix | openat/mkdir 使用 SHAOFS_REALPATH，兼容 `FSHAO/path` 与 `FSHAO:/path` |
| `junction/CMakeLists.txt` | Build | 新增 `SHAOFS_IO_PREEMPT` option，开启时定义 `IO_PREEMPT=1` |
| `lib/caladan/runtime/softirq.c` | User fix | 恢复 timer soft interrupt 处理（修复 sleep/barrier）；storage softirq pending 时使用 runqueue head insertion |
| `fs/mytest/*.c` | New | 15+ 测试/工具/基准测试程序 |
| `junction/fs/shaofs/dsa.cc` | Perf + fallback | 当前实现使用 DML 硬件路径、Caladan `runtime_async_park` 和 per-thread tcache；硬件/提交失败时回退 CPU memcpy |
| `junction/fs/shaofs/dsa.h` | Feature | 暴露 `dsa_init`、`dsa_copy`、`dsa_copyv` 和 `ShaofsDsaOptions`；`dsa_batch_task_num=32` |
| `junction/fs/CMakeLists.txt` | Build | 当前查找静态 `libdml.a` 和 `dml/dml.h`，找不到会 FATAL |
| `junction/fs/CMakeLists.txt` | Build | 新增 `SHAOFS_CRASH_CONSISTENCY` option，默认 ON，并把 `shaofs/journal.cc` 纳入 fs library |
| `lib/CMakeLists.txt` | Build fix | Caladan `shared.mk` 查询改用 `make --no-print-directory` 并 strip trailing whitespace，避免 nested make 输出污染 linker flags |
| `shaofs/fs.h` | Crash consistency | SuperBlock 新增 `journal_blockstart` / `journal_blocknum`；新增 `CRASH_CONSISTENCY` 和 `DEFAULT_JOURNAL_BLOCKS` |
| `shaofs/fs.cc` | Crash consistency | mount 时执行 `journal_init()` / `journal_recover()` / `journal_mark_dirty()`，并扫描目录 extents 建立 metadata map |
| `shaofs/journal.h` | New | journal API 和 `CRASH_CONSISTENCY=0` no-op fallback |
| `shaofs/journal.cc` | New | metadata-only redo journal、dirty mount marker、transaction replay、dirty repair |
| `shaofs/file.cc` | Crash consistency | final_flush 通过 journal 写 imap，clean shutdown 清 dirty marker；目录块分配后登记为 metadata block |
| `shaofs/file.cc` / `shaofs/file.h` | Feature | 新增 `shaofs_sync_all()`；与 `final_flush()` 共用 `flush_all_dirty_state()`，但 runtime `sync()` 不清 dirty marker |
| `shaofs/group.cc` | Crash consistency | GDT sync 改为 `journal_write_metadata()` |
| `/home/syh/mkfs/fs.h` | Crash consistency | mkfs 侧 SuperBlock 同步新增 journal 字段和 `DEFAULT_JOURNAL_BLOCKS` |
| `/home/syh/mkfs/mkfs.c` | Crash consistency | mkfs 在盘尾预留并清空 journal 区，data group 只使用 journal 前空间 |
| `junction/fs/mytest/journal_layout_probe.c` | New test | 验证 journal superblock 布局可 mount |
| `junction/fs/mytest/journal_recovery_prepare.c` | New test | 构造崩溃前文件/目录状态，`--crash` 配合 SIGKILL |
| `junction/fs/mytest/journal_recovery_check.c` | New test | 验证 dirty mount repair 后目录、文件大小和数据内容 |
| `junction/fs/mytest/benchmark/fio/filesetup.c` | FIO adapter | 补丁后识别 `FSHAO/`、`FSHAO:/`，并容忍 ShaOFS 上 `ftruncate` 不支持 |
| `junction/fs/mytest/benchmark/fio/helper_thread.c` | FIO adapter | 补丁后 `timerfd_create/settime` 失败不再 assert，回退 select timeout |
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
| `junction/fs/mytest/benchmark/patch/example.f` | Benchmark config | 当前用于 Junction/ShaOFS 的 Filebench 示例 workload |
| `junction/fs/mytest/benchmark/fxmark/Makefile` | FxMark adapter | 补丁后增加 `-pthread`，支持 pthread worker 模型 |
| `junction/fs/mytest/benchmark/fxmark/src/bench.c` | FxMark adapter | 补丁后用 `pthread_create()` 代替 `fork()` 创建 worker；屏障等待使用 `sched_yield()` |
| `junction/fs/mytest/benchmark/fxmark/src/DRBL.c` | FxMark adapter | 补丁后 DRBL worker 自己按 wall-clock deadline 结束，避免依赖 `SIGALRM` 及时投递 |
| `junction/fs/mytest/benchmark/fxmark/src/util.c` | FxMark adapter | 补丁后 `mkdir_p()` 使用进程内递归 `mkdir()`，并兼容 `FSHAO` / `FSHAO:` 根别名 |
| `junction/fs/mytest/benchmark/patch/fxmark_changes.patch` | Handover artifact | 保存 FxMark Junction 适配源码补丁 |
| `junction/fs/mytest/benchmark/patch/toggle_fxmark.sh` | Tooling | 一键 apply/revert FxMark 补丁，并自动 `make -j $(nproc)` |
| `junction/fs/mytest/test_sync_syscall.c` | New test | 最小 `sync()` syscall smoke test，写入 ShaOFS 文件后调用 `syscall(SYS_sync)` 并检查返回值 |
| `junction/fs/mytest/benchmark/filebench_wml/shaofs_randomread*.f` | Benchmark config | 从 Filebench `workloads/randomread.f` 派生的 ShaOFS randomread workload，当前为 untracked 工作区文件 |
| `junction/fs/mytest/benchmark/filebench_wml/fileserver.f` | Benchmark config | 当前 ShaOFS fileserver smoke workload：`FSHAO:`、40 files、1 thread、2s runtime；2026-05-13 已在 ShaOFS/ext4 上跑通 smoke 对比 |
| `junction/fs/mytest/benchmark/filebench_wml/webserver.f` | Benchmark config | 当前 ShaOFS webserver workload：`FSHAO:`、1000 files、100 threads、60s runtime；2026-05-13 已在 Junction/ShaOFS 上跑通 |
| `/home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh` | Benchmark tooling | repo 外部 ext4 randomread 对比脚本，包含 ext4 reset、cgroup v2 CPU/memory 限制和 Filebench 运行 |
| `/home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh` | Benchmark tooling | repo 外部 ext4 fileserver 对比脚本，包含 ext4 reset、临时 WML `$dir` 替换、cgroup v2 CPU/memory 限制、log/CSV 输出 |
| `lib/caladan/inc/base/syscall.h` / `lib/caladan/base/syscall.S` | User DMA support | Caladan wrapper 当前包含 `syscall_mlock()`，供 `storage_prepare_user_dma()` pin 用户页 |
| `junction/syscall/seccomp.cc` | User DMA support | seccomp allowlist 当前包含 Caladan `mlock`，并按 request 放行 VFIO DMA map/unmap ioctl |
| `lib/caladan/runtime/storage.c` | User DMA support | 新增 user DMA registration cache、`storage_prepare_user_dma()`、`storage_read_aligned()`、`storage_write_user_dma()` |
| `lib/caladan/inc/runtime/storage.h` | User DMA support | 声明 user-buffer DMA 相关 storage API |
| `junction/fs/file.h` | User DMA support | `File` 内嵌 `DirectReadHint`，供 ShaOFS O_DIRECT read fast path 使用 |
| `junction/fs/core.cc` | User DMA support | ShaOFS O_DIRECT open 时调用 `file_prepare_direct_read_hint()` |
| `junction/fs/file.cc` | User DMA support | ShaOFS O_DIRECT read/pread 优先使用 `file_read_direct_hint()`；write 时使 hint invalid |
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
| `lib/caladan/runtime/sched.c` | FS base fix | 调度器用 `thread_save_fsbase()` / `thread_fsbase_to_run()` 区分用户 FS base 与 runtime FS base，修复 ShaOFS guard 内 park 污染 TLS |
| `junction/fs/shaofs/utili.h` | FS base fix | `RuntimeFSBaseGuard` 更新 `runtime_fsbase_depth`，只在切换窗口短暂禁用抢占，guard 内允许 yield |
| `junction/fs/mytest/test_many_inodes.c` | New test | 验证创建大量 inode 能越过 inode cache 容量 |
| `junction/fs/mytest/test_many_inodes_read_threads.c` | New test | 大量小文件创建后多 pthread 读整文件并校验数据 |

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

- 2026-05-09 crash consistency 交接整理时重新检查：`HANDOVER.md` 当前是 tracked modified 文件，不是 untracked 文件。
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

---

## 第十三章：2026-05-09 crash consistency 交接整理验证记录

### 13.1 本次实际检查过的内容

```bash
sed -n '1,260p' HANDOVER.md
sed -n '260,620p' HANDOVER.md
sed -n '620,1040p' HANDOVER.md
sed -n '1040,1420p' HANDOVER.md
find junction/fs/shaofs -maxdepth 1 -type f | sort
find junction/fs/mytest -maxdepth 1 -type f | sort
sed -n '1,180p' junction/fs/CMakeLists.txt
sed -n '1,180p' junction/CMakeLists.txt
grep -n 'SHAOFS_IO_PREEMPT\|SHAOFS_CRASH_CONSISTENCY' build/CMakeCache.txt
nl -ba junction/fs/shaofs/journal.h | sed -n '1,140p'
nl -ba junction/fs/shaofs/journal.cc | sed -n '1,260p'
nl -ba junction/fs/shaofs/journal.cc | sed -n '260,620p'
nl -ba junction/fs/shaofs/fs.cc | sed -n '1,110p'
nl -ba junction/fs/shaofs/blockCache.cc | sed -n '1,120p'
nl -ba junction/fs/shaofs/file.cc | sed -n '60,95p;300,325p;532,552p'
nl -ba junction/fs/shaofs/group.cc | sed -n '318,335p'
nl -ba /home/syh/mkfs/mkfs.c | sed -n '78,140p;180,230p'
git -C /home/syh/MyProj1/junction status --short HANDOVER.md
git -C /home/syh/MyProj1/junction diff --check -- <current crash-consistency files>
```

### 13.2 本次实际运行过的构建和测试

本轮 crash consistency 开发和交接整理中运行过以下关键命令和测试：

```bash
cmake -S . -B build -DSHAOFS_CRASH_CONSISTENCY=OFF
cmake --build build --target junction_run -- -j$(nproc)
cmake -S . -B build -DSHAOFS_CRASH_CONSISTENCY=ON
cmake --build build --target junction_run -- -j$(nproc)

gcc -O2 junction/fs/mytest/journal_layout_probe.c -o build/junction/mytest/journal_layout_probe -lpthread
gcc -O2 junction/fs/mytest/journal_recovery_prepare.c -o build/junction/mytest/journal_recovery_prepare -lpthread
gcc -O2 junction/fs/mytest/journal_recovery_check.c -o build/junction/mytest/journal_recovery_check -lpthread
gcc -O2 junction/fs/mytest/test_fsync.c -o build/junction/mytest/test_fsync -lpthread
gcc -O2 junction/fs/mytest/test_dir.c -o build/junction/mytest/test_dir -lpthread
gcc -O2 junction/fs/mytest/bench_seq_rw.c -o build/junction/mytest/bench_seq_rw -lpthread
gcc -O2 junction/fs/mytest/4KB_iops.c -o build/junction/mytest/4KB_iops -lpthread

cd /home/syh/mkfs && printf 'syh2syh\n' | sudo -S bash ./mkfs.sh
printf 'syh2syh\n' | sudo -S lib/caladan/iokerneld ias
```

已验证：

- `SHAOFS_CRASH_CONSISTENCY=OFF` 构建成功。
- `SHAOFS_CRASH_CONSISTENCY=ON` 构建成功，最终 `build/CMakeCache.txt` 当前为 `SHAOFS_CRASH_CONSISTENCY:BOOL=ON`。
- mkfs 输出 `Journal: start=234419030 blocks=4096`。
- `journal_layout_probe` 可以正常 mount 带 journal 字段的新 superblock。
- `timeout -s KILL 2s ./junction_run ... journal_recovery_prepare --crash` 以退出码 137 模拟非 clean shutdown。
- 下一次运行 `journal_recovery_check` 时 mount 阶段输出 `[journal] previous mount was dirty, repairing metadata state`，检查结果 `journal recovery check failures=0`。
- `test_fsync` 通过：`32 passed, 0 failed`。
- `test_dir` 通过：`22 passed, 0 failed`。
- `bench_seq_rw 32` 最终结果：写 `64.29 MB/s`，读 `9467.46 MB/s`，数据校验 PASS。
- `4KB_iops 4 FSHAO:/base_iops 1048576 100 0 20000 0` 最终结果：`9438393.29 IOPS`，`36868.72 MB/s`。
- 测试结束后已执行 `pkill -9 iokerneld`，并用 `pgrep -a iokerneld` / `pgrep -a junction_run` 确认没有残留进程。

### 13.3 本次确认的客观状态

- `junction/fs/CMakeLists.txt` 当前包含 `option(SHAOFS_CRASH_CONSISTENCY ... ON)`，并将 `shaofs/journal.cc` 纳入 `fs` library。
- `junction/fs/shaofs/fs.h` 当前包含 `CRASH_CONSISTENCY` 默认值、`DEFAULT_JOURNAL_BLOCKS=4096`、`SuperBlock::journal_blockstart` 和 `SuperBlock::journal_blocknum`。
- `/home/syh/mkfs/fs.h` 和 `/home/syh/mkfs/mkfs.c` 已同步 journal 布局；`mkfs.c` 会把盘尾 4096 blocks 预留并清零。
- `lib/CMakeLists.txt` 的 `make -f shared.mk print-*` 查询已加 `--no-print-directory` 和 `OUTPUT_STRIP_TRAILING_WHITESPACE`，这是为修复当前环境下 linker flags 被 nested make 输出污染的问题。
- 顶层工作区仍存在大量既有 modified/untracked 文件；本次 crash consistency 开发新增/修改了 ShaOFS journal 相关文件和测试，但不要把所有 dirty 文件都归因于本次修改。
- `/home/syh/mkfs` 目录不是顶层 Junction git 的一部分；其中 `fs.h` / `mkfs.c` 是本轮 crash consistency 需要同步维护的外部格式化工具文件。

### 13.4 本次未重新验证的内容

- 未重新运行完整 `scripts/build.sh`，只用 CMake build 了 `junction_run` target。
- 未重新运行完整 ShaOFS 单元测试套件，只回归了 `test_fsync`、`test_dir` 和 crash recovery 测试。
- 未重新运行 FIO target benchmark。
- 未重新跑 ext4 对比测试。
- 未构造 torn transaction header、坏 checksum、journal image 损坏、home block 部分写等细粒度崩溃点；当前 crash 测试是进程级 `SIGKILL` dirty mount 场景。
- 未验证 `CRASH_CONSISTENCY=OFF` 的运行时行为，仅验证了它可以成功编译。

---

## 第十四章：2026-05-10 Filebench / inode / FS base 交接整理验证记录

### 14.1 本次实际检查过的内容

```bash
sed -n '1,1500p' HANDOVER.md
sed -n '1,220p' lib/caladan/inc/runtime/thread.h
sed -n '1,150p;560,625p;800,850p' lib/caladan/runtime/sched.c
sed -n '60,115p' junction/fs/shaofs/utili.h
sed -n '1,110p' junction/bindings/runtime.h
sed -n '1,180p' junction/fs/shaofs/inode.cc
sed -n '1,220p' junction/fs/shaofs/inodeCache.h
sed -n '1,280p' junction/fs/shaofs/inodeCache.cc
sed -n '1,130p' junction/fs/shaofs/fs.h
sed -n '1,220p' /home/syh/mkfs/fs.h
sed -n '70,150p;180,240p' /home/syh/mkfs/mkfs.c
sed -n '1,220p' junction/fs/mytest/benchmark/patch/toggle_filebench.sh
sed -n '1,620p' junction/fs/mytest/benchmark/patch/filebench_changes.patch
sed -n '1,240p' junction/fs/mytest/benchmark/patch/example.f
sed -n '1,260p' junction/fs/mytest/test_many_inodes.c
sed -n '1,300p' junction/fs/mytest/test_many_inodes_read_threads.c
sed -n '1,220p' junction/fs/mytest/benchmark/filebench/procflow.h
sed -n '1,220p' junction/fs/mytest/benchmark/filebench/procflow.c
sed -n '1,180p' junction/fs/mytest/benchmark/filebench/aslr.c
git -C /home/syh/MyProj1/junction status --short
git -C /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench status --short
sed -n '/SHAOFS_IO_PREEMPT/,+2p;/SHAOFS_CRASH_CONSISTENCY/,+2p' build/CMakeCache.txt
```

### 14.2 本轮会话中已运行过的关键构建和测试

以下记录来自 2026-05-10 本轮开发调试过程；本次最后的文档整理阶段没有重新跑这些长测试。

已运行并确认过的关键项：

- `toggle_filebench.sh apply` 可以应用 Filebench patch 并重新构建 Filebench。
- ShaOFS 重新构建过 `junction_run`，最终 `build/CMakeCache.txt` 中 `SHAOFS_IO_PREEMPT=ON`、`SHAOFS_CRASH_CONSISTENCY=ON`。
- `test_many_inodes` 创建 10000 个小文件通过，用于验证 inode 分配越过 8192 cache capacity。
- `test_many_inodes_read_threads` 创建 10000 个 16KB 文件后，用 3 个 pthread 循环读整文件并校验内容通过。
- Filebench `example.f` 在 Junction 上跑通 60s，没有 timeout 或崩溃；示例输出约为 `1324058 ops/s`、`6.9GB/s`。
- 测试过程中使用 `timeout` 包裹 `junction_run`，并在结束后清理 IOKernel。

### 14.3 本次确认的客观状态

- `lib/caladan/inc/runtime/thread.h` 当前 `thread_t` 包含 `runtime_fsbase_depth`。
- `lib/caladan/runtime/sched.c` 当前存在 `thread_save_fsbase()` 和 `thread_fsbase_to_run()`；park 路径调用 `thread_save_fsbase()`，resume 路径调用 `thread_fsbase_to_run()`。
- `junction/fs/shaofs/utili.h:RuntimeFSBaseGuard` 当前只在 FS base 切换窗口短暂禁用抢占，并维护 `runtime_fsbase_depth`。
- `junction/bindings/runtime.h:RuntimeLibcGuard` 仍是整个 guard 生命周期内禁用抢占的短调用 guard；不要把它直接用于 ShaOFS 可 yield 路径。
- `junction/fs/shaofs/inode.cc:alloc_inum()` 当前按 `INODENUM` 全范围扫描 inode bitmap，`DEFAULT_INODECACHE_CAPACITY=8192` 不再限制可分配 inode 总数。
- `junction/fs/shaofs/fs.h` 当前 `MYPREFIX` 仍是 `"FSHAO"`，`SHAOFS_REALPATH()` 兼容可选冒号。
- `junction/fs/mytest/benchmark/patch/filebench_changes.patch` 当前是 Filebench 适配的唯一补丁载体；不要直接修改 Filebench 子仓库后忘记更新该 patch。
- Filebench 子仓库当前显示多个 modified 文件是预期的 patch applied 状态；同时还存在 autotools/configure/build 生成文件，不能简单把整个子仓库 dirty 状态都当作源码改动。

### 14.4 本次未重新验证的内容

- 未重新运行完整 `scripts/build.sh`。
- 未重新运行完整 ShaOFS 单元测试套件。
- 未重新运行 FIO target benchmark。
- 未重新运行 Filebench benchmark；本章记录的是本轮会话中此前跑通过的结果。
- 未完整创建到 `INODENUM=32768` 的 inode 耗尽边界。
- 未验证 Filebench 适配对所有 workload 类型的兼容性；当前只确认 `example.f` 这类读 whole-file workload 可以跑通。
- 未进一步审计 Junction 所有 runtime libc / lazy binding / signal trampoline / syscall entry 的 FS base 策略；当前确认的是 ShaOFS guard 内 yield 场景的修复。

---

## 第十五章：2026-05-12 user-buffer DMA / Filebench randomread / syscall 交接整理验证记录

### 15.1 本次实际检查过的内容

```bash
sed -n '1,260p' HANDOVER.md
sed -n '260,620p' HANDOVER.md
sed -n '620,1040p' HANDOVER.md
sed -n '1040,1480p' HANDOVER.md
sed -n '1540,1745p' HANDOVER.md
sed -n '1745,2245p' HANDOVER.md
git -C /home/syh/MyProj1/junction status --short
git -C /home/syh/MyProj1/junction status --short --untracked-files=all junction/fs/mytest/benchmark/filebench_wml
git -C /home/syh/MyProj1/junction status --short --untracked-files=all junction/fs/mytest/benchmark/patch junction/fs/shaofs/OPTIMIZATION_REPORT.md junction/fs/shaofs/IOPS_BENCHMARK_REPORT.md
git -C junction/fs/mytest/benchmark/filebench status --short
git -C junction/fs/mytest/benchmark/filebench diff --stat
git -C junction/fs/mytest/benchmark/fio status --short
git -C junction/fs/mytest/benchmark/fio diff --stat
sed -n '1,860p' junction/fs/shaofs/file.cc
sed -n '1,180p' junction/fs/shaofs/file.h
sed -n '1,220p' junction/fs/shaofs/syscall.cc
sed -n '1,180p' junction/fs/shaofs/inode.h
sed -n '1,180p' junction/fs/shaofs/inode.cc
sed -n '1,520p' lib/caladan/runtime/storage.c
sed -n '1,160p' lib/caladan/inc/runtime/storage.h
sed -n '1,220p' junction/syscall/seccomp.cc
sed -n '1,220p' lib/caladan/inc/base/syscall.h
sed -n '1,140p' lib/caladan/base/syscall.S
sed -n '1,360p' junction/fs/file.cc
sed -n '1,360p' junction/fs/file.h
sed -n '460,620p' junction/fs/core.cc
sed -n '1,160p' junction/fs/mytest/benchmark/filebench_wml/shaofs_randomread_direct_16t.f
sed -n '1,160p' junction/fs/mytest/benchmark/filebench_wml/shaofs_randomread_count_8t.f
sed -n '1,160p' junction/fs/mytest/benchmark/filebench_wml/shaofs_randomread.f
sed -n '1,260p' /home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh
sed -n '/SHAOFS_IO_PREEMPT/p;/SHAOFS_CRASH_CONSISTENCY/p' build/CMakeCache.txt
```

### 15.2 本次确认的客观状态

- `file_read_direct()` / `file_write_direct()` 当前调用 `storage_prepare_user_dma()`，并在实际 I/O 中调用 `storage_read_aligned()` / `storage_write_user_dma()`；旧文档中“direct I/O 仍总是经过 SPDK bounce buffer memcpy”的描述已不符合当前代码。
- user-buffer DMA 合约由 `user_dma_request_ok()` 强制：`buf` 2MB 对齐、`len` 是 2MB 倍数、`offset` 4KB 对齐。
- `storage_prepare_user_dma()` 当前使用 `syscall_mlock()`、`spdk_mem_register()`、`spdk_vtophys()` 和 2MB 页粒度 cache。
- `junction/syscall/seccomp.cc` 当前 allowlist 包含 `ALLOW_CALADAN_SYSCALL(mlock)`，并按 request 放行 `VFIO_IOMMU_MAP_DMA` 和 `VFIO_IOMMU_UNMAP_DMA`。
- `junction/fs/core.cc:usys_openat()` 在 ShaOFS O_DIRECT open 时调用 `file_prepare_direct_read_hint()`；`junction/fs/file.cc` 在 direct read/pread 时优先使用 `file_read_direct_hint()`，write 时使 hint invalid。
- Filebench patch 当前覆盖 9 个源文件，包括 `flag.h`；旧文档中“覆盖 8 个源文件”的说法已修正。
- `junction/fs/mytest/benchmark/filebench_wml/shaofs_randomread*.f` 当前存在但为 untracked 工作区文件。
- `/home/syh/fs_test/scripts/run_ext4_filebench_randomread_cgroup.sh` 当前存在于 repo 外部，默认使用 1 核、300MB 内存、direct I/O 和 Filebench randomread 形态。
- 当前 `build/CMakeCache.txt` 中 `SHAOFS_IO_PREEMPT:BOOL=ON`，`SHAOFS_CRASH_CONSISTENCY:BOOL=ON`。
- 本次整理中有多条 `find` / `ls` / `rg` 命令因当前 sandbox 的 `bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted` 失败；随后使用 `git status`、`sed` 和定点文件读取完成了必要核对。

### 15.3 本次未重新验证的内容

- 未重新运行 `scripts/build.sh` 或 CMake build。
- 未启动 IOKernel。
- 未重新运行 ShaOFS / Filebench / FIO / ext4 benchmark。
- 未运行最小 user-buffer DMA 正确性测试；因此本次只确认代码路径存在，未确认当前机器运行时一定成功打印 `storage: enabled direct DMA into user buffers`。
- 虽然已确认当前 build cache 两个 ShaOFS 开关都是 ON，接手者做正式实验前仍应重新 `cmake -S . -B build ...` 固化开关，避免继承旧 cache 状态。
- 未确认 Filebench `directio=1` 是否满足 ShaOFS 当前 2MB user-buffer DMA 合约；正式 randomread 测试前应单独验证。

---

## 第十六章：2026-05-13 Filebench fileserver/webserver 与 ext4 对比交接整理验证记录

### 16.1 本次实际检查过的内容

```bash
sed -n '1,140p' HANDOVER.md
rg -n 'Extent 数组|Filebench|alloc_blocks|webserver|fileserver|TODO|验证' HANDOVER.md
git -C /home/syh/MyProj1/junction status --short
git -C junction/fs/mytest/benchmark/filebench diff --stat
git -C junction/fs/mytest/benchmark/filebench status --short
sed -n '1,220p' junction/fs/shaofs/extent.cc
sed -n '220,390p' junction/fs/shaofs/extent.cc
sed -n '1,140p' junction/fs/mytest/benchmark/filebench_wml/fileserver.f
sed -n '1,160p' junction/fs/mytest/benchmark/filebench_wml/webserver.f
sed -n '1,220p' junction/fs/mytest/benchmark/filebench/fb_cvar.c
sed -n '1,220p' /home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh
bash -n /home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh
git -C /home/syh/MyProj1/junction diff --check -- HANDOVER.md
git -C /home/syh/MyProj1/junction diff --stat -- HANDOVER.md
```

本次文档整理过程中，部分普通只读命令仍可能被 sandbox 的 `bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted` 拦截；必要时已改用已批准的只读命令或提权只读检索完成核对。

### 16.2 本轮会话中已运行过的关键构建和测试

以下是 2026-05-13 本轮 Filebench 调试/验证阶段实际运行过并记录到的关键结果；不是本次最后文档编辑阶段重新跑出的长测试。

已运行并确认过的关键项：

- 修改 `junction/fs/shaofs/extent.cc` 后执行过 `cmake --build build --target junction_run -- -j$(nproc)`，构建成功；仍只有既有 objpool 相关 warning。
- 执行过 `/home/syh/mkfs/mkfs.sh` 重新格式化 ShaOFS 测试盘。
- 启动过 `sudo lib/caladan/iokerneld ias`，并在测试结束后用 `pkill -9 iokerneld` 清理。
- ShaOFS `fileserver.f` 使用 `timeout 20s ./junction_run ... filebench -f .../fileserver.f` 跑通，结果：

```text
IO Summary: 1979495 ops 989422.475 ops/s 32981/692595 rd/wr 579.4mb/s 0.001ms/op
```

- ext4 `fileserver.f` 使用 `/home/syh/fs_test/scripts/run_ext4_filebench_fileserver_cgroup.sh` 跑通，结果：

```text
IO Summary: 1254505 ops 627211.104 ops/s 20907/439050 rd/wr 367.4mb/s 0.001ms/op
Cgroup: cpu_usage_usec_delta=1953362 mem_peak_bytes=314572800 mem.high_delta=0 mem.max_delta=694 mem.oom_delta=0
```

- ShaOFS `webserver.f` 首次用 `timeout 20s` 运行时退出码为 124；结合 WML 中 `run 60` 判断为 timeout 太短。
- ShaOFS `webserver.f` 使用 `timeout 90s ./junction_run ... filebench -f .../webserver.f` 完整跑通，结果：

```text
IO Summary: 40842810 ops 680679.092 ops/s 219574/21957 rd/wr 3412.3mb/s 0.145ms/op
```

测试结束后已清理 IOKernel；本轮最后一次 ShaOFS Filebench 运行后，测试盘最后状态可视为 ShaOFS/SPDK 场景。若接下来要跑 ext4，应先执行 `/home/syh/mkfs/reset_ext4.sh`。

### 16.3 本次确认的客观状态

- `junction/fs/shaofs/extent.cc` 当前包含 EOF append 预分配逻辑：`should_prealloc_append()`、`append_prealloc_blocks()`、`bmap_prealloc_append()`、`zero_fresh_blocks()`、`discard_preallocated_blocks()` 等。
- 预分配只在普通文件 append 到当前 EOF 后第一个 logical block 时触发；目录、稀疏远距离写和非 append 场景仍使用原单块分配 fallback。
- `fileserver.f` 当前是缩小版 ShaOFS smoke workload：`FSHAO:`、40 files、1 thread、4KB file/io、`run 2`、`appendfilerand iters=20`。
- `webserver.f` 当前使用 `set $dir=FSHAO:`，不是 upstream 默认 `/tmp`；参数为 1000 files、100 threads、`iosize=1m`、`meanappendsize=16k`、`run 60`。
- Filebench patch 当前覆盖 9 个源文件；`fb_cvar.c` 包含 build-directory fallback，可从可执行文件所在目录的 `cvars/.libs` 加载 cvar 插件。
- Filebench 子仓库当前处于 patch-applied dirty 状态，`git diff --stat` 显示 9 个源码文件修改，同时存在 autotools/configure/build 生成文件。这个状态是 `toggle_filebench.sh apply` 后的预期结果，但源码修改仍应通过 `filebench_changes.patch` 管理。
- 顶层 Junction 工作区仍存在既有 modified/untracked 文件。本次交接整理前后都不应假设所有 dirty 文件来自当前 Agent；当前重点 dirty 文件包括 `HANDOVER.md`、`junction/fs/shaofs/extent.cc`，以及既有的 `junction/fs/mytest/iops.c`、`junction/fs/mytest/testwholepath.c` 和大量 untracked benchmark/test 文件。

### 16.4 本次未重新验证的内容

- 最后文档整理阶段没有重新运行 `scripts/build.sh`、CMake build、IOKernel 或长 benchmark；长测试结果来自本轮前面的实际调试运行。
- 未运行 `webserver.f` 的 ext4 对比脚本，也未编写对应 ext4 webserver cgroup 脚本。
- 未重新运行 Filebench randomread、FIO、ShaOFS 全量单元测试或完整 inode 耗尽测试。
- 未做多轮重复实验、置信区间统计、CPU cycle/perf 采样、NVMe 设备端带宽计数或 `DIRECTPATH DISABLED` 根因确认。
- 未确认当前 `fileserver.f` 缩小参数是否适合作为论文最终 workload；当前只适合作为跑通和 smoke 对比。

---

## 第十七章：2026-05-15 sync syscall 与 FxMark 交接整理验证记录

### 17.1 本次实际检查过的内容

```bash
sed -n '1,2420p' HANDOVER.md
git status --short
rg -n 'usys_sync|shaofs_sync_all|flush_all_dirty_state|sync_all|fdatasync|fsync' \
  junction/fs junction/kernel junction/syscall -g'*.[ch]' -g'*.cc' -g'*.h'
sed -n '600,635p' junction/fs/file.cc
sed -n '70,115p' junction/fs/shaofs/file.cc
sed -n '1,80p' junction/fs/shaofs/file.h
rg -n '^sync|fdatasync|fsync' junction/syscall/usys.txt junction/kernel/usys.h
sed -n '130,255p' junction/fs/shaofs/generic_cache/cache.h
sed -n '1,260p' junction/fs/mytest/benchmark/patch/fxmark_changes.patch
sed -n '1,220p' junction/fs/mytest/benchmark/patch/toggle_fxmark.sh
sed -n '1,180p' junction/fs/mytest/test_sync_syscall.c
sed -n '1,120p' build/junction/caladan_test.config
git status --short \
  junction/fs/mytest/benchmark/patch/fxmark_changes.patch \
  junction/fs/mytest/benchmark/patch/toggle_fxmark.sh \
  junction/fs/mytest/test_sync_syscall.c \
  HANDOVER.md
```

部分普通只读命令仍可能被当前 sandbox 的 `bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted` 拦截；必要时使用已批准的只读/写入命令完成核对。

### 17.2 本轮会话中已运行过的关键构建和测试

以下记录来自 2026-05-15 前后本轮开发调试过程；本次最后的文档整理阶段没有重新启动 IOKernel 或重新跑长 benchmark。

已运行并确认过的关键项：

- 修改 Junction syscall table 后重新构建过 `junction_run`，`sync` 已不再报 unsupported syscall。
- 编译过 `junction/fs/mytest/test_sync_syscall.c` 到 `build/junction/mytest/test_sync_syscall`。
- 在 Junction/ShaOFS 中运行 `test_sync_syscall`，观察到：

```text
sync ret=0 errno=0 (Success)
```

- `toggle_fxmark.sh apply` 可以应用 `fxmark_changes.patch` 并执行 `make -j "$(nproc)"` 构建 FxMark。
- `toggle_fxmark.sh revert` 可以反向应用同一 patch 并重新构建；随后再次 `apply` 成功。
- 启动过 `sudo lib/caladan/iokerneld ias`，运行 FxMark DRBL `--ncore 1/2/4/8` smoke 测试；每次 `junction_run` 都用 `timeout` 包裹。
- FxMark DRBL 观察到的 smoke 输出：

```text
--ncore 1: # ncpu secs works works/sec
           1 5.000181 30830592.000000 6165895.194594

--ncore 2: # ncpu secs works works/sec
           2 2.500116 30744576.000000 12297259.807145

--ncore 4: # ncpu secs works works/sec
           4 1.250023 30773248.000000 24618135.579051

--ncore 8: # ncpu secs works works/sec
           8 0.625031 30765056.000000 49221658.050092
```

- 测试结束后已执行 `pkill -9 iokerneld`，并用 `pgrep -a iokerneld` / `pgrep -a junction_run` 确认没有残留进程。

### 17.3 本次确认的客观状态

- `junction/syscall/usys.txt` 当前包含 `sync`，位置在 `fsync` / `fdatasync` 之后。
- `junction/kernel/usys.h` 当前声明了 `long usys_sync(void);`。
- `junction/fs/file.cc:usys_sync()` 当前无条件调用 `shaofs_sync_all()` 并返回 0。它不是按 mount namespace 或文件系统实例筛选的通用 Linux `sync()` 实现；当前项目语义是“刷 ShaOFS 全局内存脏状态”。
- `junction/fs/shaofs/file.cc` 当前把 `final_flush()` 的公共刷写逻辑提取为 `flush_all_dirty_state()`；`shaofs_sync_all()` 调用该函数但不调用 `journal_mark_clean()`，`final_flush()` 则在刷写后调用 `journal_mark_clean()`。
- `generic_cache/cache.h::flush_all()` 当前先在 shard lock 下收集 dirty+valid entry handle，再释放 shard lock 后逐个获取 read lock 并写回 backend。这是为了让 runtime `sync()` / `final_flush()` 避免长期持有 shard lock 时进入 backend write。
- `fxmark_changes.patch` 当前覆盖 `Makefile`、`src/bench.c`、`src/DRBL.c`、`src/util.c`；`toggle_fxmark.sh` 当前支持 `apply` / `revert` 并在切换后自动构建。
- `build/junction/caladan_test.config` 当前为单 runtime kthread 配置：

```text
runtime_kthreads 1
runtime_spinning_kthreads 1
runtime_guaranteed_kthreads 0
runtime_quantum_us 0
enable_storage 1
```

- 顶层 git 状态中 `fxmark_changes.patch`、`toggle_fxmark.sh` 和 `test_sync_syscall.c` 当前是 untracked。FxMark 源码目录处于 patch-applied dirty 状态是预期的，但源码改动应继续通过 `fxmark_changes.patch` 管理。
- FxMark 目录中还存在 `Makefile.orig` 和 `src/bench.c.orig` 这两个未跟踪备份文件；它们在本轮最终 patch 生成前已存在/被发现，未纳入 `fxmark_changes.patch`。接手者如要清理，应先确认不是用户仍需要的临时备份。

### 17.4 本次未重新验证的内容

- 最后文档整理阶段没有重新运行 `scripts/build.sh`、CMake build、IOKernel 或 FxMark；长测试结果来自本轮前面的实际调试运行。
- 未验证 FxMark 除 DRBL 外的其他 workload。
- 未验证 FxMark `--directio 1` 与 ShaOFS 当前严格 O_DIRECT user-buffer DMA 合约的兼容性。
- 未调整 `caladan_test.config` 到多 runtime kthread，因此未确认 FxMark/ShaOFS 的真实多核扩展性。
- 未重新跑 Filebench、FIO、ext4 对比或 ShaOFS 全量单元测试。
- 未重新格式化磁盘后重复 FxMark 多轮统计；当前结果只作为跑通 smoke 记录。

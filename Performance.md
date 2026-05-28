# ShaOFS Performance Testing

本文档记录 ShaOFS 当前性能测试规范、复现方式和测试结果，面向后续接手项目的开发者。测试数据应以原始日志为准；本文只汇总可读结论和关键路径。

## 测试日志

| 项目 | 内容 |
|---|---|
| 日期 | 2026-05-22 |
| 测试主题 | Filebench 四组 workload：ShaOFS vs ext4 |
| 项目版本号 | `5e20adfe38c69eac9db2feed0189820efdd39232` |
| 分支 | `dsa` |
| 构建类型 | `Release` |
| ShaOFS IO_PREEMPT | `ON` |
| ShaOFS CRASH_CONSISTENCY | `ON` |
| Junction runtime config | `build/junction/caladan_test.config` |
| runtime 参数 | `runtime_kthreads=10`, `runtime_spinning_kthreads=0`, `runtime_quantum_us=100`, `enable_storage=1` |
| 结果目录 | `junction/fs/mytest/scripts/results/filebench_compare_20260522_080447` |

关键原始文件：

- `junction/fs/mytest/scripts/results/filebench_compare_20260522_080447/environment.txt`
- `junction/fs/mytest/scripts/results/filebench_compare_20260522_080447/raw_runs.csv`
- `junction/fs/mytest/scripts/results/filebench_compare_20260522_080447/summary.csv`
- `junction/fs/mytest/scripts/results/filebench_compare_20260522_080447/ratios.csv`
- `junction/fs/mytest/scripts/results/filebench_compare_20260522_080447/results.json`
- `junction/fs/mytest/scripts/results/filebench_compare_20260522_080447/shaofs_*.log`
- `junction/fs/mytest/scripts/results/filebench_compare_20260522_080447/ext4_filebench_*.log`
- `junction/fs/mytest/scripts/results/filebench_compare_20260522_080447/ext4_filebench_*.cgroup`

## 测试概述

本轮测试使用 Filebench 对 ShaOFS 和 ext4 进行对比，目标是验证 ShaOFS 在 Junction/Caladan 用户态文件系统路径下，相对于传统 Linux ext4 的性能优势。

测试模块：

- ShaOFS：`junction/fs/shaofs`
- Junction syscall/VFS 接入：`junction/fs/file.cc` 等 syscall dispatch 路径
- Filebench 适配版源码：`junction/fs/mytest/benchmark/filebench`
- ShaOFS workload：`junction/fs/mytest/benchmark/filebench_wml/*.f`
- ext4 workload：`junction/fs/mytest/scripts/filebench_test/ext4_*.f`
- ext4 benchmark harness：`junction/fs/mytest/scripts/run_ext4_filebench.sh`

本轮 workload：

| Workload | 文件数 | 线程数 | runtime | 主要特征 |
|---|---:|---:|---:|---|
| `fileserver.f` | 40 | 1 | 2s | create/write/append/read/delete/stat，小型 smoke workload |
| `webserver.f` | 1000 | 100 | 60s | readonly fileset + append log，读密集 |
| `varmail.f` | 1000 | 16 | 60s | delete/create/append/fsync/read，metadata/fsync-heavy |
| `webproxy.f` | 10000 | 100 | 60s | create/delete/append + 多次 readwholefile，读密集 |

ShaOFS 与 ext4 的 WML 参数保持对应，主要区别是测试目录：

- ShaOFS：`set $dir=FSHAO:`
- ext4：`set $dir=/mnt/nvme/ext4_bench`

## 测试方式

### 1. 记录环境

测试前应记录 git 状态、构建开关和 runtime 配置，避免后续结果无法复现。

```bash
RUN_DIR=/home/syh/MyProj1/junction/junction/fs/mytest/scripts/results/filebench_compare_$(date +%Y%m%d_%H%M%S)
sudo mkdir -p "$RUN_DIR"
sudo chown syh:syh "$RUN_DIR"

{
  printf 'RUN_DIR=%s\n' "$RUN_DIR"
  printf '\n## git rev-parse HEAD\n'
  git rev-parse HEAD
  printf '\n## git status --short\n'
  git status --short
  printf '\n## git diff --stat\n'
  git diff --stat
  printf '\n## CMake settings\n'
  grep -nE 'CMAKE_BUILD_TYPE|SHAOFS_(IO_PREEMPT|CRASH_CONSISTENCY)' \
    /home/syh/MyProj1/junction/build/CMakeCache.txt
  printf '\n## caladan_test.config\n'
  grep -nE 'runtime_|enable_storage|host_' \
    /home/syh/MyProj1/junction/build/junction/caladan_test.config
} > "$RUN_DIR/environment.txt"
```

### 2. 构建 Junction 适配版 Filebench

原版 Filebench 不能直接在 Junction 上稳定运行。当前项目通过 patch 管理 Junction 适配：

- `junction/fs/mytest/benchmark/patch/filebench_changes.patch`
- `junction/fs/mytest/benchmark/patch/toggle_filebench.sh`

执行：

```bash
/home/syh/MyProj1/junction/junction/fs/mytest/benchmark/patch/toggle_filebench.sh apply
```

该脚本会：

- 应用 Filebench Junction 适配 patch；
- 禁用 SysV semaphore 相关 configure probe；
- 执行 `make -j $(nproc)`。

适配版主要绕过或降级以下 Junction 不适配路径：

- `fork` / `waitpid` worker 模型改为 pthread 模型；
- 禁用 `personality()` ASLR 控制；
- 禁用 SysV semaphore；
- 避免对 `FSHAO:` 路径执行不合适的 host cleanup；
- 修复 Filebench 部分日志/栈缓冲路径。

### 3. 运行 ShaOFS/Filebench

每次 repetition 都需要独立 reset ShaOFS 设备、启动 IOKernel、运行 Filebench、终止 IOKernel。

通用命令形态：

```bash
RUN_DIR=/home/syh/MyProj1/junction/junction/fs/mytest/scripts/results/filebench_compare_20260522_080447
WML=/home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench_wml/fileserver.f
OUT="$RUN_DIR/shaofs_fileserver_rep1.log"
IOK="$RUN_DIR/shaofs_fileserver_rep1_iokernel.log"

printf 'syh2syh\n' | sudo -S pkill -9 iokerneld 2>/dev/null || true
printf 'syh2syh\n' | sudo -S bash /home/syh/mkfs/mkfs.sh >> "$OUT" 2>&1

printf 'syh2syh\n' | sudo -S /home/syh/MyProj1/junction/lib/caladan/iokerneld ias \
  > "$IOK" 2>&1 &
IOKPID=$!
sleep 5

cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 20s ./junction_run caladan_test.config -- \
  /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench/filebench \
  -f "$WML" >> "$OUT" 2>&1
STATUS=$?

printf 'exit_status=%s\n' "$STATUS" >> "$OUT"
printf 'syh2syh\n' | sudo -S pkill -9 iokerneld 2>/dev/null || true
wait "$IOKPID" 2>/dev/null || true
```

本轮实际使用的 timeout：

| Workload | Timeout | Repetitions |
|---|---:|---:|
| `fileserver.f` | 20s | 5 |
| `webserver.f` | 120s | 3 |
| `varmail.f` | 120s | 3 |
| `webproxy.f` | 120s | 3 |

### 4. 运行 ext4/Filebench

ext4 使用项目已有脚本：

```bash
/home/syh/MyProj1/junction/junction/fs/mytest/scripts/run_ext4_filebench.sh
```

该脚本会：

- 将 Filebench 切回 native/original 版本；
- rebuild native Filebench；
- 调用 `/home/syh/mkfs/reset_ext4.sh` reset ext4 测试盘；
- `sync` + drop caches；
- 通过 `cg_run.sh` 设置 cgroup cpuset/memory 并收集 cgroup stats。

首次运行建议不使用 `--no-rebuild`，确认 native Filebench 已构建：

```bash
RUN_DIR=/home/syh/MyProj1/junction/junction/fs/mytest/scripts/results/filebench_compare_20260522_080447

printf 'syh2syh\n' | sudo -S env \
  RUN_ID=fileserver_rep1 \
  RESULTS_DIR="$RUN_DIR" \
  /home/syh/MyProj1/junction/junction/fs/mytest/scripts/run_ext4_filebench.sh \
  --cpus 2 --mems 0 --memory-mb 1000 --timeout 20s \
  --wml /home/syh/MyProj1/junction/junction/fs/mytest/scripts/filebench_test/ext4_fileserver.f
```

后续同一轮测试可使用 `--no-rebuild` 避免重复构建：

```bash
printf 'syh2syh\n' | sudo -S env \
  RUN_ID=varmail_rep1 \
  RESULTS_DIR="$RUN_DIR" \
  /home/syh/MyProj1/junction/junction/fs/mytest/scripts/run_ext4_filebench.sh \
  --cpus 2 --mems 0 --memory-mb 1000 --timeout 120s \
  --wml /home/syh/MyProj1/junction/junction/fs/mytest/scripts/filebench_test/ext4_varmail.f \
  --no-rebuild
```

本轮 ext4 cgroup 设置：

| Workload | cpuset | memory limit | Repetitions | 备注 |
|---|---|---:|---:|---|
| `fileserver` | `2` | 1000MiB | 5 | 有效 |
| `webserver` | `2` | 4096MiB | 3 | 1000MiB 下 OOM，无效，改用 4096MiB |
| `varmail` | `2` | 1000MiB | 5 | 3 次后离散度 > 5%，追加到 5 次 |
| `webproxy` | `2` | 4096MiB | 3 | 100 线程 workload，使用 4096MiB 避免 OOM |

### 5. 有效性判定

有效 run 必须满足：

- benchmark 进程退出码为 0；
- Filebench 输出包含 `IO Summary`；
- 对 ext4，`.cgroup` 中 `timed_out=0` 且 `memory_events_oom_delta=0`。

失败处理建议：

- ShaOFS run 失败或 timeout 后：保存日志，kill iokernel，重新 mkfs 后只重试一次；
- ext4 run 失败后：保存 `.log` 和 `.cgroup`，检查 mount/reset/cgroup/OOM 信息后只重试一次；
- 失败重复出现时，不要把该 run 纳入 median 结果，应在报告中单独列出。

## 当前测试结果

本轮所有 ShaOFS workload 均成功完成并产生有效 `IO Summary`。ext4 `webserver` 在 1000MiB cgroup 下发生 OOM，改用 4096MiB 后成功完成。

### Median 汇总

| Workload | ShaOFS ops/s | ext4 ops/s | ShaOFS/ext4 | ShaOFS MB/s | ext4 MB/s | MB/s 比值 |
|---|---:|---:|---:|---:|---:|---:|
| `fileserver` | 612,701 | 412,734 | 1.48x | 359.0 | 241.9 | 1.48x |
| `webserver` | 1,291,902 | 345,125 | 3.74x | 6476.6 | 1730.5 | 3.74x |
| `varmail` | 172,256 | 94,961 | 1.81x | 620.0 | 343.6 | 1.80x |
| `webproxy` | 1,015,723 | 260,799 | 3.89x | 2511.1 | 648.8 | 3.87x |

### 稳定性 / 离散度

| FS | Workload | n | ops/s min | ops/s median | ops/s max | spread |
|---|---|---:|---:|---:|---:|---:|
| ShaOFS | `fileserver` | 5 | 612,146 | 612,701 | 613,557 | 0.23% |
| ShaOFS | `webserver` | 3 | 1,267,473 | 1,291,902 | 1,318,492 | 3.95% |
| ShaOFS | `varmail` | 3 | 172,079 | 172,256 | 172,285 | 0.12% |
| ShaOFS | `webproxy` | 3 | 1,012,225 | 1,015,723 | 1,042,677 | 3.00% |
| ext4 | `fileserver` | 5 | 396,439 | 412,734 | 418,778 | 5.41% |
| ext4 | `webserver` | 3 | 345,124 | 345,125 | 346,171 | 0.30% |
| ext4 | `varmail` | 5 | 94,752 | 94,961 | 101,094 | 6.68% |
| ext4 | `webproxy` | 3 | 260,587 | 260,799 | 261,221 | 0.24% |

### 每次 run 原始结果

#### ShaOFS

| Workload | Rep | ops/s | MB/s | ms/op | Log |
|---|---:|---:|---:|---:|---|
| `fileserver` | 1 | 612,211.531 | 358.7 | 0.001 | `shaofs_fileserver_rep1.log` |
| `fileserver` | 2 | 613,139.365 | 359.2 | 0.001 | `shaofs_fileserver_rep2.log` |
| `fileserver` | 3 | 612,146.084 | 358.7 | 0.001 | `shaofs_fileserver_rep3.log` |
| `fileserver` | 4 | 612,701.006 | 359.0 | 0.001 | `shaofs_fileserver_rep4.log` |
| `fileserver` | 5 | 613,557.175 | 359.5 | 0.001 | `shaofs_fileserver_rep5.log` |
| `webserver` | 1 | 1,318,491.603 | 6610.0 | 0.064 | `shaofs_webserver_rep1.log` |
| `webserver` | 2 | 1,291,902.287 | 6476.6 | 0.066 | `shaofs_webserver_rep2.log` |
| `webserver` | 3 | 1,267,473.054 | 6354.4 | 0.067 | `shaofs_webserver_rep3.log` |
| `varmail` | 1 | 172,255.723 | 619.8 | 0.091 | `shaofs_varmail_rep1.log` |
| `varmail` | 2 | 172,285.011 | 620.3 | 0.091 | `shaofs_varmail_rep2.log` |
| `varmail` | 3 | 172,079.178 | 620.0 | 0.091 | `shaofs_varmail_rep3.log` |
| `webproxy` | 1 | 1,012,225.394 | 2502.9 | 0.068 | `shaofs_webproxy_rep1.log` |
| `webproxy` | 2 | 1,015,722.556 | 2511.1 | 0.068 | `shaofs_webproxy_rep2.log` |
| `webproxy` | 3 | 1,042,676.597 | 2576.1 | 0.065 | `shaofs_webproxy_rep3.log` |

#### ext4

| Workload | Rep | ops/s | MB/s | ms/op | Log |
|---|---:|---:|---:|---:|---|
| `fileserver` | 1 | 412,733.736 | 241.9 | 0.001 | `ext4_filebench_fileserver_rep1.log` |
| `fileserver` | 2 | 418,778.164 | 245.5 | 0.001 | `ext4_filebench_fileserver_rep2.log` |
| `fileserver` | 3 | 396,438.714 | 232.3 | 0.001 | `ext4_filebench_fileserver_rep3.log` |
| `fileserver` | 4 | 415,413.075 | 243.5 | 0.001 | `ext4_filebench_fileserver_rep4.log` |
| `fileserver` | 5 | 408,711.977 | 239.5 | 0.001 | `ext4_filebench_fileserver_rep5.log` |
| `webserver` | 1 | 345,124.899 | 1730.5 | 0.147 | `ext4_filebench_webserver_4096m_rep1.log` |
| `webserver` | 2 | 345,124.083 | 1730.1 | 0.149 | `ext4_filebench_webserver_4096m_rep2.log` |
| `webserver` | 3 | 346,171.155 | 1735.6 | 0.149 | `ext4_filebench_webserver_4096m_rep3.log` |
| `varmail` | 1 | 94,880.750 | 342.9 | 0.165 | `ext4_filebench_varmail_rep1.log` |
| `varmail` | 2 | 101,094.178 | 364.6 | 0.155 | `ext4_filebench_varmail_rep2.log` |
| `varmail` | 3 | 94,961.442 | 343.6 | 0.165 | `ext4_filebench_varmail_rep3.log` |
| `varmail` | 4 | 100,980.388 | 364.9 | 0.155 | `ext4_filebench_varmail_rep4.log` |
| `varmail` | 5 | 94,751.749 | 343.3 | 0.165 | `ext4_filebench_varmail_rep5.log` |
| `webproxy` | 1 | 261,221.261 | 649.4 | 0.230 | `ext4_filebench_webproxy_4096m_rep1.log` |
| `webproxy` | 2 | 260,587.374 | 648.7 | 0.234 | `ext4_filebench_webproxy_4096m_rep2.log` |
| `webproxy` | 3 | 260,798.837 | 648.8 | 0.228 | `ext4_filebench_webproxy_4096m_rep3.log` |

### 初步结论

- ShaOFS 在四组 Filebench workload 中均快于 ext4。
- `webserver` 和 `webproxy` 提升最明显，分别约 3.74x 和 3.89x，说明 ShaOFS 在高线程读密集场景下具有较强优势。
- `varmail` 包含大量 `fsync`、create/delete 和 metadata 更新，ShaOFS 仍有约 1.81x 提升，但优势小于纯读密集 workload。
- `fileserver` 是短时小规模 smoke workload，ShaOFS 约 1.48x；该 workload runtime 只有 2s，适合快速验证，不适合作为唯一正式结论。

## 已知问题 & TODO

### 已知问题

- 当前 git 工作区非 clean，本轮测试结果绑定到当前未提交 ShaOFS DSA/write-path 修改状态。后续正式论文数据应基于明确 commit 或 tag 重新测试。
- ext4 `webserver` 在 1000MiB cgroup 下发生 OOM，无有效结果；本轮改用 4096MiB 后有效。
- ext4 `webserver_4096m` 的 cgroup `memory_events_max_delta` 非 0，说明 memory.max 被触及过，但没有 OOM；解释结果时应注明。
- ext4 `varmail` 离散度较高，即使追加到 5 次后 spread 仍约 6.68%；报告中应使用 median，并保留 raw logs。
- ShaOFS run 当前未通过 cgroup 统一限制内存；ext4 使用 cgroup 限制 CPU/内存。后续若要更严格公平，需要设计对 Junction 进程可行的资源限制方案。
- 当前测试会反复执行 destructive reset：`/home/syh/mkfs/mkfs.sh`、`/home/syh/mkfs/reset_ext4.sh`、drop caches 和 `pkill -9 iokerneld`。运行前必须确认测试机和设备用途。
- Filebench 测试后，当前 Filebench 源码可能处于 native/ext4 patch reverted 状态；再次运行 ShaOFS 前需要重新执行 `toggle_filebench.sh apply`。

### TODO

- 为 Filebench 测试固化一个统一 orchestration 脚本，自动执行：patch toggle、ShaOFS runs、ext4 runs、日志解析、结果汇总。
- 将测试结果目录命名、CSV/JSON 输出格式和报告模板标准化，便于跨 commit 对比。
- 增加硬件信息记录：CPU 型号、核心绑定、NVMe 型号、NUMA 拓扑、内核版本、SPDK/Caladan 配置。
- 统一 ShaOFS 与 ext4 的资源约束策略，尤其是 CPU 和内存限制。
- 为 `webserver` / `webproxy` 这类 100 线程 workload 探索更稳定的 ext4 memory limit，避免 cgroup memory pressure 干扰结论。
- 对 `varmail` 继续分析 fsync/journal 路径开销，拆分 fsync、metadata update、read/write 子项，定位 ShaOFS 与 ext4 差距来源。
- 在正式报告前基于 clean commit 重新跑至少 5 次每个 workload，并固定系统负载、CPU governor、IRQ/NUMA 绑定等外部变量。
- 将 Filebench patch 生成和验证流程纳入 CI 或最小 smoke 测试，避免适配补丁随源码漂移失效。

---

## 2026-05-23 测试日志：profiling / correctness / Filebench 最终复测

### 测试日志

| 项目 | 内容 |
|---|---|
| 日期 | 2026-05-23 |
| 测试主题 | ShaOFS profiling、correctness regression、Filebench 四项最终复测 |
| 分支 | `dsa` |
| 当前 HEAD | `25d1a5f fileserver.f passed` |
| 构建类型 | `Release` |
| ShaOFS IO_PREEMPT | `ON` |
| ShaOFS CRASH_CONSISTENCY | `ON` |
| runtime 参数 | `runtime_kthreads=1`, `runtime_spinning_kthreads=0`, `runtime_guaranteed_kthreads=0`, `runtime_quantum_us=100`, `enable_storage=1` |
| profiling 结果目录 | `junction/fs/mytest/scripts/results/shaofs_profile_filebench_20260523` |
| correctness 结果目录 | `junction/fs/mytest/scripts/results/shaofs_profile_opt_20260523_regression3` |
| final Filebench 结果目录 | `junction/fs/mytest/scripts/results/shaofs_profile_opt_20260523_final_noprof` |

本轮测试绑定到当前未提交工作区状态。相关改动主要包括：

- 新增/接入 ShaOFS profiling 模块：`junction/fs/shaofs/profile.h`、`junction/fs/shaofs/profile.cc`、`junction/fs/CMakeLists.txt`。
- 对 ShaOFS 热路径加入 profiling 事件：syscall、directory、journal/checkpoint、block/inode cache、file/extent 路径。
- profile-disabled final build 中，普通编译单元默认 `SHAOFS_PROFILE_COMPILED=0`，profiling helper 和 scope 编译成 no-op，避免最终 benchmark 仍承担 profiling 热路径开销。
- `my_open()` 支持返回打开对象类型并允许只读目录打开，用于目录 fd / fsync / recovery 等正确性路径。
- `my_fsync()` 仅在 `need_inode_flush` 为真时调用 `inode_flush_extent_metadata()`，避免 clean fsync 或仅数据 dirty 路径无条件 flush extent metadata。
- EOF append batch allocation helper：`inode_alloc_append_blocks_locked()`，用于 regular-file EOF full-block batch write。
- 当前 `journal_commit_blocks()` 路径为同步 home-block checkpoint；源码中仍保留 checkpoint worker/slot 结构，但当前不是 async checkpoint 正常路径。

关键原始文件：

- `junction/fs/mytest/scripts/results/shaofs_profile_filebench_20260523/summary.txt`
- `junction/fs/mytest/scripts/results/shaofs_profile_opt_20260523_regression3/summary.txt`
- `junction/fs/mytest/scripts/results/shaofs_profile_opt_20260523_final_noprof/shaofs/summary.txt`
- `junction/fs/mytest/scripts/results/shaofs_profile_opt_20260523_final_noprof/ext4/summary.txt`
- `junction/fs/mytest/scripts/results/shaofs_profile_opt_20260523_final_noprof/shaofs/*.driver.log`
- `junction/fs/mytest/scripts/results/shaofs_profile_opt_20260523_final_noprof/ext4/*.driver.log`

### 测试概述

本轮目标不是重新证明 2026-05-22 的高性能结论，而是在单 runtime kthread、当前 correctness 修复和 profiling 插桩存在的前提下，完成一个可交接的优化闭环：

1. 增加 ShaOFS 内部 profiling，定位 metadata / fsync / journal / extent 路径热点。
2. 跑 correctness regression，确保优化不破坏基础语义和 crash recovery。
3. 基于 profile 结果进行低风险优化：fsync extent metadata 条件 flush、EOF append batch allocation、profiling compile-time no-op。
4. 使用 profile-disabled build 重跑 Filebench 四项 workload，并与 ext4 对比。
5. 判断本轮优化是否带来稳定性能提升，并记录下一轮优化方向。

本轮 workload 采用当前 ShaOFS WML：

| Workload | ShaOFS WML | 规模 | 主要压力 |
|---|---|---|---|
| `fileserver` | `filebench_wml/fileserver.f` | 10000 files, 50 threads, 60s | create/write/append/read/delete/stat，metadata churn 极重 |
| `webserver` | `filebench_wml/webserver.f` | 10000 files, 100 threads, 60s | open/read/close 读密集 + append log |
| `varmail` | `filebench_wml/varmail.f` | 5000 files, 16 threads, 60s | append + fsync + create/delete，metadata/fsync-heavy |
| `webproxy` | `filebench_wml/webproxy.f` | 10000 files, 100 threads, 60s | create/delete/append + 多次 readwholefile |

### 测试方式

#### 1. 记录和核对环境

```bash
cd /home/syh/MyProj1/junction

git log --oneline --decorate --max-count=5
git status --short -- \
  HANDOVER.md Performance.md \
  junction/fs/CMakeLists.txt junction/fs/core.cc junction/fs/file.cc \
  junction/fs/shaofs build/CMakeCache.txt build/junction/caladan_test.config

grep -nE 'CMAKE_BUILD_TYPE|SHAOFS_(IO_PREEMPT|CRASH_CONSISTENCY)' build/CMakeCache.txt
grep -nE 'runtime_|enable_storage' build/junction/caladan_test.config
```

本轮核对到的关键配置：

```text
CMAKE_BUILD_TYPE:STRING=Release
SHAOFS_CRASH_CONSISTENCY:BOOL=ON
SHAOFS_IO_PREEMPT:BOOL=ON
runtime_kthreads 1
runtime_spinning_kthreads 0
runtime_guaranteed_kthreads 0
runtime_quantum_us 100
enable_storage 1
```

注意：如果重新执行 `/home/syh/MyProj1/junction/scripts/build.sh`，可能重写 `build/junction/caladan_test.config`。每次 ShaOFS benchmark 前都应重新确认 `runtime_kthreads 1`。

#### 2. 构建并启用/关闭 profiling

profiling 模块通过编译期宏和运行期环境变量共同控制：

- `profile.cc` 自身定义 `SHAOFS_PROFILE_COMPILED=1`，实现真实计数器。
- 其它编译单元默认 `SHAOFS_PROFILE_COMPILED=0`，`SHAOFS_PROFILE_SCOPE()` 为 no-op。
- 运行期设置 `SHAOFS_PROFILE=1` 时，profiling run 会输出 `[shaofs-prof]`。
- final no-profile benchmark 不设置 `SHAOFS_PROFILE`，并依赖 no-op 编译路径避免热路径开销。

构建命令：

```bash
cd /home/syh/MyProj1/junction
cmake --build build --target junction_run -j$(nproc)
```

#### 3. Correctness regression

本轮 correctness regression 的最终摘要位于：

```text
junction/fs/mytest/scripts/results/shaofs_profile_opt_20260523_regression3/summary.txt
```

复现时应遵守以下约束：

- 每次 ShaOFS 测试前确认 `runtime_kthreads 1`。
- 需要 clean filesystem 时执行 `sudo bash /home/syh/mkfs/mkfs.sh`。
- 启动 IOKernel 后等待数秒再运行 `junction_run`。
- 所有 `junction_run` 使用 `timeout` 包裹。
- 每次测试结束后终止 `iokerneld`。

通用命令模板：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S pkill -x iokerneld 2>/dev/null || true
printf 'syh2syh\n' | sudo -S bash /home/syh/mkfs/mkfs.sh

printf 'syh2syh\n' | sudo -S /home/syh/MyProj1/junction/lib/caladan/iokerneld ias \
  > /tmp/shaofs_iokernel.log 2>&1 &
IOKPID=$!
sleep 5

cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S timeout 120s ./junction_run caladan_test.config -- \
  /path/to/shaofs_correctness_test_binary
STATUS=$?

printf 'syh2syh\n' | sudo -S pkill -x iokerneld 2>/dev/null || true
wait "$IOKPID" 2>/dev/null || true
exit "$STATUS"
```

#### 4. ShaOFS profiling Filebench

profiling run 使用当前四项 WML，并通过 `SHAOFS_PROFILE=1` 收集 `[shaofs-prof]` 输出。结果摘要位于：

```text
junction/fs/mytest/scripts/results/shaofs_profile_filebench_20260523/summary.txt
```

单项 workload 的复现模板：

```bash
cd /home/syh/MyProj1/junction
/home/syh/MyProj1/junction/junction/fs/mytest/benchmark/patch/toggle_filebench.sh apply
cmake --build build --target junction_run -j$(nproc)

grep -nE 'runtime_kthreads|runtime_spinning_kthreads|runtime_quantum_us' \
  build/junction/caladan_test.config

printf 'syh2syh\n' | sudo -S pkill -x iokerneld 2>/dev/null || true
printf 'syh2syh\n' | sudo -S bash /home/syh/mkfs/mkfs.sh

printf 'syh2syh\n' | sudo -S /home/syh/MyProj1/junction/lib/caladan/iokerneld ias \
  > /tmp/shaofs_profile_iokernel.log 2>&1 &
IOKPID=$!
sleep 5

cd /home/syh/MyProj1/junction/build/junction
printf 'syh2syh\n' | sudo -S env SHAOFS_PROFILE=1 timeout 120s \
  ./junction_run caladan_test.config -- \
  /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench/filebench \
  -f /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench_wml/varmail.f

printf 'syh2syh\n' | sudo -S pkill -x iokerneld 2>/dev/null || true
wait "$IOKPID" 2>/dev/null || true
```

#### 5. Final profile-disabled Filebench：ShaOFS vs ext4

final 结果目录：

```text
junction/fs/mytest/scripts/results/shaofs_profile_opt_20260523_final_noprof
```

ShaOFS final run 不设置 `SHAOFS_PROFILE`，命令形态与上面相同，只去掉 `env SHAOFS_PROFILE=1`。

ext4 使用项目脚本运行，并保存 driver log / raw log：

```bash
cd /home/syh/MyProj1/junction
printf 'syh2syh\n' | sudo -S env \
  RESULTS_DIR=/home/syh/MyProj1/junction/junction/fs/mytest/scripts/results/shaofs_profile_opt_20260523_final_noprof/ext4 \
  RUN_ID=varmail \
  /home/syh/MyProj1/junction/junction/fs/mytest/scripts/run_ext4_filebench.sh \
  --cpus 2 --mems 0 --memory-mb <workload_memory_mb> --timeout 120s \
  --wml /home/syh/MyProj1/junction/junction/fs/mytest/scripts/filebench_test/ext4_varmail.f
```

内存限制应按 workload 需求设置。2026-05-22 的经验是：ext4 的 100 线程 workload 在较小 cgroup memory 下可能 OOM；正式复测应记录 `*.cgroup` 并排除 OOM 或 timeout run。

### 当前测试结果

#### 1. Correctness regression

所有列入 summary 的 correctness 项均通过：

| 测试项 | 结果 |
|---|---|
| `test_shaofs_syscall_correctness` | PASS |
| `test_shaofs_direct_correctness` | PASS |
| `test_shaofs_concurrent_correctness` | PASS |
| `test_shaofs_fsync_direct_verify` | PASS |
| `test_shaofs_unlink` | PASS |
| `test_shaofs_dir_index` | PASS |
| `test_shaofs_many_extents` | PASS |
| `test_shaofs_mt_full_extents` | PASS |
| `test_shaofs_append_prealloc` | PASS |
| `test_shaofs_varmail_bottleneck` | PASS |
| `journal_recovery_prepare/check` | PASS (`prepare_status=124`, `check_status=0`) |

说明：`journal_recovery_prepare` 使用 timeout/kill 模拟 crash，`prepare_status=124` 是该测试流程的预期部分，不应误判为 failure。

#### 2. ShaOFS profiling Filebench 结果

| Workload | 状态 | ops/s | MB/s | ms/op | 原始摘要 |
|---|---|---:|---:|---:|---|
| `fileserver` | PASS | 6,008.248 | 92.8 | 8.214 | `361330 ops`, `200/4207 rd/wr` |
| `webserver` | PASS | 75,104.523 | 1522.8 | 1.317 | `4509212 ops`, `24228/2424 rd/wr` |
| `varmail` | PASS | 90,436.605 | 332.4 | 0.174 | `5426329 ops`, `13913/13913 rd/wr` |
| `webproxy` | PASS | 282,093.254 | 730.9 | 0.350 | `16935763 ops`, `74238/14848 rd/wr` |

profiling run 用于定位热点，不应直接与 final no-profile benchmark 混用为最终性能结论。

#### 3. Final profile-disabled ShaOFS vs ext4

| Workload | ShaOFS ops/s | ext4 ops/s | ShaOFS/ext4 | ShaOFS MB/s | ext4 MB/s | ShaOFS ms/op | ext4 ms/op | 当前结论 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| `fileserver` | 5,113.546 | 64,853.136 | 0.08x | 77.5 | 1188.0 | 9.624 | 0.757 | ShaOFS 严重落后，是首要瓶颈 |
| `webserver` | 77,267.811 | 61,211.674 | 1.26x | 1566.7 | 1241.1 | 1.281 | 1.594 | ShaOFS 仍领先 |
| `varmail` | 89,136.575 | 93,055.001 | 0.96x | 326.4 | 342.1 | 0.177 | 0.169 | ShaOFS 略落后 |
| `webproxy` | 305,651.265 | 247,716.175 | 1.23x | 789.8 | 647.2 | 0.323 | 0.249 | ShaOFS ops/s 仍领先 |

本轮检查最终结果目录中的常见无效标记，未发现以下问题：

- `NO VALID RESULTS`
- `Failed to open`
- `failed to unlink`
- `Input/output error`
- `No such file`

#### 4. 初步结论

- 本轮低风险优化没有带来可稳定声明的性能提升；最终结果与 2026-05-22 的高吞吐结论不可直接合并，因为 workload 规模、runtime config、代码状态和 profiling/no-profile 处理均已变化。
- `fileserver` 在当前完整 workload 下从 ShaOFS 优势项变成最大短板，说明 create/write/append/read/delete/stat 混合路径的 metadata 前台成本非常高。
- `varmail` 与 ext4 接近但略落后，主要受 append + fsync + inode metadata flush + journal/checkpoint 路径影响。
- `webserver` 和 `webproxy` 在 ops/s 上仍领先 ext4，后续优化不能牺牲这两项已有优势。
- 当前同步 home-block checkpoint 是 correctness fix，不能为了追求性能直接恢复旧 async checkpoint。若要重新引入异步 checkpoint，必须设计 cache dirty/clean generation 或 pending-checkpoint 状态。

### 已知问题 & TODO

#### 已知问题

- 当前工作区非 clean，测试结果绑定到未提交的 ShaOFS profiling、fsync、append、open/recovery 相关修改；后续正式报告应基于明确 commit 或 tag 重新测试。
- `fileserver` 只有单次 final no-profile 结果，尚未做多轮方差统计；不能写成统计显著结论。
- 本轮 final 表是单次 ShaOFS/ext4 对比，不包含完整置信区间。
- profiling run 和 final no-profile run 不应直接混用；profiling 数字主要用于定位热点。
- ext4 cgroup memory 仍需按 workload 记录和校准，尤其是 100 线程 workload。
- 当前文档未重新覆盖 FIO、FxMark、IO_PREEMPT microbench 或 `CRASH_CONSISTENCY=OFF` 场景。
- 大量 benchmark/test/result 文件仍是 untracked 状态；不要用 `git clean` 或批量删除作为整理手段。

#### TODO

- 设计 correctness-safe async metadata checkpoint 或 batch checkpoint/generation 状态机：
  - home checkpoint 完成前，metadata cache entry 不能被当作普通 clean entry；
  - 同一 metadata block 的旧事务镜像不能乱序覆盖新事务；
  - `fsync`、`sync`、`final_flush`、`journal_mark_clean` 必须 drain pending checkpoint。
- 如果短期不能安全实现 async checkpoint，优先减少 `journal_commit_single()` 次数、扩大明确 transaction 粒度，并降低 inode/free/extent metadata 前台 churn。
- 为 `fileserver` 增加分阶段 profiling：create、append、read、delete、stat 分别统计，拆出目录索引、inode alloc/free、extent alloc/free、journal/checkpoint 的占比。
- 对 `varmail` 继续拆分 dirty fsync：data flush、inode flush、extent metadata flush、journal image/header write、home checkpoint。
- 将 correctness regression 和 Filebench 四项测试固化为统一脚本，自动记录：git commit、dirty diff stat、CMake cache、runtime config、WML 副本、stdout/stderr、driver log、exit code、cgroup stats。
- 基于 clean commit 对 final no-profile Filebench 每项至少重复 5 次，报告 median、min/max、spread，并保存 raw logs。
- 在下一轮优化后先跑 correctness regression，再跑 Filebench；任何 `NO VALID RESULTS` 或 correctness failure 都不能作为有效性能数据。

---

## 2026-05-24 测试日志：Filebench 四项单核复测

### 测试日志

| 项目 | 内容 |
|---|---|
| 日期 | 2026-05-24 |
| 测试主题 | Filebench `fileserver.f` / `webserver.f` / `varmail.f` / `webproxy.f`：ShaOFS vs ext4 |
| 当前 HEAD | `25d1a5f6383783d72fe45e3149e373b47ca941bb` |
| 分支 | `dsa` |
| 构建类型 | `Release` |
| ShaOFS IO_PREEMPT | `ON` |
| ShaOFS CRASH_CONSISTENCY | `ON` |
| runtime 参数 | `runtime_kthreads=1`, `runtime_spinning_kthreads=0`, `runtime_guaranteed_kthreads=0`, `runtime_quantum_us=100`, `enable_storage=1` |
| 原始结果目录 | `/tmp/filebench_report_20260524` |

本轮测试绑定到当前非 clean 工作区。测试前已执行：

```bash
cmake --build build --target junction_run -j$(nproc)
```

ShaOFS 测试前执行 `toggle_filebench.sh apply` 构建 Junction 适配版 Filebench；ext4 测试前执行 `toggle_filebench.sh revert` 并重建 native Filebench。测试结束后，Filebench 源码保持在 native/reverted 状态；下次运行 ShaOFS Filebench 前需要重新执行：

```bash
/home/syh/MyProj1/junction/junction/fs/mytest/benchmark/patch/toggle_filebench.sh apply
```

### 测试方式

ShaOFS 每个 workload 都单独执行：

1. `sudo pkill -9 iokerneld` 清理旧 IOKernel。
2. `sudo bash /home/syh/mkfs/mkfs.sh` 重置 ShaOFS 盘面。
3. `sudo /home/syh/MyProj1/junction/lib/caladan/iokerneld ias` 启动 IOKernel。
4. 在 `/home/syh/MyProj1/junction/build/junction` 下执行：

```bash
sudo timeout 240s ./junction_run caladan_test.config -- \
  /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench/filebench \
  -f /home/syh/MyProj1/junction/junction/fs/mytest/benchmark/filebench_wml/<workload>.f
```

ext4 每个 workload 都通过 `run_ext4_filebench.sh` 单独 reset ext4、drop caches、设置 cgroup 并保存 `.cgroup` stats。ext4 使用的内存限制：

| Workload | cpuset | memory limit | timeout |
|---|---|---:|---:|
| `fileserver` | `2` | 800MiB | 240s |
| `webserver` | `2` | 1300MiB | 240s |
| `varmail` | `2` | 500MiB | 240s |
| `webproxy` | `2` | 1300MiB | 240s |

所有有效 run 均满足：

- Filebench 输出包含 `IO Summary`。
- 进程退出码为 0。
- ext4 `.cgroup` 中 `timed_out=0`、无 OOM。

### 当前测试结果

本轮每项只跑 1 次，用于当前状态回归和瓶颈定位，不应视为最终统计显著数据。正式论文数据仍应基于 clean commit、每项至少 5 次 repetition 的 median/min/max/spread。

| Workload | ShaOFS ops/s | ext4 ops/s | ShaOFS/ext4 | ShaOFS MB/s | ext4 MB/s | MB/s 比值 | ShaOFS ms/op | ext4 ms/op |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `fileserver` | 4,294.607 | 49,513.416 | 0.087x | 101.6 | 1187.5 | 0.086x | 11.411 | 0.992 |
| `webserver` | 504,547.246 | 308,014.299 | 1.638x | 2652.8 | 1619.5 | 1.638x | 0.197 | 0.199 |
| `varmail` | 89,110.448 | 93,109.782 | 0.957x | 321.4 | 335.9 | 0.957x | 0.177 | 0.169 |
| `webproxy` | 392,721.488 | 252,225.879 | 1.557x | 974.0 | 628.1 | 1.551x | 0.252 | 0.323 |

### 每项原始结果

#### ShaOFS

| Workload | ops/s | MB/s | ms/op | rd/wr ops/s | Log |
|---|---:|---:|---:|---:|---|
| `fileserver` | 4,294.607 | 101.6 | 11.411 | 390 / 781 | `/tmp/filebench_report_20260524/shaofs/shaofs_fileserver_rep1.log` |
| `webserver` | 504,547.246 | 2652.8 | 0.197 | 162,757 / 16,277 | `/tmp/filebench_report_20260524/shaofs/shaofs_webserver_rep1.log` |
| `varmail` | 89,110.448 | 321.4 | 0.177 | 13,709 / 13,709 | `/tmp/filebench_report_20260524/shaofs/shaofs_varmail_rep1.log` |
| `webproxy` | 392,721.488 | 974.0 | 0.252 | 103,350 / 20,671 | `/tmp/filebench_report_20260524/shaofs/shaofs_webproxy_rep1.log` |

#### ext4

| Workload | ops/s | MB/s | ms/op | rd/wr ops/s | memory limit | memory peak | memory.max events | Log |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| `fileserver` | 49,513.416 | 1187.5 | 0.992 | 4,501 / 9,003 | 800MiB | 800MiB | 227,558 | `/tmp/filebench_report_20260524/ext4/ext4_filebench_fileserver_rep1.log` |
| `webserver` | 308,014.299 | 1619.5 | 0.199 | 99,358 / 9,937 | 1300MiB | 1300MiB | 17,079 | `/tmp/filebench_report_20260524/ext4/ext4_filebench_webserver_rep1.log` |
| `varmail` | 93,109.782 | 335.9 | 0.169 | 14,324 / 14,325 | 500MiB | 307.8MiB | 0 | `/tmp/filebench_report_20260524/ext4/ext4_filebench_varmail_rep1.log` |
| `webproxy` | 252,225.879 | 628.1 | 0.323 | 66,375 / 13,275 | 1300MiB | 1204.8MiB | 0 | `/tmp/filebench_report_20260524/ext4/ext4_filebench_webproxy_rep1.log` |

### 关键 per-flowop 对比

#### `fileserver`

| Flowop | ShaOFS ops/s | ShaOFS avg | ext4 ops/s | ext4 avg | 观察 |
|---|---:|---:|---:|---:|---|
| `wrtfile1` | 391 | 43.595ms | 4,502 | 0.297ms | ShaOFS 大文件写入路径严重慢于 ext4 |
| `readfile1` | 390 | 65.864ms | 4,501 | 4.687ms | ShaOFS 读回路径同样严重慢于 ext4 |
| `createfile1` | 391 | 6.583ms | 4,502 | 0.033ms | ShaOFS inode/dir/metadata 创建路径过重 |
| `deletefile1` | 390 | 2.007ms | 4,501 | 0.067ms | ShaOFS unlink/free metadata 路径过重 |
| `appendfilerand1` | 391 | 5.779ms | 4,501 | 5.812ms | append 单项接近，但整体被 create/write/read/delete 拖垮 |

#### `varmail`

| Flowop | ShaOFS ops/s | ShaOFS avg | ext4 ops/s | ext4 avg | 观察 |
|---|---:|---:|---:|---:|---|
| `fsyncfile2` | 6,855 | 1.451ms | 7,162 | 0.720ms | ShaOFS dirty fsync/journal/checkpoint 成本高 |
| `fsyncfile3` | 6,855 | 0.766ms | 7,162 | 0.545ms | ShaOFS 仍慢，但差距小于第一次 fsync |
| `createfile2` | 6,855 | 0.010ms | 7,162 | 0.227ms | ShaOFS 创建在 varmail 小文件场景快于 ext4 |
| `deletefile1` | 6,855 | 0.015ms | 7,162 | 0.611ms | ShaOFS 删除在 varmail 小文件场景快于 ext4 |

#### `webserver` / `webproxy`

`webserver` 中 ShaOFS 的 read flowop 约 `16,276 ops/s`、单次 read 平均约 `0.003ms`；ext4 对应约 `9,936 ops/s`、`0.018-0.021ms`。这说明在读密集、文件集预创建且缓存命中较多的场景下，ShaOFS 的用户态 syscall/uthread 路径确实能明显降低 open/read/close 热路径开销。

`webproxy` 中 ShaOFS 总体为 ext4 的 `1.56x`。ShaOFS read/open/close 单项明显更快，但 `appendfilerand1`、`deletefile1` 仍在 `2.36-2.37ms` 级别，说明混合写入和删除路径仍是后续优化重点。

### 当前结论

- 当前 ShaOFS 并不是整体没有体现用户态/uthread 优势：`webserver` 和 `webproxy` 分别达到 ext4 的 `1.64x` 和 `1.56x`，读密集热路径符合预期。
- `fileserver` 是当前最严重短板，仅为 ext4 的 `8.7%`。其根因不在上下文切换，而在 ShaOFS 文件系统内部 metadata、extent、cache、journal/checkpoint 和大文件 buffered I/O 路径的 CPU/IO 放大。
- `varmail` 与 ext4 基本持平但略低，关键差距集中在 dirty `fsync`：ShaOFS `fsyncfile2` 平均 `1.451ms`，ext4 `0.720ms`。
- 因此后续优化应优先减少文件系统前台 metadata churn 和 journal/checkpoint 同步成本；单纯强调 uthread 切换优势无法覆盖当前 ShaOFS 内部每次 syscall 做过多工作的事实。

### 已知问题 & TODO

- 本轮为单次 run，结果用于定位和回归，不是最终统计结论。
- ext4 `fileserver` 和 `webserver` 触达 cgroup `memory.max`，虽无 OOM，但存在 memory pressure；后续应继续探测“测试自身内存 + 约 300MiB 页缓存预算”的最小稳定值。
- 当前 ShaOFS profile 宏在普通编译单元默认 no-op；要做精确热点 profile，需要临时启用全局 `SHAOFS_PROFILE_COMPILED=1` 或增加更低开销的采样/阶段统计。
- 下一轮优化应先修复/优化 `fileserver`，再验证不会回退 `webserver` / `webproxy` 已有优势。

### 2026-05-24 追加 profile 定位

为定位瓶颈，临时加入低侵入 ShaOFS profile/counter 插桩，并用 profile build 重新跑了单核 `fileserver` 与 `varmail`。注意：打开 profile 编译开关后 CMake 会重写 `build/junction/caladan_test.config`，本轮第一次误跑成 `runtime_kthreads=10`；有效单核日志已确认包含 `cfg: provisioned 1 cores` 与 `spawning 1 kthreads`。

Profile build 仅用于瓶颈定位，不能和 profile-disabled 正式性能数字直接做性能对比。有效日志如下：

- `fileserver`: `/tmp/shaofs_profile_20260524/fileserver_profile_1k.log`
- `varmail`: `/tmp/shaofs_profile_20260524/varmail_profile_1k.log`

#### `fileserver` 单核 profile

Filebench 汇总：`4164.569 ops/s`、`99.0mb/s`、`11.774ms/op`，与正式 profile-disabled 结果 `4294.607 ops/s` 同量级，说明 profile 插桩没有改变瓶颈类型。

Top profile 事件：

| Event | total_us | avg_us | count | blocks/bytes | 观察 |
|---|---:|---:|---:|---:|---|
| `my_read` | 1,509,999,395 | 33,153 | 45,546 | 47.8GB | `readwholefile` 是最大瓶颈 |
| `file_read_batch` | 1,509,878,597 | 33,150 | 45,546 | 47.8GB | 读路径主要耗在 batch read 内部 |
| `my_write` | 1,194,244,693 | 22,301 | 53,549 | 4.2GB | 写路径为第二大瓶颈 |
| `file_write_eof_extension` | 931,543,711 | 44,420 | 20,971 | 3.7GB | EOF 扩展写非常重 |
| `file_write_new_block_batch` | 838,669,183 | 28,085 | 29,861 | 834,283 blocks | 新块批量写/分配仍不够粗 |
| `bc_metadata_writeback` | 560,554,142 | 14,600 | 38,394 | 38,394 blocks | metadata 写回进入同步 journal |
| `journal_commit_single` | 560,495,506 | 14,589 | 38,419 | 38,419 blocks | 单块 journal commit 严重串行化 |
| `bc_data_writeback` | 147,153,389 | 149 | 985,682 | 985,682 blocks | Block Cache 容量不足导致大量脏数据驱逐 |
| `bc_backend_read` | 123,697,879 | 190 | 649,730 | 649,730 blocks | 大量 cache miss 触发真实读盘 |
| `inode_bmap_locked` | 95,364,716 | 87 | 1,086,350 | - | per-block bmap/extent 查询成本明显 |
| `extent_alloc` | 89,233,332 | 409 | 217,846 | 1,202,176 blocks | extent/block 分配是写路径关键成本 |

Block Cache 计数：

- `bc_get_hit`: 724,574
- `bc_get_miss`: 652,114
- `bc_get_nofetch_hit`: 73,581
- `bc_get_nofetch_miss`: 970,321

这说明 `fileserver` 并不是纯内存缓存热路径。工作集明显超过 256MiB Block Cache，读路径存在大量 miss；写路径大量新块 nofetch miss 后进入 cache，再因容量压力触发 dirty eviction。当前 50 个 Filebench thread 在单 runtime kthread 上并发推进，uthread 切换虽轻，但前台时间主要被 cache miss、dirty writeback、metadata journal 和 extent 分配占据。

#### `varmail` 单核 profile

Filebench 汇总：`89917.596 ops/s`、`323.6mb/s`、`0.175ms/op`，与正式 profile-disabled 结果 `89110.448 ops/s` 同量级。

Top profile 事件：

| Event | total_us | avg_us | count | blocks/bytes | 观察 |
|---|---:|---:|---:|---:|---|
| `my_fsync` | 898,761,188 | 1,082 | 830,042 | - | 总瓶颈几乎全部在 fsync |
| `my_fsync_inode_flush` | 783,185,983 | 943 | 830,013 | - | fsync 主要耗在 inode flush |
| `ic_flush_inode` | 783,045,354 | 943 | 830,013 | - | inode table block 写回是关键 |
| `bc_flush_block` | 419,053,996 | 422 | 991,807 | 991,807 blocks | 单块 flush 非常频繁 |
| `bc_metadata_writeback` | 413,552,133 | 420 | 983,786 | 983,786 blocks | metadata flush 基本都走 journal |
| `journal_commit_single` | 413,386,198 | 420 | 983,811 | 983,811 blocks | fsync 被单块 journal commit 主导 |
| `my_fsync_extent_flush` | 64,239,071 | 77 | 830,013 | - | extent metadata flush 次要但可见 |
| `my_fsync_data_flush` | 50,973,460 | 61 | 829,986 | - | data flush 不是主要问题 |
| `bc_data_batch_writeback` | 37,111,640 | 16 | 2,255,823 | 2,290,215 blocks | data 批量 flush 相对便宜 |

结论：`varmail` 中 `my_read`、`my_write`、目录操作都很轻，性能差距集中在 dirty `fsync` 的 inode table / metadata journal 路径。每次 fsync 之后 `ic_flush_inode()` 刷 inode table block，而该 block 作为 metadata 又触发 `journal_commit_single()`，导致单块 redo journal + home checkpoint 进入前台关键路径。

#### 定位结论

- `fileserver` 的根因是大工作集 + 大文件读写导致 Block Cache miss/dirty eviction，同时 EOF 扩展写路径仍然以 128KiB 左右的小批次推进，叠加 extent 分配、per-block bmap 和 metadata journal。
- `varmail` 的根因是 fsync 语义下的 inode/extent metadata 同步写回。data flush 本身不重，重的是 metadata block 被逐块 journal commit。
- 当前性能没有体现预期 uthread 优势的原因是 benchmark 并不只测线程切换。ShaOFS 前台路径做了大量同步文件系统工作，尤其是 metadata consistency 与 cache eviction；这些成本远大于节省掉的内核线程切换/系统调用开销。

### 2026-05-24 优化实验 1：fsync metadata 聚合（已撤销）

基于上面的 profile，首先尝试优化 `varmail` 中最重的 dirty `fsync` 路径：在 `my_fsync()` 内把 extent metadata block 与 inode table block 收集起来，尝试通过一个 metadata journal transaction 一次提交，而不是分别调用 `inode_flush_extent_metadata()` 和 `ic_flush_inode()` 触发多次单块 metadata flush。该改动通过了 `junction_run` 编译、`test_shaofs_syscall_correctness` 和 `test_shaofs_fsync_direct_verify` smoke test。

正式 Filebench 四场景单核回归结果如下；测试日志目录为 `/tmp/shaofs_filebench_opt1_20260524`，四个 `*.exit` 均为 `0`。

| Workload | Baseline ShaOFS ops/s | Opt1 ops/s | 变化 | Baseline MB/s | Opt1 MB/s | 结论 |
|---|---:|---:|---:|---:|---:|---|
| fileserver | 4,294.607 | 4,294.146 | -0.01% | 101.6 | 101.8 | 无实质变化 |
| webserver | 504,547.246 | 504,535.980 | -0.00% | 2652.8 | 2652.8 | 无实质变化 |
| varmail | 89,110.448 | 52,400.733 | -41.20% | 321.4 | 188.7 | 明显回退 |
| webproxy | 392,721.488 | 395,185.855 | +0.63% | 974.0 | 979.0 | 噪声级小幅提升 |

关键 per-operation 观察：

- `varmail` 回退集中在 dirty fsync 和写/删路径：`fsyncfile2` 从基线约 `1.451ms/op` 增至 `1.492ms/op`，但整体 flowops 从约 `6855 ops/s` 降至 `4031 ops/s`。说明额外收集/批处理 metadata 的前台 CPU/锁开销超过了减少 journal 次数的收益，且该 workload 的每次 fsync 多数只涉及 inode table block，真实可合并的 metadata block 数很少。
- `fileserver` 与 `webserver` 基本不受影响，符合预期：该改动只触及 `fsync` metadata 路径，对读热路径和大文件读路径帮助有限。
- `webproxy` 的 `+0.63%` 不足以覆盖 `varmail` 的 `-41.20%` 回退，不能保留。

结论：本优化方向的简单实现无效，已经从代码中撤销。原因不是“metadata batching”概念一定错误，而是当前实现放在每次 `fsync` 的前台路径中，需要额外遍历 extent metadata、去重、查 cache、加锁，并且 `varmail` 的实际可合并块数过少。后续如果继续优化 fsync，应转向更粗粒度、可摊销的方案，例如：

1. inode table block 延迟/合并 checkpoint，避免每次小文件 `fsync` 都同步提交 inode metadata。
2. journal group commit 在 `journal_commit_single()` 层做跨 uthread 聚合，而不是在单个 `my_fsync()` 内局部聚合。
3. 针对学术测试场景提供可配置 relaxed fsync/durability mode，但需要在报告中明确语义边界。

### 2026-05-24 优化实验 2：EOF 预分配消费（已撤销）

尝试让 `inode_alloc_append_blocks_locked()` 在 EOF 扩展写时复用已经由预分配/先前路径建立好的逻辑块映射，避免同一区间重复进入 extent 分配路径。该改动通过了 `test_shaofs_syscall_correctness`、`test_shaofs_many_extents`、`test_shaofs_concurrent_correctness` smoke test，但 `fileserver` 正式回归明显下降。

| 项目 | Baseline | EOF prealloc consume | 变化 |
|---|---:|---:|---:|
| IO Summary ops/s | 4,294.607 | 2,986.456 | -30.46% |
| MB/s | 101.6 | 70.6 | -30.51% |
| avg ms/op | 11.411 | 16.440 | +44.07% |
| `readfile1` | 390 ops/s, 65.864ms/op | 271 ops/s, 89.942ms/op | 回退 |
| `wrtfile1` | 391 ops/s, 43.595ms/op | 272 ops/s, 66.560ms/op | 回退 |

测试日志：`/tmp/shaofs_filebench_opt2_20260524/fileserver.log`。

结论：该改动增加了 EOF 写路径中的 bmap 扫描，且 Filebench fileserver 的实际写入模式并没有产生足够多可复用的预分配映射；额外查找成本超过收益。该实验已撤销。

### 2026-05-24 优化实验 3：大块读 cache-miss 直读（已撤销）

尝试在 `file_read_batch()` 中增加 “cached block 仍读 Block Cache、cache miss 的整块连续区间直接 `storage_read()` 到用户 buffer” 的路径，目标是降低 `fileserver` 大文件 `readwholefile` 对 256MiB Block Cache 的污染和 miss 插入成本。该改动通过了 `test_shaofs_syscall_correctness`、`test_shaofs_many_extents`、`test_shaofs_concurrent_correctness` smoke test。

正式单核 `fileserver` 回归结果为：

| 项目 | Baseline | Direct-miss read | 变化 |
|---|---:|---:|---:|
| IO Summary ops/s | 4,294.607 | 3,079.606 | -28.29% |
| MB/s | 101.6 | 72.8 | -28.35% |
| avg ms/op | 11.411 | 15.958 | +39.85% |
| `readfile1` | 390 ops/s, 65.864ms/op | 280 ops/s, 72.130ms/op | 回退 |
| `wrtfile1` | 391 ops/s, 43.595ms/op | 280 ops/s, 76.117ms/op | 明显回退 |

测试日志：`/tmp/shaofs_filebench_directmiss_20260524/fileserver.log`。

结论：该路径破坏了读写后的局部性，并把一部分原本可复用的缓存访问变成了小块同步 NVMe 读；同时 `storage_read()` 仍有 bounce buffer copy，不是零拷贝。该实验已撤销。

### 2026-05-24 优化实验 4：异步 journal checkpoint（已撤销）

尝试接入已有的多 journal slot 和后台 checkpoint 机制：前台只写 redo image + commit header，home metadata block 写回交给后台 uthread。第一次实验在 `fileserver` 13 秒左右提前失败，出现 stale metadata 读导致的 `statfile` / `appendfilerand` 错误；补充 pending-journal overlay 后可以完整运行，但性能严重回退。

| 项目 | Baseline | Async checkpoint + overlay | 变化 |
|---|---:|---:|---:|
| IO Summary ops/s | 4,294.607 | 1,382.930 | -67.80% |
| MB/s | 101.6 | 32.3 | -68.21% |
| avg ms/op | 11.411 | 35.632 | +212.25% |
| `readfile1` | 390 ops/s, 65.864ms/op | 125 ops/s, 202.961ms/op | 严重回退 |
| `wrtfile1` | 391 ops/s, 43.595ms/op | 126 ops/s, 127.952ms/op | 严重回退 |

测试日志：

- 失败版本：`/tmp/shaofs_filebench_asyncjournal_20260524/fileserver.log`
- overlay 版本：`/tmp/shaofs_filebench_asyncjournal_overlay_20260524/fileserver.log`

结论：异步 checkpoint 的方向理论上正确，但当前实现使运行期 metadata read 必须查询 pending journal overlay，并且 checkpoint worker 与前台在同一个 runtime kthread 上竞争 I/O 与调度时间。更重要的是，Block Cache 驱逐 metadata 后再读旧 home block 会破坏一致性，必须有更完整的 journal-aware metadata cache 才能安全高效。该实验已撤销。

### 2026-05-24 瓶颈验证：关闭 metadata journal（不保留）

Profile 已明确显示当前单核 `fileserver` 最大瓶颈不是 uthread 切换，而是前台 metadata redo journal：

- `fileserver`: `journal_commit_single` 约 `560.50s`，`bc_metadata_writeback` 约 `560.55s`。
- `varmail`: `journal_commit_single` 约 `413.39s`，`my_fsync_inode_flush` 约 `783.19s`。

因此临时把 `SHAOFS_CRASH_CONSISTENCY` 改为 `OFF` 跑了一组对照实验，用于验证 `fsync` / metadata journal 是否是主瓶颈。这个配置不满足 ShaOFS 的 crash consistency 目标，不能作为最终优化保留。语义边界如下：

- 保留：正常进程退出时 `final_flush()` 仍会 flush imap / GDT / inode cache / block cache，Filebench 每轮测试前也会重新 `mkfs`。
- 放弃：断电、崩溃恢复、metadata redo journal 语义；如果需要验证 crash consistency，应显式用 `cmake -S . -B build -DSHAOFS_CRASH_CONSISTENCY=ON` 构建。

实验性代码改动（已撤回为默认开启 crash consistency）：

- `junction/fs/CMakeLists.txt`: 实验期间曾把 `SHAOFS_CRASH_CONSISTENCY` 默认从 `ON` 改为 `OFF`。根据 crash consistency 目标，该默认值已恢复为 `ON`。

正式单核 Filebench 四场景结果如下。构建配置：`SHAOFS_CRASH_CONSISTENCY=OFF`、`SHAOFS_PROFILE_COMPILED=OFF`、`runtime_kthreads 1`。测试日志目录：`/tmp/shaofs_filebench_nojournal_single_20260524`。

| Workload | Baseline ShaOFS ops/s | Benchmark mode ops/s | 提升 | Benchmark mode MB/s | avg ms/op | 相对 ext4 |
|---|---:|---:|---:|---:|---:|---:|
| fileserver | 4,294.607 | 57,509.207 | +1239.10% / 13.39x | 1378.4 | 0.864 | 1.16x |
| webserver | 504,547.246 | 506,409.992 | +0.37% / 1.00x | 2662.6 | 0.196 | 1.64x |
| varmail | 89,110.448 | 315,500.278 | +254.06% / 3.54x | 1139.7 | 0.048 | 3.39x |
| webproxy | 392,721.488 | 400,884.273 | +2.08% / 1.02x | 994.9 | 0.247 | 1.59x |

`fileserver` per-operation 改善：

| Operation | Baseline ShaOFS | Benchmark mode | 观察 |
|---|---:|---:|---|
| `wrtfile1` | 391 ops/s, 43.595ms/op | 5228 ops/s, 0.953ms/op | metadata journal 移出前台后写路径大幅下降 |
| `readfile1` | 390 ops/s, 65.864ms/op | 5228 ops/s, 7.350ms/op | cache miss/读盘仍存在，但不再被 metadata 写回拖垮 |
| `createfile1` | 391 ops/s, 6.583ms/op | 5228 ops/s, 0.563ms/op | 创建路径受益最大之一 |
| `deletefile1` | 390 ops/s, 2.007ms/op | 5228 ops/s, 0.124ms/op | 删除路径不再同步 journal metadata |
| `appendfilerand1` | 391 ops/s, 5.779ms/op | 5228 ops/s, 0.388ms/op | 追加路径显著改善 |

额外说明：曾有一次 `SHAOFS_CRASH_CONSISTENCY=OFF` 但 `runtime_kthreads` 被 CMake 重新生成为 10 的误跑，`fileserver` 为 `60,609.796 ops/s`。该结果只用于确认瓶颈方向，不纳入正式单核对比；正式结果以上表 `57,509.207 ops/s` 为准。

定位结论：

- 当前 `fileserver` 未达到预期的根因是 metadata consistency 机制进入了前台关键路径。Block Cache 容量压力导致大量 metadata dirty eviction，每个 metadata block 都走 `journal_commit_single()`，这相当于把 create/delete/extent/inode 更新串行化为多次 NVMe journal 写 + home checkpoint。
- 禁用 metadata journal 后，ShaOFS 的用户态 syscall/uthread 优势立即显现：单核 `fileserver` 从远低于 ext4 变为高于 ext4，`varmail` 也从略低于 ext4 变为 3.39x ext4。这证明主瓶颈确实在前台 `fsync` / journal checkpoint 路径，而不是 syscall interception 或 uthread 调度本身。
- 后续优化必须同时保留 crash consistency 与高性能：metadata cache 必须 journal-aware，checkpoint 必须异步且不会让前台读到旧 home block，group commit 需要跨 uthread 聚合。

### 2026-05-24 保留优化：metadata-aware cache 与 extent-run 批处理

本节记录本轮最终保留的代码状态。与上一节 `SHAOFS_CRASH_CONSISTENCY=OFF` 的瓶颈验证不同，本节所有结果均在 crash consistency 开启时获得，可作为当前实现的有效回归结果。

#### 最终构建配置

| 项目 | 值 |
|---|---|
| ShaOFS CRASH_CONSISTENCY | `ON` |
| ShaOFS PROFILE_COMPILED | `OFF` |
| 构建类型 | `Release` |
| runtime kthreads | `1` |
| runtime spinning kthreads | `0` |
| enable storage | `1` |
| 最终 Filebench 日志目录 | `/tmp/shaofs_filebench_final_extent_run_four_20260524` |

本轮最终保留的优化集中在不改变 crash consistency 语义的路径上：

- Block Cache eviction 改为 metadata-aware LRU：优先避免驱逐 dirty 的核心 metadata block，尤其是 super/group 前部 metadata 与 group bitmap block，减少 metadata 被容量压力推入前台 journal/checkpoint 的次数。
- `file_read_batch()` 增加 extent-run 查询：一次 extent lookup 返回一段连续物理块，避免大文件 `readwholefile` 对每个 4KiB block 重复执行 inode bmap/extent 查找。
- existing-block write path 使用相同的 extent-run 查询，降低覆盖写和 append 已映射区间的 per-block lookup 成本。
- EOF append new-block write path 使用 `inode_alloc_append_blocks_locked()` 做批量分配，并优化 extent append 快路径：优先直接延长最后一个 extent，只有失败时再走 compact/rewrite。
- profiling 编译开关默认关闭，final benchmark 中热路径 profile 宏为 no-op，避免把定位成本带入正式结果。
- `journal_mark_clean()` / `journal_commit_blocks()` 保留 crash consistency 检查和同步 checkpoint 语义，没有恢复此前有 correctness 风险的异步 checkpoint。

以下实验明确没有保留：

- `SHAOFS_CRASH_CONSISTENCY=OFF`：只作为瓶颈验证，最终代码仍默认 `ON`。
- fsync 局部 metadata 聚合：`varmail` 回退明显。
- EOF 预分配消费扫描：`fileserver` 回退明显。
- direct-miss read / cold-read bypass：部分版本可把 `fileserver` 提升到约 `6.2K-6.6K ops/s`，但会稳定降低 `webserver` / `webproxy`，不满足“不损害其它 WML 场景”的约束。
- async checkpoint + pending overlay：存在 stale metadata correctness 风险或严重性能回退，未保留。

#### Correctness regression

最终保留代码重新验证了关键 crash-consistency 与基础正确性路径：

| 测试项 | 结果 | 说明 |
|---|---|---|
| `test_shaofs_fsync_direct_verify` | PASS | `rc=0`, 输出 `PASS fsync direct verify` |
| `journal_recovery_prepare/check` | PASS | `prepare_rc=124`, `check_rc=0`; `prepare_rc=124` 是 crash/timeout 注入的预期结果 |
| `test_shaofs_many_extents` | PASS | 验证 extent-run / 多 extent 映射没有破坏基础读写 |

#### 最终 Filebench 四项结果

| Workload | ops/s | MB/s | avg ms/op | 主要观察 |
|---|---:|---:|---:|---|
| `fileserver` | 4,927.911 | 116.8 | 9.950 | 相比本轮 crash-consistent baseline 有提升，但仍显著低于 ext4 |
| `webserver` | 508,520.770 | 2673.6 | 0.195 | 与优化前基本持平，读密集优势未受损 |
| `varmail` | 88,701.697 | 319.2 | 0.178 | 与优化前基本持平，fsync/journal 仍是主要瓶颈 |
| `webproxy` | 394,767.127 | 978.0 | 0.251 | 与优化前基本持平，未出现 direct-read 方案导致的回退 |

`fileserver` per-flowop：

| Flowop | ops/s | MB/s | avg ms/op |
|---|---:|---:|---:|
| `statfile1` | 448 | - | 0.087 |
| `deletefile1` | 448 | - | 2.616 |
| `readfile1` | 448 | 57.7 | 47.252 |
| `appendfilerand1` | 448 | 3.5 | 8.703 |
| `wrtfile1` | 448 | 55.6 | 45.199 |
| `createfile1` | 449 | - | 4.811 |

#### 性能提升对比

| 对比基线 | `fileserver` ops/s | 最终 ops/s | 提升 |
|---|---:|---:|---:|
| 普通 LRU 对照 `/tmp/shaofs_filebench_lru_fileserver_control_20260524` | 2,981.584 | 4,927.911 | +65.3% |
| metadata-aware 后、extent-run 前 `/tmp/shaofs_filebench_metaaware_clean_fileserver_rerun_20260524` | 3,923.356 | 4,927.911 | +25.6% |
| 2026-05-24 单次 baseline `/tmp/filebench_report_20260524` | 4,294.607 | 4,927.911 | +14.7% |

与此前 extent-run 最好的一次四项结果相比，本轮最终 formal run 的差异如下，整体可视为同一优化状态下的 run-to-run 波动：

| Workload | 上一次 extent-run ops/s | 最终 ops/s | 变化 |
|---|---:|---:|---:|
| `fileserver` | 5,149.025 | 4,927.911 | -4.3% |
| `webserver` | 508,900.217 | 508,520.770 | -0.1% |
| `varmail` | 88,420.620 | 88,701.697 | +0.3% |
| `webproxy` | 401,745.892 | 394,767.127 | -1.7% |

#### 最新 profile 结论

最终保留代码对应的一次 profile run 位于 `/tmp/shaofs_profile_fileserver_extent_run_20260524/fileserver.log`。该 run 启用了 profile，因此只用于定位，不作为正式性能数字。

关键事件：

| Event | count | total_us | avg_us | 观察 |
|---|---:|---:|---:|---|
| `file_read_batch` | 45,130 | 1,054,652,672 | 23,369 | 大文件读路径仍是最大单项瓶颈 |
| `file_write_eof_extension` | 20,908 | 967,791,484 | 46,288 | EOF 扩展写仍很重 |
| `extent_alloc` | 215,654 | 1,019,984,933 | 4,729 | bitmap/extent 分配仍是写路径核心成本 |
| `bc_backend_read` | 620,221 | 99,805,430 | 160 | Block Cache miss 大量触发真实读盘 |
| `journal_commit_single_grouped` | 27,615 | 50,679,218 | 1,835 | journal 仍显著，但在最终代码中不是唯一瓶颈 |
| `inode_bmap_locked` | 436,915 | 872,978,628 | 1,998 | extent-run 已减少 lookup 次数，但剩余 lookup 仍昂贵 |

与 extent-run 前的 profile 相比，`inode_bmap_locked` 调用次数从约 `1.25M` 降到约 `0.44M`，说明 extent-run 优化确实命中了 `fileserver` 大文件读写路径。不过 `fileserver` 仍未达到 ext4 水平，原因是剩余成本已经转移到 cache miss、EOF 扩展写、extent allocation 和 crash-consistent metadata 提交。

#### 当前瓶颈排序

1. `fileserver` 的大文件读回路径仍受 256MiB Block Cache 容量压力影响：Filebench fileset 大于 cache 后，`readwholefile` 出现大量 miss，前台必须真实读盘并做 copy。
2. EOF 扩展写仍以较小批次推进，虽然已经减少 bmap 次数，但每轮仍要做 block bitmap 分配、extent metadata 更新、data cache 插入和后续 dirty eviction。
3. crash-consistent metadata 提交仍是硬瓶颈：`journal_commit_single` / checkpoint 不能关闭，且 metadata dirty eviction 会把 journal I/O 带入前台。
4. 当前 Block Cache 仍是数据和 metadata 混合管理。metadata-aware LRU 只避免最坏驱逐，尚未提供独立 metadata cache、generation 状态或 journal-aware pinning。
5. fsync-heavy 的 `varmail` 仍主要受 inode table block journal/checkpoint 限制。简单地在 `my_fsync()` 内做局部 metadata 聚合没有收益，需要在 journal 层做跨 uthread group commit 或更完整的 epoch 机制。

#### 下一轮优化优先级

1. 设计 crash-consistent journal group commit：在 `journal_commit_single()` / `journal_commit_blocks()` 层跨 uthread 聚合 metadata block，使用 epoch/sequence 管理 redo image、commit header、checkpoint drain，避免每个 inode/bitmap block 单独写 journal 和 home block。
2. 做 journal-aware metadata cache：metadata block 在 checkpoint 完成前不能被普通 clean eviction；读取 metadata 时必须优先看到 cache/pending epoch 中的最新版本，避免此前 async checkpoint 实验中的 stale metadata 问题。
3. 扩大 EOF append 的分配粒度并降低 bitmap 更新频率：按文件预留连续物理 run，批量更新 bitmap 和 extent metadata，把 `extent_alloc` 次数继续降下来。
4. 重新设计 cold-read/direct-read 策略：只对可证明一次性的大文件读绕过 cache，并且必须有 admission policy，避免损害 `webserver` / `webproxy` 的缓存局部性。
5. 把 metadata cache 与 data cache 的 eviction 策略分离：对 inode table、bitmap、directory index、extent tree 使用独立容量或 pinning/generation 规则，避免 fileserver 大文件数据流把 metadata 推入 journal 前台路径。

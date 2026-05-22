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

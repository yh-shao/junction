● 当前 ShaOFS 的 DSA 策略可以概括为：

  ▎ DSA 只用于 buffered I/O 路径中 user buffer 与 ShaOFS block cache buffer 
  ▎ 之间的内存拷贝 offload；不用于 NVMe/SPDK 磁盘 I/O 本身，也不用于 O_DIRECT 
  ▎ 的用户 buffer DMA。
  ▎ 是否真的 offload 到 DSA 由“拷贝方向 + 拷贝大小 + batch 段数 + 当前 runtime 
  ▎ 是否适合 yield/隐藏延迟 + 硬件可用性”共同决定；否则统一回退 CPU memcpy()。

  下面按层次梳理。

  ---
  1. DSA API：按拷贝方向分类

  当前 DSA 接口定义在 junction/fs/shaofs/dsa.h:11：

  enum ShaofsDsaCopyKind {
      SHAOFS_DSA_READ_TO_USER = 0,
      SHAOFS_DSA_WRITE_FROM_USER,
      SHAOFS_DSA_INTERNAL,
      SHAOFS_DSA_KIND_NR,
  };

  也就是说，ShaOFS 不再只有一个全局阈值，而是把 copy 分为三类：

  1. SHAOFS_DSA_READ_TO_USER
    - ShaOFS block cache → 用户 read buffer。
    - 用于 buffered read batch 路径。
  2. SHAOFS_DSA_WRITE_FROM_USER
    - 用户 write buffer → ShaOFS block cache。
    - 用于 buffered write / append / EOF extension 路径。
  3. SHAOFS_DSA_INTERNAL
    - ShaOFS 内部 copy。
    - 当前主要是保留 API 默认类型；dsa_copy() / dsa_copyv() 会走 internal
  kind，见 junction/fs/shaofs/dsa.h:24。

  当前核心接口是：

  void dsa_copy_ex(void* dst, const void* src, size_t len, ShaofsDsaCopyKind 
  kind);
  void dsa_copyv_ex(const Segment* vecs, size_t nr, ShaofsDsaCopyKind kind);

  dsa_copy_ex() 是单段 copy；dsa_copyv_ex() 是多段 batch copy。

  ---
  2. 默认阈值策略

  默认策略在 junction/fs/shaofs/dsa.cc:41：

  static constexpr size_t kDsaDefaultWriteBusyBytes = 64 * 1024;
  static constexpr size_t kDsaDefaultReadBusyBytes = 128 * 1024;
  static constexpr size_t kDsaDefaultInternalBusyBytes = 64 * 1024;
  static constexpr size_t kDsaDefaultSingleBusyBytes = 256 * 1024;
  static constexpr size_t kDsaDefaultIdleBytes = 256 * 1024;
  static constexpr size_t kDsaDefaultParallelBytes = 128 * 1024;

  含义如下：

  ┌─────────────────┬──────────┬────────────────────────────────────────────┐
  │      场景       │ 默认阈值 │                    说明                    │
  ├─────────────────┼──────────┼────────────────────────────────────────────┤
  │ 单段 copy       │    256KB │ dsa_copy_ex() 的默认 offload 门槛          │
  ├─────────────────┼──────────┼────────────────────────────────────────────┤
  │ batch           │    128KB │ buffered read batch 总 copy                │
  │ read_to_user    │          │ 大小达到该值才考虑 DSA                     │
  ├─────────────────┼──────────┼────────────────────────────────────────────┤
  │ batch           │     64KB │ buffered write/append batch 总 copy        │
  │ write_from_user │          │ 大小达到该值才考虑 DSA                     │
  ├─────────────────┼──────────┼────────────────────────────────────────────┤
  │ batch internal  │     64KB │ 内部 batch copy 默认值                     │
  ├─────────────────┼──────────┼────────────────────────────────────────────┤
  │                 │          │ 如果 copy                                  │
  │ idle_bytes      │    256KB │ 足够大，即使当前不能明确隐藏延迟，也可考虑 │
  │                 │          │  DSA                                       │
  ├─────────────────┼──────────┼────────────────────────────────────────────┤
  │ parallel_bytes  │    128KB │ 如果当前进程线程数足够多，达到该值可考虑   │
  │                 │          │ DSA                                        │
  └─────────────────┴──────────┴────────────────────────────────────────────┘

  注意：single copy 和 batch copy 是两套阈值。
  当前 write-side 优化重点是让 append/new-block 写入走 batch，因此 write batch
  阈值是 64KB，而不是 single 的 256KB。

  ---
  3. 环境变量调节

  策略初始化在 junction/fs/shaofs/dsa.cc:173，支持这些环境变量：

  SHAOFS_DSA_ENABLE_HW
  SHAOFS_DSA_FORCE
  SHAOFS_DSA_STATS
  SHAOFS_DSA_WRITE_BUSY_BYTES
  SHAOFS_DSA_READ_BUSY_BYTES
  SHAOFS_DSA_BATCH_IDLE_BYTES
  SHAOFS_DSA_PARALLEL_BYTES

  具体含义：

  - SHAOFS_DSA_ENABLE_HW=0
    - 完全禁用 DSA hardware path，所有 copy 回退 CPU。
  - SHAOFS_DSA_FORCE=1
    - 达到基本阈值后，不再要求 runtime_async_would_hide_latency() 或并行度判断。
    - 但它不会绕过硬件不可用、preempt-disabled、copy size 阈值、batch
  段数等基本限制。
  - SHAOFS_DSA_STATS=1
    - 开启统计。
    - 统计在 final_flush() 中输出，调用点在 junction/fs/shaofs/file.cc:94。
  - SHAOFS_DSA_WRITE_BUSY_BYTES
    - 调整 SHAOFS_DSA_WRITE_FROM_USER 的 batch 阈值。
  - SHAOFS_DSA_READ_BUSY_BYTES
    - 调整 SHAOFS_DSA_READ_TO_USER 的 batch 阈值。
  - SHAOFS_DSA_BATCH_IDLE_BYTES
    - 调整“copy 足够大则即使没有明显 latency hiding 也 offload”的阈值。
  - SHAOFS_DSA_PARALLEL_BYTES
    - 调整多线程进程下更积极 offload 的阈值。

  目前没有单独的 env var 调整 single-copy 阈值；single-copy 默认仍是
  256KB，除非通过 ShaofsDsaOptions::threshold 统一覆盖。

  ---
  4. Offload 判断：不是“达到阈值就一定用 DSA”

  单段 copy 判断在 junction/fs/shaofs/dsa.cc:236：

  static bool dsa_should_offload(size_t len, ShaofsDsaCopyKind kind, 
  ShaofsDsaCpuReason* reason)

  决策顺序是：

  1. DSA hardware path 必须 ready。
  2. 当前线程必须 preempt_enabled()。
  3. len 必须达到该 kind 的 single threshold。
  4. 然后满足以下任一条件才 offload：
    - SHAOFS_DSA_FORCE=1
    - runtime_async_would_hide_latency() 为 true
    - 当前是 Junction thread 且进程线程数大于 2，并且 len >= parallel_bytes
    - len >= idle_bytes

  否则记录 no_latency_hide，走 CPU memcpy。

  batch copy 判断在 junction/fs/shaofs/dsa.cc:262：

  static bool dsa_batch_should_offload(size_t len, size_t nr, ShaofsDsaCopyKind 
  kind, ShaofsDsaCpuReason* reason)

  batch 额外要求：

  1. DSA ready。
  2. DSA batch request buffer 初始化成功。
  3. 当前 preempt_enabled()。
  4. batch segment 数量不能少于 DML 的最小 batch size。
  5. 总长度达到该方向的 batch threshold。
  6. 再进入和 single copy 类似的 force / latency hiding / parallel / idle 判断。

  因此当前策略是：

  ▎ 小 copy、无法隐藏延迟的 copy、preempt-disabled 上下文中的 copy、batch 
  ▎ 段数太少的 copy，都不会强行使用 DSA。

  ---
  5. CPU fallback 是默认安全路径

  dsa_copy_ex() 在 junction/fs/shaofs/dsa.cc:538，所有失败或不适合 offload
  的情况都会回退 CPU memcpy()：

  - DSA 未 ready。
  - preempt disabled。
  - below threshold。
  - copy 太大，超过 DML 32-bit 长度限制。
  - request 分配失败。
  - DML job setup 失败。
  - submit 失败。
  - completion error。

  dsa_copyv_ex() 在 junction/fs/shaofs/dsa.cc:615，batch 路径也一样：

  - nr == 1 时直接退化为 dsa_copy_ex()。
  - 有效 segment 数为 0 时直接返回。
  - 有效 segment 数为 1 时退化为 single copy。
  - segment 太多，超过 dsa_batch_task_num = 32，则整批 CPU memcpy_v()。
  - 单段长度超过 DML 限制，也 CPU fallback。
  - batch setup / submit / completion 失败，也 CPU fallback。

  所以 DSA 是一个 opportunistic offload path，不是 correctness-critical path。

  ---
  6. DSA 等待模型：submit 后当前 uthread park/yield

  DSA job submit 成功后，当前 uthread 会等待完成：

  - single copy：junction/fs/shaofs/dsa.cc:590
  - batch copy：junction/fs/shaofs/dsa.cc:713

  等待函数是 shaofs_dsa_wait()，在 junction/fs/shaofs/dsa.cc:439：

  if (likely(preempt_enabled())) {
      runtime_async_park(op);
      return;
  }
  while (!op->poll(op)) cpu_relax();

  设计意图是：

  1. 正常情况下，提交 DSA job 后调用 runtime_async_park()。
  2. 当前 uthread yield，让 Caladan runtime 运行其他 uthread。
  3. DSA 完成后 runtime 再唤醒该 uthread。
  4. 如果处于 preempt-disabled 状态，则不能进入 scheduler park，只能本地
  polling。

  不过当前 offload 判断本身已经拒绝 preempt-disabled
  上下文，所以这里主要是双保险。

  ---
  7. 当前 DSA 使用点：buffered read

  file_read() 在 junction/fs/shaofs/file.cc:254：

  ssize_t file_read(int inum, char* buf, off_t offset, size_t len)
  {
      if (len >= kFileBatchCopyMin) return file_read_batch(inum, buf, offset,
  len);
      return file_read_blockwise(inum, buf, offset, len);
  }

  kFileBatchCopyMin = 64KB，定义在 junction/fs/shaofs/file.cc:107。

  小 read

  小于 64KB 的 read 走 file_read_blockwise()。
  当前这里实际使用的是 CPU memcpy()，DSA 调用被注释掉了：

  junction/fs/shaofs/file.cc:164：

  // dsa_copy(buf + bytes_read, block_read_acc->data + blk_offset, copy_len);
  memcpy(buf + bytes_read, block_read_acc->data + blk_offset, copy_len);

  也就是说：

  ▎ 小 read 当前完全不使用 DSA。

  大 read

  大于等于 64KB 的 read 走 file_read_batch()，见
  junction/fs/shaofs/file.cc:179。

  它会：

  1. 获取 inode read lock。
  2. 最多聚合 dsa_batch_task_num = 32 个 block。
  3. 对 sparse hole 直接 memset() 补零，不走 DSA。
  4. 对真实物理块获取 block read accessor。
  5. 构造 Segment{dst=user_buf, src=block_cache_data, len}。
  6. 调用：

  junction/fs/shaofs/file.cc:241：

  dsa_copyv_ex(vecs.data(), vecs.size(), SHAOFS_DSA_READ_TO_USER);

  然后由 DSA policy 判断是否真的 offload。
  默认 read batch 阈值是 128KB，所以 64KB read 虽然进入 batch read
  path，但未必会 offload；可能仍因 below_threshold 回 CPU。

  ---
  8. 当前 DSA 使用点：buffered write existing-block

  普通 file_write() 在 junction/fs/shaofs/file.cc:615：

  if (len < kFileBatchCopyMin) return file_write_blockwise(inum, buf, offset,
  len);

  ssize_t batch_written = file_write_batch_existing(...);
  ...
  ssize_t eof_written = file_write_eof_extension(...);
  ...
  ssize_t rest = file_write_blockwise(...);

  小 write / scalar write

  小于 64KB 的 write 走 file_write_blockwise()。

  里面每个 block copy 会调用：

  junction/fs/shaofs/file.cc:303：

  dsa_copy_ex(block_write_acc->data + blk_offset,
              buf + bytes_written,
              copy_len,
              SHAOFS_DSA_WRITE_FROM_USER);

  slow path / allocation path 也一样，见 junction/fs/shaofs/file.cc:343。

  但是注意：

  - blockwise 每次最多 copy 一个 block 片段，通常 <= 4KB。
  - single-copy 默认 threshold 是 256KB。
  - 所以这些调用通常都会因为 below_threshold 回退 CPU memcpy()。

  也就是说：

  ▎ 小 write 虽然代码路径经过 dsa_copy_ex()，但默认策略下几乎不会真正 offload 到
  ▎  DSA。

  大 write 覆盖已有块

  大于等于 64KB 的普通 write 先尝试 file_write_batch_existing()，见
  junction/fs/shaofs/file.cc:529。

  它只批处理：

  - 写入范围仍在现有 file_size 内；
  - 物理块已经存在；
  - 不是 sparse hole；
  - 能拿到 block cache handle 的部分。

  构造 batch 后调用：

  junction/fs/shaofs/file.cc:599：

  dsa_copyv_ex(vecs.data(), vecs.size(), SHAOFS_DSA_WRITE_FROM_USER);

  默认 write batch 阈值是 64KB，因此这条路径是当前 write-side 最容易真正触发 DSA
   的路径之一。

  ---
  9. 当前 DSA 使用点：buffered O_APPEND

  当前 buffered O_APPEND 通过 file_write_append() 处理，见
  junction/fs/shaofs/file.cc:453。

  它的关键策略：

  1. 持有 inode write lock。
  2. 在锁内读取当前 file_size 作为 append 起点。
  3. 如果是 regular file、EOF block-aligned、剩余长度 >= 64KB，则走 batch
  new-block path。
  4. 否则走 scalar locked fallback。

  关键代码在 junction/fs/shaofs/file.cc:478：

  if (write_acc->type == REGULAR &&
      (current_offset & (BLOCK_SIZE - 1)) == 0 &&
      len - bytes_written >= kFileBatchCopyMin)
      ret = file_write_batch_new_blocks_locked(...);

  这条路径解决了两个问题：

  - 避免旧的 O_APPEND 在 inode 锁外 lseek(SEEK_END) 导致并发 append 抢同一个
  EOF。
  - 让大块 append/new-block 写可以形成 DSA batch。

  ---
  10. 当前 DSA 使用点：EOF extension / Filebench append-like write

  Filebench 的 append flowops 很多并不是 O_APPEND，而是：

  lseek(SEEK_END) + write()

  因此当前又加了 file_write_eof_extension()，见 junction/fs/shaofs/file.cc:491。

  它的策略：

  1. 持有 inode write lock。
  2. 检查当前 write offset 是否仍等于实时 inode->file_size。
  3. 如果仍是 EOF extension，并且 block-aligned、长度 >= 64KB，则走 batch
  new-block path。
  4. 否则 scalar fallback。

  关键检查在 junction/fs/shaofs/file.cc:513：

  if (current_offset != write_acc->file_size) break;

  这避免了基于过期 EOF 快照做错误 reservation。

  ---
  11. new-block batch write 如何使用 DSA

  new-block batch helper 是当前 write-side DSA 优化的核心，见
  junction/fs/shaofs/file.cc:411：

  static ssize_t file_write_batch_new_blocks_locked(...)

  它只处理非常保守的场景：

  - regular file；
  - offset 必须等于当前 inode->file_size；
  - offset 必须 block-aligned；
  - 只处理 full-block；
  - 最多一次 batch 32 个 block；
  - batch 总长度必须 >= 64KB。

  关键代码：

  junction/fs/shaofs/file.cc:417：

  size_t batch_blocks = MIN(full_blocks, (size_t)dsa_batch_task_num);
  size_t batch_len = batch_blocks * BLOCK_SIZE;
  if (batch_len < kFileBatchCopyMin) return 0;

  它为每个 4KB block 单独构造一个 segment：

  junction/fs/shaofs/file.cc:438：

  vecs.push_back({accessors.back()->data, buf + i * BLOCK_SIZE, BLOCK_SIZE});

  然后调用：

  junction/fs/shaofs/file.cc:441：

  dsa_copyv_ex(vecs.data(), vecs.size(), SHAOFS_DSA_WRITE_FROM_USER);

  这里特意保持“一块一个 segment”，而不是合并成一个大 segment。原因是：

  - dsa_copyv_ex() 如果只有一个 segment，会退化成 dsa_copy_ex()。
  - single-copy threshold 默认是 256KB。
  - 但 write batch threshold 是 64KB。
  - 因此多个 4KB segment 可以更容易走 DML batch path。

  copy 完成后才：

  - mark dirty；
  - 设置 block cache entry valid=1；
  - 更新 dirty data range；
  - 推进 inode->file_size。

  这保证 file_size 不会提前暴露未初始化数据。

  ---
  12. O_DIRECT 不使用 DSA

  file.cc 从 junction/fs/shaofs/file.cc:637 开始是 O_DIRECT I/O：

  /* O_DIRECT I/O */
  ssize_t file_read_direct(...)

  O_DIRECT 路径的目标是让 NVMe/SPDK 直接 DMA 到用户 buffer，前提是满足严格
  alignment / registration contract。

  因此：

  ▎ Direct I/O 是 storage DMA 路径，不是 DSA memory-copy offload 路径。

  DSA 当前主要服务于 buffered I/O，因为 buffered I/O 必须在 user buffer 和 block
   cache buffer 之间 copy。

  ---
  13. 统计输出

  统计结构在 junction/fs/shaofs/dsa.cc:88，会记录：

  - 每个 kind 的 DSA single ops / bytes。
  - 每个 kind 的 DSA batch ops / bytes / avg_segments。
  - 每个 kind 回退 CPU 的 reason 和 bytes。

  输出函数是 dsa_dump_stats()，见 junction/fs/shaofs/dsa.cc:735。

  只有设置：

  SHAOFS_DSA_STATS=1

  才会输出。

  输出时机在 final_flush()，见 junction/fs/shaofs/file.cc:94：

  flush_all_dirty_state();
  dsa_dump_stats();
  journal_mark_clean();

  常见 CPU fallback reason 包括：

  - hw_unavailable
  - preempt_disabled
  - below_threshold
  - too_few_segments
  - too_many_segments
  - no_latency_hide
  - too_large
  - alloc_fail
  - setup_fail
  - submit_fail
  - completion_error

  这套统计对判断 DSA 为什么没触发很重要。之前 Filebench 中大量 write copy 没触发
   DSA，主要就是 below_threshold。

  ---
  14. 当前策略的核心取舍

  当前策略不是“尽可能多用 DSA”，而是：

  1. 小 copy 坚决走 CPU
    - DSA job setup / submit / completion / uthread park 都有开销。
    - 4KB、8KB 这类 copy 用 DSA 通常不划算。
  2. read 比 write 更保守
    - read batch 默认 128KB。
    - write batch 默认 64KB。
    - 因为之前实验显示强行降低 read threshold 会让 webserver 明显退化。
  3. 优先让 append/new-block write 形成 batch
    - Filebench 写入常常是追加或 EOF extension。
    - 单个 4KB copy 不适合 DSA，但一个 syscall 内聚合多个 4KB block 后可以形成
  DML batch。
  4. 必须能隐藏延迟，或者 copy 足够大
    - 如果 runtime 判断当前 DSA park 不能隐藏延迟，且 copy 又不够大，就 CPU
  fallback。
    - SHAOFS_DSA_FORCE=1
  可以绕过这层收益判断，但仍不能绕过基本安全条件和大小阈值。
  5. 正确性优先于 aggressive batching
    - 不跨 syscall 聚合。
    - 不保留用户 buffer。
    - new-block valid 和 file_size 推进都在 copy 完成后。
    - append EOF reservation 在 inode write lock 内完成。
    - DSA copy 对调用者同步返回：函数返回时 copy 已完成或已 CPU fallback 完成。

  ---
  15. 一句话总结

  当前 ShaOFS 的 DSA 策略是：

  ▎ 对 buffered read/write 中足够大的 user-buffer ↔ block-cache-buffer copy 
  ▎ 做方向感知的 opportunistic DSA offload；优先通过 batch copy 覆盖大 
  ▎ read、已有块大 write、buffered append 和 EOF extension new-block write；小 
  ▎ copy、不能隐藏延迟、preempt-disabled、硬件不可用或 DML 失败时全部安全回退 
  ▎ CPU memcpy。

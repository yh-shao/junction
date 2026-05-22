#include "fs.h"
#include "dsa.h"
#include "junction/kernel/proc.h"
#include "dml/dml.h"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
extern "C" {
#include <asm/ops.h>
#include <base/slab.h>
#include <base/tcache.h>
#include <runtime/async.h>
}

struct ShaofsDsaReq {
    runtime_async_op async;
    uint32_t is_inited;    // 判断这个 job 是否已经初始化过
    dml_status_t status;
    alignas(64) dml_job_t job;
};
static_assert(offsetof(ShaofsDsaReq, async) == 0);

struct ShaofsDsaBatchReq {
    runtime_async_op async;
    dml_status_t status;
    uint32_t task_count;
    Segment vecs[dsa_batch_task_num];
    alignas(64) dml_job_t job;
};
static_assert(offsetof(ShaofsDsaBatchReq, async) == 0);


static uint32_t dsa_hw_job_size;         // job 描述符的大小
static uint32_t dsa_req_size;
static uint32_t dsa_batch_buffer_size;
static uint32_t dsa_batch_req_size;
static bool dsa_ready;

static constexpr size_t kDsaDefaultWriteBusyBytes = 64 * 1024;
static constexpr size_t kDsaDefaultReadBusyBytes = 128 * 1024;
static constexpr size_t kDsaDefaultInternalBusyBytes = 64 * 1024;
static constexpr size_t kDsaDefaultSingleBusyBytes = 256 * 1024;
static constexpr size_t kDsaDefaultIdleBytes = 256 * 1024;
static constexpr size_t kDsaDefaultParallelBytes = 128 * 1024;
static constexpr uintptr_t kDsaBatchBufferAlign = 64;

struct ShaofsDsaPolicy {
    bool enable_hw;
    bool force;
    bool stats;
    size_t single_busy_bytes[SHAOFS_DSA_KIND_NR];
    size_t batch_busy_bytes[SHAOFS_DSA_KIND_NR];
    size_t idle_bytes;
    size_t parallel_bytes;
};

static ShaofsDsaPolicy dsa_policy;

static struct slab    dsa_req_slab;
static struct tcache* dsa_req_tcache;
static DEFINE_SPINLOCK(dsa_req_pool_lock);
static DEFINE_PERTHREAD(struct tcache_perthread, dsa_req_pt);
static DEFINE_PERTHREAD(bool, dsa_req_pt_ready);

static struct slab    dsa_batch_req_slab;
static struct tcache* dsa_batch_req_tcache;
static DEFINE_SPINLOCK(dsa_batch_req_pool_lock);
static DEFINE_PERTHREAD(struct tcache_perthread, dsa_batch_req_pt);
static DEFINE_PERTHREAD(bool, dsa_batch_req_pt_ready);

enum ShaofsDsaCpuReason {
    DSA_CPU_HW_UNAVAILABLE = 0,
    DSA_CPU_PREEMPT_DISABLED,
    DSA_CPU_BELOW_THRESHOLD,
    DSA_CPU_TOO_FEW_SEGMENTS,
    DSA_CPU_TOO_MANY_SEGMENTS,
    DSA_CPU_NO_LATENCY_HIDE,
    DSA_CPU_TOO_LARGE,
    DSA_CPU_ALLOC_FAIL,
    DSA_CPU_SETUP_FAIL,
    DSA_CPU_SUBMIT_FAIL,
    DSA_CPU_COMPLETION_ERROR,
    DSA_CPU_REASON_NR,
};

struct ShaofsDsaStats {
    std::atomic<uint64_t> cpu_bytes[SHAOFS_DSA_KIND_NR][DSA_CPU_REASON_NR];
    std::atomic<uint64_t> dsa_single_ops[SHAOFS_DSA_KIND_NR];
    std::atomic<uint64_t> dsa_single_bytes[SHAOFS_DSA_KIND_NR];
    std::atomic<uint64_t> dsa_batch_ops[SHAOFS_DSA_KIND_NR];
    std::atomic<uint64_t> dsa_batch_bytes[SHAOFS_DSA_KIND_NR];
    std::atomic<uint64_t> dsa_batch_segments[SHAOFS_DSA_KIND_NR];
};

static ShaofsDsaStats dsa_stats;

static ShaofsDsaCopyKind dsa_normalize_kind(ShaofsDsaCopyKind kind)
{
    return kind >= SHAOFS_DSA_READ_TO_USER && kind < SHAOFS_DSA_KIND_NR ? kind : SHAOFS_DSA_INTERNAL;
}

static const char* dsa_kind_name(ShaofsDsaCopyKind kind)
{
    switch (dsa_normalize_kind(kind))
    {
    case SHAOFS_DSA_READ_TO_USER:
        return "read_to_user";
    case SHAOFS_DSA_WRITE_FROM_USER:
        return "write_from_user";
    case SHAOFS_DSA_INTERNAL:
        return "internal";
    default:
        return "unknown";
    }
}

static const char* dsa_cpu_reason_name(ShaofsDsaCpuReason reason)
{
    switch (reason)
    {
    case DSA_CPU_HW_UNAVAILABLE:
        return "hw_unavailable";
    case DSA_CPU_PREEMPT_DISABLED:
        return "preempt_disabled";
    case DSA_CPU_BELOW_THRESHOLD:
        return "below_threshold";
    case DSA_CPU_TOO_FEW_SEGMENTS:
        return "too_few_segments";
    case DSA_CPU_TOO_MANY_SEGMENTS:
        return "too_many_segments";
    case DSA_CPU_NO_LATENCY_HIDE:
        return "no_latency_hide";
    case DSA_CPU_TOO_LARGE:
        return "too_large";
    case DSA_CPU_ALLOC_FAIL:
        return "alloc_fail";
    case DSA_CPU_SETUP_FAIL:
        return "setup_fail";
    case DSA_CPU_SUBMIT_FAIL:
        return "submit_fail";
    case DSA_CPU_COMPLETION_ERROR:
        return "completion_error";
    default:
        return "unknown";
    }
}

static uint64_t dsa_parse_u64_env(const char* name, uint64_t fallback)
{
    const char* value = getenv(name);
    if (!value || *value == '\0') return fallback;

    char* end = nullptr;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') return fallback;
    return static_cast<uint64_t>(parsed);
}

static bool dsa_parse_bool_env(const char* name, bool fallback)
{
    return dsa_parse_u64_env(name, fallback ? 1 : 0) != 0;
}

static size_t dsa_parse_size_env(const char* name, size_t fallback)
{
    uint64_t parsed = dsa_parse_u64_env(name, fallback);
    return parsed > SIZE_MAX ? fallback : static_cast<size_t>(parsed);
}

static void dsa_policy_init(const ShaofsDsaOptions* opts)
{
    dsa_policy.enable_hw = opts ? opts->enable_hw : true;
    dsa_policy.force = opts ? opts->dsa_first : false;
    dsa_policy.stats = false;

    dsa_policy.single_busy_bytes[SHAOFS_DSA_READ_TO_USER] = kDsaDefaultSingleBusyBytes;
    dsa_policy.single_busy_bytes[SHAOFS_DSA_WRITE_FROM_USER] = kDsaDefaultSingleBusyBytes;
    dsa_policy.single_busy_bytes[SHAOFS_DSA_INTERNAL] = kDsaDefaultSingleBusyBytes;
    dsa_policy.batch_busy_bytes[SHAOFS_DSA_READ_TO_USER] = kDsaDefaultReadBusyBytes;
    dsa_policy.batch_busy_bytes[SHAOFS_DSA_WRITE_FROM_USER] = kDsaDefaultWriteBusyBytes;
    dsa_policy.batch_busy_bytes[SHAOFS_DSA_INTERNAL] = kDsaDefaultInternalBusyBytes;
    dsa_policy.idle_bytes = kDsaDefaultIdleBytes;
    dsa_policy.parallel_bytes = kDsaDefaultParallelBytes;

    if (opts && opts->threshold != 0)
    {
        for (int i = 0; i < SHAOFS_DSA_KIND_NR; i++)
        {
            dsa_policy.single_busy_bytes[i] = opts->threshold;
            dsa_policy.batch_busy_bytes[i] = opts->threshold;
        }
    }

    dsa_policy.enable_hw = dsa_parse_bool_env("SHAOFS_DSA_ENABLE_HW", dsa_policy.enable_hw);
    dsa_policy.force = dsa_parse_bool_env("SHAOFS_DSA_FORCE", dsa_policy.force);
    dsa_policy.stats = dsa_parse_bool_env("SHAOFS_DSA_STATS", dsa_policy.stats);
    dsa_policy.batch_busy_bytes[SHAOFS_DSA_WRITE_FROM_USER] = dsa_parse_size_env("SHAOFS_DSA_WRITE_BUSY_BYTES", dsa_policy.batch_busy_bytes[SHAOFS_DSA_WRITE_FROM_USER]);
    dsa_policy.batch_busy_bytes[SHAOFS_DSA_READ_TO_USER] = dsa_parse_size_env("SHAOFS_DSA_READ_BUSY_BYTES", dsa_policy.batch_busy_bytes[SHAOFS_DSA_READ_TO_USER]);
    dsa_policy.idle_bytes = dsa_parse_size_env("SHAOFS_DSA_BATCH_IDLE_BYTES", dsa_policy.idle_bytes);
    dsa_policy.parallel_bytes = dsa_parse_size_env("SHAOFS_DSA_PARALLEL_BYTES", dsa_policy.parallel_bytes);
}

static bool dsa_process_has_parallelism()
{
    return junction::IsJunctionThread() && junction::myproc().thread_count() > 2;
}

static void dsa_record_cpu(ShaofsDsaCopyKind kind, ShaofsDsaCpuReason reason, size_t bytes)
{
    if (likely(!dsa_policy.stats)) return;
    kind = dsa_normalize_kind(kind);
    if (reason < 0 || reason >= DSA_CPU_REASON_NR) return;
    dsa_stats.cpu_bytes[kind][reason].fetch_add(bytes, std::memory_order_relaxed);
}

static void dsa_record_single(ShaofsDsaCopyKind kind, size_t bytes)
{
    if (likely(!dsa_policy.stats)) return;
    kind = dsa_normalize_kind(kind);
    dsa_stats.dsa_single_ops[kind].fetch_add(1, std::memory_order_relaxed);
    dsa_stats.dsa_single_bytes[kind].fetch_add(bytes, std::memory_order_relaxed);
}

static void dsa_record_batch(ShaofsDsaCopyKind kind, size_t bytes, size_t segments)
{
    if (likely(!dsa_policy.stats)) return;
    kind = dsa_normalize_kind(kind);
    dsa_stats.dsa_batch_ops[kind].fetch_add(1, std::memory_order_relaxed);
    dsa_stats.dsa_batch_bytes[kind].fetch_add(bytes, std::memory_order_relaxed);
    dsa_stats.dsa_batch_segments[kind].fetch_add(segments, std::memory_order_relaxed);
}

static bool dsa_should_offload(size_t len, ShaofsDsaCopyKind kind, ShaofsDsaCpuReason* reason)
{
    kind = dsa_normalize_kind(kind);
    if (!dsa_ready)
    {
        *reason = DSA_CPU_HW_UNAVAILABLE;
        return false;
    }
    if (!preempt_enabled())
    {
        *reason = DSA_CPU_PREEMPT_DISABLED;
        return false;
    }
    if (len < dsa_policy.single_busy_bytes[kind])
    {
        *reason = DSA_CPU_BELOW_THRESHOLD;
        return false;
    }
    if (dsa_policy.force || runtime_async_would_hide_latency()) return true;
    if (dsa_process_has_parallelism() && len >= dsa_policy.parallel_bytes) return true;
    if (len >= dsa_policy.idle_bytes) return true;

    *reason = DSA_CPU_NO_LATENCY_HIDE;
    return false;
}

static bool dsa_batch_should_offload(size_t len, size_t nr, ShaofsDsaCopyKind kind, ShaofsDsaCpuReason* reason)
{
    kind = dsa_normalize_kind(kind);
    if (!dsa_ready || dsa_batch_req_size == 0)
    {
        *reason = DSA_CPU_HW_UNAVAILABLE;
        return false;
    }
    if (!preempt_enabled())
    {
        *reason = DSA_CPU_PREEMPT_DISABLED;
        return false;
    }
    if (nr < DML_MIN_BATCH_SIZE)
    {
        *reason = DSA_CPU_TOO_FEW_SEGMENTS;
        return false;
    }
    if (len < dsa_policy.batch_busy_bytes[kind])
    {
        *reason = DSA_CPU_BELOW_THRESHOLD;
        return false;
    }
    if (dsa_policy.force || runtime_async_would_hide_latency()) return true;
    if (dsa_process_has_parallelism() && len >= dsa_policy.parallel_bytes) return true;
    if (len >= dsa_policy.idle_bytes) return true;

    *reason = DSA_CPU_NO_LATENCY_HIDE;
    return false;
}

static int dsa_req_pool_init(size_t req_size)
{
    if (req_size < TCACHE_MIN_ITEM_SIZE) return -EINVAL;
    int ret;

    spin_lock_np(&dsa_req_pool_lock);
    if (dsa_req_tcache)
    {
        ret = dsa_req_size == req_size ? 0 : -EINVAL;
        spin_unlock_np(&dsa_req_pool_lock);
        return ret;
    }

    ret = slab_create(&dsa_req_slab, "shaofs_dsa_req", req_size, 0);
    if (ret)
    {
        spin_unlock_np(&dsa_req_pool_lock);
        return ret;
    }

    dsa_req_tcache = slab_create_tcache(&dsa_req_slab, TCACHE_DEFAULT_MAG_SIZE);
    if (!dsa_req_tcache)
    {
        slab_destroy(&dsa_req_slab);
        spin_unlock_np(&dsa_req_pool_lock);
        return -ENOMEM;
    }

    dsa_req_size = req_size;
    spin_unlock_np(&dsa_req_pool_lock);
    return 0;
}
static struct tcache_perthread* shaofs_dsa_req_get_pt()
{
    struct tcache_perthread* pt = perthread_ptr(dsa_req_pt);
    if (unlikely(!perthread_read(dsa_req_pt_ready)))
    {
        tcache_init_perthread(dsa_req_tcache, pt);
        perthread_store(dsa_req_pt_ready, true);
    }
    return pt;
}
static ShaofsDsaReq* shaofs_dsa_req_alloc()
{
    if (unlikely(!dsa_req_tcache)) return nullptr;
    preempt_disable();
    ShaofsDsaReq* req = reinterpret_cast<ShaofsDsaReq*>(tcache_alloc(shaofs_dsa_req_get_pt()));
    preempt_enable();
    return req;
}
static void shaofs_dsa_req_free(ShaofsDsaReq* req)
{
    if (!req) return;
    preempt_disable();
    tcache_free(shaofs_dsa_req_get_pt(), req);
    preempt_enable();
}

static int dsa_batch_req_pool_init(size_t req_size)
{
    if (req_size < TCACHE_MIN_ITEM_SIZE) return -EINVAL;
    int ret;

    spin_lock_np(&dsa_batch_req_pool_lock);
    if (dsa_batch_req_tcache)
    {
        ret = dsa_batch_req_size == req_size ? 0 : -EINVAL;
        spin_unlock_np(&dsa_batch_req_pool_lock);
        return ret;
    }

    ret = slab_create(&dsa_batch_req_slab, "shaofs_dsa_batch_req", req_size, 0);
    if (ret)
    {
        spin_unlock_np(&dsa_batch_req_pool_lock);
        return ret;
    }

    dsa_batch_req_tcache = slab_create_tcache(&dsa_batch_req_slab, TCACHE_DEFAULT_MAG_SIZE);
    if (!dsa_batch_req_tcache)
    {
        slab_destroy(&dsa_batch_req_slab);
        spin_unlock_np(&dsa_batch_req_pool_lock);
        return -ENOMEM;
    }

    dsa_batch_req_size = req_size;
    spin_unlock_np(&dsa_batch_req_pool_lock);
    return 0;
}
static struct tcache_perthread* shaofs_dsa_batch_req_get_pt()
{
    struct tcache_perthread* pt = perthread_ptr(dsa_batch_req_pt);
    if (unlikely(!perthread_read(dsa_batch_req_pt_ready)))
    {
        tcache_init_perthread(dsa_batch_req_tcache, pt);
        perthread_store(dsa_batch_req_pt_ready, true);
    }
    return pt;
}
static ShaofsDsaBatchReq* shaofs_dsa_batch_req_alloc()
{
    if (unlikely(!dsa_batch_req_tcache)) return nullptr;
    preempt_disable();
    ShaofsDsaBatchReq* req = reinterpret_cast<ShaofsDsaBatchReq*>(tcache_alloc(shaofs_dsa_batch_req_get_pt()));
    preempt_enable();
    return req;
}
static void shaofs_dsa_batch_req_free(ShaofsDsaBatchReq* req)
{
    if (!req) return;
    preempt_disable();
    tcache_free(shaofs_dsa_batch_req_get_pt(), req);
    preempt_enable();
}

static uint8_t* dsa_batch_buffer(ShaofsDsaBatchReq* req)
{
    uintptr_t end = reinterpret_cast<uintptr_t>(&req->job) + dsa_hw_job_size;
    return reinterpret_cast<uint8_t*>((end + kDsaBatchBufferAlign - 1) & ~(kDsaBatchBufferAlign - 1));
}

static void memcpy_v(const Segment* vecs, size_t nr)
{
    for (size_t i = 0; i < nr; i++)
    {
        if (unlikely(!vecs[i].dst) || unlikely(!vecs[i].src)) return;
        if (vecs[i].len == 0) continue;
        memcpy(vecs[i].dst, vecs[i].src, vecs[i].len);
    }
}

static bool shaofs_dsa_poll(runtime_async_op* op)
{
    ShaofsDsaReq* req = reinterpret_cast<ShaofsDsaReq*>(op);
    req->status = dml_check_job(&req->job);
    return req->status != DML_STATUS_BEING_PROCESSED;
}

static bool shaofs_dsa_batch_poll(runtime_async_op* op)
{
    ShaofsDsaBatchReq* req = reinterpret_cast<ShaofsDsaBatchReq*>(op);
    req->status = dml_check_job(&req->job);
    return req->status != DML_STATUS_BEING_PROCESSED;
}

static void shaofs_dsa_wait(runtime_async_op* op)
{
    if (likely(preempt_enabled()))
    {
        runtime_async_park(op);
        return;
    }

    // runtime_async_park enters Caladan scheduler and requires a clean preemption state. 
    // If the caller is already preempt-disabled, keep the DSA job off the scheduler pending list and poll it locally.
    while (!op->poll(op)) cpu_relax();
}

int dsa_init(const ShaofsDsaOptions* opts)
{
    dsa_policy_init(opts);
    if (!dsa_policy.enable_hw)
    {
        log_info("[shaofs_dsa_init] DSA hardware path disabled by policy.\n");
        return 0;
    }

    dml_path_t execution_path = DML_PATH_HW;
    if (dml_get_job_size(execution_path, &dsa_hw_job_size) != DML_STATUS_OK)
    {
        log_info("[shaofs_dsa_init] Warning: Could not get DML job size. Falling back to CPU memcpy.\n");
        return 0;
    }
    dml_job_t* job = reinterpret_cast<dml_job_t*>(malloc(dsa_hw_job_size));
    if (!job)
    {
        log_info("[shaofs_dsa_init] Warning: Could not allocate DML job.\n");
        return -ENOMEM;
    }
    if (dml_init_job(execution_path, job) != DML_STATUS_OK)
    {
        log_info("[shaofs_dsa_init] Warning: DML init failed. Falling back to CPU memcpy.\n");
        free(job);
        return 0;
    }

    job->operation = DML_OP_BATCH;
    dml_status_t status = dml_get_batch_size(job, dsa_batch_task_num, &dsa_batch_buffer_size);
    if (status != DML_STATUS_OK)
    {
        dsa_batch_buffer_size = 0;
        log_info("[shaofs_dsa_init] Warning: DML batch setup failed (Status: %d). Multi-segment copies will use CPU memcpy.\n", status);
    }

    uint8_t src = 0xAA, dst = 0x00;
    job->operation             = DML_OP_MEM_MOVE;
    job->source_first_ptr      = &src;
    job->destination_first_ptr = &dst;
    job->source_length         = 1;
    job->destination_length    = 1;

    status = dml_execute_job(job, DML_WAIT_MODE_BUSY_POLL);
    dml_finalize_job(job);
    free(job);
    if (status != DML_STATUS_OK || dst != 0xAA)
    {
        log_info("[shaofs_dsa_init] DSA execution failed (Status: %d). Falling back to CPU memcpy.\n", status);
        return 0;
    }

    dsa_req_size = offsetof(ShaofsDsaReq, job) + dsa_hw_job_size;
    int ret = dsa_req_pool_init(dsa_req_size);
    if (ret)
    {
        log_info("[shaofs_dsa_init] Warning: Could not initialize DSA request cache. Falling back to CPU memcpy.\n");
        return ret;
    }

    if (dsa_batch_buffer_size != 0)
    {
        const uintptr_t batch_req_size = offsetof(ShaofsDsaBatchReq, job) + dsa_hw_job_size + kDsaBatchBufferAlign + dsa_batch_buffer_size;
        ret = dsa_batch_req_pool_init(batch_req_size);
        if (ret)
        {
            dsa_batch_buffer_size = 0;
            dsa_batch_req_size = 0;
            log_info("[shaofs_dsa_init] Warning: Could not initialize DSA batch request cache. Multi-segment copies will use CPU memcpy.\n");
        }
    }

    dsa_ready = true;
    log_info("[shaofs_dsa_init] DSA hardware path initialized successfully (force=%d, stats=%d, write_busy=%zu, read_busy=%zu, idle=%zu, parallel=%zu).\n", dsa_policy.force, dsa_policy.stats, dsa_policy.batch_busy_bytes[SHAOFS_DSA_WRITE_FROM_USER], dsa_policy.batch_busy_bytes[SHAOFS_DSA_READ_TO_USER], dsa_policy.idle_bytes, dsa_policy.parallel_bytes);
    return 0;
}

void dsa_copy(void* dst, const void* src, size_t len)
{
    dsa_copy_ex(dst, src, len, SHAOFS_DSA_INTERNAL);
}

void dsa_copy_ex(void* dst, const void* src, size_t len, ShaofsDsaCopyKind kind)
{
    kind = dsa_normalize_kind(kind);
    if (unlikely(!dst) || unlikely(!src) || len == 0) return;

    ShaofsDsaCpuReason reason = DSA_CPU_BELOW_THRESHOLD;
    if (len > DML_MAX_32U)
    {
        dsa_record_cpu(kind, DSA_CPU_TOO_LARGE, len);
        memcpy(dst, src, len);
        return;
    }
    if (!dsa_should_offload(len, kind, &reason) || dsa_req_size == 0)
    {
        dsa_record_cpu(kind, reason, len);
        memcpy(dst, src, len);
        return;
    }

    ShaofsDsaReq* req = shaofs_dsa_req_alloc();
    if (!req)
    {
        dsa_record_cpu(kind, DSA_CPU_ALLOC_FAIL, len);
        memcpy(dst, src, len);
        return;
    }

    if (unlikely(req->is_inited != 0xDEADBEEF))   // 只会进行一次初始化
    {
        dml_status_t init_status = dml_init_job(DML_PATH_HW, &req->job);
        if (unlikely(init_status != DML_STATUS_OK))
        {
            shaofs_dsa_req_free(req);
            dsa_record_cpu(kind, DSA_CPU_SETUP_FAIL, len);
            memcpy(dst, src, len);
            return;
        }

        req->job.operation  = DML_OP_MEM_MOVE;
        req->job.flags     |= DML_FLAG_BLOCK_ON_FAULT;

        req->is_inited = 0xDEADBEEF; // 标记为已初始化
    }

    req->async.poll     = shaofs_dsa_poll;
    req->async.complete = nullptr;
    req->status                    = DML_STATUS_BEING_PROCESSED;
    req->job.source_first_ptr      = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(src));
    req->job.destination_first_ptr = reinterpret_cast<uint8_t*>(dst);
    req->job.source_length         = len;
    req->job.destination_length    = len;

    dml_status_t status = dml_submit_job(&req->job);
    if (unlikely(status != DML_STATUS_OK))
    {
        shaofs_dsa_req_free(req);
        dsa_record_cpu(kind, DSA_CPU_SUBMIT_FAIL, len);
        memcpy(dst, src, len);
        return;
    }

    dsa_record_single(kind, len);
    shaofs_dsa_wait(&req->async);
    status = req->status;
    shaofs_dsa_req_free(req);
    if (unlikely(status != DML_STATUS_OK))
    {
        dsa_record_cpu(kind, DSA_CPU_COMPLETION_ERROR, len);
        memcpy(dst, src, len);
    }
}

void dsa_copyv(const Segment* vecs, size_t nr)
{
    dsa_copyv_ex(vecs, nr, SHAOFS_DSA_INTERNAL);
}

void dsa_copyv_ex(const Segment* vecs, size_t nr, ShaofsDsaCopyKind kind)   // 复制 nr 个 buffer
{
    kind = dsa_normalize_kind(kind);
    if (unlikely(!vecs) || nr == 0) return;

    if (nr == 1)
    {
        dsa_copy_ex(vecs[0].dst, vecs[0].src, vecs[0].len, kind);
        return;
    }

    size_t total_len = 0;
    size_t active_nr = 0;
    Segment active[dsa_batch_task_num];

    for (size_t i = 0; i < nr; i++)
    {
        if (unlikely(!vecs[i].dst) || unlikely(!vecs[i].src)) return;
        if (vecs[i].len == 0) continue;
        if (unlikely(vecs[i].len > DML_MAX_32U) || active_nr == dsa_batch_task_num)
        {
            dsa_record_cpu(kind, unlikely(vecs[i].len > DML_MAX_32U) ? DSA_CPU_TOO_LARGE : DSA_CPU_TOO_MANY_SEGMENTS, total_len + vecs[i].len);
            memcpy_v(vecs, nr);
            return;
        }
        active[active_nr++] = vecs[i];
        total_len += vecs[i].len;
    }

    if (active_nr == 0) return;
    if (active_nr == 1)
    {
        dsa_copy_ex(active[0].dst, active[0].src, active[0].len, kind);
        return;
    }

    ShaofsDsaCpuReason reason = DSA_CPU_BELOW_THRESHOLD;
    if (!dsa_batch_should_offload(total_len, active_nr, kind, &reason))
    {
        dsa_record_cpu(kind, reason, total_len);
        memcpy_v(active, active_nr);
        return;
    }

    ShaofsDsaBatchReq* req = shaofs_dsa_batch_req_alloc();
    if (!req)
    {
        dsa_record_cpu(kind, DSA_CPU_ALLOC_FAIL, total_len);
        memcpy_v(active, active_nr);
        return;
    }

    dml_status_t status = dml_init_job(DML_PATH_HW, &req->job);
    if (unlikely(status != DML_STATUS_OK))
    {
        shaofs_dsa_batch_req_free(req);
        dsa_record_cpu(kind, DSA_CPU_SETUP_FAIL, total_len);
        memcpy_v(active, active_nr);
        return;
    }

    uint32_t batch_buffer_size = 0;
    req->job.operation = DML_OP_BATCH;
    status = dml_get_batch_size(&req->job, static_cast<uint32_t>(active_nr), &batch_buffer_size);
    if (unlikely(status != DML_STATUS_OK || batch_buffer_size > dsa_batch_buffer_size))
    {
        dml_finalize_job(&req->job);
        shaofs_dsa_batch_req_free(req);
        dsa_record_cpu(kind, DSA_CPU_SETUP_FAIL, total_len);
        memcpy_v(active, active_nr);
        return;
    }

    req->async.poll     = shaofs_dsa_batch_poll;
    req->async.complete = nullptr;
    req->status         = DML_STATUS_BEING_PROCESSED;
    req->task_count     = static_cast<uint32_t>(active_nr);
    req->job.destination_first_ptr = dsa_batch_buffer(req);
    req->job.destination_length    = batch_buffer_size;

    for (uint32_t i = 0; i < req->task_count; i++)
    {
        req->vecs[i] = active[i];
        status = dml_batch_set_mem_move_by_index(&req->job, i, const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(active[i].src)), reinterpret_cast<uint8_t*>(active[i].dst), static_cast<uint32_t>(active[i].len), DML_FLAG_BLOCK_ON_FAULT);
        if (unlikely(status != DML_STATUS_OK))
        {
            dml_finalize_job(&req->job);
            shaofs_dsa_batch_req_free(req);
            dsa_record_cpu(kind, DSA_CPU_SETUP_FAIL, total_len);
            memcpy_v(active, active_nr);
            return;
        }
    }

    status = dml_submit_job(&req->job);
    if (unlikely(status != DML_STATUS_OK))
    {
        dml_finalize_job(&req->job);
        shaofs_dsa_batch_req_free(req);
        dsa_record_cpu(kind, DSA_CPU_SUBMIT_FAIL, total_len);
        memcpy_v(active, active_nr);
        return;
    }

    dsa_record_batch(kind, total_len, active_nr);
    shaofs_dsa_wait(&req->async);
    status = req->status;
    dml_finalize_job(&req->job);
    if (unlikely(status != DML_STATUS_OK))
    {
        dsa_record_cpu(kind, DSA_CPU_COMPLETION_ERROR, total_len);
        memcpy_v(req->vecs, req->task_count);
    }
    shaofs_dsa_batch_req_free(req);
}

void dsa_dump_stats()
{
    if (!dsa_policy.stats) return;

    for (int i = 0; i < SHAOFS_DSA_KIND_NR; i++)
    {
        auto kind = static_cast<ShaofsDsaCopyKind>(i);
        uint64_t single_ops = dsa_stats.dsa_single_ops[i].load(std::memory_order_relaxed);
        uint64_t single_bytes = dsa_stats.dsa_single_bytes[i].load(std::memory_order_relaxed);
        uint64_t batch_ops = dsa_stats.dsa_batch_ops[i].load(std::memory_order_relaxed);
        uint64_t batch_bytes = dsa_stats.dsa_batch_bytes[i].load(std::memory_order_relaxed);
        uint64_t batch_segments = dsa_stats.dsa_batch_segments[i].load(std::memory_order_relaxed);
        log_info("[shaofs_dsa_stats] kind=%s dsa_single_ops=%lu dsa_single_bytes=%lu dsa_batch_ops=%lu dsa_batch_bytes=%lu dsa_batch_avg_segments=%lu", dsa_kind_name(kind), single_ops, single_bytes, batch_ops, batch_bytes, batch_ops ? batch_segments / batch_ops : 0);

        for (int r = 0; r < DSA_CPU_REASON_NR; r++)
        {
            uint64_t bytes = dsa_stats.cpu_bytes[i][r].load(std::memory_order_relaxed);
            if (bytes == 0) continue;
            log_info("[shaofs_dsa_stats] kind=%s cpu_reason=%s bytes=%lu", dsa_kind_name(kind), dsa_cpu_reason_name(static_cast<ShaofsDsaCpuReason>(r)), bytes);
        }
    }
}

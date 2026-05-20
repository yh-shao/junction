#include "fs.h"
#include "dsa.h"
#include "junction/kernel/proc.h"
#include "dml/dml.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
extern "C" {
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
    uint32_t is_inited;    // 判断这个 job 是否已经初始化过
    dml_status_t status;
    alignas(64) dml_job_t job;
};
static_assert(offsetof(ShaofsDsaBatchReq, async) == 0);

static uint32_t dsa_hw_job_size;         // job 描述符的大小
static uint32_t dsa_batch_buffer_size;   // batch buffer 的大小
static uint32_t dsa_req_size;
static uint32_t dsa_batch_req_size;
static size_t dsa_threshold;
static bool dsa_first;
static bool dsa_ready;
static constexpr size_t kDefaultDsaThreshold = 64 * 1024;
static constexpr uintptr_t kDsaBatchBufferAlignment = 64;

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

static bool dsa_process_has_parallelism()
{
    return junction::IsJunctionThread() && junction::myproc().thread_count() > 2;
}
static bool dsa_should_offload(size_t len)
{
    if (!preempt_enabled()) return false;
    return len >= dsa_threshold && dsa_ready && (dsa_first || runtime_async_would_hide_latency() || dsa_process_has_parallelism());
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

static void memcpy_v(const Segment* vecs, size_t nr)
{
    for (size_t i = 0; i < nr; i++)
    {
        if (vecs[i].len == 0) continue;
        memcpy(vecs[i].dst, vecs[i].src, vecs[i].len);
    }
}

static uint8_t* dsa_batch_buffer(ShaofsDsaBatchReq* req)
{
    uintptr_t start = reinterpret_cast<uintptr_t>(&req->job) + dsa_hw_job_size;
    start = (start + kDsaBatchBufferAlignment - 1) & ~(kDsaBatchBufferAlignment - 1);
    return reinterpret_cast<uint8_t*>(start);
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
    req->status = dml_check_job(reinterpret_cast<dml_job_t*>(&req->job));
    return req->status != DML_STATUS_BEING_PROCESSED;
}

int dsa_init(const ShaofsDsaOptions* opts)
{
    if (opts && !opts->enable_hw) return 0;

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

    dml_status_t status = dml_get_batch_size(job, dsa_batch_task_num, &dsa_batch_buffer_size);
    if (status != DML_STATUS_OK)
    {
        log_info("[shaofs_dsa_init] Warning: Could not get DSA batch size (Status: %d). Falling back to CPU memcpy.\n", status);
        dml_finalize_job(job);
        free(job);
        return 0;
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

    dsa_batch_req_size = offsetof(ShaofsDsaBatchReq, job) + dsa_hw_job_size + kDsaBatchBufferAlignment + dsa_batch_buffer_size;
    ret = dsa_batch_req_pool_init(dsa_batch_req_size);
    if (ret)
    {
        log_info("[shaofs_dsa_init] Warning: Could not initialize DSA batch request cache. Falling back to CPU memcpy.\n");
        return ret;
    }

    dsa_threshold = opts ? opts->threshold : kDefaultDsaThreshold;
    dsa_first     = opts ? opts->dsa_first : false;
    dsa_ready     = true;
    log_info("[shaofs_dsa_init] DSA hardware path initialized successfully (threshold=%zu bytes, force=%d).\n", dsa_threshold, dsa_first);
    return 0;
}

void dsa_copy(void* dst, const void* src, size_t len)
{
    if (unlikely(!dst) || unlikely(!src) || len == 0) return;

    if (!dsa_should_offload(len) || dsa_req_size == 0 || len > DML_MAX_32U)   // 使用 CPU 进行 memcpy
    {
        memcpy(dst, src, len);
        return;
    }

    ShaofsDsaReq* req = shaofs_dsa_req_alloc();
    if (!req)
    {
        memcpy(dst, src, len);
        return;
    }

    if (unlikely(req->is_inited != 0xDEADBEEF))   // 只会进行一次初始化
    {
        dml_status_t init_status = dml_init_job(DML_PATH_HW, &req->job);
        if (unlikely(init_status != DML_STATUS_OK))
        {
            shaofs_dsa_req_free(req);
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
        memcpy(dst, src, len);
        return;
    }

    runtime_async_park(&req->async);
    status = req->status;
    shaofs_dsa_req_free(req);
    if (unlikely(status != DML_STATUS_OK)) memcpy(dst, src, len);
}

static void dsa_copyv_chunk(const Segment* vecs, size_t nr)
{
    size_t total_len = 0;
    for (size_t i = 0; i < nr; i++)
    {
        if (unlikely(!vecs[i].dst) || unlikely(!vecs[i].src)) return;
        if (unlikely(vecs[i].len > DML_MAX_32U))
        {
            memcpy_v(vecs, nr);
            return;
        }
        total_len += vecs[i].len;
    }

    if (nr == 0 || total_len == 0) return;
    if (nr == 1)
    {
        dsa_copy(vecs[0].dst, vecs[0].src, vecs[0].len);
        return;
    }

    if (!dsa_should_offload(total_len) || dsa_batch_req_size == 0)
    {
        memcpy_v(vecs, nr);
        return;
    }

    ShaofsDsaBatchReq* req = shaofs_dsa_batch_req_alloc();
    if (!req)
    {
        memcpy_v(vecs, nr);
        return;
    }

    dml_job_t* job = reinterpret_cast<dml_job_t*>(&req->job);
    if (unlikely(req->is_inited != 0xDEADBEEF))   // 只会进行一次初始化
    {
        dml_status_t status = dml_init_job(DML_PATH_HW, job);
        if (unlikely(status != DML_STATUS_OK))
        {
            shaofs_dsa_batch_req_free(req);
            memcpy_v(vecs, nr);
            return;
        }

        job->operation      = DML_OP_BATCH;

        req->is_inited = 0xDEADBEEF;
    }

    req->async.poll     = shaofs_dsa_batch_poll;
    req->async.complete = nullptr;
    req->status = DML_STATUS_BEING_PROCESSED;

    uint32_t batch_buffer_size = 0;
    dml_status_t status = dml_get_batch_size(job, static_cast<uint32_t>(nr), &batch_buffer_size);
    if (unlikely(status != DML_STATUS_OK || batch_buffer_size > dsa_batch_buffer_size))
    {
        shaofs_dsa_batch_req_free(req);
        memcpy_v(vecs, nr);
        return;
    }
    job->destination_first_ptr = dsa_batch_buffer(req);
    job->destination_length    = batch_buffer_size;

    for (size_t i = 0; i < nr; i++)
    {
        status = dml_batch_set_mem_move_by_index(job, static_cast<uint32_t>(i), const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(vecs[i].src)), reinterpret_cast<uint8_t*>(vecs[i].dst), static_cast<uint32_t>(vecs[i].len), 0);
        if (unlikely(status != DML_STATUS_OK))
        {
            shaofs_dsa_batch_req_free(req);
            memcpy_v(vecs, nr);
            return;
        }
    }

    status = dml_submit_job(job);
    if (unlikely(status != DML_STATUS_OK))
    {
        shaofs_dsa_batch_req_free(req);
        memcpy_v(vecs, nr);
        return;
    }

    runtime_async_park(&req->async);
    status = req->status;
    shaofs_dsa_batch_req_free(req);
    if (unlikely(status != DML_STATUS_OK)) memcpy_v(vecs, nr);
}

void dsa_copyv(const Segment* vecs, size_t nr)   // 复制 nr 个 buffer
{
    if (unlikely(!vecs) || nr == 0) return;

    while (nr > 0)
    {
        size_t chunk = MIN(nr, dsa_batch_task_num);
        dsa_copyv_chunk(vecs, chunk);
        vecs += chunk;
        nr   -= chunk;
    }
}

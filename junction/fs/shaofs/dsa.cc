#include "fs.h"
#include "dml/dml.h"
#include "dsa.h"

uint32_t DSA_HWpath_job_size;   // 获取一次即可
static uint32_t req_total_size; // 包含 job 描述符的总请求大小

#define DSA_THRESHOLD 1

void prewarm_dsa_driver() 
{
    dsa_ready = 0;
    DSA_HWpath_job_size = 0;
    req_total_size = 0;

    dml_path_t execution_path = DML_PATH_HW;
    if (dml_get_job_size(execution_path, &DSA_HWpath_job_size) != DML_STATUS_OK) 
    {
        log_info("[Pre-warm] Warning: Could not get DML job size. Hardware might be unavailable.\n");
        return;
    }
    dml_job_t *job = (dml_job_t*)malloc(DSA_HWpath_job_size);
    if (!job)
    {
        log_info("[Pre-warm] Warning: Could not allocate DML job.\n");
        return;
    }

    if (dml_init_job(execution_path, job) != DML_STATUS_OK) 
    {
        log_info("[Pre-warm] Warning: DML init failed.\n");
        free(job);
        return;
    }

    uint8_t src = 0xAA, dst = 0x00;
    job->operation             = DML_OP_MEM_MOVE;
    job->source_first_ptr      = &src;
    job->destination_first_ptr = &dst;
    job->source_length         = 1;
    job->destination_length    = 1;

    dml_status_t status = dml_execute_job(job, DML_WAIT_MODE_BUSY_POLL);

    dml_finalize_job(job);
    free(job);

    if (status != DML_STATUS_OK || dst != 0xAA)
    {
        log_info("[Pre-warm] DSA execution failed (Status: %d). Falling back to CPU memcpy.\n", status);
        return;
    }

    req_total_size = offsetof(struct dsa_req, job) + DSA_HWpath_job_size;
    dsa_ready = 1;
    log_info("[Pre-warm] DSA Hardware driver loaded successfully.\n");
}

void dsa_copy(void *dst, const void *src, size_t len)
{
    if (len < DSA_THRESHOLD || !dsa_ready || req_total_size == 0 || len > DML_MAX_32U)
    {
        memcpy(dst, src, len);
        return;
    }
    
    struct dsa_req* req = (struct dsa_req*)malloc(req_total_size);
    if (!req)
    {
        memcpy(dst, src, len);
        return;
    }

    dml_status_t init_status = dml_init_job(DML_PATH_HW, &req->job);
    if (unlikely(init_status != DML_STATUS_OK))
    {
        free(req);
        memcpy(dst, src, len);
        return;
    }
    req->job.operation             = DML_OP_MEM_MOVE;
    req->job.source_first_ptr      = (uint8_t*)src;
    req->job.destination_first_ptr = (uint8_t*)dst;
    req->job.source_length         = len;
    req->job.destination_length    = len;
    req->job.flags                |= DML_FLAG_BLOCK_ON_FAULT; 

    dml_status_t status = dml_submit_job(&req->job);
    if (unlikely(status != DML_STATUS_OK))   // 提交失败（如硬件队列满），回退到 CPU
    {
        log_info("DSA job submission failed with status %d, falling back to CPU memcpy", status);
        dml_finalize_job(&req->job);
        free(req);
        memcpy(dst, src, len);
        return;
    }

    req->waiting_th = thread_self();  // 记录当前线程，准备挂起
    dsa_req_enqueue_and_park(req);
}
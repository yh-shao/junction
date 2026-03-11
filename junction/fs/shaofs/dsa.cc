#include "fs.h"
#include "dml/dml.h"
#include "dsa.h"

uint32_t DSA_HWpath_job_size;   // 获取一次即可

void prewarm_dsa_driver() 
{
    dml_path_t execution_path = DML_PATH_HW;
    if (dml_get_job_size(execution_path, &DSA_HWpath_job_size) != DML_STATUS_OK) 
    {
        log_info("[Pre-warm] Warning: Could not get DML job size. Hardware might be unavailable.\n");
        return;
    }
    dml_job_t *job = (dml_job_t*)malloc(DSA_HWpath_job_size);

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
    
    if (status == DML_STATUS_OK && dst == 0xAA) log_info("[Pre-warm] DSA Hardware driver loaded successfully.\n"); 
    else log_info("[Pre-warm] DSA execution failed (Status: %d). Fallback might be needed.\n", status);

    dml_finalize_job(job);
    free(job);

    for (int i = 0; i < 100; i++) list_head_init(&pending_dsa_jobs[i]);

    dsa_ready = 1;
}

#define DSA_THRESHOLD 1      // 经验值：小于 2KB 用 CPU 拷贝更快（应该由测试结果决定）
void dsa_copy(void *dst, const void *src, size_t len)    // caller中不应持有 spinlock，因为该函数可能会挂起线程
{
    if (len < DSA_THRESHOLD)   // 小数据或异常情况，直接回退到 CPU memcpy
    {
        memcpy(dst, src, len);
        return;
    }

    struct kthread *k = getk();
    
    uint32_t req_total_size = offsetof(struct dsa_req, job) + DSA_HWpath_job_size;
    struct dsa_req *req = (struct dsa_req *)malloc(req_total_size);

    dml_init_job(DML_PATH_HW, &req->job);
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
        memcpy(dst, src, len);
        return;
    }

    req->waiting_th = thread_self();  // 记录当前线程，准备挂起

    list_add_tail(&pending_dsa_jobs[this_thread_id()], &req->link);
    log_info("ready to yield uthread %p on kthread %u for DSA job", req->waiting_th, this_thread_id());
    thread_park_and_preempt_enable();
}
#pragma once

#include "fs.h"
#include <cstring>
#include <algorithm>
#include <iostream>
#include "dml/dml.h"

#define DSA_THRESHOLD_BYTES 10    // 阈值：低于此大小时，使用 CPU memcpy。 (具体设置多大的阈值，应由测试结果决定)

class DmaWorker {
public:
    static DmaWorker& instance()   // 获取当前线程的单例实例
    {
        static thread_local DmaWorker worker;
        return worker;
    }

    bool mem_move(void* dest, const void* src, size_t size) 
    {
        if (size < DSA_THRESHOLD_BYTES)   // 小数据直接用 CPU，避免 PCIe/Accelerator 开销
        {
            log_info("[DmaWorker] Using memcpy for small size %zu", size);
            memcpy(dest, src, size);
            return true;
        }

        if (!init_success_)   // 尝试初始化 (如果尚未初始化)
        {
            log_info("[DmaWorker] DML initialization failed, falling back to memcpy");  // 如果初始化失败（例如没有硬件），回退到 memcpy
            memcpy(dest, src, size);
            return true; 
        }

        log_info("config DSA job");
        job_->operation             = DML_OP_MEM_MOVE;
        job_->source_first_ptr      = (uint8_t*)src;
        job_->destination_first_ptr = (uint8_t*)dest;
        job_->source_length         = (uint64_t)size;
        job_->destination_length    = (uint64_t)size;

        dml_status_t status = dml_execute_job(job_, DML_WAIT_MODE_BUSY_POLL);

        if (status != DML_STATUS_OK)    // 错误处理：如果硬件出错，回退到软件拷贝
        {
            log_info("[DmaWorker] DML execution failed, falling back to memcpy");
            memcpy(dest, src, size);
            return false;
        }
        log_info("[DmaWorker] DSA execution succeeded for size %zu", size);

        return true;
    }

private:
    dml_job_t* job_ = nullptr;
    bool init_success_ = false;

    DmaWorker() 
    {
        log_info("[DmaWorker] initializing DML job ...");

        dml_path_t execution_path = DML_PATH_HW; // 或者 DML_PATH_AUTO
        uint32_t size = 0;
        if (dml_get_job_size(execution_path, &size) != DML_STATUS_OK)
        {
            log_info("[DmaWorker] DML get_job_size failed");
            return;
        }

        job_ = (dml_job_t*)malloc(size);
        if (!job_) return;

        if (dml_init_job(execution_path, job_) != DML_STATUS_OK) 
        {
            log_info("[DmaWorker] DML job initialization failed");
            free(job_);
            job_ = nullptr;
            return;
        }
        
        init_success_ = true;

        log_info("[DmaWorker] DML job initialized successfully");
    }

    ~DmaWorker() 
    {
        if (job_) 
        {
            dml_finalize_job(job_);
            free(job_);
        }
    }
    
    // 禁止拷贝
    DmaWorker(const DmaWorker&) = delete;
    DmaWorker& operator=(const DmaWorker&) = delete;
};


inline void dsa_memcpy(void* dest, const void* src, size_t size) 
{
    DmaWorker::instance().mem_move(dest, src, size);
}
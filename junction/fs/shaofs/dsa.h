#pragma once
#include <cstddef>

extern uint32_t DSA_HWpath_job_size;
void prewarm_dsa_driver();  // 预热 DSA 驱动，load需要的库
void dsa_copy(void *dst, const void *src, size_t len);
extern struct list_head pending_dsa_jobs[100];
extern int dsa_ready;
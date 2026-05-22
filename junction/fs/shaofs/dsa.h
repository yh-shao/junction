#pragma once
#include <cstddef>
#define dsa_batch_task_num 32    // 一个 dsa batch 中最多包含多少个子任务

struct ShaofsDsaOptions {
    size_t  threshold;
    bool    enable_hw;
    bool    dsa_first;   // 只要拷贝的数据量达到了阈值 (dsa_threshold) 且硬件可用，就把拷贝任务卸载到 DSA 硬件上 （而不考虑使用 DSA 是否有性能收益）
};

enum ShaofsDsaCopyKind {
    SHAOFS_DSA_READ_TO_USER = 0,
    SHAOFS_DSA_WRITE_FROM_USER,
    SHAOFS_DSA_INTERNAL,
    SHAOFS_DSA_KIND_NR,
};

struct Segment {
    void*       dst;
    const void* src;
    size_t      len;
};

int dsa_init(const ShaofsDsaOptions* opts);
void dsa_copy(void* dst, const void* src, size_t len);
void dsa_copy_ex(void* dst, const void* src, size_t len, ShaofsDsaCopyKind kind);
void dsa_copyv(const Segment* vecs, size_t nr);
void dsa_copyv_ex(const Segment* vecs, size_t nr, ShaofsDsaCopyKind kind);
void dsa_dump_stats();

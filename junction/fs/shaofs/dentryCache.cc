#include "dentryCache.h"

static GlobalDentryCache* g_dentry_cache_ptr = nullptr;

GlobalDentryCache& get_dentry_cache() 
{
    if (unlikely(g_dentry_cache_ptr == nullptr)) {
        init_dentry_cache();
    }
    return *g_dentry_cache_ptr;
}

void init_dentry_cache(size_t capacity, size_t shard_num) 
{
    if (g_dentry_cache_ptr != nullptr) return;
    
    // g_dentry_cache_ptr = new GlobalDentryCache(
    //     capacity, shard_num, 
    //     &DentryBackend::getInstance(), 
    //     LRUPolicy<DentryKey>::create_policy_instance, 
    //     nullptr // 在堆上自动分配对象池
    // );

    g_dentry_cache_ptr = new GlobalDentryCache(
        capacity, shard_num, 
        &DentryBackend::getInstance(), 
        []() { return new LRUPolicy<DentryKey, DentryValue>(); }, 
        nullptr 
    );

    get_dentry_cache().put(DentryKey(ROOT_INO, "/"), {ROOT_INO, DIRECTORY});
}
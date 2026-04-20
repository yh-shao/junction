#include "dentryCache.h"

static GlobalDentryCache* g_dentry_cache_ptr = nullptr;

GlobalDentryCache& get_dentry_cache() 
{
    if (unlikely(g_dentry_cache_ptr == nullptr)) 
    {
        log_err("FATAL: get_dentry_cache() called BEFORE init_dentry_cache()!");
        init_dentry_cache();
    }
    return *g_dentry_cache_ptr;
}

void init_dentry_cache(size_t capacity, size_t shard_num) 
{
    if (g_dentry_cache_ptr != nullptr) 
    {
        if (unlikely(g_dentry_cache_ptr == nullptr)) log_err("FATAL: get_dentry_cache() called BEFORE init_dentry_cache()!");
        return;
    }
    g_dentry_cache_ptr = new GlobalDentryCache(capacity, shard_num, &DentryBackend::getInstance(), []() { return new LRUPolicy<DentryKey, DentryValue>(); }, nullptr);

    get_dentry_cache().put(DentryKey(ROOT_INO, "/"), {ROOT_INO, DIRECTORY});
}
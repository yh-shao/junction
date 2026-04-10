#pragma once
#include "fs.h"
#include "generic_cache/objpool.h"
#include "generic_cache/backend.h"
#include "generic_cache/replace_policy.h"
#include "generic_cache/sharded_cache.h"
extern "C" {
#include "runtime/storage.h"
}

class NVMeSSD : public Backend<BlockID, BlockData> {
public:
    bool read(const BlockID& key, BlockData& value)
    {
        // int ret = storage_read(static_cast<void*>(value.data), key, 1);
        // return (ret == 0);
        return DMA_read_block(static_cast<void*>(value.data), key);
    }
    bool write(const BlockID& key, BlockData const& value)
    {
        // int ret = storage_write(static_cast<const void*>(value.data), key, 1);
        // log_info("write to block %lu", key);                                                                    
        // return (ret == 0);
        return DMA_write_block(static_cast<const void*>(value.data), key);
    }

    static NVMeSSD& getInstance()   // singleton，全局只有 1 个 NVMeSSD 实例
    {
        static NVMeSSD instance;
        return instance;
    }
    NVMeSSD(const NVMeSSD&) = delete;
    void operator=(const NVMeSSD&) = delete;
private:
    NVMeSSD() = default;
};

#define DEFAULT_BLOCKCACHE_CAPACITY 65536    // 默认 BlockCache 容量（单位：块数），即 256MB（65536 * 4096 Bytes）。可以通过 init_block_cache() 的参数调整。
#define DEFAULT_SHARD_NUM           32       // 默认分片数量，可以通过 init_block_cache() 的参数调整。每个分片的容量 = capacity / shard_num。

using GlobalBlockCache = ShardedCache<BlockID, BlockData>;
GlobalBlockCache& get_block_cache();
void init_block_cache(size_t capacity = DEFAULT_BLOCKCACHE_CAPACITY, size_t shard_num = DEFAULT_SHARD_NUM);

bool bc_read(BlockID id, void* buffer);         // 从 BlockCache 读取一个 Block 到 user buffer（大小为 BLOCK_SIZE），存在 memcpy
void bc_write(BlockID id, const void* buffer);  // 将 user buffer 中的数据写入 cache 并标记 dirty，存在 memcpy
void bc_flush_all();                            // 将 cache 中所有脏数据刷回后端存储
bool bc_flush_block(BlockID id);                // 将指定 block 刷写到后端（如果在缓存中且为脏）
void bc_prefetch(BlockID id);                   // 拉取块数据到内存

using BlockHandle = GlobalBlockCache::Handle;
BlockHandle bc_get_handle(BlockID id);          // 获取 Block 的 handle，用于直接操作 BlockCache 中的数据，无需 memcpy（注意：通过 handle 修改数据后需要调用 handle.access().mark_dirty() 来标记脏数据）

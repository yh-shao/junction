#pragma once
#include "fs.h"
#include "generic_cache/sharded_cache.h"
#include "generic_cache/replace_policy.h"
#include "generic_cache/backend.h"
#include "dir.h"

#define DEFAULT_DENTRYCACHE_CAPACITY 16384
#define DEFAULT_DENTRY_SHARD_NUM     256

struct DentryKey {     // Dentry 缓存的 Key：由【父目录的 Inode 号】和【当前层级文件名】组合而成
    int  parent_inum;
    char name[NAMESIZ];

    DentryKey() : parent_inum(-1) 
    { 
        name[0] = '\0'; 
    }
    DentryKey(int p_inum, const char* n) : parent_inum(p_inum) 
    {
        strncpy(name, n, NAMESIZ);
        name[NAMESIZ - 1] = '\0';
    }

    bool operator==(const DentryKey& other) const   // 重载 == 运算符，供哈希表处理冲突时精准匹配
    {
        return parent_inum == other.parent_inum && strncmp(name, other.name, NAMESIZ) == 0;
    }
};

namespace std {
    template <>
    struct hash<DentryKey> {
        std::size_t operator()(const DentryKey& k) const 
        {
            std::size_t h1 = std::hash<int>()(k.parent_inum);
            std::size_t h2 = 5381; // djb2 经典哈希魔法值
            for(int i = 0; i < NAMESIZ && k.name[i] != '\0'; ++i) {
                h2 = ((h2 << 5) + h2) + k.name[i]; 
            }
            return h1 ^ (h2 << 1); 
        }
    };
}

struct DentryValue {   // 目录项缓存的具体内容
    int inum;
    file_type_t type;
};

class DentryBackend : public Backend<DentryKey, DentryValue> {
public:
    bool read(const DentryKey& key, DentryValue& value) override
    {
        file_type_t type = UNKNOWN;
        int inum = dir_lookup(key.parent_inum, key.name, &type);
        value.inum = inum;   // 若 inum 为 -1，则表示这个元素“不存在”  （Negative Cache）
        value.type = type;
        return true;
    } 
    bool write(const DentryKey& key, const DentryValue& value) override { return true; }  // Dentry Cache 是纯内存加速层，不负责把目录项"写回"磁盘（由 dir_add_entry 负责），因此 write 直接返回 true 即可，吸收掉 Cache 驱逐(evict)时的写回动作。

    static DentryBackend& getInstance() {
        static DentryBackend instance;
        return instance;
    }
};

using GlobalDentryCache = ShardedCache<DentryKey, DentryValue, std::hash<DentryKey>>;
GlobalDentryCache& get_dentry_cache();
void init_dentry_cache(size_t capacity = DEFAULT_DENTRYCACHE_CAPACITY, size_t shard_num = DEFAULT_DENTRY_SHARD_NUM);
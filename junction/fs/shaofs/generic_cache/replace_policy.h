#pragma once
#include "cache_entry.h"

/**  
 * @brief Interface for Cache Replacement Policies, e.g., LRU, CLOCK, ... 
*/
template <typename Key, typename Value>
class ReplacementPolicy {
public:
    using EntryType = CacheEntry<Key, Value>;

    virtual void  touch(EntryType* entry) = 0;  // 命中或新加入时调用，通知策略更新其内部状态
    virtual void remove(EntryType* entry) = 0;  // 从 Cache 主动移除时调用，通知策略更新其内部状态
    virtual EntryType* getVictim() = 0;         // 获取 victim 候选者

    virtual ~ReplacementPolicy() = default;
};


// 用 policy_meta 表示某个结点是否已经存在于 LRU 链表中
template <typename Key, typename Value>
class LRUPolicy : public ReplacementPolicy<Key, Value> {
    using EntryType = CacheEntry<Key, Value>;

private:
    struct list_head lru_list;  // LRU 链表头 （头部为最近刚访问过，尾部为最久未访问）

public:
    LRUPolicy() 
    {
        list_head_init(&lru_list); // 初始化为空链表
    }

    void touch(EntryType* entry) override 
    {
        if (entry->policy_meta == 1)  // 如果已经在 LRU 链表中
        {
            if (lru_list.n.next == &entry->policy_node) return;  // 已经在头部了，直接 return 即可
            list_del(&entry->policy_node);  // 先从链表中摘除
        }
        list_add(&lru_list, &entry->policy_node);  // 插入到头部（Most Recently Used）
        entry->policy_meta = 1; // 标记为已挂载
    }
    
    void remove(EntryType* entry) override 
    {
        if (entry->policy_meta == 1)   // 只有当节点在链表中时才执行删除
        {
            list_del(&entry->policy_node);
            entry->policy_meta = 0; // 恢复为游离态
        }
    }

    EntryType* getVictim() override 
    {
        if (list_empty(&lru_list)) return nullptr;
        struct list_node* tail_node = lru_list.n.prev;        // LRU 链表的尾部节点即为最久未使用
        return list_entry(tail_node, EntryType, policy_node); // 转换回 CacheEntry 指针
    }
};
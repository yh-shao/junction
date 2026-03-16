#pragma once
#include <list>
#include <unordered_map>
#include "utili.h"

/**  
 * @brief Interface for Cache Replacement Policies, e.g., LRU, CLOCK, ... 
*/
template <typename Key>
class ReplacementPolicy {
public:
    virtual void touch(const Key& key) = 0;   // Notify the policy that a specific Key was accessed
    virtual void remove(const Key& key) = 0;  // Notify the policy that a Key was removed (or tell policy to stop tracking it)
    virtual Key  getVictim() const = 0;       // Return the candidate Key to be evicted.

    virtual ~ReplacementPolicy() = default;
};

template <typename Key>
class LRUPolicy : public ReplacementPolicy<Key> {
private:
    std::list<Key> lruList; 
    std::unordered_map<Key, typename std::list<Key>::iterator> keyMap;
public:
    void touch(const Key& key) override 
    {
        auto it = keyMap.find(key);
        if (it != keyMap.end())
        {
            if (it->second != --lruList.end()) lruList.splice(lruList.end(), lruList, it->second);   // 只有当不在队尾时才 splice （微小的性能提升）
        } 
        else 
        {
            lruList.push_back(key);
            keyMap.emplace(key, --lruList.end());
        }
    }
    void remove(const Key& key) override 
    {
        auto it = keyMap.find(key);
        if (it != keyMap.end())
        {
            lruList.erase(it->second);
            keyMap.erase(it);
        }
    }
    Key getVictim() const override 
    {
        if (lruList.empty())
        {
            log_err("Cannot get victim from an empty LRU policy");
            return Key(); 
        }
        return lruList.front(); 
    }

    size_t size()  const { return lruList.size();  }
    bool   empty() const { return lruList.empty(); }

    static ReplacementPolicy<Key>* create_policy_instance() { return new LRUPolicy<Key>(); }
};
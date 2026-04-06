#pragma once
#include <cstdlib>
#include <stdexcept>
#include <new>

/**
 * ObjectPool - fixed-size object pool
 * Warning:
 * * This pool does NOT contain any internal synchronization. Thread safety must be guaranteed by the caller.
 * * the first sizeof(void*) bytes of each free object are overwritten with the freelist pointer. Consider use _freelist_hook as the first field to absorb this.
 */

template <typename T>
class ObjectPool {    // 如果要存放复杂的非 POD 类型，请谨慎使用
public:
    static_assert(sizeof(T) >= sizeof(void*), "Object size must be >= sizeof(void*) to use embedded pointers");

    ObjectPool(size_t count, void* base_addr = nullptr) : count_(count), free_count_(count), free_head_(nullptr), external_memory_(base_addr != nullptr)  // 初始化对象池：count 个对象（对象的前 8B 会被覆盖，不要在前 8B 存储需要“持久存在”的数据；Caller 拿到对象后，必须手动 reset）
    {
        if (external_memory_) 
        {
            data_ = static_cast<T*>(base_addr);
            for (size_t i = 0; i < count; i++) 
                new (&data_[i]) T();    // 在裸内存上调用构造函数（Placement New）
        }
        else                  
        {
            data_ = new T[count_];   // 在堆上开辟内存，并会自动调用所有对象的构造函数
        }

        for (size_t i = 0; i < count_ - 1; i++)
            *reinterpret_cast<void**>(&data_[i]) = &data_[i + 1];  // 将 &data_[i] 强转为 void**，并写入下一个对象的地址

        if (count_ > 0)   // 最后一个元素指向 nullptr
        {
            *reinterpret_cast<void**>(&data_[count_ - 1]) = nullptr;
            free_head_ = &data_[0]; // 头指针指向第一个元素
        }
        
        // log_info("ObjectPool<%s> initialized: %zu objects", typeid(T).name(), count_);
    }
    ~ObjectPool()  // 每个 obj 的前 8B 不应存储对析构有影响的数据，否则会有问题
    {
        if (external_memory_) 
        {
            for (size_t i = 0; i < count_; i++) 
                data_[i].~T();                   
        }
        else 
        {
            delete[] data_;         // 会自动调用所有对象的析构函数
        }
    }

    T* alloc()  // 从池中获取一个对象指针 （由 caller 负责进行 reset）
    {
        if (free_head_ == nullptr)  // 链表头为空，说明无可用对象
        {
            log_err("ObjectPool<%s> exhausted", typeid(T).name());
            return nullptr; // 池已空
        }

        void* obj = free_head_;   // 取出头结点
        free_head_ = *reinterpret_cast<void**>(obj);  // free_head_ 指向链表中的下一个结点
        free_count_--;
        return static_cast<T*>(obj);
    }

    void free(T* obj)  // 将对象归还给池
    {
        if (!obj) return;
        if (obj < data_ || obj >= data_ + count_)   // 越界检查，防止归还了不属于本池的指针
        {
            log_err("ObjectPool<%s> out of bound", typeid(T).name());
            return; 
        }
        
        // 头插法：将 obj 插入到 free_head_ 之前
        *reinterpret_cast<void**>(obj) = free_head_;
        free_head_ = obj;
        free_count_++;
    }

    size_t capacity()            const { return count_;      }
    size_t available_obj_count() const { return free_count_; }

private:
    size_t              count_, free_count_;
    T*                  data_;            // 对象数组起始指针
    bool                external_memory_; // 是否托管外部内存
    void*               free_head_;       // 空闲链表的头部
};
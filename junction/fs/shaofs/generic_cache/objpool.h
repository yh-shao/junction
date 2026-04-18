#pragma once
#include <cstdlib>
#include <stdexcept>
#include <new>
#include <cstring>
#include <cstddef>
#include <cstdint>

/** 对象池
注意：
  * 本 ObjectPool 不含内置锁，并发安全由上层负责 
  * 支持基于外部传入的内存空间划分出若干个 object，构建 object pool
  * 池中每个对象的首 8 字节会被覆盖用于存储空闲链表中的 next 指针，建议在 Object 类型中预留首部的 8B 用于存储指针，从而避免可能存在的覆盖问题
  * Object 类型如果是复杂的非 POD 类型，请谨慎使用，防止首部 8 字节被覆盖（导致析构时出现问题），最好在 Object 类型中预留首部的 8B （void* reserved_padding）
  * Object 类型中不能包含虚函数，否则虚函数表会被 next 指针覆盖
  * 上层从 Object Pool 中获取一个 Object（的指针） 后应当先进行 reset，避免读取到旧数据
*/

template <typename T>
class ObjectPool {
private:
    size_t              count_, free_count_;
    T*                  data_;            // 对象数组起始指针
    bool                external_memory_; // 是否托管外部内存
    void*               free_head_;       // 空闲链表的头部

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

        for (size_t i = 0; i + 1 < count_; i++)
        {
            void* next_ptr = &data_[i + 1];
            memcpy(&data_[i], &next_ptr, sizeof(void*));    // 写入下一个对象的地址
        }

        if (count_ > 0)   // 最后一个元素指向 nullptr
        {
            void* null_ptr = nullptr;
            memcpy(&data_[count_ - 1], &null_ptr, sizeof(void*));
            free_head_ = &data_[0]; // 头指针指向第一个元素
        }
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
    ObjectPool(const ObjectPool&)            = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    T* alloc()  // 从池中获取一个对象指针 （由 caller 负责进行 reset）
    {
        if (free_head_ == nullptr)  // 链表头为空，说明无可用对象
        {
            // log_err("ObjectPool<%s> exhausted", typeid(T).name());
            return nullptr; // 池已空
        }

        void* obj = free_head_;   // 取出头结点
        
        void* next_ptr = nullptr;
        memcpy(&next_ptr, obj, sizeof(void*));
        free_head_ = next_ptr;    // free_head_ 指向链表中的下一个结点

        free_count_--;
        return static_cast<T*>(obj);
    }

    void free(T* obj)  // 将对象归还给池
    {
        if (!obj) return;
        std::uintptr_t obj_addr = reinterpret_cast<std::uintptr_t>(obj);
        std::uintptr_t base_addr = reinterpret_cast<std::uintptr_t>(data_);
        std::uintptr_t end_addr = base_addr + count_ * sizeof(T);
        if (obj_addr < base_addr || obj_addr >= end_addr) 
        {
            // log_err("ObjectPool out of bound");
            return; 
        }
        if ((obj_addr - base_addr) % sizeof(T) != 0) 
        {
            // log_err("ObjectPool unaligned pointer");
            return;
        }
        
        // 头插法：将 obj 插入到 free_head_ 之前
        memcpy(obj, &free_head_, sizeof(void*));
        free_head_ = obj;
        free_count_++;
    }

    size_t capacity()            const { return count_;      }
    size_t available_obj_count() const { return free_count_; }
};
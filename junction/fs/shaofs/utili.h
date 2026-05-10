#pragma once

extern "C" {
#include "base/types.h"
#include "base/lock.h"
#include "base/log.h"
#include "base/bitmap.h"
#include "base/list.h"
#include "asm/ops.h"
#include "runtime/smalloc.h"
#include "runtime/thread.h"
#include "runtime/storage.h"
#include "runtime/sync.h"
#include "../runtime/defs.h"
}

#ifndef BYTENUM
#define BYTENUM(n)  (((n) + 7) / 8)
#endif

#ifndef CEIL
#define CEIL(x, b)  (((x) + (b) - 1) / (b))
#endif

#ifndef MIN
#define MIN(m,n)    ((m) < (n) ? (m) : (n))
#endif

#ifndef ALIGN
#define ALIGN(x, k) (((x) + (k) - 1) / (k) * (k))
#endif

struct SpinGuard 
{
    spinlock_t* m;
    explicit SpinGuard(spinlock_t* mm) : m(mm) { spin_lock(m); }
    ~SpinGuard() { spin_unlock(m); }
    SpinGuard(const SpinGuard&) = delete;
    SpinGuard& operator=(const SpinGuard&) = delete;
};
struct ReadGuard 
{
    rwmutex_t* m;
    bool locked;

    explicit ReadGuard(rwmutex_t* mm) : m(mm), locked(true) { rwmutex_rdlock(m); }
    ~ReadGuard() { if (locked) rwmutex_unlock(m); }

    // 提供手动控制接口
    void unlock() { if (locked) { rwmutex_unlock(m); locked = false; } }
    void lock()   { if (!locked) { rwmutex_rdlock(m); locked = true; } }

    ReadGuard(const ReadGuard&) = delete;
    ReadGuard& operator=(const ReadGuard&) = delete;
};
struct WriteGuard 
{
    rwmutex_t* m;
    bool locked;

    explicit WriteGuard(rwmutex_t* mm) : m(mm), locked(true) { rwmutex_wrlock(m); }
    ~WriteGuard() { if (locked) rwmutex_unlock(m); }

    // 提供手动控制接口
    void unlock() { if (locked) { rwmutex_unlock(m); locked = false; } }
    void lock()   { if (!locked) { rwmutex_wrlock(m); locked = true; } }

    WriteGuard(const WriteGuard&) = delete;
    WriteGuard& operator=(const WriteGuard&) = delete;
};

class RuntimeFSBaseGuard    // 切换到 runtime 的 fsbase，在析构时恢复之前的 fsbase
{     
 public:
  [[nodiscard]] RuntimeFSBaseGuard() noexcept 
  {
    preempt_disable();
    prev_fs_base_ = _readfsbase_u64();
    thread_t* th = thread_self();
    if (th) th->runtime_fsbase_depth++;
    _writefsbase_u64(perthread_read(runtime_fsbase));
    preempt_enable();
  }

  ~RuntimeFSBaseGuard() 
  {
    preempt_disable();
    _writefsbase_u64(prev_fs_base_);
    thread_t* th = thread_self();
    if (th && th->runtime_fsbase_depth) th->runtime_fsbase_depth--;
    preempt_enable();
  }

 private:
  uint64_t prev_fs_base_;
};

static inline int  atomic_read(volatile int* ptr)         { return __atomic_load_n   (ptr,    __ATOMIC_SEQ_CST); }
static inline void atomic_write(volatile int* ptr, int x) {        __atomic_store_n  (ptr, x, __ATOMIC_SEQ_CST); }
static inline int  atomic_inc(volatile int* ptr)          { return __atomic_add_fetch(ptr, 1, __ATOMIC_SEQ_CST); }
static inline int  atomic_dec(volatile int* ptr)          { return __atomic_sub_fetch(ptr, 1, __ATOMIC_SEQ_CST); }

class kguard {
private:
    struct kthread* k_;

public:
    kguard() { k_ = getk(); }   // 创建时自动调用 getk()
    ~kguard() { putk(); }       // 离开作用域时自动调用 putk()

    // 禁用拷贝构造和拷贝赋值，防止多次调用 putk()
    kguard(const kguard&) = delete;
    kguard& operator=(const kguard&) = delete;

    // 禁用移动构造和移动赋值（对于这种严格的域级作用锁，通常不需要移动）
    kguard(kguard&&) = delete;
    kguard& operator=(kguard&&) = delete;

    // 重载指针操作符，使得可以直接像指针一样使用 kguard 对象
    struct kthread* operator->() const { return k_;  }
    struct kthread& operator*()  const { return *k_; }

    struct kthread* get() const { return k_; }   // 提供一个获取裸指针的普通方法作为备用
};

/* Bitmap 相关操作 */
typedef unsigned long* bitmap_t;

// 从 hint 开始环形寻找一个空闲 bit，并置 1
static inline int alloc_one_bit_from_bitmap_locked(bitmap_t bmap, uint32_t nr_bits, uint32_t* hint_io)  
{
    uint32_t hint = (*hint_io >= nr_bits) ? 0 : *hint_io;
    uint32_t off;

    for (off = hint; off < nr_bits; ++off)         // 从 hint 一路查找到末尾
        if (!bitmap_test(bmap, off)) goto found;
    
    for (off = 0; off < hint; ++off)               // 如果没找到，从头查找到 hint
        if (!bitmap_test(bmap, off)) goto found;

    // 没有空闲 bit
    *hint_io = 0;
    return -1;

found:  // 找到了空闲 bit
    bitmap_set(bmap, off);
    *hint_io = (off + 1 == nr_bits) ? 0 : off + 1;
    return (int)off;
}

// （尽力）批量分配连续空闲 bit
// 从 hint 开始，在 bitmap 中环形搜索第一个空闲的 bit，一旦找到第一个空闲 bit，就以它为起点，向后尽量多地分配连续的空闲 bit。当分配数量达到了 requested、或者遇到了被占用的 bit、或者触碰到了位图的物理边界（nr_bits）时，分配停止。
// 通过 out_start 带回起始位置，并通过返回值告诉调用者实际分配成功了多少个 bit。
static inline int alloc_consecutive_bits_locked(bitmap_t bmap, uint32_t nr_bits, uint32_t* hint_io, int requested, int* out_start)
{
    if (nr_bits == 0 || requested <= 0) return 0;

    uint32_t hint = (*hint_io >= nr_bits) ? 0 : *hint_io;
    uint32_t off;

    // 寻找第一个空闲位
    for (off = hint; off < nr_bits; ++off) 
        if (!bitmap_test(bmap, off)) goto found;
    for (off = 0; off < hint; ++off) 
        if (!bitmap_test(bmap, off)) goto found;
    
    // 没有空闲位了
    *hint_io = 0;
    return 0;

found:   // 从找到的空闲位开始，贪婪地连续分配
    *out_start = (int)off;
    int count = 0;
    while (count < requested && off < nr_bits && !bitmap_test(bmap, off))    // 确保不超额、不越界、且当前位确实空闲
    {
        bitmap_set(bmap, off);
        off++;
        count++;
    }

    *hint_io = (off == nr_bits) ? 0 : off;
    return count;
}
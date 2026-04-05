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

typedef unsigned long* bitmap_t;

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
    prev_fs_base_ = _readfsbase_u64();
    _writefsbase_u64(perthread_read(runtime_fsbase));
  }

  ~RuntimeFSBaseGuard() 
  {
    _writefsbase_u64(prev_fs_base_);
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
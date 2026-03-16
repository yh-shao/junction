#pragma once

extern "C" {
#include "base/types.h"
#include "base/lock.h"
#include "base/log.h"
#include "base/bitmap.h"
#include "asm/ops.h"
#include "runtime/smalloc.h"
#include "runtime/thread.h"
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
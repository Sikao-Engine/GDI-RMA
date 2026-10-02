/*
 * gdi_rma_atomic.h -- the tiny atomic layer the FFI lifecycle needs.
 *
 * Why not C11 <stdatomic.h>: MSVC gates it behind /experimental:c11atomics and
 * otherwise emits
 *     vcruntime_c11_stdatomic.h: fatal error C1189: "C atomic support is not enabled"
 * and this library must build with the plain /std:c11 xmake gives it (and with
 * gcc/clang on Linux).  The two compiler families both expose lock-free
 * intrinsics WITHOUT any platform header, so the helpers below wrap exactly
 * those and nothing else: MSVC's _Interlocked* intrinsics from <intrin.h> and
 * the GCC/Clang __atomic builtins.  No <windows.h>, no <pthread.h>, no MPI
 * dependency -- gdi_rma_api_version() has to stay callable in a process that
 * never touched MPI.
 *
 * Scope: only the init/finalize reference count, the open-database count and
 * the one-shot init lock.  Everything else follows GDI-RMA's own rule that a
 * database object is driven by one thread at a time.
 */
#ifndef GDI_RMA_ATOMIC_H
#define GDI_RMA_ATOMIC_H

#include <stdbool.h>

#if defined(_MSC_VER)
#include <intrin.h>

typedef volatile long gdi_rma_atomic;

static inline long gdi_rma_atomic_load(gdi_rma_atomic* p) {
  /* a full fence on x64/x86: an add of 0 returns the value and orders memory */
  return _InterlockedExchangeAdd(p, 0L);
}
static inline void gdi_rma_atomic_store(gdi_rma_atomic* p, long value) {
  _InterlockedExchange(p, value);
}
static inline long gdi_rma_atomic_add(gdi_rma_atomic* p, long delta) {
  return _InterlockedExchangeAdd(p, delta) + delta;
}
static inline bool gdi_rma_atomic_cas(gdi_rma_atomic* p, long expected, long desired) {
  return _InterlockedCompareExchange(p, desired, expected) == expected;
}

#elif defined(__GNUC__) || defined(__clang__)

typedef volatile long gdi_rma_atomic;

static inline long gdi_rma_atomic_load(gdi_rma_atomic* p) {
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void gdi_rma_atomic_store(gdi_rma_atomic* p, long value) {
  __atomic_store_n(p, value, __ATOMIC_SEQ_CST);
}
static inline long gdi_rma_atomic_add(gdi_rma_atomic* p, long delta) {
  return __atomic_fetch_add(p, delta, __ATOMIC_SEQ_CST);
}
static inline bool gdi_rma_atomic_cas(gdi_rma_atomic* p, long expected, long desired) {
  return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST);
}

#else
#error "gdi_rma_atomic.h: no lock-free atomic implementation for this compiler"
#endif

/* The one-shot mutual exclusion around init/finalize.  A spin lock is the right
 * shape here: it is only ever held across MPI_Init/GDI_Init/GDI_Finalize on the
 * process's first and last reference, and it never nests. */
typedef gdi_rma_atomic gdi_rma_spinlock;

static inline void gdi_rma_spin_lock(gdi_rma_spinlock* lock) {
  while (!gdi_rma_atomic_cas(lock, 0L, 1L)) {
    /* busy wait: init is a once-per-process event */
  }
}

static inline void gdi_rma_spin_unlock(gdi_rma_spinlock* lock) {
  gdi_rma_atomic_store(lock, 0L);
}

#endif /* GDI_RMA_ATOMIC_H */

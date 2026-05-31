/*
 * userspace/libc/thread.h -- minimal threads (real kernel threads).
 * ================================================================
 *
 * A thread SHARES its creating process's address space (same CR3 -> shared
 * globals, heap, code) but has its OWN kernel stack, user stack, registers and
 * FPU state. This is the foundation for shared-memory worker pools (tensor /
 * render workers). It is NOT a full pthreads clone — just create / exit / join.
 *
 * ABI (must match kernel/include/syscall.h):
 *   SYS_THREAD_CREATE(entry, arg, user_stack) -> tid   (79)
 *   SYS_THREAD_EXIT(retval)                    -> noreturn (80)
 *   SYS_THREAD_JOIN(tid, &retval)              -> 0/neg  (81)
 *
 * The caller MUST provide each thread its own stack buffer (16-aligned region;
 * pass the TOP — it grows down). The kernel 16-aligns the pointer it is given.
 * The entry function runs as `entry(arg)` (SysV: arg arrives in RDI). Because
 * there is no per-thread crt0, the entry function MUST terminate the thread
 * itself by calling thread_exit(retval) — if it simply returns it would `ret` to
 * a garbage address.
 *
 * The wrappers themselves are implemented (non-inline) in userspace/libc/syscall.c
 * and declared here + in syscall.h, so this header is just the public surface.
 */

#ifndef LIBC_THREAD_H
#define LIBC_THREAD_H

#ifndef SYS_THREAD_CREATE
#define SYS_THREAD_CREATE 79
#define SYS_THREAD_EXIT   80
#define SYS_THREAD_JOIN   81
#endif

// A thread entry takes a single void* argument and returns void; it MUST end by
// calling thread_exit() (or fall through to it).
typedef void (*thread_fn_t)(void* arg);

// Create a new thread running entry(arg) on the supplied stack TOP. Returns the
// new tid (>0) on success or a negative errno.
int  thread_create(void (*entry)(void*), void* arg, void* stack_top);

// Terminate the calling thread with retval. Does not return.
void thread_exit(int retval) __attribute__((noreturn));

// Block until thread `tid` exits; store its return value via retval_out if non-NULL.
// Returns 0 on success, negative errno on failure.
int  thread_join(int tid, int* retval_out);

#endif /* LIBC_THREAD_H */

#ifndef FUTEX_H
#define FUTEX_H

#include "types.h"

// Futex operations (userspace API)
#define FUTEX_WAIT              0   // Block until woken or value mismatch
#define FUTEX_WAKE              1   // Wake N waiters
#define FUTEX_WAIT_TIMEOUT      2   // Block with timeout

// Initialize futex subsystem
void futex_init(void);

// Futex syscall (SYS_FUTEX implementation)
int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                  uint64_t timeout, uint64_t uaddr2, uint64_t val3);

#endif

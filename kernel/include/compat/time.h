/* Freestanding kernel compatibility shim for <time.h> */
#ifndef _KERNEL_COMPAT_TIME_H
#define _KERNEL_COMPAT_TIME_H

#include "../types.h"

typedef uint64_t time_t;

/* Kernel stub: returns timer ticks or 0 if no timer available */
/* Real implementation should use PIT/HPET/RTC */
static inline time_t time(time_t* t) {
    time_t now = 0; /* TODO: Read from kernel clock */
    if (t) *t = now;
    return now;
}

/* Stub for ctime - returns a static placeholder string */
static inline const char* ctime(const time_t* t) {
    (void)t;
    return "(no clock)\n";
}

#endif

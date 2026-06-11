/* Freestanding kernel compatibility shim for <time.h> */
#ifndef _KERNEL_COMPAT_TIME_H
#define _KERNEL_COMPAT_TIME_H

#include "../types.h"
#include "../rtc.h"

typedef uint64_t time_t;

/* Returns seconds since 1970-01-01 00:00:00 UTC via the CMOS RTC. */
static inline time_t time(time_t* t) {
    time_t now = (time_t)rtc_unix_time();
    if (t) *t = now;
    return now;
}

/* Stub for ctime - returns a static placeholder string */
static inline const char* ctime(const time_t* t) {
    (void)t;
    return "(no clock)\n";
}

#endif

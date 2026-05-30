// userspace/libc/time.h - Time functions

#ifndef TIME_H
#define TIME_H

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef unsigned long size_t;
typedef long time_t;
typedef long clock_t;
typedef long suseconds_t;

// Clock ticks per second
#define CLOCKS_PER_SEC 100

// Time structure (broken-down time)
struct tm {
    int tm_sec;    // Seconds [0,60]
    int tm_min;    // Minutes [0,59]
    int tm_hour;   // Hours [0,23]
    int tm_mday;   // Day of month [1,31]
    int tm_mon;    // Month [0,11]
    int tm_year;   // Years since 1900
    int tm_wday;   // Day of week [0,6] (Sunday = 0)
    int tm_yday;   // Day of year [0,365]
    int tm_isdst;  // Daylight saving flag
};

// Timespec structure (for high-resolution time)
struct timespec {
    time_t tv_sec;   // Seconds
    long tv_nsec;    // Nanoseconds
};

// Timeval structure
struct timeval {
    time_t tv_sec;       // Seconds
    suseconds_t tv_usec; // Microseconds
};

// Clock types
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

// Time functions
time_t time(time_t* tloc);
clock_t clock(void);
double difftime(time_t time1, time_t time0);

// Time conversion
struct tm* gmtime(const time_t* timep);
struct tm* gmtime_r(const time_t* timep, struct tm* result);
struct tm* localtime(const time_t* timep);
struct tm* localtime_r(const time_t* timep, struct tm* result);
time_t mktime(struct tm* tm);

// Time formatting
char* asctime(const struct tm* tm);
char* asctime_r(const struct tm* tm, char* buf);
char* ctime(const time_t* timep);
char* ctime_r(const time_t* timep, char* buf);
size_t strftime(char* s, size_t max, const char* format, const struct tm* tm);

// High-resolution time
int clock_gettime(int clk_id, struct timespec* tp);
int clock_settime(int clk_id, const struct timespec* tp);
int clock_getres(int clk_id, struct timespec* res);

// Nanosleep
int nanosleep(const struct timespec* req, struct timespec* rem);

#endif

// userspace/libc/time.c - Time functions implementation

#include "time.h"
#include "string.h"
#include "syscall.h"

/* SYS_TIME (41) returns seconds since the Unix epoch via the CMOS RTC.
 * SYS_GET_TICKS_MS (40) returns monotonic milliseconds since boot. */
#define SYS_TIME         41
#define SYS_GET_TICKS_MS 40

/* Prototype for the raw 6-arg syscall helper defined in syscall.c.
 * The symbol is not declared in syscall.h but IS exported from syscall.o,
 * so we pull it in with an extern rather than duplicating the asm stub. */
static inline long _time_sc6(long n, long a1, long a2, long a3,
                              long a4, long a5, long a6) {
    long ret;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}

static clock_t start_clock = 0;

// Days in each month (non-leap year)
static const int days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

// Day names and month names for formatting
static const char* day_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char* month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Check if year is leap year
static int is_leap_year(int year) {
    if (year % 400 == 0) return 1;
    if (year % 100 == 0) return 0;
    if (year % 4 == 0) return 1;
    return 0;
}

// Get days in month
static int get_days_in_month(int month, int year) {
    if (month == 1 && is_leap_year(year)) {  // February in leap year
        return 29;
    }
    return days_in_month[month];
}

// Calculate day of week (Zeller's congruence)
static int calculate_day_of_week(int year, int month, int day) {
    if (month < 2) {
        month += 12;
        year--;
    }
    int q = day;
    int m = month;
    int k = year % 100;
    int j = year / 100;
    int h = (q + ((13 * (m + 1)) / 5) + k + (k / 4) + (j / 4) - (2 * j)) % 7;
    return (h + 6) % 7;  // Convert to Sunday = 0
}

// Calculate day of year
static int calculate_day_of_year(int year, int month, int day) {
    int yday = day;
    for (int m = 0; m < month; m++) {
        yday += get_days_in_month(m, year);
    }
    return yday - 1;  // Zero-indexed
}

// ============================================================================
// BASIC TIME FUNCTIONS
// ============================================================================

// Get current wall-clock time (seconds since 1970-01-01 00:00:00 UTC).
// Calls SYS_TIME (41) which reads the CMOS RTC and converts to epoch seconds.
time_t time(time_t* tloc) {
    time_t current_time = (time_t)_time_sc6(SYS_TIME, 0, 0, 0, 0, 0, 0);

    if (tloc) {
        *tloc = current_time;
    }

    return current_time;
}

// Get processor time
clock_t clock(void) {
    // Return approximate CPU time in clock ticks
    // In a real implementation, would query kernel for process CPU time
    // For now, return a simple counter
    static clock_t ticks = 0;
    return ticks++;
}

// Compute difference between two times
double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

// ============================================================================
// TIME CONVERSION FUNCTIONS
// ============================================================================

// Convert time_t to broken-down time (UTC)
struct tm* gmtime_r(const time_t* timep, struct tm* result) {
    if (!timep || !result) {
        return NULL;
    }

    time_t t = *timep;

    // Calculate seconds, minutes, hours
    result->tm_sec = t % 60;
    t /= 60;
    result->tm_min = t % 60;
    t /= 60;
    result->tm_hour = t % 24;
    t /= 24;

    // Calculate year and day of year
    // Start from Unix epoch: 1970-01-01
    int year = 1970;
    int days = (int)t;

    while (1) {
        int days_in_year = is_leap_year(year) ? 366 : 365;
        if (days < days_in_year) {
            break;
        }
        days -= days_in_year;
        year++;
    }

    result->tm_year = year - 1900;
    result->tm_yday = days;

    // Calculate month and day of month
    int month = 0;
    while (month < 12) {
        int days_this_month = get_days_in_month(month, year);
        if (days < days_this_month) {
            break;
        }
        days -= days_this_month;
        month++;
    }

    result->tm_mon = month;
    result->tm_mday = days + 1;

    // Calculate day of week
    result->tm_wday = calculate_day_of_week(year, month, days + 1);

    result->tm_isdst = 0;  // UTC doesn't have DST

    return result;
}

struct tm* gmtime(const time_t* timep) {
    static struct tm result;
    return gmtime_r(timep, &result);
}

// Convert time_t to broken-down time (local time)
// For simplicity, this is the same as gmtime (no timezone support)
struct tm* localtime_r(const time_t* timep, struct tm* result) {
    return gmtime_r(timep, result);
}

struct tm* localtime(const time_t* timep) {
    static struct tm result;
    return localtime_r(timep, &result);
}

// Convert broken-down time to time_t
time_t mktime(struct tm* tm) {
    if (!tm) {
        return (time_t)-1;
    }

    // Normalize the time structure
    int year = tm->tm_year + 1900;
    int month = tm->tm_mon;
    int day = tm->tm_mday;

    // Calculate days since epoch
    int days = 0;

    // Add days for complete years
    for (int y = 1970; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }

    // Add days for complete months
    for (int m = 0; m < month; m++) {
        days += get_days_in_month(m, year);
    }

    // Add remaining days
    days += day - 1;

    // Calculate total seconds
    time_t result = days * 86400;
    result += tm->tm_hour * 3600;
    result += tm->tm_min * 60;
    result += tm->tm_sec;

    // Update tm structure with calculated values
    tm->tm_wday = calculate_day_of_week(year, month, day);
    tm->tm_yday = calculate_day_of_year(year, month, day);
    tm->tm_isdst = 0;

    return result;
}

// ============================================================================
// TIME FORMATTING FUNCTIONS
// ============================================================================

// Convert broken-down time to string: "Sun Sep 16 01:03:52 1973\n"
char* asctime_r(const struct tm* tm, char* buf) {
    if (!tm || !buf) {
        return NULL;
    }

    // Format: "Day Mon DD HH:MM:SS YYYY\n"
    static char temp[26];
    char* str = buf ? buf : temp;

    const char* day = (tm->tm_wday >= 0 && tm->tm_wday < 7) ?
                      day_names[tm->tm_wday] : "???";
    const char* mon = (tm->tm_mon >= 0 && tm->tm_mon < 12) ?
                      month_names[tm->tm_mon] : "???";

    int len = 0;

    // Day
    str[len++] = day[0];
    str[len++] = day[1];
    str[len++] = day[2];
    str[len++] = ' ';

    // Month
    str[len++] = mon[0];
    str[len++] = mon[1];
    str[len++] = mon[2];
    str[len++] = ' ';

    // Day of month
    str[len++] = '0' + (tm->tm_mday / 10);
    str[len++] = '0' + (tm->tm_mday % 10);
    str[len++] = ' ';

    // Hour
    str[len++] = '0' + (tm->tm_hour / 10);
    str[len++] = '0' + (tm->tm_hour % 10);
    str[len++] = ':';

    // Minute
    str[len++] = '0' + (tm->tm_min / 10);
    str[len++] = '0' + (tm->tm_min % 10);
    str[len++] = ':';

    // Second
    str[len++] = '0' + (tm->tm_sec / 10);
    str[len++] = '0' + (tm->tm_sec % 10);
    str[len++] = ' ';

    // Year
    int year = tm->tm_year + 1900;
    str[len++] = '0' + (year / 1000);
    str[len++] = '0' + ((year / 100) % 10);
    str[len++] = '0' + ((year / 10) % 10);
    str[len++] = '0' + (year % 10);
    str[len++] = '\n';
    str[len] = '\0';

    return str;
}

char* asctime(const struct tm* tm) {
    static char buf[26];
    return asctime_r(tm, buf);
}

// Convert time_t to string
char* ctime_r(const time_t* timep, char* buf) {
    if (!timep) {
        return NULL;
    }

    struct tm tm_buf;
    struct tm* tm = localtime_r(timep, &tm_buf);
    if (!tm) {
        return NULL;
    }

    return asctime_r(tm, buf);
}

char* ctime(const time_t* timep) {
    static char buf[26];
    return ctime_r(timep, buf);
}

// Format time according to format string
size_t strftime(char* s, size_t max, const char* format, const struct tm* tm) {
    if (!s || !format || !tm || max == 0) {
        return 0;
    }

    size_t written = 0;

    for (const char* p = format; *p && written < max - 1; p++) {
        if (*p != '%') {
            s[written++] = *p;
            continue;
        }

        p++;  // Skip '%'
        if (!*p) break;

        char buf[32];
        const char* str = NULL;

        switch (*p) {
            case 'a':  // Abbreviated weekday name
                str = (tm->tm_wday >= 0 && tm->tm_wday < 7) ?
                      day_names[tm->tm_wday] : "???";
                break;
            case 'b':  // Abbreviated month name
            case 'h':
                str = (tm->tm_mon >= 0 && tm->tm_mon < 12) ?
                      month_names[tm->tm_mon] : "???";
                break;
            case 'd':  // Day of month [01-31]
                buf[0] = '0' + (tm->tm_mday / 10);
                buf[1] = '0' + (tm->tm_mday % 10);
                buf[2] = '\0';
                str = buf;
                break;
            case 'H':  // Hour [00-23]
                buf[0] = '0' + (tm->tm_hour / 10);
                buf[1] = '0' + (tm->tm_hour % 10);
                buf[2] = '\0';
                str = buf;
                break;
            case 'M':  // Minute [00-59]
                buf[0] = '0' + (tm->tm_min / 10);
                buf[1] = '0' + (tm->tm_min % 10);
                buf[2] = '\0';
                str = buf;
                break;
            case 'S':  // Second [00-60]
                buf[0] = '0' + (tm->tm_sec / 10);
                buf[1] = '0' + (tm->tm_sec % 10);
                buf[2] = '\0';
                str = buf;
                break;
            case 'Y':  // Year with century
                {
                    int year = tm->tm_year + 1900;
                    buf[0] = '0' + (year / 1000);
                    buf[1] = '0' + ((year / 100) % 10);
                    buf[2] = '0' + ((year / 10) % 10);
                    buf[3] = '0' + (year % 10);
                    buf[4] = '\0';
                    str = buf;
                }
                break;
            case 'm':  // Month [01-12]
                buf[0] = '0' + ((tm->tm_mon + 1) / 10);
                buf[1] = '0' + ((tm->tm_mon + 1) % 10);
                buf[2] = '\0';
                str = buf;
                break;
            case 'n':  // Newline
                s[written++] = '\n';
                continue;
            case 't':  // Tab
                s[written++] = '\t';
                continue;
            case '%':  // Literal %
                s[written++] = '%';
                continue;
            default:   // Unknown format
                s[written++] = '%';
                if (written < max - 1) {
                    s[written++] = *p;
                }
                continue;
        }

        // Copy the formatted string
        if (str) {
            while (*str && written < max - 1) {
                s[written++] = *str++;
            }
        }
    }

    s[written] = '\0';
    return written;
}

// ============================================================================
// HIGH-RESOLUTION TIME FUNCTIONS
// ============================================================================

// Get time from specified clock
int clock_gettime(int clk_id, struct timespec* tp) {
    if (!tp) {
        return -1;
    }

    switch (clk_id) {
        case CLOCK_REALTIME:
            tp->tv_sec = time(NULL);   /* RTC wall-clock via SYS_TIME */
            tp->tv_nsec = 0;
            return 0;

        case CLOCK_MONOTONIC: {
            /* SYS_GET_TICKS_MS (40) returns monotonic ms since boot. */
            long ms = _time_sc6(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0);
            tp->tv_sec  = ms / 1000;
            tp->tv_nsec = (ms % 1000) * 1000000L;
            return 0;
        }

        case CLOCK_PROCESS_CPUTIME_ID:
        case CLOCK_THREAD_CPUTIME_ID:
            tp->tv_sec = clock() / CLOCKS_PER_SEC;
            tp->tv_nsec = (clock() % CLOCKS_PER_SEC) * (1000000000L / CLOCKS_PER_SEC);
            return 0;

        default:
            return -1;
    }
}

// Set time for specified clock (stub)
int clock_settime(int clk_id, const struct timespec* tp) {
    (void)clk_id;
    (void)tp;
    return -1;  // Not implemented
}

// Get resolution of specified clock
int clock_getres(int clk_id, struct timespec* res) {
    if (!res) {
        return -1;
    }

    // Return resolution based on CLOCKS_PER_SEC
    res->tv_sec = 0;
    res->tv_nsec = 1000000000L / CLOCKS_PER_SEC;

    (void)clk_id;
    return 0;
}

// ============================================================================
// SLEEP FUNCTIONS
// ============================================================================

// Sleep for specified time
int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!req) {
        return -1;
    }

    // Convert to seconds for sleep syscall
    unsigned int seconds = (unsigned int)req->tv_sec;
    if (req->tv_nsec >= 500000000L) {
        seconds++;  // Round up if >= 0.5 seconds
    }

    if (seconds > 0) {
        sleep(seconds);
    }

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }

    return 0;
}

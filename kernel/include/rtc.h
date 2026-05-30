/*
 * rtc.h -- CMOS Real-Time Clock driver interface.
 * ================================================
 *
 * Provides a consistent view of the hardware RTC via two public functions:
 *
 *   rtc_read()      -- fill an rtc_time_t struct with the current time
 *   rtc_unix_time() -- return seconds since the Unix epoch (1970-01-01 UTC)
 *
 * Both functions handle BCD vs binary mode (Status B bit 2) and 12/24h mode
 * (Status B bit 1) transparently.
 *
 * rtc_init() should be called once during kernel startup (e.g. from main.c)
 * so the driver logs the boot time to the serial console.
 */

#ifndef RTC_H
#define RTC_H

#include "types.h"

/*
 * rtc_time_t -- broken-down calendar time read from the CMOS RTC.
 *
 * All fields are in natural (binary, not BCD) form after rtc_read() returns.
 *   year  -- full four-digit year, e.g. 2026
 *   month -- 1..12
 *   day   -- 1..31
 *   hour  -- 0..23 (24-hour form, AM/PM converted internally)
 *   min   -- 0..59
 *   sec   -- 0..59
 */
typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  min;
    uint8_t  sec;
} rtc_time_t;

/*
 * rtc_init() -- log the current RTC time at boot via kprintf.
 * Call once from kernel main after serial/kprintf is up.
 */
void rtc_init(void);

/*
 * rtc_read() -- fill *out with the current CMOS RTC date/time.
 *
 * Algorithm:
 *  1. Wait for the Update-In-Progress flag (Status A bit 7) to be clear.
 *  2. Read all RTC registers.
 *  3. Wait for UIP to be clear again and re-read.
 *  4. If the two reads disagree, use the second reading.
 *  5. Convert BCD to binary if Status B bit 2 is 0.
 *  6. Convert 12h to 24h if Status B bit 1 is 0.
 *  7. Compute the full 4-digit year using the century register (reg 0x32)
 *     if it reads a plausible value (0x20 or 0x21), otherwise infer from
 *     the 2-digit year (yy < 70 => 2000+yy, else 1900+yy).
 */
void rtc_read(rtc_time_t *out);

/*
 * rtc_unix_time() -- convert the current RTC time to a Unix timestamp.
 *
 * Returns seconds since 1970-01-01 00:00:00 UTC (no leap-second
 * adjustment; the RTC is assumed to be set to UTC, as is standard).
 * Uses a days-from-civil algorithm valid for dates from 1970 onwards.
 */
int64_t rtc_unix_time(void);

/*
 * Proposed kernel syscall additions (for handlers.c / syscall.h wiring):
 *
 *   SYS_TIME    = 41   -- returns rtc_unix_time() as int64_t
 *                         arg1 ignored (mirrors POSIX time(NULL))
 *
 *   SYS_GETTIME = 42   -- fills user-supplied rtc_time_t*
 *                         arg1 = user pointer to rtc_time_t
 *                         returns 0 on success, -EFAULT on bad pointer
 *
 * Handler stubs the integrator must add to handlers.c:
 *
 *   int64_t sys_time(uint64_t arg1, ...) {
 *       (void)arg1; ...
 *       return rtc_unix_time();
 *   }
 *
 *   int64_t sys_gettime(uint64_t uptr, ...) {
 *       rtc_time_t *ut = (rtc_time_t *)(uintptr_t)uptr;
 *       if (!ut) return EFAULT;
 *       rtc_read(ut);
 *       return ESUCCESS;
 *   }
 *
 * These must also be added to syscall.h (#define SYS_TIME 41, etc.),
 * the dispatch table in syscall.c, and declared as extern in syscall.h.
 */

#endif /* RTC_H */

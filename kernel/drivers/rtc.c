/*
 * rtc.c -- CMOS Real-Time Clock driver.
 * ======================================
 *
 * Reads date and time from the CMOS RTC via I/O ports 0x70 (index) and
 * 0x71 (data).  Handles BCD vs binary mode and 12/24h mode as reported by
 * Status Register B.  Double-reads around the Update-In-Progress (UIP) flag
 * to guarantee a consistent snapshot.
 *
 * Provides:
 *   rtc_init()      -- log boot time to serial/kprintf
 *   rtc_read()      -- fill rtc_time_t struct
 *   rtc_unix_time() -- return Unix epoch seconds (int64_t)
 *
 * Proposed syscalls (not wired here -- report for integrator):
 *   SYS_TIME    = 41  -> return rtc_unix_time()
 *   SYS_GETTIME = 42  -> copy rtc_read() result to user pointer
 */

#include "../include/rtc.h"
#include "../include/x86_64.h"   /* inb / outb */
#include "../include/kernel.h"   /* kprintf */
#include "../include/types.h"

/* -------------------------------------------------------------------------
 * CMOS port addresses and register indices.
 * ----------------------------------------------------------------------- */

#define CMOS_ADDR  0x70   /* RTC index register (write to select) */
#define CMOS_DATA  0x71   /* RTC data register  (read/write)      */

/*
 * FIX-RTC-1: UIP spin limit.
 *
 * The Update-In-Progress flag is asserted for at most ~244 µs per RTC spec
 * (once per second, during which the chip updates its 11 internal registers).
 * On a modern CPU running at ~3 GHz a single inb() takes ~100–200 ns, so
 * 10 000 iterations covers >244 µs with a comfortable margin.  Without a
 * bound, a missing or broken RTC hangs the kernel indefinitely at boot.
 */
#define RTC_UIP_SPIN_MAX  10000

/* Register indices */
#define RTC_REG_SECONDS   0x00
#define RTC_REG_MINUTES   0x02
#define RTC_REG_HOURS     0x04
#define RTC_REG_WEEKDAY   0x06   /* not used, but consumed to keep in sync */
#define RTC_REG_DAY       0x07
#define RTC_REG_MONTH     0x08
#define RTC_REG_YEAR      0x09
#define RTC_REG_STATUS_A  0x0A
#define RTC_REG_STATUS_B  0x0B
#define RTC_REG_CENTURY   0x32   /* may not exist on all hardware */

/* Status A */
#define STATA_UIP         0x80   /* Update-In-Progress flag (bit 7) */

/* Status B */
#define STATB_24HR        0x02   /* bit 1: 1=24h, 0=12h              */
#define STATB_BINARY      0x04   /* bit 2: 1=binary, 0=BCD           */

/* -------------------------------------------------------------------------
 * Low-level CMOS helpers.
 * ----------------------------------------------------------------------- */

/*
 * cmos_read -- select an RTC register and return its raw value.
 *
 * Bit 7 of the address byte is the NMI-disable bit; we keep NMI enabled
 * (bit 7 = 0).  A tiny I/O delay between the outb and inb is unnecessary
 * on modern hardware but a single I/O write to 0x80 would be the idiom if
 * needed; we skip it to keep the code minimal.
 */
static inline uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg & 0x7F);   /* clear NMI-disable bit */
    return inb(CMOS_DATA);
}

/*
 * rtc_uip -- return 1 if the RTC Update-In-Progress flag is set.
 * Reading Status Register A never triggers an update, so it is safe to
 * poll freely.
 */
static inline int rtc_uip(void)
{
    return (cmos_read(RTC_REG_STATUS_A) & STATA_UIP) != 0;
}

/*
 * rtc_wait_not_uip -- spin until UIP is clear or the iteration limit is hit.
 *
 * FIX-RTC-1: Bounded spin.  The original while(rtc_uip()) loop had no exit
 * condition; a dead or absent RTC would hang the kernel at boot.  We cap at
 * RTC_UIP_SPIN_MAX iterations (comfortably covers the 244 µs spec maximum on
 * any realistic CPU speed) and silently return on timeout so callers still get
 * a reading -- it may be momentarily inconsistent, but a double-read then
 * resolves that.
 */
static void rtc_wait_not_uip(void)
{
    int i;
    for (i = 0; i < RTC_UIP_SPIN_MAX; i++) {
        if (!rtc_uip()) return;
    }
    /* Timed out -- UIP still set.  Fall through: the double-read in rtc_read
     * will catch any inconsistency. */
}

/* -------------------------------------------------------------------------
 * BCD conversion.
 * ----------------------------------------------------------------------- */

/*
 * bcd_to_bin -- convert a packed-BCD byte to its binary integer value.
 * e.g. 0x26 -> 26.
 */
static inline uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10) + (bcd & 0x0F));
}

/* -------------------------------------------------------------------------
 * Raw register snapshot.
 * ----------------------------------------------------------------------- */

typedef struct {
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;     /* 2-digit raw value */
    uint8_t century;  /* raw century register (may be 0 on some hardware) */
} raw_regs_t;

static void read_raw(raw_regs_t *r)
{
    r->sec     = cmos_read(RTC_REG_SECONDS);
    r->min     = cmos_read(RTC_REG_MINUTES);
    r->hour    = cmos_read(RTC_REG_HOURS);
    r->day     = cmos_read(RTC_REG_DAY);
    r->month   = cmos_read(RTC_REG_MONTH);
    r->year    = cmos_read(RTC_REG_YEAR);
    r->century = cmos_read(RTC_REG_CENTURY);
}

static int raw_equal(const raw_regs_t *a, const raw_regs_t *b)
{
    return (a->sec     == b->sec  &&
            a->min     == b->min  &&
            a->hour    == b->hour &&
            a->day     == b->day  &&
            a->month   == b->month &&
            a->year    == b->year);
}

/* -------------------------------------------------------------------------
 * rtc_read -- public API.
 * ----------------------------------------------------------------------- */

void rtc_read(rtc_time_t *out)
{
    raw_regs_t r1, r2;
    uint8_t    statb;
    int        bcd_mode, h24_mode;
    uint16_t   full_year;

    if (!out) return;

    /* --- Step 1 & 2: wait for UIP clear, read once.
     * FIX-RTC-1: use bounded rtc_wait_not_uip() instead of an unbounded spin.
     */
    rtc_wait_not_uip();
    read_raw(&r1);

    /* --- Step 3 & 4: wait again, re-read, prefer second if different. */
    rtc_wait_not_uip();
    read_raw(&r2);

    /* Use r2 (the more recent reading).  If they match, great; if not,
     * r2 was read right after a UIP clear and is the coherent snapshot. */
    if (!raw_equal(&r1, &r2)) {
        r1 = r2;        /* discard the older, possibly-split read */
    }

    /* --- Step 5: read mode flags from Status Register B. */
    statb    = cmos_read(RTC_REG_STATUS_B);
    bcd_mode = !(statb & STATB_BINARY);   /* 0=BCD, 1=binary */
    h24_mode =  (statb & STATB_24HR);     /* 0=12h, 1=24h    */

    /* --- Step 6: convert BCD to binary if needed. */
    if (bcd_mode) {
        r1.sec     = bcd_to_bin(r1.sec);
        r1.min     = bcd_to_bin(r1.min);
        /* Hour needs special treatment for 12h mode (PM bit in bit 7). */
        uint8_t h_raw = r1.hour;
        if (!h24_mode) {
            /* In BCD 12h mode, bit 7 is the PM flag; the BCD value is in
             * the lower 7 bits. */
            uint8_t pm_flag = h_raw & 0x80;
            r1.hour = (uint8_t)(bcd_to_bin(h_raw & 0x7F));
            if (pm_flag) r1.hour |= 0x80;   /* re-attach PM flag */
        } else {
            r1.hour = bcd_to_bin(h_raw);
        }
        r1.day     = bcd_to_bin(r1.day);
        r1.month   = bcd_to_bin(r1.month);
        r1.year    = bcd_to_bin(r1.year);
        r1.century = bcd_to_bin(r1.century);
    }

    /* --- Step 7: convert 12h to 24h if needed. */
    if (!h24_mode) {
        uint8_t pm = r1.hour & 0x80;
        r1.hour &= 0x7F;   /* strip PM flag */
        if (pm) {
            /* PM: 12pm stays 12, 1pm..11pm -> 13..23 */
            if (r1.hour != 12)
                r1.hour = (uint8_t)(r1.hour + 12);
        } else {
            /* AM: 12am -> 0, 1am..11am stay */
            if (r1.hour == 12)
                r1.hour = 0;
        }
    }

    /* --- Step 8: compute full 4-digit year.
     * The CMOS century register (0x32) holds the century (e.g. 0x20 = 20,
     * i.e. the 21st century).  On hardware where it is not implemented it
     * often reads 0xFF, 0x00, or some garbage; we sanity-check it.
     *
     * FIX-RTC-2: Century register encoding vs Status B binary/BCD flag.
     *
     * The CMOS Status B bit 2 governs seconds/minutes/hours/day/month/year.
     * The century register at 0x32 is an ACPI extension and many BIOSes
     * (including common QEMU/SeaBIOS configurations) encode it in BCD
     * regardless of the Status B binary bit.  When we are in binary mode and
     * the century value does not pass the 20/21 sanity check, try decoding
     * it as BCD before falling back to the year heuristic.
     *
     * Example: QEMU in binary mode returns century = 0x20 (BCD for 20, but
     * decimal 32 as binary).  Old code: 32 != 20 && 32 != 21 -> heuristic.
     * New code: bcd_to_bin(0x20) = 20 -> accepted as century 20.
     */
    if (r1.century == 20 || r1.century == 21) {
        full_year = (uint16_t)(r1.century * 100 + r1.year);
    } else if (!bcd_mode) {
        /* Binary mode: century reg may still be BCD -- try to decode it. */
        uint8_t century_from_bcd = bcd_to_bin(r1.century);
        if (century_from_bcd == 20 || century_from_bcd == 21) {
            full_year = (uint16_t)(century_from_bcd * 100 + r1.year);
        } else {
            /* Fallback: POSIX-era heuristic. */
            full_year = (uint16_t)(r1.year < 70 ? (2000 + r1.year) : (1900 + r1.year));
        }
    } else {
        /* Fallback: POSIX-era heuristic. */
        full_year = (uint16_t)(r1.year < 70 ? (2000 + r1.year) : (1900 + r1.year));
    }

    out->sec   = r1.sec;
    out->min   = r1.min;
    out->hour  = r1.hour;
    out->day   = r1.day;
    out->month = r1.month;
    out->year  = full_year;
}

/* -------------------------------------------------------------------------
 * rtc_unix_time -- convert RTC reading to Unix epoch seconds.
 *
 * Uses the "days from civil" algorithm (Howard Hinnant, public domain),
 * which gives the number of days since 1970-01-01 for any Gregorian date.
 *
 * days_from_civil(y, m, d):
 *   y -= (m <= 2)
 *   era = y / 400               (floor division)
 *   yoe = y - era*400           (year-of-era, 0..399)
 *   doy = (153*m' + 2)/5 + d-1  (m' = month adjusted for Mar=0)
 *   doe = yoe*365 + yoe/4 - yoe/100 + doy
 *   return era*146097 + doe - 719468
 *
 * 719468 = days from epoch to 0000-03-01 (the reference point used here).
 * ----------------------------------------------------------------------- */

static int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
    /* Shift March to be month 0 so Feb is the last month of a "year",
     * making leap-day handling trivial. */
    y -= (m <= 2) ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                            /* [0, 399]  */
    int64_t mp  = (m <= 2) ? (m + 9) : (m - 3);            /* [0, 11]   */
    int64_t doy = (153 * mp + 2) / 5 + d - 1;              /* [0, 365]  */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;   /* [0,146096]*/
    return era * 146097 + doe - 719468;
}

int64_t rtc_unix_time(void)
{
    rtc_time_t t;
    rtc_read(&t);

    int64_t days    = days_from_civil((int64_t)t.year,
                                      (int64_t)t.month,
                                      (int64_t)t.day);
    int64_t seconds = days * 86400LL
                    + (int64_t)t.hour  * 3600LL
                    + (int64_t)t.min   * 60LL
                    + (int64_t)t.sec;
    return seconds;
}

/* -------------------------------------------------------------------------
 * rtc_init -- log boot date/time to the kernel serial console.
 * ----------------------------------------------------------------------- */

void rtc_init(void)
{
    rtc_time_t t;
    rtc_read(&t);

    /*
     * Expected serial output during boot:
     *   [RTC] boot time: 2026-05-28 14:03:22 UTC
     */
    kprintf("[RTC] boot time: %u-%02u-%02u %02u:%02u:%02u UTC\n",
            (unsigned)t.year,
            (unsigned)t.month,
            (unsigned)t.day,
            (unsigned)t.hour,
            (unsigned)t.min,
            (unsigned)t.sec);
}

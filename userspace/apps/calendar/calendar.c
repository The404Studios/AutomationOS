/*
 * calendar.c -- Monthly calendar GUI (freestanding, ring 3).
 * ===========================================================
 *
 * Opens a 420x380 window titled "Calendar". Displays the current month
 * as a grid with weekday headers and numbered day cells. TODAY is
 * highlighted with an accent colour. Left/Right arrow keys or clicks on
 * the "<" / ">" zones in the header navigate to the previous/next month.
 *
 * Time source (priority):
 *   1. SYS_GETTIME (42) -- fills rtc_time_t; today's date is live.
 *   2. Fallback:          fixed 2026-05 so the app always renders.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/calendar/calendar.c -o /tmp/cal.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/cal.o /tmp/wlc.o /tmp/bf.o -o /tmp/cal.elf
 *
 *   # Verify no stack-canary reference:
 *   objdump -d /tmp/cal.elf | grep 'fs:0x28'   # must produce no output
 *
 * Serial output:
 *   [CAL] starting
 *   [CAL] today YYYY-MM-DD
 *   -- OR --
 *   [CAL] no RTC, using fallback
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -----------------------------------------------------------------------
 * Syscall numbers and inline helper (3-arg form, sufficient here).
 * --------------------------------------------------------------------- */
#define SYS_WRITE    3
#define SYS_YIELD    15
#define SYS_GETTIME  42

static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * RTC broken-down time struct (must mirror kernel rtc_time_t exactly).
 * --------------------------------------------------------------------- */
typedef struct {
    unsigned short year;
    unsigned char  month;
    unsigned char  day;
    unsigned char  hour;
    unsigned char  min;
    unsigned char  sec;
} rtc_time_t;

/* -----------------------------------------------------------------------
 * Minimal freestanding helpers.
 * --------------------------------------------------------------------- */
typedef unsigned int  u32;
typedef int           i32;

static unsigned long cal_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void serial(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)cal_strlen(m));
}

/* Write an unsigned integer into buf, returns pointer past the last digit. */
static char *fmt_uint(char *buf, unsigned long v, int min_digits)
{
    char tmp[20];
    int  n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; } }
    /* zero-pad */
    while (n < min_digits) tmp[n++] = '0';
    /* reverse into buf */
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return buf + n;
}

/* Append a NUL-terminated string to dst; returns pointer past new NUL. */
static char *str_app(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = '\0';
    return dst;
}

/* -----------------------------------------------------------------------
 * Calendar arithmetic (no libc).
 * --------------------------------------------------------------------- */

/* Leap year test. */
static int is_leap(int y)
{
    return (y % 4 == 0) && ((y % 100 != 0) || (y % 400 == 0));
}

/* Days in a given month (1-based month). */
static int days_in_month(int y, int m)
{
    static const int tab[13] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && is_leap(y)) return 29;
    return tab[m];
}

/*
 * Day-of-week for the 1st of (year, month) -- 0=Sunday … 6=Saturday.
 *
 * Uses Tomohiko Sakamoto's algorithm (public domain):
 *   Given (y, m, d), computes (y += m < 3; then
 *   return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7)
 * where t[] = {0,3,2,5,0,3,5,1,4,6,2,4}.
 * Result: 0=Sunday, 1=Monday, …, 6=Saturday.
 */
static int weekday_of(int year, int month, int day)
{
    static const int t[12] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (month < 3) year--;
    int y = year;
    return (y + y/4 - y/100 + y/400 + t[month - 1] + day) % 7;
}

/* -----------------------------------------------------------------------
 * Layout / colour constants.
 * --------------------------------------------------------------------- */
#define WIN_W        420
#define WIN_H        380

/* Header band: contains month/year title + nav arrows. */
#define HDR_H        44
#define HDR_Y        0

/* Weekday-name bar sits directly below the header. */
#define WDH_H        22
#define WDH_Y        HDR_H

/* Grid starts below weekday bar. */
#define GRID_Y       (HDR_H + WDH_H)

/* 7 columns, 6 rows max. Cell dimensions. */
#define CELL_W       56
#define CELL_H       50
#define GRID_LEFT    14    /* left margin so 7*56+14*2 = 420 */

/* Arrow-click zones at the top corners (each 40px wide). */
#define ARROW_W      40
#define ARROW_H      HDR_H

/* Colours (ARGB32). */
#define COL_BG       0xFF0D1B2Au   /* dark navy background            */
#define COL_HDR_BG   0xFF112233u   /* slightly lighter header band    */
#define COL_WDH_BG   0xFF0A1520u   /* weekday header bar              */
#define COL_TEXT     0xFFD0DCEAu   /* normal light text               */
#define COL_DIM      0xFF4A6080u   /* dim text for empty cells        */
#define COL_SEP      0xFF1E3050u   /* grid line colour                */
#define COL_TODAY    0xFF4C9AFFu   /* today accent fill               */
#define COL_TODAY_T  0xFF001020u   /* text on today (dark on accent)  */
#define COL_ARROW    0xFF8ABAEAU   /* arrow symbol colour             */
#define COL_WDH_T    0xFF6A9FD8u   /* weekday header text colour      */
#define COL_CELL_HOV 0xFF1A2E44u   /* subtle hover highlight (unused) */

/* -----------------------------------------------------------------------
 * Drawing primitives.
 * --------------------------------------------------------------------- */

static void fill_rect(u32 *buf, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 col)
{
    i32 x0 = x < 0 ? 0 : x;
    i32 y0 = y < 0 ? 0 : y;
    i32 x1 = x + w; if (x1 > WIN_W) x1 = WIN_W;
    i32 y1 = y + h; if (y1 > WIN_H) y1 = WIN_H;
    if (x0 >= x1 || y0 >= y1) return;
    for (i32 yy = y0; yy < y1; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x0; xx < x1; xx++)
            row[xx] = col;
    }
}

static void draw_string_centered(u32 *buf, u32 stride_px,
                                 i32 cx, i32 y, const char *s, u32 col)
{
    int tw = font_text_width(s);
    font_draw_string(buf, (int)stride_px, WIN_W, WIN_H,
                     cx - tw / 2, y, s, col);
}

/* Draw a left-pointing triangle at (cx,cy) with half-size sz. */
static void draw_left_arrow(u32 *buf, u32 stride_px, i32 cx, i32 cy,
                            i32 sz, u32 col)
{
    for (i32 dy = -sz; dy <= sz; dy++) {
        i32 half = sz - (dy < 0 ? -dy : dy);
        for (i32 dx = -half; dx <= 0; dx++) {
            i32 px = cx + dx, py = cy + dy;
            if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                buf[(u32)py * stride_px + (u32)px] = col;
        }
    }
}

/* Draw a right-pointing triangle at (cx,cy) with half-size sz. */
static void draw_right_arrow(u32 *buf, u32 stride_px, i32 cx, i32 cy,
                             i32 sz, u32 col)
{
    for (i32 dy = -sz; dy <= sz; dy++) {
        i32 half = sz - (dy < 0 ? -dy : dy);
        for (i32 dx = 0; dx <= half; dx++) {
            i32 px = cx + dx, py = cy + dy;
            if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                buf[(u32)py * stride_px + (u32)px] = col;
        }
    }
}

/* -----------------------------------------------------------------------
 * Render the full calendar frame.
 *
 * Parameters:
 *   buf/stride_px  -- pixel buffer
 *   view_year      -- year being displayed
 *   view_month     -- month being displayed (1-12)
 *   today_year/month/day -- actual today (0 if unknown)
 * --------------------------------------------------------------------- */
static void render(u32 *buf, u32 stride_px,
                   int view_year, int view_month,
                   int today_year, int today_month, int today_day)
{
    static const char * const month_names[12] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    static const char * const wday_names[7] = {
        "Su","Mo","Tu","We","Th","Fr","Sa"
    };

    /* ---- Background ---- */
    fill_rect(buf, stride_px, 0, 0, WIN_W, WIN_H, COL_BG);

    /* ---- Header band ---- */
    fill_rect(buf, stride_px, 0, HDR_Y, WIN_W, HDR_H, COL_HDR_BG);

    /* Month name + year string, centered */
    char title[32];
    {
        char *p = title;
        p = str_app(p, month_names[view_month - 1]);
        *p++ = ' ';
        char ybuf[8];
        fmt_uint(ybuf, (unsigned long)view_year, 4);
        p = str_app(p, ybuf);
    }
    draw_string_centered(buf, stride_px, WIN_W / 2,
                         HDR_Y + (HDR_H - FONT_H) / 2, title, COL_TEXT);

    /* Left arrow "<" */
    draw_left_arrow(buf, stride_px,
                    ARROW_W / 2, HDR_Y + HDR_H / 2, 7, COL_ARROW);

    /* Right arrow ">" */
    draw_right_arrow(buf, stride_px,
                     WIN_W - ARROW_W / 2, HDR_Y + HDR_H / 2, 7, COL_ARROW);

    /* ---- Weekday header bar ---- */
    fill_rect(buf, stride_px, 0, WDH_Y, WIN_W, WDH_H, COL_WDH_BG);
    for (int col = 0; col < 7; col++) {
        i32 cx = GRID_LEFT + col * CELL_W + CELL_W / 2;
        i32 ty = WDH_Y + (WDH_H - FONT_H) / 2;
        draw_string_centered(buf, stride_px, cx, ty, wday_names[col], COL_WDH_T);
    }

    /* Separator under weekday bar */
    fill_rect(buf, stride_px, 0, WDH_Y + WDH_H - 1, WIN_W, 1, COL_SEP);

    /* ---- Day grid ---- */
    int first_wday = weekday_of(view_year, view_month, 1); /* 0=Sun */
    int num_days   = days_in_month(view_year, view_month);

    /* Draw subtle grid lines */
    for (int row = 0; row <= 6; row++)
        fill_rect(buf, stride_px,
                  0, GRID_Y + row * CELL_H,
                  WIN_W, 1, COL_SEP);
    for (int col = 0; col <= 7; col++)
        fill_rect(buf, stride_px,
                  GRID_LEFT + col * CELL_W, GRID_Y,
                  1, 6 * CELL_H, COL_SEP);

    /* Place day numbers */
    for (int day = 1; day <= num_days; day++) {
        int slot = first_wday + day - 1;   /* slot in a 7-column grid */
        int grid_row = slot / 7;
        int grid_col = slot % 7;

        i32 cx = GRID_LEFT + grid_col * CELL_W;
        i32 cy = GRID_Y    + grid_row * CELL_H;

        /* Is this today? */
        int is_today = (today_day  == day &&
                        today_month == view_month &&
                        today_year  == view_year);

        /* Accent fill for today */
        if (is_today) {
            fill_rect(buf, stride_px, cx + 1, cy + 1,
                      CELL_W - 2, CELL_H - 2, COL_TODAY);
        }

        /* Day number -- centred in cell */
        char nbuf[4];
        fmt_uint(nbuf, (unsigned long)day, 1);
        int tw = font_text_width(nbuf);
        i32 tx = cx + (CELL_W - tw)  / 2;
        i32 ty = cy + (CELL_H - FONT_H) / 2;

        u32 text_col = is_today ? COL_TODAY_T : COL_TEXT;
        font_draw_string(buf, (int)stride_px, WIN_W, WIN_H,
                         tx, ty, nbuf, text_col);
    }
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    serial("[CAL] starting\n");

    /* ---- Read today's date via SYS_GETTIME ---- */
    rtc_time_t rtc;
    int today_year = 0, today_month = 0, today_day = 0;
    long ret = sc(SYS_GETTIME, (long)&rtc, 0, 0);
    if (ret == 0) {
        today_year  = rtc.year;
        today_month = rtc.month;
        today_day   = rtc.day;

        /* Serial: "[CAL] today YYYY-MM-DD" */
        char msg[40];
        char *p = msg;
        p = str_app(p, "[CAL] today ");
        char tmp[8];
        fmt_uint(tmp, (unsigned long)today_year, 4);
        p = str_app(p, tmp);
        *p++ = '-';
        fmt_uint(tmp, (unsigned long)today_month, 2);
        p = str_app(p, tmp);
        *p++ = '-';
        fmt_uint(tmp, (unsigned long)today_day, 2);
        p = str_app(p, tmp);
        *p++ = '\n';
        *p   = '\0';
        serial(msg);
    } else {
        serial("[CAL] no RTC, using fallback\n");
        today_year  = 2026;
        today_month = 5;
        today_day   = 28;   /* a known date so today highlight is visible */
    }

    /* ---- Connect to compositor ---- */
    if (wl_connect() != 0) {
        serial("[CAL] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Calendar");
    if (!win) {
        serial("[CAL] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    u32 stride_px = win->stride / 4u;

    /* ---- Displayed month (start at today) ---- */
    int view_year  = today_year;
    int view_month = today_month;

    /* If fallback was used the year/month above are the fallback values. */

    int dirty = 1;   /* force initial draw */

    /* ---- Event loop ---- */
    for (;;) {
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_KEY) {
                /* ea=keycode, eb=pressed */
                if (eb == 1) {   /* key-down only */
                    if (ea == 105) {
                        /* Left arrow: previous month */
                        view_month--;
                        if (view_month < 1) { view_month = 12; view_year--; }
                        dirty = 1;
                    } else if (ea == 106) {
                        /* Right arrow: next month */
                        view_month++;
                        if (view_month > 12) { view_month = 1; view_year++; }
                        dirty = 1;
                    }
                }
            } else if (kind == WL_EVENT_POINTER) {
                /* ea=x, eb=y, ec=buttons; act on button-press (bit 0 rising). */
                if (ec & 1) {
                    i32 mx = (i32)ea;
                    i32 my = (i32)eb;
                    /* Left-arrow zone: top-left ARROW_W x ARROW_H */
                    if (mx < ARROW_W && my < ARROW_H) {
                        view_month--;
                        if (view_month < 1) { view_month = 12; view_year--; }
                        dirty = 1;
                    }
                    /* Right-arrow zone: top-right ARROW_W x ARROW_H */
                    else if (mx >= WIN_W - ARROW_W && my < ARROW_H) {
                        view_month++;
                        if (view_month > 12) { view_month = 1; view_year++; }
                        dirty = 1;
                    }
                }
            }
        }

        if (dirty) {
            render(win->pixels, stride_px,
                   view_year, view_month,
                   today_year, today_month, today_day);
            wl_commit(win);
            dirty = 0;
        }

        sc(SYS_YIELD, 0, 0, 0);
    }
}

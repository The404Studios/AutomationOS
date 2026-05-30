/*
 * userspace/apps/procmon/procmon.c -- AI Process Monitor GUI.
 * ============================================================
 *
 * A live, AI-drivable process monitor window (620 x 480, "Process Monitor")
 * that renders via the M3 WL compositor + 8x16 bitmap font.
 *
 * Layout (dark theme):
 *   [  0.. 47] Header bar  : "Process Monitor" + uptime + free/total mem
 *   [ 48.. 99] Stats strip : process count | uptime HH:MM:SS | mem KB
 *   [100..409] Process table: PID / NAME / STATE / CPU / MEM columns
 *                             row selection via click
 *   [410..447] Action buttons: Suspend | Resume | Kill | Nice+ | Nice-
 *   [448..479] Status bar   : last action feedback
 *
 * Data sources:
 *   aictl_list()    <- SYS_PROCLIST  (44) -- always attempted
 *   aictl_query()   <- SYS_PROC_QUERY(60) -- for CPU/MEM columns; degrades to "n/a"
 *   aictl_sysinfo() <- SYS_SYSINFO   (62) -- header strip; degrades to "--"
 *
 * Safety guards:
 *   Never sends a control verb to pid 0, pid 1, or own PID.
 *
 * Serial output:
 *   "[PROCMON] starting\n"
 *   "[AICTL] ctl pid=P verb=V rc=R\n"
 *
 * Build (ALL flags DIRECTLY on cmdline -- no shell variable):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace \
 *       -c userspace/apps/procmon/procmon.c -o /tmp/pm.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 -I userspace \
 *       -c userspace/lib/aictl/aictl.c -o /tmp/aictl.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 -I userspace \
 *       -c userspace/lib/ui/ui.c -o /tmp/ui.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 -I userspace \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 -I userspace \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/pm.o /tmp/aictl.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/pm.elf
 *
 *   # Verify no stack-canary reference (must produce no output):
 *   objdump -d /tmp/pm.elf | grep fs:0x28
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/aictl/aictl.h"

/* -------------------------------------------------------------------------
 * Extra syscalls used directly in this TU.
 * ---------------------------------------------------------------------- */
#define SYS_WRITE    3
#define SYS_GETPID   8
#define SYS_YIELD    15
#define SYS_GET_TICKS_MS 40

static inline long _sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* -------------------------------------------------------------------------
 * Window geometry.
 * ---------------------------------------------------------------------- */
#define WIN_W   620
#define WIN_H   480

/* Zone boundaries (top pixel of each zone). */
#define HDR_Y       0
#define HDR_H      48
#define STATS_Y    48
#define STATS_H    52
#define TABLE_Y   100
#define TABLE_H   310
#define BTN_Y     410
#define BTN_H      38
#define STATUS_Y  448
#define STATUS_H   32

/* Table column X positions (relative to window). */
#define COL_PID_X     8
#define COL_NAME_X   56
#define COL_STATE_X  248
#define COL_CPU_X    320
#define COL_MEM_X    430
#define COL_HDR_Y   (TABLE_Y + 6)
#define COL_DATA_Y0 (TABLE_Y + 26)
#define ROW_H        18

/* Maximum visible rows. */
#define MAX_ROWS    16
/* Full process buffer size. */
#define MAX_PROCS   64

/* Refresh every N frames. */
#define REFRESH_PERIOD  30

/* -------------------------------------------------------------------------
 * Color palette (dark "Aether" theme).
 * ---------------------------------------------------------------------- */
#define C_BG          0xFF0D1117u
#define C_HDR_TOP     0xFF1A1F2Eu
#define C_HDR_BOT     0xFF10141Cu
#define C_STATS_BG    0xFF131822u
#define C_PANEL       0xFF161B26u
#define C_PANEL2      0xFF1E2435u
#define C_BORDER      0xFF2A3248u
#define C_ACCENT      0xFF4E9EFFu
#define C_ACCENT2     0xFF7B5EFFu
#define C_ACCENT3     0xFF00E5B4u
#define C_TEXT        0xFFEAEDF5u
#define C_TEXT2       0xFF9BA8C8u
#define C_TEXT3       0xFF5A6585u
#define C_ROW_EVEN    0xFF161B26u
#define C_ROW_ODD     0xFF1A2030u
#define C_ROW_SEL     0xFF1E3A5Fu
#define C_ROW_SEL_BRD 0xFF4E9EFFu
#define C_SEP         0xFF252D42u
#define C_BTN_BG      0xFF1E2435u
#define C_BTN_HOV     0xFF2A3650u
#define C_BTN_SUSP    0xFF1F3528u   /* green-tinted  */
#define C_BTN_RESU    0xFF1A3540u   /* blue-tinted   */
#define C_BTN_KILL    0xFF4A1818u   /* red           */
#define C_BTN_NICE    0xFF2A2A40u   /* purple-tinted */
#define C_BTN_LBL     0xFFEEEEEEu
#define C_STATUS_BG   0xFF0A0E14u
#define C_STATUS_OK   0xFF4CFF82u
#define C_STATUS_ERR  0xFFFF5555u
#define C_STATUS_INFO 0xFF9BA8C8u

/* State display colors. */
static u32 state_color(u32 s)
{
    switch (s) {
        case 0:  return 0xFF9BA8C8u;   /* created  -- grey  */
        case 1:  return 0xFF4E9EFFu;   /* ready    -- blue  */
        case 2:  return 0xFF4CFF82u;   /* running  -- green */
        case 3:  return 0xFFFFBF42u;   /* blocked  -- amber */
        case 4:  return 0xFFFF5555u;   /* dead     -- red   */
        default: return C_TEXT3;
    }
}

static const char *state_str(u32 s)
{
    switch (s) {
        case 0:  return "new  ";
        case 1:  return "ready";
        case 2:  return "RUN  ";
        case 3:  return "block";
        case 4:  return "dead ";
        default: return "?    ";
    }
}

/* -------------------------------------------------------------------------
 * Freestanding string/number helpers (no libc).
 * ---------------------------------------------------------------------- */

static unsigned long _strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void _write(const char *s)
{
    _sc(SYS_WRITE, 1, (long)s, (long)_strlen(s));
}

/* Copy up to n-1 chars, always NUL-terminates. */
static void _strncpy(char *dst, const char *src, int n)
{
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Write decimal of v into buf (at least 22 bytes).  Returns buf. */
static char *_utoa(u64 v, char *buf)
{
    char tmp[22];
    int i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { u64 x = v; while (x) { tmp[i++] = (char)('0' + x % 10); x /= 10; } }
    int j;
    for (j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[j] = '\0';
    return buf;
}

/* Append src to *p, advance *p to new NUL.  Returns new *p. */
static char *_cat(char *p, const char *src)
{
    while (*src) *p++ = *src++;
    *p = '\0';
    return p;
}

/* Append decimal of v (min `w` wide, padded left with `pad`) to *p. */
static char *_cat_uint(char *p, u64 v, int w, char pad)
{
    char nb[22];
    _utoa(v, nb);
    int len = (int)_strlen(nb);
    while (len < w) { *p++ = pad; len++; }
    return _cat(p, nb);
}

/* Format ms as "HH:MM:SS" into buf (>= 9 bytes). */
static void _fmt_hms(char *buf, u64 ms)
{
    u64 s  = ms / 1000ULL;
    u64 hh = s / 3600ULL;
    u64 mm = (s % 3600ULL) / 60ULL;
    u64 ss = s % 60ULL;
    char nb[8];
    char *p = buf;
    p = _cat_uint(p, hh, 2, '0'); *p++ = ':';
    p = _cat_uint(p, mm, 2, '0'); *p++ = ':';
    _cat_uint(p, ss, 2, '0');
}

/* Format signed int to *p. */
static char *_cat_int(char *p, long v)
{
    if (v < 0) { *p++ = '-'; *p = '\0'; v = -v; }
    return _cat_uint(p, (u64)v, 0, ' ');
}

/* -------------------------------------------------------------------------
 * Drawing primitives.
 * ---------------------------------------------------------------------- */

static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 spx,
                      i32 x, i32 y, i32 w, i32 h, u32 col)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * spx;
        for (i32 xx = x1; xx < x2; xx++)
            row[xx] = col;
    }
}

static void fill_grad_v(u32 *buf, u32 bw, u32 bh, u32 spx,
                        i32 x, i32 y, i32 w, i32 h,
                        u32 ctop, u32 cbot)
{
    for (i32 dy = 0; dy < h; dy++) {
        u32 t = (u32)dy * 255u / (u32)(h > 1 ? h - 1 : 1);
        u32 r = ((ctop >> 16 & 0xFF) * (255 - t) + (cbot >> 16 & 0xFF) * t) / 255;
        u32 g = ((ctop >>  8 & 0xFF) * (255 - t) + (cbot >>  8 & 0xFF) * t) / 255;
        u32 b = ((ctop       & 0xFF) * (255 - t) + (cbot       & 0xFF) * t) / 255;
        fill_rect(buf, bw, bh, spx, x, y + dy, w, 1, 0xFF000000u | (r<<16)|(g<<8)|b);
    }
}

/* Draw a clickable-style button rectangle with label text centred. */
static void draw_btn(u32 *buf, u32 bw, u32 bh, u32 spx,
                     i32 x, i32 y, i32 w, i32 h,
                     const char *label, u32 bg, u32 fg)
{
    fill_rect(buf, bw, bh, spx, x, y, w, h, bg);
    /* 1-px border */
    fill_rect(buf, bw, bh, spx, x,     y,     w, 1, C_BORDER);
    fill_rect(buf, bw, bh, spx, x,     y+h-1, w, 1, C_BORDER);
    fill_rect(buf, bw, bh, spx, x,     y,     1, h, C_BORDER);
    fill_rect(buf, bw, bh, spx, x+w-1, y,     1, h, C_BORDER);
    /* Centred label. */
    int tw = font_text_width(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - FONT_H) / 2;
    font_draw_string(buf, (int)spx, (int)bw, (int)bh, tx, ty, label, fg);
}

/* -------------------------------------------------------------------------
 * Hit-test helpers for mouse input.
 * ---------------------------------------------------------------------- */
typedef struct { i32 x, y, w, h; } rect_t;

static int rect_hit(const rect_t *r, int mx, int my)
{
    return mx >= r->x && mx < r->x + r->w &&
           my >= r->y && my < r->y + r->h;
}

/* -------------------------------------------------------------------------
 * Application state.
 * ---------------------------------------------------------------------- */
#define N_BTNS  5

typedef struct {
    /* Process data. */
    procinfo_t    procs[MAX_PROCS];
    proc_detail_t details[MAX_ROWS]; /* details for visible rows only */
    int           proc_count;
    int           detail_ok;         /* 1 if SYS_PROC_QUERY is wired */

    /* System-wide stats. */
    sysinfo_t     sysinfo;
    int           sysinfo_ok;        /* 1 if SYS_SYSINFO is wired */

    /* UI selection. */
    int           selected;          /* -1 = none; else index into procs[] */

    /* Own PID (never a valid control target). */
    u32           own_pid;

    /* Refresh throttle. */
    int           frame;
    int           refresh_ctr;

    /* Status message. */
    char          status[128];
    u32           status_color;

    /* Button hit-test rects (filled in _start). */
    rect_t        btn_rect[N_BTNS];  /* [0]=Suspend [1]=Resume [2]=Kill [3]=Nice+ [4]=Nice- */

    /* Row hit-test rects (filled per-frame). */
    rect_t        row_rect[MAX_ROWS];
} procmon_t;

static procmon_t g_pm;

/* -------------------------------------------------------------------------
 * Serial helpers.
 * ---------------------------------------------------------------------- */
static void serial_ctl(u32 pid, int verb, long rc)
{
    char buf[80];
    char *p = buf;
    p = _cat(p, "[AICTL] ctl pid=");
    p = _cat_uint(p, (u64)pid, 0, ' ');
    p = _cat(p, " verb=");
    p = _cat_uint(p, (u64)(unsigned)verb, 0, ' ');
    p = _cat(p, " rc=");
    p = _cat_int(p, rc);
    p = _cat(p, "\n");
    _write(buf);
}

/* -------------------------------------------------------------------------
 * Guard: returns 1 if pid is safe to control.
 * ---------------------------------------------------------------------- */
static int pid_ok(u32 pid)
{
    return pid != 0 && pid != 1 && pid != g_pm.own_pid;
}

/* -------------------------------------------------------------------------
 * Perform a control action and update status.
 * ---------------------------------------------------------------------- */
static void do_action(int btn_idx)
{
    procmon_t *st = &g_pm;

    if (st->selected < 0 || st->selected >= st->proc_count) {
        char *p = st->status;
        _strncpy(p, "No process selected.", 128);
        st->status_color = C_STATUS_INFO;
        return;
    }

    u32 pid = st->procs[st->selected].pid;

    if (!pid_ok(pid)) {
        char *p = st->status;
        _strncpy(p, "Cannot control system/self process.", 128);
        st->status_color = C_STATUS_ERR;
        return;
    }

    long rc;
    int  verb;
    const char *verb_name;

    switch (btn_idx) {
        case 0:  /* Suspend */
            verb = AICTL_VERB_SUSPEND;
            verb_name = "suspend";
            rc = aictl_suspend(pid);
            break;
        case 1:  /* Resume */
            verb = AICTL_VERB_RESUME;
            verb_name = "resume";
            rc = aictl_resume(pid);
            break;
        case 2:  /* Kill */
            verb = AICTL_VERB_KILL;
            verb_name = "kill";
            rc = aictl_kill(pid);
            break;
        case 3:  /* Nice+ (lower number = higher priority; decrement) */
            verb = AICTL_VERB_SETPRIO;
            verb_name = "nice+";
            {
                /* Current prio from detail if available, else assume 10. */
                int cur_prio = (st->detail_ok && st->selected < MAX_ROWS)
                               ? (int)st->details[st->selected].prio : 10;
                int new_prio = cur_prio > 0 ? cur_prio - 1 : 0;
                rc = aictl_setprio(pid, new_prio);
            }
            break;
        case 4:  /* Nice- (increment priority number = lower priority) */
            verb = AICTL_VERB_SETPRIO;
            verb_name = "nice-";
            {
                int cur_prio = (st->detail_ok && st->selected < MAX_ROWS)
                               ? (int)st->details[st->selected].prio : 10;
                int new_prio = cur_prio + 1;
                rc = aictl_setprio(pid, new_prio);
            }
            break;
        default:
            return;
    }

    serial_ctl(pid, verb, rc);

    /* Build status string (reset first). */
    st->status[0] = '\0';
    char *p = st->status;
    if (rc == 0) {
        p = _cat(p, verb_name);
        p = _cat(p, " pid=");
        p = _cat_uint(p, (u64)pid, 0, ' ');
        p = _cat(p, " (");
        char name[33];
        _strncpy(name, st->procs[st->selected].name, 33);
        p = _cat(p, name);
        p = _cat(p, ") OK");
        st->status_color = C_STATUS_OK;
    } else {
        p = _cat(p, verb_name);
        p = _cat(p, " pid=");
        p = _cat_uint(p, (u64)pid, 0, ' ');
        p = _cat(p, " failed rc=");
        p = _cat_int(p, rc);
        st->status_color = C_STATUS_ERR;
    }

    /* Force immediate refresh so the table reflects new state. */
    st->refresh_ctr = 0;
}

/* -------------------------------------------------------------------------
 * Data refresh.
 * ---------------------------------------------------------------------- */
static void do_refresh(procmon_t *st)
{
    /* Enumerate processes. */
    int cnt = aictl_list(st->procs, MAX_PROCS);
    if (cnt < 0) cnt = 0;
    if (cnt > MAX_PROCS) cnt = MAX_PROCS;
    st->proc_count = cnt;

    /* Fetch details for visible rows. */
    int vis = cnt < MAX_ROWS ? cnt : MAX_ROWS;
    st->detail_ok = 0;
    for (int i = 0; i < vis; i++) {
        int r = aictl_query(st->procs[i].pid, &st->details[i]);
        if (r == 0) st->detail_ok = 1;
    }

    /* System-wide stats. */
    int r = aictl_sysinfo(&st->sysinfo);
    st->sysinfo_ok = (r == 0);
}

/* -------------------------------------------------------------------------
 * Draw the header bar.
 * ---------------------------------------------------------------------- */
static void draw_header(u32 *buf, u32 bw, u32 bh, u32 spx, procmon_t *st)
{
    fill_grad_v(buf, bw, bh, spx, 0, HDR_Y, (i32)bw, HDR_H, C_HDR_TOP, C_HDR_BOT);

    /* Accent stripe at top. */
    fill_rect(buf, bw, bh, spx, 0, HDR_Y, (i32)bw, 2, C_ACCENT);

    /* Title. */
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     12, HDR_Y + (HDR_H - FONT_H) / 2,
                     "Process Monitor", C_ACCENT);

    /* Uptime (right side). */
    char ubuf[32];
    if (st->sysinfo_ok) {
        char hms[12];
        _fmt_hms(hms, st->sysinfo.uptime_ms);
        char *p = ubuf;
        p = _cat(p, "Up ");
        _cat(p, hms);
    } else {
        _strncpy(ubuf, "Up --:--:--", 32);
    }
    int uw = font_text_width(ubuf);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     (int)bw - uw - 12, HDR_Y + (HDR_H - FONT_H) / 2,
                     ubuf, C_TEXT2);

    /* Bottom separator. */
    fill_rect(buf, bw, bh, spx, 0, HDR_Y + HDR_H - 1, (i32)bw, 1, C_BORDER);
}

/* -------------------------------------------------------------------------
 * Draw the stats strip.
 * ---------------------------------------------------------------------- */
static void draw_stats(u32 *buf, u32 bw, u32 bh, u32 spx, procmon_t *st)
{
    fill_rect(buf, bw, bh, spx, 0, STATS_Y, (i32)bw, STATS_H, C_STATS_BG);

    /* Stat items separated by vertical bars. */
    char sb[48];
    char *p;
    int x = 12;
    int y = STATS_Y + (STATS_H - FONT_H) / 2;

    /* Processes. */
    p = sb;
    p = _cat(p, "Procs: ");
    p = _cat_uint(p, (u64)st->proc_count, 0, ' ');
    font_draw_string(buf, (int)spx, (int)bw, (int)bh, x, y, sb, C_TEXT2);
    x += font_text_width(sb) + 12;
    fill_rect(buf, bw, bh, spx, x, STATS_Y + 8, 1, STATS_H - 16, C_BORDER);
    x += 12;

    /* Uptime (verbose). */
    if (st->sysinfo_ok) {
        char hms[12];
        _fmt_hms(hms, st->sysinfo.uptime_ms);
        p = sb;
        p = _cat(p, "Uptime: ");
        _cat(p, hms);
    } else {
        _strncpy(sb, "Uptime: --:--:--", sizeof(sb));
    }
    font_draw_string(buf, (int)spx, (int)bw, (int)bh, x, y, sb, C_TEXT2);
    x += font_text_width(sb) + 12;
    fill_rect(buf, bw, bh, spx, x, STATS_Y + 8, 1, STATS_H - 16, C_BORDER);
    x += 12;

    /* Memory. */
    if (st->sysinfo_ok && st->sysinfo.total_mem > 0) {
        u64 free_kb  = st->sysinfo.free_mem  / 1024ULL;
        u64 total_kb = st->sysinfo.total_mem / 1024ULL;
        p = sb;
        p = _cat(p, "Mem: ");
        p = _cat_uint(p, free_kb,  0, ' ');
        p = _cat(p, " / ");
        p = _cat_uint(p, total_kb, 0, ' ');
        p = _cat(p, " KB free");
    } else {
        _strncpy(sb, "Mem: n/a", sizeof(sb));
    }
    font_draw_string(buf, (int)spx, (int)bw, (int)bh, x, y, sb, C_TEXT2);

    /* Bottom border. */
    fill_rect(buf, bw, bh, spx, 0, STATS_Y + STATS_H - 1, (i32)bw, 1, C_BORDER);
}

/* -------------------------------------------------------------------------
 * Draw the process table.
 * ---------------------------------------------------------------------- */
static void draw_table(u32 *buf, u32 bw, u32 bh, u32 spx, procmon_t *st)
{
    fill_rect(buf, bw, bh, spx, 0, TABLE_Y, (i32)bw, TABLE_H, C_BG);

    /* Column header row. */
    fill_rect(buf, bw, bh, spx, 0, COL_HDR_Y - 2, (i32)bw, FONT_H + 6, C_PANEL2);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     COL_PID_X,   COL_HDR_Y, "PID",   C_ACCENT2);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     COL_NAME_X,  COL_HDR_Y, "NAME",  C_ACCENT2);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     COL_STATE_X, COL_HDR_Y, "STATE", C_ACCENT2);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     COL_CPU_X,   COL_HDR_Y, "CPU",   C_ACCENT2);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     COL_MEM_X,   COL_HDR_Y, "MEM",   C_ACCENT2);
    /* Separator under header. */
    fill_rect(buf, bw, bh, spx, 0, COL_HDR_Y + FONT_H + 4, (i32)bw, 1, C_SEP);

    /* Data rows. */
    int vis = st->proc_count < MAX_ROWS ? st->proc_count : MAX_ROWS;
    for (int i = 0; i < MAX_ROWS; i++) {
        i32 ry = COL_DATA_Y0 + i * ROW_H;

        /* Store hit-test rect for this row. */
        st->row_rect[i].x = 0;
        st->row_rect[i].y = ry - 1;
        st->row_rect[i].w = (i32)bw;
        st->row_rect[i].h = ROW_H;

        if (i >= vis) {
            fill_rect(buf, bw, bh, spx, 0, ry - 1, (i32)bw, ROW_H, C_BG);
            continue;
        }

        procinfo_t   *pi = &st->procs[i];
        proc_detail_t *pd = &st->details[i];

        /* Row background. */
        u32 row_bg;
        if (i == st->selected) {
            row_bg = C_ROW_SEL;
        } else if (pi->state == 2) {
            row_bg = 0xFF0D1F14u; /* running: very slightly green tinted */
        } else {
            row_bg = (i & 1) ? C_ROW_ODD : C_ROW_EVEN;
        }
        fill_rect(buf, bw, bh, spx, 0, ry - 1, (i32)bw, ROW_H, row_bg);

        /* Selection indicator stripe on left edge. */
        if (i == st->selected) {
            fill_rect(buf, bw, bh, spx, 0, ry - 1, 2, ROW_H, C_ROW_SEL_BRD);
        }

        char nb[24];

        /* PID column. */
        _utoa((u64)pi->pid, nb);
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         COL_PID_X, ry, nb, C_TEXT2);

        /* NAME column (up to 22 chars). */
        char name[24];
        _strncpy(name, pi->name, 23);
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         COL_NAME_X, ry, name,
                         (i == st->selected) ? C_TEXT : C_TEXT2);

        /* STATE column (colored). */
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         COL_STATE_X, ry, state_str(pi->state),
                         state_color(pi->state));

        /* CPU column (from detail or "n/a"). */
        if (st->detail_ok && i < MAX_ROWS) {
            _utoa(pd->cpu_ticks, nb);
            font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                             COL_CPU_X, ry, nb, C_TEXT3);
        } else {
            font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                             COL_CPU_X, ry, "n/a", C_TEXT3);
        }

        /* MEM column (pages or "n/a"). */
        if (st->detail_ok && i < MAX_ROWS) {
            _utoa((u64)pd->mem_pages, nb);
            font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                             COL_MEM_X, ry, nb, C_TEXT3);
        } else {
            font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                             COL_MEM_X, ry, "n/a", C_TEXT3);
        }
    }

    /* If proc list is entirely unavailable. */
    if (st->proc_count <= 0) {
        font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                         COL_NAME_X, COL_DATA_Y0,
                         "(process list unavailable)", C_TEXT3);
    }

    /* Bottom border. */
    fill_rect(buf, bw, bh, spx, 0, TABLE_Y + TABLE_H - 1, (i32)bw, 1, C_BORDER);
}

/* Button layout constants (5 buttons across width). */
#define BTN_W    110
#define BTN_GAP    8
#define BTN_TOTAL  ((BTN_W + BTN_GAP) * N_BTNS - BTN_GAP)
#define BTN_X0     ((WIN_W - BTN_TOTAL) / 2)

static const char *BTN_LABELS[N_BTNS]  = { "Suspend", "Resume", "Kill", "Nice+", "Nice-" };
static const u32   BTN_COLORS[N_BTNS]  = { C_BTN_SUSP, C_BTN_RESU, C_BTN_KILL, C_BTN_NICE, C_BTN_NICE };

/* -------------------------------------------------------------------------
 * Draw the action button row.
 * ---------------------------------------------------------------------- */
static void draw_buttons(u32 *buf, u32 bw, u32 bh, u32 spx, procmon_t *st)
{
    fill_rect(buf, bw, bh, spx, 0, BTN_Y, (i32)bw, BTN_H, C_BG);

    for (int i = 0; i < N_BTNS; i++) {
        i32 bx = BTN_X0 + i * (BTN_W + BTN_GAP);
        i32 by = BTN_Y + (BTN_H - 26) / 2;

        draw_btn(buf, bw, bh, spx,
                 bx, by, BTN_W, 26,
                 BTN_LABELS[i], BTN_COLORS[i], C_BTN_LBL);

        /* Store hit-test rect. */
        st->btn_rect[i].x = bx;
        st->btn_rect[i].y = by;
        st->btn_rect[i].w = BTN_W;
        st->btn_rect[i].h = 26;
    }
}

/* -------------------------------------------------------------------------
 * Draw the status bar.
 * ---------------------------------------------------------------------- */
static void draw_status(u32 *buf, u32 bw, u32 bh, u32 spx, procmon_t *st)
{
    fill_rect(buf, bw, bh, spx, 0, STATUS_Y, (i32)bw, STATUS_H, C_STATUS_BG);
    fill_rect(buf, bw, bh, spx, 0, STATUS_Y, (i32)bw, 1, C_BORDER);
    font_draw_string(buf, (int)spx, (int)bw, (int)bh,
                     10, STATUS_Y + (STATUS_H - FONT_H) / 2,
                     st->status, st->status_color);
}

/* -------------------------------------------------------------------------
 * Entry point.
 * ---------------------------------------------------------------------- */
void _start(void)
{
    _write("[PROCMON] starting\n");

    /* Own PID -- never a valid target for control verbs. */
    g_pm.own_pid = (u32)_sc(SYS_GETPID, 0, 0, 0);
    g_pm.selected = -1;
    g_pm.refresh_ctr = 0;
    g_pm.frame = 0;
    g_pm.status_color = C_STATUS_INFO;
    _strncpy(g_pm.status, "Ready. Click a process row to select.", 128);

    /* Initial data load. */
    do_refresh(&g_pm);

    /* Connect to compositor. */
    if (wl_connect() != 0) {
        _write("[PROCMON] wl_connect FAILED\n");
        for (;;) _sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Process Monitor");
    if (!win) {
        _write("[PROCMON] wl_create_window FAILED\n");
        for (;;) _sc(SYS_YIELD, 0, 0, 0);
    }

    u32 spx = win->stride / 4u;

    /* Main loop. */
    for (;;) {
        /* --- Input handling --- */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                int mx = ea, my = eb;
                int buttons = ec;
                /* Only act on button-down (buttons != 0). */
                if (buttons != 0) {
                    /* Check action buttons. */
                    int handled = 0;
                    for (int i = 0; i < N_BTNS && !handled; i++) {
                        if (rect_hit(&g_pm.btn_rect[i], mx, my)) {
                            do_action(i);
                            handled = 1;
                        }
                    }
                    /* Check process rows. */
                    if (!handled) {
                        for (int i = 0; i < MAX_ROWS; i++) {
                            if (rect_hit(&g_pm.row_rect[i], mx, my)) {
                                if (i < g_pm.proc_count) {
                                    g_pm.selected = i;
                                    /* Reset and build status string. */
                                    g_pm.status[0] = '\0';
                                    char *p = g_pm.status;
                                    p = _cat(p, "Selected: ");
                                    char sel_name[33];
                                    _strncpy(sel_name, g_pm.procs[i].name, 33);
                                    p = _cat(p, sel_name);
                                    p = _cat(p, "  pid=");
                                    p = _cat_uint(p, (u64)g_pm.procs[i].pid, 0, ' ');
                                    (void)p;
                                    g_pm.status_color = C_STATUS_INFO;
                                }
                                break;
                            }
                        }
                    }
                }
            }
            /* Keyboard not used; just drain. */
        }

        /* --- Data refresh (throttled) --- */
        g_pm.refresh_ctr++;
        if (g_pm.refresh_ctr >= REFRESH_PERIOD) {
            g_pm.refresh_ctr = 0;
            do_refresh(&g_pm);
            /* If selected index went out of range after refresh, deselect. */
            if (g_pm.selected >= g_pm.proc_count)
                g_pm.selected = -1;
        }

        /* --- Render --- */
        fill_rect(win->pixels, win->w, win->h, spx,
                  0, 0, (i32)win->w, (i32)win->h, C_BG);

        draw_header (win->pixels, win->w, win->h, spx, &g_pm);
        draw_stats  (win->pixels, win->w, win->h, spx, &g_pm);
        draw_table  (win->pixels, win->w, win->h, spx, &g_pm);
        draw_buttons(win->pixels, win->w, win->h, spx, &g_pm);
        draw_status (win->pixels, win->w, win->h, spx, &g_pm);

        wl_commit(win);
        _sc(SYS_YIELD, 0, 0, 0);

        g_pm.frame++;
    }
}

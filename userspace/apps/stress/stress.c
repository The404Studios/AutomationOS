/*
 * stress.c -- Kernel stability stress-test application (freestanding, ring 3).
 * ============================================================================
 *
 * Opens a 560x420 window titled "Stress Test".  Exercises the kernel via
 * four concurrent test tracks that can be started/stopped and tuned with
 * on-screen controls:
 *
 *   1. SPAWN/KILL churn   - spawn sbin/calculator, SIGKILL after a moment;
 *                           exercises loader + force-quit + PCB teardown.
 *   2. MEMORY churn       - SYS_MMAP / touch / SYS_MUNMAP many cycles with
 *                           varying sizes; exercises the VMM allocator.
 *   3. SUSPEND/RESUME     - keep one child suspended then resumed rapidly;
 *                           exercises proc_ctl / scheduler state transitions.
 *   4. LEAK WATCH         - SYS_SYSINFO sampled at baseline and every N cycles;
 *                           displays free_mem delta and proc_count delta;
 *                           prints WARNING if either keeps drifting.
 *
 * Build (flags DIRECTLY on the command line — no Makefile required):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/stress/stress.c -o /tmp/stress.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/stress.o /tmp/wlc.o /tmp/bf.o -o /tmp/stress.elf
 *   objdump -d /tmp/stress.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [STRESS] starting
 *   [STRESS] cycle=N spawns=A kills=B free_mem=M proc=P
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -------------------------------------------------------------------------
 * Syscall numbers (from kernel/include/syscall.h)
 * ----------------------------------------------------------------------- */
#define SYS_WRITE         3
#define SYS_WAITPID       6
#define SYS_GETPID        8
#define SYS_YIELD         15
#define SYS_SPAWN         16
#define SYS_MMAP          37
#define SYS_MUNMAP        38
#define SYS_GET_TICKS_MS  40
#define SYS_KILL          26
#define SYS_PROCLIST      44
#define SYS_PROC_CTL      61
#define SYS_SYSINFO       62

/* procapi_ctl verb codes */
#define CTL_SUSPEND    0
#define CTL_RESUME     1
#define CTL_KILL       2

/* -------------------------------------------------------------------------
 * Minimal integer types for freestanding builds.
 * ----------------------------------------------------------------------- */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed int         i32;
typedef signed long long   i64;
typedef unsigned long      ulong;

/* -------------------------------------------------------------------------
 * Inline syscall helpers (6-argument form).
 * No fs:0x28 canary possible because -fno-stack-protector is mandatory.
 * ----------------------------------------------------------------------- */
static inline i64 sc6(i64 n, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6)
{
    i64 r;
    register i64 r10 asm("r10") = a4;
    register i64 r8  asm("r8")  = a5;
    register i64 r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

#define sc(n,a,b,c)   sc6((n),(a),(b),(c),0,0,0)
#define sc4(n,a,b,c,d) sc6((n),(a),(b),(c),(d),0,0)

/* -------------------------------------------------------------------------
 * Freestanding string/math helpers.
 * ----------------------------------------------------------------------- */
static ulong k_strlen(const char *s)
{
    ulong n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc(SYS_WRITE, 1, (i64)m, (i64)k_strlen(m));
}

static i32 iabs(i32 v) { return v < 0 ? -v : v; }

/* Decimal integer → NUL-terminated string. Returns pointer into buf. */
static char *itoa(i64 v, char *buf, int bufsz)
{
    char tmp[24];
    int i = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { tmp[i++] = '0'; }
    else { while (v && i < 22) { tmp[i++] = (char)('0' + v % 10); v /= 10; } }
    if (neg && i < 23) tmp[i++] = '-';
    int j = 0;
    while (i > 0 && j < bufsz - 1) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

/* Append a string into a buffer; returns new write pointer. */
static char *append(char *dst, const char *src, char *end)
{
    while (*src && dst < end - 1) *dst++ = *src++;
    *dst = '\0';
    return dst;
}

/* Print a key=value line to serial. */
static void print_kv(const char *key, i64 val)
{
    char nbuf[24];
    char line[64];
    char *p = line;
    char *e = line + sizeof(line);
    p = append(p, key, e);
    p = append(p, itoa(val, nbuf, sizeof(nbuf)), e);
    p = append(p, "\n", e);
    print(line);
}

/* -------------------------------------------------------------------------
 * sysinfo_t mirror (from kernel/include/procapi.h)
 * ----------------------------------------------------------------------- */
typedef struct {
    u64 total_mem;
    u64 free_mem;
    u64 uptime_ms;
    u32 proc_count;
    u32 _pad;
} sysinfo_t;

/* -------------------------------------------------------------------------
 * proc_info_t mirror (from kernel/include/sched.h, for PROCLIST) -- 64 bytes
 * ----------------------------------------------------------------------- */
typedef struct {
    u32  pid;
    u32  parent_pid;
    u32  state;
    u32  flags;
    char name[32];
    u64  cpu_ticks;
    u64  ctx_switches;
} proc_info_t;

/* -------------------------------------------------------------------------
 * Window & layout constants.
 * ----------------------------------------------------------------------- */
#define WIN_W      560
#define WIN_H      420

/* Header bar height. */
#define HDR_H       28

/* Control strip below header (Start/Stop/intensity). */
#define CTRL_H      36
#define CTRL_Y      HDR_H

/* Report area below controls. */
#define REPORT_Y    (HDR_H + CTRL_H)
#define REPORT_H    (WIN_H - REPORT_Y)

/* -------------------------------------------------------------------------
 * Colors.
 * ----------------------------------------------------------------------- */
#define C_BG        0xFF1A1A2Eu  /* dark navy             */
#define C_HDR       0xFF16213Eu  /* slightly lighter navy */
#define C_CTRL      0xFF0F3460u  /* medium blue band      */
#define C_PANEL     0xFF1E1E2Eu  /* report panel          */
#define C_SEP       0xFF3A3A5Au
#define C_TEXT      0xFFCCCCCCu
#define C_TEXT_DIM  0xFF777799u
#define C_PASS      0xFF00CC66u  /* green                 */
#define C_WARN      0xFFFF6600u  /* amber                 */
#define C_ERR       0xFFFF2244u  /* red                   */
#define C_BTN_RUN   0xFF226644u  /* Start button          */
#define C_BTN_STOP  0xFF662222u  /* Stop button           */
#define C_BTN_HOVER 0xFF334455u
#define C_ACCENT    0xFF4488FFu

/* -------------------------------------------------------------------------
 * Drawing primitives.
 * ----------------------------------------------------------------------- */
static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 spx,
                      i32 x, i32 y, i32 w, i32 h, u32 col)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * spx;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = col;
    }
}

static void hline(u32 *buf, u32 bw, u32 bh, u32 spx,
                  i32 x, i32 y, i32 len, u32 col)
{
    fill_rect(buf, bw, bh, spx, x, y, len, 1, col);
}

static void draw_border(u32 *buf, u32 bw, u32 bh, u32 spx,
                        i32 x, i32 y, i32 w, i32 h, u32 col)
{
    hline(buf, bw, bh, spx, x,         y,         w, col);
    hline(buf, bw, bh, spx, x,         y + h - 1, w, col);
    fill_rect(buf, bw, bh, spx, x,         y, 1, h, col);
    fill_rect(buf, bw, bh, spx, x + w - 1, y, 1, h, col);
}

static void draw_text(u32 *buf, u32 spx, u32 bw, u32 bh,
                      i32 x, i32 y, const char *s, u32 col)
{
    font_draw_string(buf, (int)spx, (int)bw, (int)bh, x, y, s, col);
}

/* Right-align text so it ends at pixel x_right. */
static void draw_text_right(u32 *buf, u32 spx, u32 bw, u32 bh,
                            i32 x_right, i32 y, const char *s, u32 col)
{
    int tw = font_text_width(s);
    draw_text(buf, spx, bw, bh, x_right - tw, y, s, col);
}

/* Draw a filled button. Returns hit-test: 1 if (mx,my) is inside. */
static int draw_button(u32 *buf, u32 bw, u32 bh, u32 spx,
                       i32 x, i32 y, i32 w, i32 h,
                       const char *label, u32 face, i32 mx, i32 my)
{
    int hot = (mx >= x && mx < x + w && my >= y && my < y + h);
    u32 fc  = hot ? C_BTN_HOVER : face;
    fill_rect(buf, bw, bh, spx, x, y, w, h, fc);
    draw_border(buf, bw, bh, spx, x, y, w, h, C_SEP);
    int tw = font_text_width(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - FONT_H) / 2;
    draw_text(buf, spx, bw, bh, tx, ty, label, C_TEXT);
    return hot;
}

/* -------------------------------------------------------------------------
 * Intensity (1 = light, 2 = medium, 3 = heavy).
 * Controls how many inner iterations each frame.
 * ----------------------------------------------------------------------- */
#define INTENS_MAX   3

/* Cycles of mmap/munmap per tick at each intensity level. */
static const i32 MEM_ITERS[INTENS_MAX + 1] = { 0, 2, 6, 12 };
/* Target concurrent spawned processes. */
static const i32 SPAWN_CAP[INTENS_MAX + 1]  = { 0, 1, 2,  4 };
/* Suspend/resume pairs per tick. */
static const i32 SR_ITERS[INTENS_MAX + 1]   = { 0, 1, 3,  8 };

/* -------------------------------------------------------------------------
 * Stress state.
 * ----------------------------------------------------------------------- */
#define MAX_CHILDREN  4   /* absolute cap on concurrent spawned pids */

static int   running      = 0;          /* 1 = test active               */
static int   intensity    = 2;          /* 1–3                           */
static i64   my_pid       = -1;         /* cached GETPID                 */

/* Spawn/kill track. */
static i64   child_pids[MAX_CHILDREN];  /* 0 = slot free                 */
static i32   child_ticks[MAX_CHILDREN]; /* ms-tick when spawned          */
static i64   total_spawns  = 0;
static i64   total_kills   = 0;

/* Memory churn track. */
static i64   total_mmaps   = 0;
static i64   total_munmaps = 0;

/* Suspend/resume track. */
static i64   sr_pid        = 0;         /* the dedicated s/r child       */
static i64   total_suspends = 0;
static i64   total_resumes  = 0;

/* Leak watch. */
static sysinfo_t baseline;             /* sampled when test starts      */
static i64   baseline_valid  = 0;
static i64   last_free_mem   = 0;
static i32   last_proc_count = 0;
static i64   leak_cycles     = 0;      /* sampling counter              */
static i64   warn_mem        = 0;      /* 1 = potential memory leak     */
static i64   warn_proc       = 0;      /* 1 = potential proc leak       */

/* General cycle counter. */
static i64   cycle = 0;

/* -------------------------------------------------------------------------
 * Target application to spawn (a known light ELF on the initrd).
 * ----------------------------------------------------------------------- */
static const char SPAWN_PATH[] = "sbin/calculator";

/* Kill a pid safely: never touch pid 0, 1, or ourselves. */
static void safe_kill(i64 pid)
{
    if (pid <= 1) return;
    if (pid == my_pid) return;
    sc(SYS_KILL, pid, 9, 0);   /* SIGKILL */
    total_kills++;
}

/* Reap any child that has been alive long enough.
 * "Long enough" = 200 ms at intensity 1, 100 ms at 2, 50 ms at 3.      */
static void reap_old_children(i64 now_ms)
{
    static const i32 LIFETIME_MS[INTENS_MAX + 1] = { 0, 200, 100, 50 };
    i32 lifetime = LIFETIME_MS[intensity];

    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (child_pids[i] <= 0) continue;
        if ((i32)(now_ms - child_ticks[i]) >= lifetime) {
            safe_kill(child_pids[i]);
            /* Non-blocking waitpid to prevent zombie accumulation. */
            sc(SYS_WAITPID, child_pids[i], 0, 1);
            child_pids[i]  = 0;
            child_ticks[i] = 0;
        }
    }
}

/* Count currently live slots. */
static int live_children(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CHILDREN; i++)
        if (child_pids[i] > 0) n++;
    return n;
}

/* Spawn one child if we have a free slot and haven't hit the cap. */
static void try_spawn_child(i64 now_ms)
{
    i32 cap = SPAWN_CAP[intensity];
    if (live_children() >= cap) return;

    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (child_pids[i] <= 0) { slot = i; break; }
    }
    if (slot < 0) return;

    i64 pid = sc(SYS_SPAWN, (i64)SPAWN_PATH, 0, 0);
    if (pid > 1 && pid != my_pid) {
        child_pids[slot]  = pid;
        child_ticks[slot] = (i32)now_ms;
        total_spawns++;
    }
}

/* Kill & wait all tracked children (called on Stop). */
static void kill_all_children(void)
{
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (child_pids[i] > 0) {
            safe_kill(child_pids[i]);
            sc(SYS_WAITPID, child_pids[i], 0, 1);
            child_pids[i]  = 0;
            child_ticks[i] = 0;
        }
    }
}

/* -------------------------------------------------------------------------
 * Memory churn: mmap a buffer, write every page, munmap.
 * Vary size by cycle so we exercise different allocator code paths.
 * ----------------------------------------------------------------------- */
#define PAGE_SIZE   4096
#define PROT_RW     3    /* PROT_READ | PROT_WRITE */

static void do_mem_churn(void)
{
    i32 iters = MEM_ITERS[intensity];

    /* Sizes: 4 KB, 8 KB, 16 KB, 64 KB, 128 KB cycled by total_mmaps. */
    static const i32 SIZES[] = { 1, 2, 4, 16, 32 };   /* in pages */
    static int sz_idx = 0;

    for (i32 j = 0; j < iters; j++) {
        i32 npages = SIZES[sz_idx % 5];
        sz_idx++;
        ulong len = (ulong)npages * PAGE_SIZE;

        /* SYS_MMAP(hint=0, len, prot=PROT_RW) */
        i64 addr = sc4(SYS_MMAP, 0, (i64)len, PROT_RW, 0);
        if (addr <= 0) continue;   /* allocation failed; skip gracefully */

        total_mmaps++;

        /* Touch every page to actually exercise the mapper. */
        volatile u8 *p = (volatile u8 *)(ulong)addr;
        for (i32 k = 0; k < npages; k++)
            p[(ulong)k * PAGE_SIZE] = (u8)(k ^ 0xA5);

        /* SYS_MUNMAP(addr, len) */
        sc(SYS_MUNMAP, addr, (i64)len, 0);
        total_munmaps++;
    }
}

/* -------------------------------------------------------------------------
 * Suspend/resume churn: use sr_pid child (also acts as spawn/kill target).
 * We maintain a dedicated long-lived child for this track.
 * ----------------------------------------------------------------------- */
static void ensure_sr_child(i64 now_ms)
{
    if (sr_pid > 1) return;  /* already alive */

    i64 pid = sc(SYS_SPAWN, (i64)SPAWN_PATH, 0, 0);
    if (pid > 1 && pid != my_pid) {
        sr_pid = pid;
        total_spawns++;
        /* record it in a free child slot so reap logic doesn't double-kill */
        for (int i = 0; i < MAX_CHILDREN; i++) {
            if (child_pids[i] <= 0) {
                child_pids[i]  = pid;
                child_ticks[i] = (i32)now_ms + 60000; /* very long lifetime */
                break;
            }
        }
    }
}

static void do_suspend_resume(i64 now_ms)
{
    i32 iters = SR_ITERS[intensity];
    ensure_sr_child(now_ms);
    if (sr_pid <= 1) return;

    for (i32 j = 0; j < iters; j++) {
        /* Suspend (verb=0). */
        i64 r = sc(SYS_PROC_CTL, sr_pid, CTL_SUSPEND, 0);
        if (r < 0) { sr_pid = 0; return; }  /* process died */
        total_suspends++;

        /* Yield once so the scheduler can act. */
        sc(SYS_YIELD, 0, 0, 0);

        /* Resume (verb=1). */
        r = sc(SYS_PROC_CTL, sr_pid, CTL_RESUME, 0);
        if (r < 0) { sr_pid = 0; return; }
        total_resumes++;
    }
}

/* -------------------------------------------------------------------------
 * Leak watch: sample SYS_SYSINFO and compare to baseline.
 * Drift threshold: >2 MB mem loss or >2 zombie procs after kills.
 * ----------------------------------------------------------------------- */
#define LEAK_SAMPLE_CYCLES  20
#define LEAK_MEM_THRESH     (2ULL * 1024 * 1024)  /* 2 MB drift = warning */
#define LEAK_PROC_THRESH    3                      /* +3 procs unexplained */

static void update_leak_watch(void)
{
    leak_cycles++;
    if ((leak_cycles % LEAK_SAMPLE_CYCLES) != 0) return;

    sysinfo_t si;
    i64 r = sc(SYS_SYSINFO, (i64)&si, 0, 0);
    if (r < 0) return;

    last_free_mem   = (i64)si.free_mem;
    last_proc_count = (i32)si.proc_count;

    if (!baseline_valid) return;

    /* free_mem delta: negative means memory has been consumed and not returned. */
    i64 mem_delta = (i64)baseline.free_mem - (i64)si.free_mem;
    i32 proc_delta = (i32)si.proc_count - (i32)baseline.proc_count;

    /* Memory warning: consistent drain beyond threshold. */
    warn_mem  = (mem_delta  > (i64)LEAK_MEM_THRESH)  ? 1 : 0;
    /* Process leak: more procs than at baseline (accounting for sr_pid). */
    warn_proc = (proc_delta > LEAK_PROC_THRESH) ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * Main stress tick (called once per frame while running==1).
 * ----------------------------------------------------------------------- */
static void stress_tick(void)
{
    cycle++;
    i64 now_ms = sc(SYS_GET_TICKS_MS, 0, 0, 0);

    /* Track 1: spawn/kill churn. */
    reap_old_children(now_ms);
    try_spawn_child(now_ms);

    /* Track 2: memory churn. */
    do_mem_churn();

    /* Track 3: suspend/resume. */
    do_suspend_resume(now_ms);

    /* Track 4: leak watch. */
    update_leak_watch();

    /* Periodic serial log (every 50 cycles). */
    if ((cycle % 50) == 0) {
        sysinfo_t si;
        sc(SYS_SYSINFO, (i64)&si, 0, 0);

        char buf[128];
        char *p = buf, *e = buf + sizeof(buf);
        char n1[24], n2[24], n3[24], n4[24], n5[24];
        p = append(p, "[STRESS] cycle=",      e);
        p = append(p, itoa(cycle, n1, 24),    e);
        p = append(p, " spawns=",             e);
        p = append(p, itoa(total_spawns, n2, 24), e);
        p = append(p, " kills=",              e);
        p = append(p, itoa(total_kills,  n3, 24), e);
        p = append(p, " free_mem=",           e);
        p = append(p, itoa((i64)si.free_mem, n4, 24), e);
        p = append(p, " proc=",               e);
        p = append(p, itoa((i64)si.proc_count, n5, 24), e);
        p = append(p, "\n",                   e);
        print(buf);
    }
}

/* -------------------------------------------------------------------------
 * UI rendering helpers.
 * ----------------------------------------------------------------------- */

/* Format a large number as "NNN KB" to fit in the display. */
static const char *fmt_kb(i64 bytes, char *buf, int bsz)
{
    i64 kb = bytes / 1024;
    char tmp[24];
    char *p = buf, *e = buf + bsz;
    p = append(p, itoa(kb, tmp, 24), e);
    p = append(p, " KB", e);
    return buf;
}

static const char *fmt_i64(i64 v, char *buf, int bsz)
{
    return itoa(v, buf, bsz);
}

/* Render the full UI into the window framebuffer. */
static void render(wl_window *win, i32 mx, i32 my,
                   int *start_hot, int *stop_hot,
                   int *int_dn_hot, int *int_up_hot)
{
    u32 *buf  = win->pixels;
    u32  bw   = win->w;
    u32  bh   = win->h;
    u32  spx  = win->stride / 4u;
    char tmp1[32], tmp2[32];

    /* Report area height derived from the LIVE window height so the panel
     * reflows on resize/maximize instead of using the fixed-size constant.
     * Clamp to >=0 in case a very short window puts REPORT_Y past the bottom. */
    i32 report_h = (i32)bh - REPORT_Y;
    if (report_h < 0) report_h = 0;

    /* ---- Background ---- */
    fill_rect(buf, bw, bh, spx, 0, 0, (i32)bw, (i32)bh, C_BG);

    /* ---- Header ---- */
    fill_rect(buf, bw, bh, spx, 0, 0, (i32)bw, HDR_H, C_HDR);
    hline(buf, bw, bh, spx, 0, HDR_H - 1, (i32)bw, C_SEP);
    draw_text(buf, spx, bw, bh, 8, (HDR_H - FONT_H) / 2,
              "Stress Test", C_ACCENT);
    /* Status badge top-right. */
    const char *status_str = running ? "RUNNING" : "IDLE";
    u32 status_col = running ? C_PASS : C_TEXT_DIM;
    draw_text_right(buf, spx, bw, bh, (i32)bw - 8,
                    (HDR_H - FONT_H) / 2, status_str, status_col);

    /* ---- Control strip ---- */
    fill_rect(buf, bw, bh, spx, 0, CTRL_Y, (i32)bw, CTRL_H, C_CTRL);
    hline(buf, bw, bh, spx, 0, CTRL_Y + CTRL_H - 1, (i32)bw, C_SEP);

    /* Start / Stop buttons. */
    *start_hot = draw_button(buf, bw, bh, spx,
                             8, CTRL_Y + 4, 70, 26,
                             "Start", running ? C_BTN_HOVER : C_BTN_RUN,
                             mx, my);
    *stop_hot  = draw_button(buf, bw, bh, spx,
                             84, CTRL_Y + 4, 70, 26,
                             "Stop", running ? C_BTN_STOP : C_BTN_HOVER,
                             mx, my);

    /* Intensity control: [ - ] N [ + ] */
    draw_text(buf, spx, bw, bh, 168, CTRL_Y + (CTRL_H - FONT_H) / 2,
              "Intensity:", C_TEXT_DIM);
    *int_dn_hot = draw_button(buf, bw, bh, spx,
                              264, CTRL_Y + 4, 22, 26,
                              "-", C_BTN_HOVER, mx, my);
    char intbuf[4];
    itoa(intensity, intbuf, 4);
    draw_text(buf, spx, bw, bh,
              292, CTRL_Y + (CTRL_H - FONT_H) / 2,
              intbuf, C_ACCENT);
    *int_up_hot = draw_button(buf, bw, bh, spx,
                              308, CTRL_Y + 4, 22, 26,
                              "+", C_BTN_HOVER, mx, my);

    /* ---- Report panel ---- */
    fill_rect(buf, bw, bh, spx, 0, REPORT_Y, (i32)bw, report_h, C_PANEL);

    /* Two-column layout: left = counters, right = leak watch. */
    i32 col2 = (i32)bw / 2 + 8;
    i32 ry   = REPORT_Y + 10;
    i32 lh   = FONT_H + 6;   /* line height */

    /* -- Left column: per-test counters -- */
    draw_text(buf, spx, bw, bh, 8, ry, "SPAWN/KILL", C_ACCENT);
    ry += lh;
    draw_text(buf, spx, bw, bh,  8, ry, "Spawns :", C_TEXT_DIM);
    draw_text(buf, spx, bw, bh, 80, ry,
              fmt_i64(total_spawns,  tmp1, 32), C_TEXT);
    ry += lh;
    draw_text(buf, spx, bw, bh,  8, ry, "Kills  :", C_TEXT_DIM);
    draw_text(buf, spx, bw, bh, 80, ry,
              fmt_i64(total_kills,   tmp1, 32), C_TEXT);
    ry += lh;
    draw_text(buf, spx, bw, bh,  8, ry, "Live   :", C_TEXT_DIM);
    draw_text(buf, spx, bw, bh, 80, ry,
              fmt_i64(live_children(), tmp1, 32), C_TEXT);

    ry += lh + 4;
    hline(buf, bw, bh, spx, 8, ry, col2 - 16, C_SEP);
    ry += 6;

    draw_text(buf, spx, bw, bh, 8, ry, "MEMORY CHURN", C_ACCENT);
    ry += lh;
    draw_text(buf, spx, bw, bh,  8, ry, "MMaps  :", C_TEXT_DIM);
    draw_text(buf, spx, bw, bh, 80, ry,
              fmt_i64(total_mmaps,   tmp1, 32), C_TEXT);
    ry += lh;
    draw_text(buf, spx, bw, bh,  8, ry, "MUnmaps:", C_TEXT_DIM);
    draw_text(buf, spx, bw, bh, 80, ry,
              fmt_i64(total_munmaps, tmp1, 32), C_TEXT);

    ry += lh + 4;
    hline(buf, bw, bh, spx, 8, ry, col2 - 16, C_SEP);
    ry += 6;

    draw_text(buf, spx, bw, bh, 8, ry, "SUSPEND/RESUME", C_ACCENT);
    ry += lh;
    draw_text(buf, spx, bw, bh,  8, ry, "Suspnd :", C_TEXT_DIM);
    draw_text(buf, spx, bw, bh, 80, ry,
              fmt_i64(total_suspends, tmp1, 32), C_TEXT);
    ry += lh;
    draw_text(buf, spx, bw, bh,  8, ry, "Resume :", C_TEXT_DIM);
    draw_text(buf, spx, bw, bh, 80, ry,
              fmt_i64(total_resumes,  tmp1, 32), C_TEXT);
    ry += lh;
    draw_text(buf, spx, bw, bh,  8, ry, "SR-PID :", C_TEXT_DIM);
    draw_text(buf, spx, bw, bh, 80, ry,
              fmt_i64(sr_pid > 1 ? sr_pid : 0, tmp1, 32),
              sr_pid > 1 ? C_PASS : C_TEXT_DIM);

    /* -- Right column: leak watch -- */
    i32 ry2  = REPORT_Y + 10;

    /* Vertical divider. */
    fill_rect(buf, bw, bh, spx, col2 - 4, REPORT_Y, 1, report_h, C_SEP);

    draw_text(buf, spx, bw, bh, col2, ry2, "LEAK WATCH", C_ACCENT);
    ry2 += lh;

    sysinfo_t si;
    si.free_mem   = 0;
    si.proc_count = 0;
    sc(SYS_SYSINFO, (i64)&si, 0, 0);

    draw_text(buf, spx, bw, bh, col2, ry2, "Free mem:", C_TEXT_DIM);
    ry2 += lh;
    draw_text(buf, spx, bw, bh, col2 + 8, ry2,
              fmt_kb((i64)si.free_mem, tmp1, 32), C_TEXT);
    ry2 += lh;

    draw_text(buf, spx, bw, bh, col2, ry2, "Processes:", C_TEXT_DIM);
    ry2 += lh;
    draw_text(buf, spx, bw, bh, col2 + 8, ry2,
              fmt_i64((i64)si.proc_count, tmp1, 32), C_TEXT);
    ry2 += lh;

    if (baseline_valid) {
        i64 mem_delta  = (i64)baseline.free_mem - (i64)si.free_mem;
        i32 proc_delta = (i32)si.proc_count - (i32)baseline.proc_count;

        draw_text(buf, spx, bw, bh, col2, ry2, "Mem delta:", C_TEXT_DIM);
        ry2 += lh;
        /* negative = memory returned to pool (good); positive = leak */
        u32 md_col = (mem_delta > (i64)LEAK_MEM_THRESH)  ? C_WARN : C_PASS;
        draw_text(buf, spx, bw, bh, col2 + 8, ry2,
                  fmt_kb(mem_delta, tmp1, 32), md_col);
        ry2 += lh;

        draw_text(buf, spx, bw, bh, col2, ry2, "Proc delta:", C_TEXT_DIM);
        ry2 += lh;
        u32 pd_col = (proc_delta > LEAK_PROC_THRESH) ? C_WARN : C_PASS;
        draw_text(buf, spx, bw, bh, col2 + 8, ry2,
                  fmt_i64((i64)proc_delta, tmp1, 32), pd_col);
        ry2 += lh;

        /* Overall status badge. */
        ry2 += 6;
        hline(buf, bw, bh, spx, col2, ry2, (i32)bw - col2 - 8, C_SEP);
        ry2 += 8;

        int any_warn = warn_mem || warn_proc;
        u32 badge_col  = any_warn ? C_WARN : C_PASS;
        const char *badge_str = any_warn ? "STATUS: WARNING" : "STATUS: PASS";
        draw_text(buf, spx, bw, bh, col2, ry2, badge_str, badge_col);
        ry2 += lh;

        if (warn_mem) {
            draw_text(buf, spx, bw, bh, col2 + 8, ry2,
                      "! MEM LEAK SUSPECTED", C_ERR);
            ry2 += lh;
        }
        if (warn_proc) {
            draw_text(buf, spx, bw, bh, col2 + 8, ry2,
                      "! PROC LEAK SUSPECTED", C_ERR);
        }
    } else {
        draw_text(buf, spx, bw, bh, col2, ry2,
                  "(start to begin)", C_TEXT_DIM);
    }

    /* ---- Cycle counter bottom bar ---- */
    i32 bar_y = (i32)bh - FONT_H - 6;
    hline(buf, bw, bh, spx, 0, bar_y - 2, (i32)bw, C_SEP);
    draw_text(buf, spx, bw, bh, 8, bar_y,
              "cycle:", C_TEXT_DIM);
    draw_text(buf, spx, bw, bh, 8 + 6 * FONT_W, bar_y,
              fmt_i64(cycle, tmp1, 32), C_TEXT);

    /* Intensity reminder. */
    char ibuf[32];
    char *ip = ibuf, *ie = ibuf + sizeof(ibuf);
    char in2[8];
    ip = append(ip, "intensity:", ie);
    ip = append(ip, itoa(intensity, in2, 8), ie);
    draw_text_right(buf, spx, bw, bh, (i32)bw - 8, bar_y,
                    ibuf, C_TEXT_DIM);
}

/* -------------------------------------------------------------------------
 * Entry point.
 * ----------------------------------------------------------------------- */
void _start(void)
{
    print("[STRESS] starting\n");

    my_pid = sc(SYS_GETPID, 0, 0, 0);

    /* Initialise child slot array. */
    for (int i = 0; i < MAX_CHILDREN; i++) {
        child_pids[i]  = 0;
        child_ticks[i] = 0;
    }

    /* Connect to compositor. */
    if (wl_connect() != 0) {
        print("[STRESS] wl_connect FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Stress Test");
    if (!win) {
        print("[STRESS] wl_create_window FAILED\n");
        for (;;) sc(SYS_YIELD, 0, 0, 0);
    }

    print("[STRESS] window created\n");

    /* Button state. */
    int prev_btn = 0;
    i32 mx = 0, my = 0;

    /* Hit flags from last render. */
    int start_hot = 0, stop_hot = 0, int_dn_hot = 0, int_up_hot = 0;

    /* Debounce: only act on release (button goes 1->0). */
    int btn_was_down = 0;

    for (;;) {
        /* Drain input. */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                mx = (i32)ea;
                my = (i32)eb;
                int btn = (ec & 1);

                /* Detect rising edge (click). */
                if (btn && !btn_was_down) {
                    /* Check which button was hot at the time of click. */
                    if (start_hot && !running) {
                        /* Capture baseline sysinfo. */
                        sc(SYS_SYSINFO, (i64)&baseline, 0, 0);
                        baseline_valid = 1;
                        warn_mem  = 0;
                        warn_proc = 0;
                        cycle     = 0;
                        running   = 1;
                        print("[STRESS] test started\n");
                    }
                    if (stop_hot && running) {
                        running = 0;
                        /* Kill & wait sr_pid if alive. */
                        if (sr_pid > 1) {
                            safe_kill(sr_pid);
                            sc(SYS_WAITPID, sr_pid, 0, 1);
                            sr_pid = 0;
                        }
                        kill_all_children();
                        print("[STRESS] test stopped\n");
                    }
                    if (int_dn_hot && intensity > 1) {
                        intensity--;
                    }
                    if (int_up_hot && intensity < INTENS_MAX) {
                        intensity++;
                    }
                }
                btn_was_down = btn;
                prev_btn     = btn;
            }
        }
        (void)prev_btn;

        /* Run a stress tick. */
        if (running) stress_tick();

        /* Render. */
        render(win, mx, my, &start_hot, &stop_hot, &int_dn_hot, &int_up_hot);
        wl_commit(win);

        sc(SYS_YIELD, 0, 0, 0);
    }
}

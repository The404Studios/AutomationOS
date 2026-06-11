/*
 * bench.c -- Performance benchmark application (freestanding, ring 3).
 * =====================================================================
 *
 * Opens a 560x420 window titled "Benchmark". Measures and displays:
 *   1. memcpy throughput   (1 MB buffer, N passes, MB/s via rdtsc)
 *   2. memset throughput   (1 MB buffer, N passes, MB/s via rdtsc)
 *   3. Integer compute     (tight integer loop, iterations/sec)
 *   4. Syscall latency     (SYS_GETPID x N, ns/call via rdtsc)
 *   5. Frame rate          (draw+commit+yield loop, fps via ticks_ms)
 *
 * Timing strategy:
 *   - rdtsc (asm volatile("rdtsc")) gives a 64-bit cycle counter;
 *     a calibration pass measures cycles-per-ms using SYS_GET_TICKS_MS
 *     to derive cpu_hz_approx, giving ns from cycles.
 *   - SYS_GET_TICKS_MS (syscall 40) gives ms-granularity wall clock
 *     used for fps and long-duration cross-checks.
 *
 * Build (flags DIRECTLY on command line -- no -fstack-protector → no fs:0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/bench/bench.c -o /tmp/bench.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o /tmp/wlc.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o /tmp/bf.o
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/bench.o /tmp/wlc.o /tmp/bf.o -o /tmp/bench.elf
 *   objdump -d /tmp/bench.elf | grep fs:0x28   # MUST be empty
 *
 * Serial output:
 *   [BENCH] starting
 *   [BENCH] memcpy=XXXX MB/s memset=XXXX MB/s syscall=YY ns fps=ZZ
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"

/* -------------------------------------------------------------------------
 * Syscall numbers and inline helpers.
 * ----------------------------------------------------------------------- */
#define SYS_EXIT         0
#define SYS_WRITE        3
#define SYS_GETPID       8
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

static inline long sc6(long n, long a1, long a2, long a3,
                        long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

static inline long sys_write(const char *s, long len)
{
    return sc6(SYS_WRITE, 1, (long)s, len, 0, 0, 0);
}
static inline long sys_yield(void)   { return sc6(SYS_YIELD, 0, 0, 0, 0, 0, 0); }
static inline long sys_getpid(void)  { return sc6(SYS_GETPID, 0, 0, 0, 0, 0, 0); }
static inline long sys_ticks_ms(void){ return sc6(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0, 0); }

/* -------------------------------------------------------------------------
 * Minimal freestanding helpers (no libc).
 * ----------------------------------------------------------------------- */
typedef unsigned int       u32;
typedef int                i32;
typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned long      ulong;

static ulong k_strlen(const char *s)
{
    ulong n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sys_write(m, (long)k_strlen(m));
}

/* Simple unsigned 64-bit → decimal string. Returns pointer to static buf. */
static char _itoa_buf[32];
static char *u64_to_dec(u64 v)
{
    char *p = _itoa_buf + 31;
    *p = '\0';
    if (v == 0) { *--p = '0'; return p; }
    while (v) { *--p = '0' + (int)(v % 10); v /= 10; }
    return p;
}

/* Concatenate up to 8 strings into a static buffer and print. */
static char _print_buf[256];
static void prints(const char *a, const char *b, const char *c,
                   const char *d, const char *e, const char *f,
                   const char *g, const char *h)
{
    ulong pos = 0;
    const char *parts[8] = {a,b,c,d,e,f,g,h};
    for (int i = 0; i < 8 && parts[i]; i++) {
        const char *s = parts[i];
        while (*s && pos < sizeof(_print_buf) - 1)
            _print_buf[pos++] = *s++;
    }
    _print_buf[pos] = '\0';
    sys_write(_print_buf, (long)pos);
}

/* -------------------------------------------------------------------------
 * rdtsc helper -- returns 64-bit TSC.
 * ----------------------------------------------------------------------- */
static inline u64 rdtsc(void)
{
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* -------------------------------------------------------------------------
 * CPU frequency calibration.
 * Spin for ~20 ms of wall time (using SYS_GET_TICKS_MS) and count TSC ticks
 * to get an approximate cycles-per-ns value (scaled *1024 to avoid floats).
 *
 *   cycles_per_ns_x1024 = (delta_cycles * 1024) / (delta_ms * 1000000)
 *
 * "per_ns_x1024" means the real value is  cycles_per_ns_x1024 / 1024.
 * To convert cycles → ns:  ns = (cycles * 1024) / cycles_per_ns_x1024
 * ----------------------------------------------------------------------- */
static u64 g_cpns_x1024 = 3000; /* sane fallback: ~3 GHz */

static void calibrate_cpu(void)
{
    /* Wait for a clean ms boundary first. */
    long t0 = sys_ticks_ms();
    while (sys_ticks_ms() == t0)
        asm volatile("pause");

    u64 c0 = rdtsc();
    long ts = sys_ticks_ms();

    /* Spin for 20 ms. */
    while (sys_ticks_ms() - ts < 20)
        asm volatile("pause");

    u64 c1  = rdtsc();
    long te = sys_ticks_ms();

    long delta_ms = te - ts;
    if (delta_ms <= 0) delta_ms = 1;

    u64 delta_cyc = c1 - c0;

    /* cycles_per_ns_x1024 = delta_cyc * 1024 / (delta_ms * 1,000,000) */
    /* Avoid overflow: delta_cyc * 1024 can overflow u64 for large delta_cyc.
     * Max delta_cyc for 20ms@5GHz ~ 100M; 100M*1024 = ~100B, fits u64. OK. */
    u64 denom_ns = (u64)delta_ms * 1000000ULL;
    g_cpns_x1024 = (delta_cyc * 1024ULL) / denom_ns;
    if (g_cpns_x1024 == 0) g_cpns_x1024 = 1;
}

/* Convert a cycle delta to nanoseconds. */
static u64 cycles_to_ns(u64 cycles)
{
    return (cycles * 1024ULL) / g_cpns_x1024;
}

/* -------------------------------------------------------------------------
 * Benchmark buffers in .bss (1 MB each, static allocation, zero-cost).
 * ----------------------------------------------------------------------- */
#define BUF_BYTES (1024u * 1024u)          /* 1 MB */
#define BUF_U32S  (BUF_BYTES / 4u)

static u32 bench_src[BUF_U32S];           /* 1 MB source  */
static u32 bench_dst[BUF_U32S];           /* 1 MB dest    */

/* -------------------------------------------------------------------------
 * memcpy / memset microbenchmarks.
 * Use volatile pointers to prevent the optimizer from eliding the work.
 * ----------------------------------------------------------------------- */

/* Returns MB/s (integer) for memcpy over 1 MB repeated PASSES times. */
#define MEMCPY_PASSES 64
static u32 bench_memcpy(void)
{
    volatile u32 *src = bench_src;
    volatile u32 *dst = bench_dst;

    /* Warm cache once before timing. */
    for (u32 i = 0; i < BUF_U32S; i++) dst[i] = src[i];

    u64 c0 = rdtsc();
    for (int p = 0; p < MEMCPY_PASSES; p++) {
        for (u32 i = 0; i < BUF_U32S; i++)
            dst[i] = src[i];
    }
    u64 c1 = rdtsc();

    u64 total_ns  = cycles_to_ns(c1 - c0);
    u64 total_bytes = (u64)BUF_BYTES * MEMCPY_PASSES;

    /* MB/s = bytes / ns = bytes * 1000 / (ns * 1) ... but careful:
     * MB/s = (total_bytes / (1024*1024)) / (total_ns / 1e9)
     *      = total_bytes * 1000 / (total_ns * 1024 * 1024 / 1000)
     * Simpler: MB/s = total_bytes * 1000 / total_ns  (nearly exact) */
    if (total_ns == 0) return 99999;
    /* total_bytes * 1000 / total_ns  ~ MB/s  (since 1 MB = 1048576 B ~= 1e6) */
    return (u32)(total_bytes / (total_ns / 1000 + 1));
}

/* Returns MB/s for memset over 1 MB repeated PASSES times. */
#define MEMSET_PASSES 64
static u32 bench_memset(void)
{
    volatile u32 *dst = bench_dst;
    u32 fill = 0xDEADBEEFu;

    /* Warm. */
    for (u32 i = 0; i < BUF_U32S; i++) dst[i] = fill;

    u64 c0 = rdtsc();
    for (int p = 0; p < MEMSET_PASSES; p++) {
        for (u32 i = 0; i < BUF_U32S; i++)
            dst[i] = fill;
    }
    u64 c1 = rdtsc();

    u64 total_ns    = cycles_to_ns(c1 - c0);
    u64 total_bytes = (u64)BUF_BYTES * MEMSET_PASSES;

    if (total_ns == 0) return 99999;
    return (u32)(total_bytes / (total_ns / 1000 + 1));
}

/* -------------------------------------------------------------------------
 * Integer compute benchmark.
 * A tight loop doing multiplies + xor to defeat constant-folding.
 * Reports millions-of-iterations per second (Miter/s).
 * ----------------------------------------------------------------------- */
#define COMPUTE_ITERS 10000000ULL   /* 10M iterations */
static u32 bench_compute(void)
{
    volatile u64 acc = 0x123456789ABCULL;

    u64 c0 = rdtsc();
    for (u64 i = 0; i < COMPUTE_ITERS; i++) {
        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
        acc ^= (acc >> 33);
    }
    u64 c1 = rdtsc();
    (void)acc;

    u64 total_ns = cycles_to_ns(c1 - c0);
    if (total_ns == 0) return 99999;

    /* Miter/s = (COMPUTE_ITERS / 1e6) / (total_ns / 1e9)
     *         = COMPUTE_ITERS * 1000 / total_ns            */
    return (u32)((COMPUTE_ITERS * 1000ULL) / total_ns);
}

/* -------------------------------------------------------------------------
 * Syscall latency benchmark.
 * Issues SYS_GETPID N times and reports average ns/call.
 * ----------------------------------------------------------------------- */
#define SYSCALL_REPS 10000
static u32 bench_syscall_ns(void)
{
    u64 c0 = rdtsc();
    for (int i = 0; i < SYSCALL_REPS; i++)
        sys_getpid();
    u64 c1 = rdtsc();

    u64 total_ns = cycles_to_ns(c1 - c0);
    return (u32)(total_ns / SYSCALL_REPS);
}

/* -------------------------------------------------------------------------
 * Window / layout constants.
 * ----------------------------------------------------------------------- */
#define WIN_W  560
#define WIN_H  420

/* Colors. */
#define COL_BG        0xFF0A0A1Eu   /* very dark navy background */
#define COL_PANEL     0xFF131328u   /* slightly lighter panel     */
#define COL_BORDER    0xFF3030A0u   /* blue accent border         */
#define COL_HEADER    0xFF1A1A3Cu   /* header bar                 */
#define COL_TITLE     0xFFDDDDFFu   /* title text                 */
#define COL_LABEL     0xFF9999CCu   /* label text                 */
#define COL_VALUE     0xFF00FFAAu   /* benchmark value (green)    */
#define COL_BAR_BG    0xFF1A1A30u   /* bar background             */
#define COL_BAR_COPY  0xFF00C0FFu   /* memcpy bar fill            */
#define COL_BAR_SET   0xFF00FF80u   /* memset bar fill            */
#define COL_BAR_CPT   0xFFFFCC00u   /* compute bar fill           */
#define COL_BAR_SC    0xFFFF6060u   /* syscall bar fill (inverted)*/
#define COL_BAR_FPS   0xFFCC80FFu   /* fps bar fill               */
#define COL_SEP       0xFF303060u   /* separator line             */
#define COL_STATUS    0xFF606090u   /* status line                */

/* -------------------------------------------------------------------------
 * Drawing primitives.
 *
 * The fixed WIN_W x WIN_H layout is letterboxed inside whatever surface the
 * compositor gives us. g_clip_w / g_clip_h hold the CURRENT effective clip
 * bounds = min(layout extent, live win->{w,h}); every primitive clips to them
 * so a SMALLER (resized) window can never write past the (re)mapped buffer,
 * while a LARGER window simply leaves the extra margin painted with COL_BG.
 * They are refreshed from win->{w,h} once per frame in _start before drawing.
 * ----------------------------------------------------------------------- */
static i32 g_clip_w = WIN_W;
static i32 g_clip_h = WIN_H;

static void fill_rect(u32 *buf, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > g_clip_w) x2 = g_clip_w;
    i32 y2 = y + h; if (y2 > g_clip_h) y2 = g_clip_h;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++)
            row[xx] = color;
    }
}

static void hline(u32 *buf, u32 stride_px,
                  i32 x, i32 y, i32 len, u32 color)
{
    fill_rect(buf, stride_px, x, y, len, 1, color);
}

static void draw_rect_border(u32 *buf, u32 stride_px,
                             i32 x, i32 y, i32 w, i32 h, u32 color)
{
    hline(buf, stride_px, x, y,         w, color);
    hline(buf, stride_px, x, y + h - 1, w, color);
    fill_rect(buf, stride_px, x,         y, 1, h, color);
    fill_rect(buf, stride_px, x + w - 1, y, 1, h, color);
}

static int draw_str(u32 *buf, u32 stride_px, i32 x, i32 y,
                    const char *s, u32 color)
{
    /* Clip glyphs to the live surface (g_clip_*), not the fixed layout, so
     * text never writes past a shrunken/reallocated buffer. */
    return font_draw_string((unsigned int *)buf, (int)stride_px,
                            g_clip_w, g_clip_h, x, y, s, color);
}

/* -------------------------------------------------------------------------
 * Animated bar.
 * Draws a labeled bar from 0..bar_max with current value cur_val.
 * Returns next y position.
 * ----------------------------------------------------------------------- */
#define BAR_X      140
#define BAR_W      370
#define BAR_H      14
#define BAR_ROW_H  28   /* total row height including label + spacing */

static i32 draw_bar_row(u32 *buf, u32 stride_px,
                        i32 y,
                        const char *label,
                        const char *val_str,
                        const char *unit_str,
                        u32 cur_val, u32 bar_max,
                        u32 bar_color)
{
    /* Label */
    draw_str(buf, stride_px, 6, y + 6, label, COL_LABEL);

    /* Value + unit */
    i32 vx = BAR_X + BAR_W + 6;
    vx += draw_str(buf, stride_px, vx, y + 6, val_str,  COL_VALUE);
    draw_str(buf, stride_px, vx + 2, y + 6, unit_str, COL_LABEL);

    /* Bar background */
    fill_rect(buf, stride_px, BAR_X, y + 2, BAR_W, BAR_H, COL_BAR_BG);
    draw_rect_border(buf, stride_px, BAR_X - 1, y + 1, BAR_W + 2, BAR_H + 2,
                     COL_SEP);

    /* Bar fill */
    if (bar_max > 0) {
        u32 fill_w = (u32)BAR_W * cur_val / bar_max;
        if (fill_w > (u32)BAR_W) fill_w = (u32)BAR_W;
        if (fill_w > 0)
            fill_rect(buf, stride_px, BAR_X, y + 2, (i32)fill_w, BAR_H,
                      bar_color);
    }

    return y + BAR_ROW_H;
}

/* -------------------------------------------------------------------------
 * Full frame render.
 * ----------------------------------------------------------------------- */
typedef struct {
    u32 memcpy_mbs;    /* MB/s    */
    u32 memset_mbs;    /* MB/s    */
    u32 compute_mits;  /* Miter/s */
    u32 syscall_ns;    /* ns/call */
    u32 fps;           /* frames/s */
    u32 run_count;     /* which benchmark run # */
} bench_results_t;

/* Separator line y values; updated each frame. */
static void render_frame(u32 *buf, u32 stride_px,
                         const bench_results_t *r,
                         u32 tick_phase)    /* 0..63 animation phase */
{
    /* Background */
    fill_rect(buf, stride_px, 0, 0, WIN_W, WIN_H, COL_BG);

    /* Header bar */
    fill_rect(buf, stride_px, 0, 0, WIN_W, 28, COL_HEADER);
    hline(buf, stride_px, 0, 27, WIN_W, COL_BORDER);
    draw_str(buf, stride_px, 8, 6,  "PERFORMANCE BENCHMARK", COL_TITLE);

    /* Run counter top-right */
    {
        char rbuf[32];
        char *p = rbuf;
        const char *pf = "Run #";
        while (*pf) *p++ = *pf++;
        char *n = u64_to_dec((u64)r->run_count);
        while (*n) *p++ = *n++;
        *p = '\0';
        draw_str(buf, stride_px, WIN_W - 8 * (i32)k_strlen(rbuf) - 6, 6,
                 rbuf, COL_STATUS);
    }

    /* Column headers */
    i32 hdr_y = 34;
    fill_rect(buf, stride_px, 0, hdr_y, WIN_W, 16, COL_PANEL);
    draw_str(buf, stride_px,   6, hdr_y + 1, "Benchmark",  COL_STATUS);
    draw_str(buf, stride_px, BAR_X, hdr_y + 1, "Performance bar",  COL_STATUS);
    draw_str(buf, stride_px, BAR_X + BAR_W + 6, hdr_y + 1, "Result", COL_STATUS);
    hline(buf, stride_px, 0, hdr_y + 15, WIN_W, COL_SEP);

    i32 y = 54;

    /* ---- memcpy ---- */
    {
        /* Animate: pulse bar max slightly around the result for a "beating"
         * look -- max cycles in [mbs*0.8 .. mbs*1.2] based on tick_phase. */
        u32 bar_max = r->memcpy_mbs > 0 ? (r->memcpy_mbs * 5u / 4u + 1u) : 1000u;
        /* Animated fill: blend from 0 up to result over first 32 ticks. */
        u32 anim_val = r->memcpy_mbs;
        if (tick_phase < 32) anim_val = r->memcpy_mbs * tick_phase / 32;
        y = draw_bar_row(buf, stride_px, y,
                         "memcpy",
                         u64_to_dec((u64)r->memcpy_mbs), " MB/s",
                         anim_val, bar_max, COL_BAR_COPY);
    }

    hline(buf, stride_px, 4, y - 2, WIN_W - 8, COL_SEP);

    /* ---- memset ---- */
    {
        u32 bar_max = r->memset_mbs > 0 ? (r->memset_mbs * 5u / 4u + 1u) : 1000u;
        u32 anim_val = r->memset_mbs;
        if (tick_phase < 32) anim_val = r->memset_mbs * tick_phase / 32;
        y = draw_bar_row(buf, stride_px, y,
                         "memset",
                         u64_to_dec((u64)r->memset_mbs), " MB/s",
                         anim_val, bar_max, COL_BAR_SET);
    }

    hline(buf, stride_px, 4, y - 2, WIN_W - 8, COL_SEP);

    /* ---- compute ---- */
    {
        u32 bar_max = r->compute_mits > 0 ? (r->compute_mits * 5u / 4u + 1u) : 200u;
        u32 anim_val = r->compute_mits;
        if (tick_phase < 32) anim_val = r->compute_mits * tick_phase / 32;
        y = draw_bar_row(buf, stride_px, y,
                         "compute",
                         u64_to_dec((u64)r->compute_mits), " Miter/s",
                         anim_val, bar_max, COL_BAR_CPT);
    }

    hline(buf, stride_px, 4, y - 2, WIN_W - 8, COL_SEP);

    /* ---- syscall latency ---- */
    /* Bar is inverted: lower ns is better, show "headroom" visually by
     * filling bar proportional to how LOW the latency is vs. a 5000 ns ref. */
    {
        u32 ref_ns  = 5000u;   /* 5 us reference ceiling */
        u32 bar_val = (r->syscall_ns < ref_ns) ? (ref_ns - r->syscall_ns) : 0u;
        u32 anim_val = bar_val;
        if (tick_phase < 32) anim_val = bar_val * tick_phase / 32;
        y = draw_bar_row(buf, stride_px, y,
                         "syscall lat",
                         u64_to_dec((u64)r->syscall_ns), " ns/call",
                         anim_val, ref_ns, COL_BAR_SC);
    }

    hline(buf, stride_px, 4, y - 2, WIN_W - 8, COL_SEP);

    /* ---- fps ---- */
    {
        u32 bar_max = 120u;   /* cap at 120 fps for bar scaling */
        u32 anim_val = r->fps;
        if (tick_phase < 32) anim_val = r->fps * tick_phase / 32;
        if (anim_val > bar_max) anim_val = bar_max;
        y = draw_bar_row(buf, stride_px, y,
                         "frame rate",
                         u64_to_dec((u64)r->fps), " fps",
                         anim_val, bar_max, COL_BAR_FPS);
    }

    /* ---- CPU calibration info ---- */
    hline(buf, stride_px, 0, y, WIN_W, COL_SEP);
    y += 4;
    {
        /* cpu_mhz ~ cpns_x1024 * 1000 / 1024 */
        u64 cpu_mhz = (g_cpns_x1024 * 1000ULL) / 1024ULL;
        draw_str(buf, stride_px, 6, y,
                 "CPU (calibrated):", COL_STATUS);
        char *mhz = u64_to_dec(cpu_mhz);
        draw_str(buf, stride_px, 6 + 18 * FONT_W, y, mhz, COL_VALUE);
        draw_str(buf, stride_px, 6 + 18 * FONT_W + (i32)k_strlen(mhz) * FONT_W + 2,
                 y, " MHz", COL_LABEL);
    }
    y += FONT_H + 4;

    /* ---- IPC note ---- */
    draw_str(buf, stride_px, 6, y,
             "IPC round-trip: N/A (requires two processes)", COL_STATUS);
    y += FONT_H + 4;

    /* ---- Status / next run countdown ---- */
    hline(buf, stride_px, 0, WIN_H - 18, WIN_W, COL_SEP);
    {
        /* Simple spinning indicator */
        const char *spinch = "|/-\\";
        char spin_s[2] = { spinch[tick_phase & 3], '\0' };
        draw_str(buf, stride_px, 6, WIN_H - 14, spin_s, COL_VALUE);
        draw_str(buf, stride_px, 20, WIN_H - 14,
                 "Benchmarking... results auto-refresh every ~5s",
                 COL_STATUS);
    }
}

/* -------------------------------------------------------------------------
 * Entry point.
 * ----------------------------------------------------------------------- */
void _start(void)
{
    print("[BENCH] starting\n");

    /* Connect to compositor. */
    if (wl_connect() != 0) {
        print("[BENCH] wl_connect FAILED\n");
        for (;;) sys_yield();
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Benchmark");
    if (!win) {
        print("[BENCH] wl_create_window FAILED\n");
        for (;;) sys_yield();
    }

    print("[BENCH] window created, calibrating...\n");

    /* Calibrate TSC against the ms timer. */
    calibrate_cpu();

    print("[BENCH] calibration done\n");

    u32 stride_px = win->stride / 4u;

    /* The compositor may resize/maximize the window. We keep the fixed
     * WIN_W x WIN_H benchmark layout (letterbox) and simply repaint the FULL
     * new surface to COL_BG each frame so resized margins are never garbage.
     * `need_clear` forces a full-surface clear on the next frame after a
     * resize. The drawing primitives clip to min(WIN_*, win->*) so a SMALLER
     * window can never write past the (possibly reallocated) buffer. */
    int need_clear = 1;

    /* Benchmark results (start zeroed). */
    bench_results_t results = {0, 0, 0, 0, 0, 0};

    /* Animation / frame state. */
    u32  tick_phase  = 0;     /* 0..63 animation counter, wraps */
    long frame_start = sys_ticks_ms();
    long last_bench  = frame_start - 6000; /* force immediate first run */
    long fps_window  = frame_start;
    u32  fps_frames  = 0;

    for (;;) {
        /* Drain compositor events (pointer/key). */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_RESIZE) {
                /* Library already reallocated the buffer and updated
                 * win->{w,h,stride,pixels}. Refresh our cached stride and
                 * force a full-surface clear so the new margins are painted. */
                stride_px  = win->stride / 4u;
                need_clear = 1;
            }
            /* No other interactive input needed for a benchmark; discard. */
            (void)ea; (void)eb; (void)ec;
        }

        /* Run benchmarks every ~5 seconds. */
        long now = sys_ticks_ms();
        if (now - last_bench >= 5000) {
            last_bench = now;
            results.run_count++;
            tick_phase = 0; /* reset animation so bars sweep in */

            results.memcpy_mbs   = bench_memcpy();
            results.memset_mbs   = bench_memset();
            results.compute_mits = bench_compute();
            results.syscall_ns   = bench_syscall_ns();
            /* fps will be updated from the frame counter below */

            /* Serial report. */
            prints("[BENCH] memcpy=",  u64_to_dec(results.memcpy_mbs),  " MB/s"
                   " memset=",         u64_to_dec(results.memset_mbs),   " MB/s"
                   " compute=",        u64_to_dec(results.compute_mits), 0, 0);
            prints(" Miter/s syscall=", u64_to_dec(results.syscall_ns),
                   " ns/call fps=",    u64_to_dec(results.fps), "\n",
                   0, 0, 0);
        }

        /* FPS measurement: count frames in 1-second buckets. */
        fps_frames++;
        if (now - fps_window >= 1000) {
            results.fps = fps_frames;
            fps_frames  = 0;
            fps_window  = now;
        }

        /* Refresh clip bounds from the live window each frame: the fixed
         * WIN_W x WIN_H layout is clamped to whatever surface we currently
         * have so no primitive can write past win->{w,h} via stride_px. */
        g_clip_w = (i32)win->w < WIN_W ? (i32)win->w : WIN_W;
        g_clip_h = (i32)win->h < WIN_H ? (i32)win->h : WIN_H;

        /* On (re)size, the new margins around the fixed layout are stale
         * garbage; clear the ENTIRE surface to COL_BG once, bounded to the
         * live win->{w,h} using the live stride. */
        if (need_clear) {
            for (u32 yy = 0; yy < win->h; yy++) {
                u32 *row = win->pixels + (u64)yy * stride_px;
                for (u32 xx = 0; xx < win->w; xx++)
                    row[xx] = COL_BG;
            }
            need_clear = 0;
        }

        /* Render. */
        render_frame(win->pixels, stride_px, &results, tick_phase & 63u);

        /* Advance animation. */
        tick_phase++;

        /* Commit and yield. */
        wl_commit(win);
        sys_yield();
    }
}

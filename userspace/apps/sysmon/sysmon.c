/*
 * userspace/apps/sysmon/sysmon.c -- System Monitor application.
 * ==============================================================
 *
 * A retained-mode GUI window ("System Monitor", 420x360) built on the
 * M4 UI toolkit.  Displayed live fields:
 *
 *   - Uptime   : HH:MM:SS derived from SYS_GET_TICKS_MS (syscall 40).
 *   - PID      : own process-id via SYS_GETPID (syscall 8).
 *   - Proc list: up to MAX_PROCS rows via SYS_PROCLIST (syscall 42).
 *                If the syscall returns < 0 (not wired yet), a single
 *                "(process info unavailable)" placeholder is shown instead.
 *
 * -----------------------------------------------------------------------
 * PROPOSED SYS_PROCLIST INTERFACE (for the kernel integrator to wire)
 * -----------------------------------------------------------------------
 *  #define SYS_PROCLIST 42
 *
 *  struct procinfo {
 *      int           pid;       // process id
 *      int           ppid;      // parent pid
 *      int           state;     // 0=running,1=ready,2=blocked,3=zombie
 *      unsigned long cpu_ms;    // cumulative CPU time in milliseconds
 *      char          name[32];  // null-terminated process name
 *  };
 *
 *  Kernel handler signature (C side):
 *    long sys_proclist(struct procinfo *buf, int max);
 *    // Walks the kernel process table, fills up to `max` entries into the
 *    // user-provided buf[], returns the number of live processes written,
 *    // or a negative errno on failure.
 *
 *  x86-64 calling convention for the raw syscall:
 *    rax = 42  (SYS_PROCLIST)
 *    rdi = buf (user pointer to struct procinfo array)
 *    rsi = max (maximum entries to fill)
 *    return value in rax = count (>= 0) or -errno
 * -----------------------------------------------------------------------
 *
 * Build (all flags passed DIRECTLY on the command line – no shell vars):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace \
 *       -c userspace/apps/sysmon/sysmon.c      -o /tmp/sm.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace \
 *       -c userspace/lib/ui/ui.c               -o /tmp/ui.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace \
 *       -c userspace/lib/wl/wl_client.c        -o /tmp/wlc.o
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace \
 *       -c userspace/lib/font/bitfont.c        -o /tmp/bf.o
 *
 *   ld -nostdlib -static -n -no-pie -e _start \
 *       -T userspace/userspace.ld \
 *       /tmp/sm.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o \
 *       -o /tmp/sm.elf
 *
 *   # Verify no stack-canary reference (must produce no output):
 *   objdump -d /tmp/sm.elf | grep fs:0x28
 */

#include "../../lib/ui/ui.h"  /* ui_app_t, ui_widget_t, ui_app_create … */

/* ------------------------------------------------------------------ */
/* Syscall numbers                                                      */
/* ------------------------------------------------------------------ */
#define SYS_WRITE        3
#define SYS_GETPID       8
#define SYS_GET_TICKS_MS 40
#define SYS_PROCLIST     44   /* canonical SYS_PROCLIST (was wrongly 42 = SYS_GETTIME) */
#define SYS_MEMINFO      43   /* proposed – omitted gracefully if < 0 */

/* ------------------------------------------------------------------ */
/* procinfo struct -- byte-for-byte the kernel SYS_PROCLIST ABI         */
/* (kernel/include/sched.h proc_info_t), exactly 64 bytes:              */
/*   0  pid, 4 parent_pid, 8 state, 12 flags, 16 name[32],              */
/*   48 cpu_ticks (u64), 56 ctx_switches (u64).                         */
/* NOTE: previously this struct interleaved a `cpu_ms` field that did   */
/* not match the kernel layout (name was read from the wrong offset).   */
/* It now mirrors the kernel exactly, so name renders correctly and     */
/* cpu_ticks/ctx_switches are real per-process scheduler stats.         */
/* ------------------------------------------------------------------ */
struct procinfo {
    unsigned int       pid;          /*  0 */
    unsigned int       parent_pid;   /*  4 */
    unsigned int       state;        /*  8  0=created 1=ready 2=running 3=blocked 4=terminated */
    unsigned int       flags;        /* 12 */
    char               name[32];     /* 16 */
    unsigned long long cpu_ticks;    /* 48  timer ticks observed while running */
    unsigned long long ctx_switches; /* 56  number of times dispatched */
};

/* ------------------------------------------------------------------ */
/* Proposed meminfo struct                                              */
/* ------------------------------------------------------------------ */
struct meminfo {
    unsigned long total_kb;
    unsigned long free_kb;
    unsigned long used_kb;
};

/* ------------------------------------------------------------------ */
/* 3-argument inline syscall (rdi/rsi/rdx)                             */
/* ------------------------------------------------------------------ */
static inline long sc(long n, long a1, long a2, long a3) {
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* ------------------------------------------------------------------ */
/* Serial/debug output helpers (no libc)                               */
/* ------------------------------------------------------------------ */
static unsigned long sm_strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void sm_write(const char *s) {
    sc(SYS_WRITE, 1, (long)s, (long)sm_strlen(s));
}

/* ------------------------------------------------------------------ */
/* Integer-to-string helpers (no libc, no printf)                      */
/* ------------------------------------------------------------------ */

/* Write a decimal number into buf (NUL-terminated). Returns pointer to buf. */
static char *sm_utoa(unsigned long v, char *buf, int width, char pad) {
    char tmp[22];
    int  i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { unsigned long x = v; while (x) { tmp[i++] = (char)('0' + x % 10); x /= 10; } }
    /* tmp is reversed digits */
    int total = i;
    int out   = 0;
    /* left-pad */
    while (total < width) { buf[out++] = pad; total++; }
    /* reverse into buf */
    for (int j = i - 1; j >= 0; j--) buf[out++] = tmp[j];
    buf[out] = '\0';
    return buf;
}

/* Append src to dst, return pointer to NUL at new end of dst. */
static char *sm_append(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
    return dst;
}

/* ------------------------------------------------------------------ */
/* State names                                                          */
/* ------------------------------------------------------------------ */
static const char *state_name(int s) {
    switch (s) {
        case 0: return "run  ";
        case 1: return "ready";
        case 2: return "blk  ";
        case 3: return "zombi";
        default: return "?    ";
    }
}

/* ------------------------------------------------------------------ */
/* Application state                                                    */
/* ------------------------------------------------------------------ */
#define MAX_PROCS  12   /* rows we can show in the window */
#define WIN_W      420
#define WIN_H      360

/* Row labels in the process table (one per visible row + header + placeholders) */
#define MAX_LABEL_ROWS  (MAX_PROCS + 4)

typedef struct {
    /* Uptime / header widgets */
    ui_widget_t *lbl_uptime;
    ui_widget_t *lbl_pid;
    ui_widget_t *lbl_mem;

    /* Process table header */
    ui_widget_t *lbl_proc_hdr;

    /* Per-process row labels */
    ui_widget_t *lbl_rows[MAX_LABEL_ROWS];
    int          n_row_labels;  /* how many lbl_rows[] we allocated */

    /* Process info buffer */
    struct procinfo procs[MAX_PROCS];

    /* Memory info */
    struct meminfo  mem;
    int             mem_ok;      /* 1 if SYS_MEMINFO is wired */
    int             proclist_ok; /* 1 if SYS_PROCLIST is wired */
    int             proc_count;  /* last result from SYS_PROCLIST */
} sysmon_state_t;

static sysmon_state_t g_state;

/* ------------------------------------------------------------------ */
/* String build helpers                                                 */
/* ------------------------------------------------------------------ */

/* Format "Uptime: HH:MM:SS" into buf (>=32 bytes). */
static void fmt_uptime(char *buf, unsigned long ms) {
    unsigned long secs  = ms / 1000UL;
    unsigned long mins  = secs / 60UL;
    unsigned long hours = mins / 60UL;
    secs %= 60UL;
    mins %= 60UL;

    char nb[12];
    char *p = buf;
    p = sm_append(p, "Uptime: ");
    p = sm_append(p, sm_utoa(hours, nb, 2, '0')); *p++ = ':';
    p = sm_append(p, sm_utoa(mins,  nb, 2, '0')); *p++ = ':';
    p = sm_append(p, sm_utoa(secs,  nb, 2, '0'));
    *p = '\0';
}

/* Format one process row: "PID  NAME             STATE  CPU_MS" */
static void fmt_proc_row(char *buf, const struct procinfo *pi) {
    char nb[22];
    char *p = buf;
    /* PID (5 chars) */
    p = sm_append(p, sm_utoa((unsigned long)pi->pid, nb, 5, ' '));
    *p++ = ' ';
    /* Name (16 chars, left-aligned, space-padded) */
    int nlen = 0;
    while (pi->name[nlen] && nlen < 16) { *p++ = pi->name[nlen++]; }
    while (nlen++ < 16) *p++ = ' ';
    *p++ = ' ';
    /* State (5 chars) */
    p = sm_append(p, state_name(pi->state));
    *p++ = ' ';
    /* cpu_ticks (right-aligned 8 chars) -- real per-process CPU time */
    p = sm_append(p, sm_utoa(pi->cpu_ticks, nb, 8, ' '));
    *p++ = ' ';
    /* ctx_switches (right-aligned 7 chars) -- dispatch count */
    p = sm_append(p, sm_utoa(pi->ctx_switches, nb, 7, ' '));
    *p = '\0';
}

/* Format memory line: "Mem: used/total KB" */
static void fmt_mem(char *buf, const struct meminfo *mi) {
    char nb[22];
    char *p = buf;
    p = sm_append(p, "Mem: ");
    p = sm_append(p, sm_utoa(mi->used_kb,  nb, 0, ' '));
    p = sm_append(p, " / ");
    p = sm_append(p, sm_utoa(mi->total_kb, nb, 0, ' '));
    p = sm_append(p, " KB");
    *p = '\0';
}

/* ------------------------------------------------------------------ */
/* Per-frame tick callback                                              */
/* ------------------------------------------------------------------ */
static void sysmon_tick(void *ud) {
    sysmon_state_t *st = (sysmon_state_t *)ud;
    char buf[128];

    /* --- Uptime --- */
    long ticks = sc(SYS_GET_TICKS_MS, 0, 0, 0);
    if (ticks < 0) ticks = 0;
    fmt_uptime(buf, (unsigned long)ticks);
    ui_label_set_text(st->lbl_uptime, buf);

    /* --- Memory (if wired) --- */
    if (st->mem_ok) {
        long r = sc(SYS_MEMINFO, (long)&st->mem, (long)sizeof(st->mem), 0);
        if (r >= 0) {
            fmt_mem(buf, &st->mem);
            ui_label_set_text(st->lbl_mem, buf);
        }
    }

    /* --- Process list --- */
    long cnt = sc(SYS_PROCLIST, (long)st->procs,
                  (long)MAX_PROCS, 0);

    if (cnt < 0) {
        /* Syscall not wired yet – show placeholder in row 0 */
        if (st->n_row_labels > 0)
            ui_label_set_text(st->lbl_rows[0], "(process info unavailable)");
        for (int i = 1; i < st->n_row_labels; i++)
            ui_label_set_text(st->lbl_rows[i], "");
        return;
    }

    st->proc_count = (int)cnt;
    if (st->proc_count > MAX_PROCS) st->proc_count = MAX_PROCS;

    for (int i = 0; i < st->n_row_labels; i++) {
        if (i < st->proc_count) {
            fmt_proc_row(buf, &st->procs[i]);
            ui_label_set_text(st->lbl_rows[i], buf);
        } else {
            ui_label_set_text(st->lbl_rows[i], "");
        }
    }
}

/* ------------------------------------------------------------------ */
/* _start                                                               */
/* ------------------------------------------------------------------ */
void _start(void) {
    /* Serial boot message */
    sm_write("[SYSMON] starting\n");

    /* Own PID for display */
    long own_pid = sc(SYS_GETPID, 0, 0, 0);

    /* Probe SYS_MEMINFO availability (pass null buf, 0 size – should
       return 0 or -EINVAL; anything != -ENOSYS means it is wired). */
    long mem_probe = sc(SYS_MEMINFO, 0, 0, 0);
    g_state.mem_ok = (mem_probe != -38 && mem_probe != -1); /* -ENOSYS = -38 */

    /* ---- Build the window ---- */
    ui_app_t *app = ui_app_create("System Monitor", WIN_W, WIN_H);
    if (!app) {
        sm_write("[SYSMON] ui_app_create failed\n");
        sc(SYS_WRITE, 2, (long)"[SYSMON] ui_app_create failed\n", 30);
        /* exit(1) – syscall 60 */
        asm volatile("mov $60,%rax; mov $1,%rdi; syscall");
        __builtin_unreachable();
    }

    ui_widget_t *root = ui_app_root(app);

    /* ---- Title bar panel ---- */
    ui_widget_t *title_pnl = ui_panel(root, 0, 0, WIN_W, 32, 0xFF1A1A2E);
    ui_label(title_pnl, 10, 8, "System Monitor", 0xFF00D4FF);

    /* ---- Info panel (uptime, pid, mem) ---- */
    ui_widget_t *info_pnl = ui_panel(root, 8, 38, WIN_W - 16, 60, 0xFF16213E);

    /* uptime – will be updated each tick */
    g_state.lbl_uptime = ui_label(info_pnl, 10, 6, "Uptime: --:--:--", 0xFFE0E0E0);

    /* own PID (static) */
    {
        char pidbuf[32];
        char nb[16];
        char *p = pidbuf;
        p = sm_append(p, "PID: ");
        p = sm_append(p, sm_utoa((unsigned long)own_pid, nb, 0, ' '));
        *p = '\0';
        g_state.lbl_pid = ui_label(info_pnl, 10, 24, pidbuf, 0xFFB0B0B0);
    }

    /* memory line */
    if (g_state.mem_ok) {
        g_state.lbl_mem = ui_label(info_pnl, 10, 42, "Mem: ...", 0xFFB0B0B0);
    } else {
        g_state.lbl_mem = ui_label(info_pnl, 10, 42, "Mem: (unavailable)", 0xFF707070);
    }

    /* ---- Process list panel ---- */
    ui_widget_t *proc_pnl = ui_panel(root, 8, 104, WIN_W - 16, WIN_H - 112, 0xFF0D1117);

    /* Column header */
    g_state.lbl_proc_hdr = ui_label(proc_pnl, 6, 4,
        "  PID Name             State cpu_ms  ",
        0xFF58A6FF);

    /* Separator line drawn by a thin panel */
    ui_panel(proc_pnl, 6, 20, WIN_W - 28, 1, 0xFF30363D);

    /* Row labels – one per visible row */
    int row_y_start = 26;
    int row_h       = 18;  /* 8x16 font + 2px gap */

    for (int i = 0; i < MAX_PROCS; i++) {
        int y = row_y_start + i * row_h;
        /* Alternate row shading */
        unsigned int row_bg = (i & 1) ? 0xFF0D1117 : 0xFF161B22;
        ui_panel(proc_pnl, 4, y - 2, WIN_W - 36, row_h, row_bg);
        g_state.lbl_rows[i] = ui_label(proc_pnl, 6, y,
            "(process info unavailable)",
            0xFFCDD5DF);
    }
    g_state.n_row_labels = MAX_PROCS;

    /* ---- Register tick ---- */
    ui_app_set_tick(app, sysmon_tick, &g_state);

    sm_write("[SYSMON] window ready, entering event loop\n");

    /* ---- Run (never returns) ---- */
    ui_app_run(app);

    __builtin_unreachable();
}

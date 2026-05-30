/*
 * taskmanager/taskmanager.c  —  AutomationOS Task Manager (windowed, ring 3)
 * ===========================================================================
 *
 * Windows-11-style task manager built on the M4 UI toolkit + wl_client.
 * Live process list every ~1 s, CPU%, memory, End-Task action.
 *
 * Syscalls used:
 *   SYS_PROCLIST   = 44  — fill proc_info_t[max], returns count
 *   SYS_PROC_QUERY = 60  — per-pid proc_detail_t (cpu_ticks, mem_pages)
 *   SYS_SYSINFO    = 62  — sysinfo_t (total/free mem, uptime_ms, proc_count)
 *   SYS_PROC_CTL   = 61  — PROCAPI_CTL_KILL (verb 2) to end a task
 *   SYS_GET_TICKS_MS=40  — fallback uptime
 *   SYS_GETPID     =  8  — own pid
 *
 * Build (freestanding, no libc, NO fs:0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *       -I userspace
 *       -c userspace/apps/taskmanager/taskmanager.c -o /tmp/tm.o
 *   objdump -d /tmp/tm.o | grep 'fs:0x28'   # must be empty
 */

#include "../../lib/ui/ui.h"

/* =========================================================================
 * Syscall numbers (AOS — NOT Linux)
 * ========================================================================= */
#define SYS_EXIT         0
#define SYS_WRITE        3
#define SYS_GETPID       8
#define SYS_GET_TICKS_MS 40
#define SYS_PROCLIST     44
#define SYS_PROC_QUERY   60
#define SYS_PROC_CTL     61
#define SYS_SYSINFO      62

/* =========================================================================
 * Kernel ABI structs (mirror kernel/include/procapi.h & sched.h)
 * ========================================================================= */

/* SYS_PROCLIST fills proc_info_t[], 64 bytes each */
typedef struct {
    unsigned int       pid;
    unsigned int       parent_pid;
    unsigned int       state;        /* PROCESS_* enum */
    unsigned int       flags;        /* reserved */
    char               name[32];
    unsigned long long cpu_ticks;    /* per-process CPU ticks */
    unsigned long long ctx_switches; /* per-process dispatch count */
} proc_info_t;

/* SYS_PROC_QUERY fills proc_detail_t, 64 bytes */
typedef struct {
    unsigned int  pid;
    unsigned int  ppid;
    unsigned int  state;
    unsigned int  prio;
    unsigned long cpu_ticks;   /* total_time from PCB */
    unsigned int  mem_pages;   /* 4 KiB pages in VMA list */
    unsigned int  vma_count;
    char          name[32];
} proc_detail_t;

/* SYS_SYSINFO fills sysinfo_t, 32 bytes */
typedef struct {
    unsigned long total_mem;   /* bytes */
    unsigned long free_mem;    /* bytes */
    unsigned long uptime_ms;
    unsigned int  proc_count;
    unsigned int  _pad;
} sysinfo_t;

/* procapi_ctl verb */
#define PROCAPI_CTL_KILL 2

/* =========================================================================
 * 6-argument inline syscall — no fs:0x28 stack canary
 * ========================================================================= */
static inline long sc6(long n, long a1, long a2, long a3,
                        long a4, long a5, long a6)
{
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
#define sc(n,a1,a2,a3) sc6((n),(a1),(a2),(a3),0,0,0)

/* =========================================================================
 * Tiny string helpers (no libc)
 * ========================================================================= */
static unsigned long tm_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}
static void tm_write(const char *s) {
    sc(SYS_WRITE, 1, (long)s, (long)tm_strlen(s));
}
static void tm_memset(void *p, int c, unsigned long n) {
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = (unsigned char)c;
}

/* Append src to dst, return pointer to new NUL. */
static char *tm_app(char *d, const char *s) {
    while (*s) *d++ = *s++;
    *d = '\0';
    return d;
}

/* Decimal unsigned long → fixed-width field in buf. */
static char *tm_utoa(unsigned long v, char *buf, int width, char pad) {
    char tmp[22]; int i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { unsigned long x = v; while (x) { tmp[i++] = (char)('0' + x % 10); x /= 10; } }
    int out = 0, total = i;
    while (total < width) { buf[out++] = pad; total++; }
    for (int j = i - 1; j >= 0; j--) buf[out++] = tmp[j];
    buf[out] = '\0';
    return buf;
}

/* Simple strcmp */
static int tm_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* =========================================================================
 * Window / UI geometry
 * ========================================================================= */
#define WIN_W      720
#define WIN_H      500

#define MAX_PROCS  64

/* Visible rows in the scrollable list */
#define ROW_H      18
#define LIST_Y     90     /* top of process list area */
#define LIST_H     (WIN_H - LIST_Y - 60)
#define VISIBLE    (LIST_H / ROW_H)   /* ~19 rows */

/* =========================================================================
 * Application state
 * ========================================================================= */
typedef struct {
    proc_info_t   info[MAX_PROCS];
    proc_detail_t detail[MAX_PROCS];
    int           count;
    /* CPU% calculation: diff of cpu_ticks between consecutive polls */
    unsigned long prev_ticks[MAX_PROCS];
    unsigned int  prev_pid[MAX_PROCS];
    unsigned long total_delta;    /* sum of all deltas this cycle */

    sysinfo_t     sysinfo;
    int           sysinfo_ok;

    /* UI widgets — column headers */
    ui_widget_t  *hdr_pid;
    ui_widget_t  *hdr_name;
    ui_widget_t  *hdr_state;
    ui_widget_t  *hdr_cpu;
    ui_widget_t  *hdr_mem;

    /* Per-row labels (fixed pool, VISIBLE rows) */
    ui_widget_t  *row_bg[VISIBLE];
    ui_widget_t  *row_pid[VISIBLE];
    ui_widget_t  *row_name[VISIBLE];
    ui_widget_t  *row_state[VISIBLE];
    ui_widget_t  *row_cpu[VISIBLE];
    ui_widget_t  *row_mem[VISIBLE];

    /* Footer labels */
    ui_widget_t  *ftr_procs;
    ui_widget_t  *ftr_mem;
    ui_widget_t  *ftr_uptime;

    /* Selection + scroll */
    int           selected;   /* absolute index */
    int           scroll;     /* first visible row */

    /* End-task button and status label */
    ui_widget_t  *btn_endtask;
    ui_widget_t  *lbl_status;

    /* Tick counter to throttle refresh to ~1 s */
    int           tick_skip;

    /* Previous refresh time (ms) for CPU delta */
    unsigned long prev_ms;
} tm_state_t;

static tm_state_t g_tm;

/* =========================================================================
 * Helpers
 * ========================================================================= */
static const char *state_str(unsigned int s) {
    switch (s) {
        case 0: return "New  ";
        case 1: return "Ready";
        case 2: return "Run  ";
        case 3: return "Blkd ";
        case 4: return "Dead ";
        default: return "?    ";
    }
}

/* Format bytes as "NNNN KB" or "NN.N MB". */
static char *fmt_mem(unsigned long bytes, char *buf) {
    char nb[22];
    char *p = buf;
    if (bytes < 1024UL * 1024UL) {
        /* KB */
        unsigned long kb = (bytes + 512) / 1024;
        p = tm_app(p, tm_utoa(kb, nb, 4, ' '));
        p = tm_app(p, " KB");
    } else {
        /* MB */
        unsigned long mb = bytes / (1024UL * 1024UL);
        unsigned long frac = (bytes % (1024UL * 1024UL)) * 10 / (1024UL * 1024UL);
        p = tm_app(p, tm_utoa(mb, nb, 3, ' '));
        *p++ = '.';
        p = tm_app(p, tm_utoa(frac, nb, 1, '0'));
        p = tm_app(p, " MB");
    }
    *p = '\0';
    return buf;
}

/* Format uptime ms as "HH:MM:SS". */
static void fmt_uptime(char *buf, unsigned long ms) {
    char nb[12]; char *p = buf;
    unsigned long secs = ms / 1000UL;
    unsigned long mins = secs / 60UL;
    unsigned long hrs  = mins / 60UL;
    secs %= 60UL; mins %= 60UL;
    p = tm_app(p, "Up: ");
    p = tm_app(p, tm_utoa(hrs,  nb, 2, '0')); *p++ = ':';
    p = tm_app(p, tm_utoa(mins, nb, 2, '0')); *p++ = ':';
    p = tm_app(p, tm_utoa(secs, nb, 2, '0'));
    *p = '\0';
}

/* Find previous cpu_ticks for a pid, or 0 if new. */
static unsigned long prev_cpu_for(tm_state_t *st, unsigned int pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (st->prev_pid[i] == pid) return st->prev_ticks[i];
    }
    return 0;
}

/* Save updated cpu_ticks into prev table. */
static void save_prev_cpu(tm_state_t *st, unsigned int pid, unsigned long ticks) {
    /* Find existing slot or empty */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (st->prev_pid[i] == pid || st->prev_pid[i] == 0) {
            st->prev_pid[i]   = pid;
            st->prev_ticks[i] = ticks;
            return;
        }
    }
}

/* =========================================================================
 * Refresh: poll the kernel and update label texts
 * ========================================================================= */
static void tm_refresh(tm_state_t *st) {
    /* --- System info --- */
    long si_r = sc6(SYS_SYSINFO, (long)&st->sysinfo, 0, 0, 0, 0, 0);
    st->sysinfo_ok = (si_r == 0);

    unsigned long now_ms = (unsigned long)sc6(SYS_GET_TICKS_MS, 0,0,0,0,0,0);
    unsigned long elapsed_ms = (now_ms > st->prev_ms) ? (now_ms - st->prev_ms) : 1000;
    st->prev_ms = now_ms;
    if (elapsed_ms == 0) elapsed_ms = 1;

    /* --- Process list via SYS_PROCLIST --- */
    long cnt = sc6(SYS_PROCLIST, (long)st->info, MAX_PROCS, 0, 0, 0, 0);
    if (cnt < 0) cnt = 0;
    if (cnt > MAX_PROCS) cnt = MAX_PROCS;
    st->count = (int)cnt;

    /* --- Per-process detail via SYS_PROC_QUERY --- */
    st->total_delta = 0;
    for (int i = 0; i < st->count; i++) {
        long dr = sc6(SYS_PROC_QUERY, (long)st->info[i].pid,
                      (long)&st->detail[i], 0, 0, 0, 0);
        if (dr < 0) {
            tm_memset(&st->detail[i], 0, sizeof(proc_detail_t));
            st->detail[i].pid = st->info[i].pid;
            st->detail[i].state = st->info[i].state;
            /* Copy name */
            for (int k = 0; k < 32 && st->info[i].name[k]; k++)
                st->detail[i].name[k] = st->info[i].name[k];
        }
        unsigned long old = prev_cpu_for(st, st->info[i].pid);
        unsigned long delta = (st->detail[i].cpu_ticks > old)
                              ? (st->detail[i].cpu_ticks - old) : 0;
        save_prev_cpu(st, st->info[i].pid, st->detail[i].cpu_ticks);
        st->total_delta += delta;
        /* Store delta temporarily in vma_count field (we repurpose it
           as a scratch field since we don't render vma_count) */
        st->detail[i].vma_count = (unsigned int)(delta & 0xFFFFFFFFUL);
    }
    if (st->total_delta == 0) st->total_delta = 1;
}

/* =========================================================================
 * Render: update UI widgets from current state
 * ========================================================================= */
/* ARGB colors */
#define C_BG_TITLE   0xFF1E2A3A
#define C_BG_HEADER  0xFF253040
#define C_BG_ROW_A   0xFF1A2230
#define C_BG_ROW_B   0xFF1C2535
#define C_BG_SEL     0xFF2D4A6A
#define C_TEXT       0xFFD0E0F0
#define C_DIM        0xFF8090A0
#define C_GREEN      0xFF50D080
#define C_ORANGE     0xFFE0A040
#define C_RED        0xFFE05050
#define C_BLUE       0xFF5090E0
#define C_CYAN       0xFF40C0D0

static void tm_render(tm_state_t *st) {
    char buf[80];
    char nb[22];

    /* Clamp selection / scroll */
    if (st->selected >= st->count) st->selected = (st->count > 0) ? st->count - 1 : 0;
    if (st->selected < st->scroll) st->scroll = st->selected;
    if (st->selected >= st->scroll + VISIBLE) st->scroll = st->selected - VISIBLE + 1;
    if (st->scroll < 0) st->scroll = 0;

    /* --- Render process rows --- */
    for (int r = 0; r < VISIBLE; r++) {
        int idx = st->scroll + r;
        if (idx >= st->count) {
            ui_label_set_text(st->row_pid[r],   "");
            ui_label_set_text(st->row_name[r],  "");
            ui_label_set_text(st->row_state[r], "");
            ui_label_set_text(st->row_cpu[r],   "");
            ui_label_set_text(st->row_mem[r],   "");
            continue;
        }

        proc_detail_t *d = &st->detail[idx];

        /* PID */
        tm_utoa((unsigned long)d->pid, nb, 5, ' ');
        ui_label_set_text(st->row_pid[r], nb);

        /* Name (truncate at 20 chars) */
        {
            char name_buf[21];
            int k;
            for (k = 0; k < 20 && d->name[k]; k++) name_buf[k] = d->name[k];
            name_buf[k] = '\0';
            ui_label_set_text(st->row_name[r], name_buf);
        }

        /* State */
        ui_label_set_text(st->row_state[r], state_str(d->state));

        /* CPU% — delta / total_delta * 100 */
        {
            unsigned long delta = (unsigned long)d->vma_count; /* scratch */
            unsigned long pct = (delta * 100UL) / st->total_delta;
            if (pct > 100) pct = 100;
            char *p = buf;
            p = tm_app(p, tm_utoa(pct, nb, 3, ' '));
            p = tm_app(p, "%");
            *p = '\0';
            ui_label_set_text(st->row_cpu[r], buf);
        }

        /* Memory (pages × 4 KiB) */
        {
            unsigned long mem_bytes = (unsigned long)d->mem_pages * 4096UL;
            if (d->mem_pages == 0) {
                ui_label_set_text(st->row_mem[r], "  --  ");
            } else {
                fmt_mem(mem_bytes, buf);
                ui_label_set_text(st->row_mem[r], buf);
            }
        }
    }

    /* --- Footer --- */
    {
        char *p = buf;
        p = tm_app(p, "Procs: ");
        p = tm_app(p, tm_utoa((unsigned long)st->count, nb, 0, ' '));
        if (st->sysinfo_ok) {
            p = tm_app(p, "  (");
            p = tm_app(p, tm_utoa(st->sysinfo.proc_count, nb, 0, ' '));
            p = tm_app(p, " live)");
        }
        ui_label_set_text(st->ftr_procs, buf);
    }

    if (st->sysinfo_ok) {
        unsigned long used = st->sysinfo.total_mem - st->sysinfo.free_mem;
        char mb[24];
        char *p = buf;
        p = tm_app(p, "Mem: ");
        p = tm_app(p, fmt_mem(used, mb));
        p = tm_app(p, " / ");
        p = tm_app(p, fmt_mem(st->sysinfo.total_mem, mb));
        ui_label_set_text(st->ftr_mem, buf);

        fmt_uptime(buf, st->sysinfo.uptime_ms);
        ui_label_set_text(st->ftr_uptime, buf);
    } else {
        unsigned long ms = (unsigned long)sc6(SYS_GET_TICKS_MS,0,0,0,0,0,0);
        fmt_uptime(buf, ms);
        ui_label_set_text(st->ftr_uptime, buf);
        ui_label_set_text(st->ftr_mem, "Mem: unavailable");
    }
}

/* =========================================================================
 * Button callbacks
 * ========================================================================= */
static void on_endtask(void *ud) {
    tm_state_t *st = (tm_state_t *)ud;
    if (st->count == 0) return;
    int idx = st->selected;
    if (idx < 0 || idx >= st->count) return;

    unsigned int pid = st->detail[idx].pid;
    if (pid <= 1) {
        ui_label_set_text(st->lbl_status, "Cannot kill PID 0/1");
        return;
    }

    /* SYS_PROC_CTL(pid, PROCAPI_CTL_KILL=2, 0) */
    long r = sc6(SYS_PROC_CTL, (long)pid, PROCAPI_CTL_KILL, 0, 0, 0, 0);
    if (r == 0) {
        char msg[40];
        char nb[16];
        char *p = msg;
        p = tm_app(p, "Killed PID ");
        p = tm_app(p, tm_utoa(pid, nb, 0, ' '));
        *p = '\0';
        ui_label_set_text(st->lbl_status, msg);
    } else {
        ui_label_set_text(st->lbl_status, "Kill failed (EPERM?)");
    }
}

/* =========================================================================
 * Tick callback (~60 fps, throttle refresh to 1 s)
 * ========================================================================= */
static void tm_tick(void *ud) {
    tm_state_t *st = (tm_state_t *)ud;

    /* Refresh data every ~60 ticks (~1 s at 60 fps) */
    st->tick_skip++;
    if (st->tick_skip < 60) {
        /* Still render to update highlight if selection changed */
        tm_render(st);
        return;
    }
    st->tick_skip = 0;

    tm_refresh(st);
    tm_render(st);
}

/* =========================================================================
 * _start
 * ========================================================================= */
void _start(void) {
    tm_write("[TASKMAN] starting\n");
    tm_memset(&g_tm, 0, sizeof(g_tm));

    /* Initial data fetch */
    g_tm.prev_ms = (unsigned long)sc6(SYS_GET_TICKS_MS,0,0,0,0,0,0);
    tm_refresh(&g_tm);

    /* ------------------------------------------------------------------ */
    /* Build UI                                                             */
    /* ------------------------------------------------------------------ */
    ui_app_t *app = ui_app_create("Task Manager", WIN_W, WIN_H);
    if (!app) {
        tm_write("[TASKMAN] ui_app_create failed\n");
        sc6(SYS_EXIT, 1, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }

    ui_widget_t *root = ui_app_root(app);

    /* ---- Title bar ---- */
    ui_widget_t *title_pnl = ui_panel(root, 0, 0, WIN_W, 36, C_BG_TITLE);
    ui_label(title_pnl, 12, 10, "Task Manager  —  AutomationOS", 0xFFFFFFFF);

    /* Own PID in title */
    {
        char pb[32]; char nb[16];
        char *p = pb;
        p = tm_app(p, "PID ");
        p = tm_app(p, tm_utoa((unsigned long)sc6(SYS_GETPID,0,0,0,0,0,0), nb, 0, ' '));
        *p = '\0';
        ui_label(title_pnl, WIN_W - 80, 10, pb, C_DIM);
    }

    /* ---- Column header row ---- */
    ui_widget_t *hdr_pnl = ui_panel(root, 0, 36, WIN_W, 24, C_BG_HEADER);
    ui_label(hdr_pnl, 10,  5, "  PID",    C_CYAN);
    ui_label(hdr_pnl, 70,  5, "Name",     C_CYAN);
    ui_label(hdr_pnl, 310, 5, "State",    C_CYAN);
    ui_label(hdr_pnl, 390, 5, " CPU%",    C_CYAN);
    ui_label(hdr_pnl, 460, 5, "Memory",   C_CYAN);

    /* Thin separator */
    ui_panel(root, 0, 60, WIN_W, 1, 0xFF304050);

    /* ---- Scrollable process list area ---- */
    int list_bottom = LIST_Y + VISIBLE * ROW_H;
    ui_widget_t *list_pnl = ui_panel(root, 0, LIST_Y - 30, WIN_W, list_bottom - LIST_Y + 30,
                                     0xFF141E2A);

    for (int r = 0; r < VISIBLE; r++) {
        int y = 30 + r * ROW_H;
        unsigned int bg = (r & 1) ? C_BG_ROW_B : C_BG_ROW_A;
        g_tm.row_bg[r]    = ui_panel(list_pnl, 0, y - 1, WIN_W, ROW_H, bg);
        g_tm.row_pid[r]   = ui_label(list_pnl, 10,  y, "", C_DIM);
        g_tm.row_name[r]  = ui_label(list_pnl, 70,  y, "", C_TEXT);
        g_tm.row_state[r] = ui_label(list_pnl, 310, y, "", C_DIM);
        g_tm.row_cpu[r]   = ui_label(list_pnl, 390, y, "", C_GREEN);
        g_tm.row_mem[r]   = ui_label(list_pnl, 460, y, "", C_BLUE);
    }

    /* ---- Footer panel ---- */
    int ftr_y = WIN_H - 54;
    ui_panel(root, 0, ftr_y, WIN_W, 1, 0xFF304050);
    ui_widget_t *ftr_pnl = ui_panel(root, 0, ftr_y + 1, WIN_W, 53, 0xFF101820);

    g_tm.ftr_procs  = ui_label(ftr_pnl, 12,  4, "Procs: --",        C_DIM);
    g_tm.ftr_mem    = ui_label(ftr_pnl, 12, 20, "Mem: --",           C_DIM);
    g_tm.ftr_uptime = ui_label(ftr_pnl, 12, 36, "Up: --:--:--",     C_DIM);

    /* ---- End Task button ---- */
    g_tm.btn_endtask = ui_button(ftr_pnl, WIN_W - 130, 10, 110, 28,
                                 "End Task", on_endtask, &g_tm);
    /* Status label */
    g_tm.lbl_status = ui_label(ftr_pnl, WIN_W - 280, 36, "", C_ORANGE);

    /* ---- Register tick ---- */
    ui_app_set_tick(app, tm_tick, &g_tm);

    tm_write("[TASKMAN] window ready\n");

    /* initial render before first real tick */
    tm_render(&g_tm);

    ui_app_run(app);   /* never returns */
    __builtin_unreachable();
}

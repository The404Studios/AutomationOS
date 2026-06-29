/*
 * userspace/apps/taskman/taskman.c -- Task Manager / Force-Quit dialog.
 * ======================================================================
 *
 * A retained-mode GUI window ("Task Manager", 560x460) built on the M4 UI
 * toolkit.  Shows a live process list, lets the user select a row and send
 * either SIGTERM ("End Task") or SIGKILL ("Force Quit") to the selected
 * process.
 *
 * Syscalls used:
 *   SYS_WRITE    =  3  -- serial output
 *   SYS_GETPID   =  8  -- own PID (to prevent self-kill)
 *   SYS_KILL     = 26  -- send signal to process
 *   SYS_YIELD    = 15  -- yield CPU in tick
 *   SYS_PROCLIST = 44  -- fill procinfo_t array; returns count or -errno
 *
 * procinfo_t layout (EXACTLY 64 bytes, as specified by the kernel ABI):
 *   offset  0: unsigned int pid          (4 bytes)
 *   offset  4: unsigned int parent_pid   (4 bytes)
 *   offset  8: unsigned int state        (4 bytes)
 *   offset 12: unsigned int flags        (4 bytes)
 *   offset 16: char         name[32]     (32 bytes)
 *   offset 48: u64          cpu_ticks    (8 bytes)  -- per-process CPU ticks
 *   offset 56: u64          ctx_switches (8 bytes)  -- per-process dispatch count
 *   total: 64 bytes
 *
 *   state: 0=CREATED, 1=READY, 2=RUNNING, 3=BLOCKED, 4=TERMINATED
 *
 * Build (ALL flags DIRECTLY on the gcc cmdline):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -I userspace \
 *       -c userspace/apps/taskman/taskman.c -o /tmp/tm.o
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
 *       /tmp/tm.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o -o /tmp/tm.elf
 *
 *   # Verify no stack-canary reference (must produce no output):
 *   objdump -d /tmp/tm.elf | grep fs:0x28
 *
 * Serial output lines:
 *   [TASKMAN] starting
 *   [TASKMAN] N procs
 *   [TASKMAN] kill pid=P sig=S rc=R
 */

#include "../../lib/ui/ui.h"

/* -------------------------------------------------------------------------
 * Syscall numbers
 * ----------------------------------------------------------------------- */
#define SYS_WRITE    3
#define SYS_GETPID   8
#define SYS_YIELD    15
#define SYS_KILL     26
#define SYS_PROCLIST 44

/* Signal numbers */
#define SIGTERM 15
#define SIGKILL  9

/* -------------------------------------------------------------------------
 * Inline 3-argument syscall (rdi / rsi / rdx).
 * ----------------------------------------------------------------------- */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* -------------------------------------------------------------------------
 * procinfo_t  -- kernel ABI: exactly 64 bytes.
 * ----------------------------------------------------------------------- */
typedef unsigned long long tm_u64;
typedef struct {
    unsigned int pid;         /*  0 */
    unsigned int parent_pid;  /*  4 */
    unsigned int state;       /*  8 */
    unsigned int flags;       /* 12 */
    char         name[32];    /* 16 */
    tm_u64       cpu_ticks;   /* 48 -- per-process CPU ticks            */
    tm_u64       ctx_switches;/* 56 -- per-process dispatch count       */
                              /* total: 64 */
} procinfo_t;

/* Compile-time size check -- will produce a zero-length array error if wrong. */
typedef char _procinfo_size_check[sizeof(procinfo_t) == 64 ? 1 : -1];

/* -------------------------------------------------------------------------
 * String helpers (no libc).
 * ----------------------------------------------------------------------- */
static unsigned long tm_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void tm_write(const char *s)
{
    sc(SYS_WRITE, 1, (long)s, (long)tm_strlen(s));
}

/* Copy up to n-1 chars from src into dst, always NUL-terminates. */
static void tm_strncpy(char *dst, const char *src, int n)
{
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Write an unsigned decimal into buf (at least 22 bytes).
 * Returns pointer to the start of the number inside buf. */
static char *tm_utoa(unsigned long v, char *buf)
{
    char tmp[22];
    int  i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    else { unsigned long x = v; while (x) { tmp[i++] = (char)('0' + x % 10); x /= 10; } }
    /* tmp contains reversed digits; copy forward */
    int j;
    for (j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[j] = '\0';
    return buf;
}

/* Append src to *p; advance *p to the new NUL. */
static void tm_cat(char **p, const char *src)
{
    while (*src) { **p = *src++; (*p)++; }
    **p = '\0';
}

/* Append decimal of v to *p, width w, padded with pad. */
static void tm_cat_uint(char **p, unsigned long v, int w, char pad)
{
    char nb[22];
    tm_utoa(v, nb);
    int len = (int)tm_strlen(nb);
    while (len < w) { **p = pad; (*p)++; len++; }
    tm_cat(p, nb);
}

/* -------------------------------------------------------------------------
 * Layout / capacity constants.
 * ----------------------------------------------------------------------- */
#define WIN_W       680   /* widened to fit TICKS + CTXSW columns */
#define WIN_H       460
#define MAX_ROWS     12   /* visible process rows */
#define PROC_CAP    256   /* SYS_PROCLIST probe capacity = kernel MAX_PROCESSES, */
                          /* so proc_count is the TRUE total, not silently pinned */
                          /* at MAX_ROWS (we still render only MAX_ROWS rows).    */
#define TICK_PERIOD  30   /* refresh proc list every N frames */

/* Row geometry inside the list panel */
#define ROW_H        22
#define ROW_YSTART    8

/* Colors (ARGB32) */
#define C_WIN_BG     0xFF1C1C1E   /* Aether Dark window background */
#define C_TITLE_BG   0xFF1A1A2E   /* deep navy title bar */
#define C_TITLE_FG   0xFF00D4FF   /* cyan accent */
#define C_HDR_BG     0xFF16213E   /* header row background */
#define C_HDR_FG     0xFF58A6FF   /* header text blue */
#define C_LIST_BG    0xFF0D1117   /* list panel background */
#define C_ROW_EVEN   0xFF161B22   /* even-row shading */
#define C_ROW_ODD    0xFF0D1117   /* odd-row shading */
#define C_ROW_SEL    0xFF1E4070   /* selected-row highlight */
#define C_ROW_DEAD   0xFF3A1A1A   /* recently killed row */
#define C_TEXT       0xFFCDD5DF   /* normal row text */
#define C_TEXT_DIM   0xFF707070   /* dimmed / empty */
#define C_BTN_NORMAL 0xFF2C2C2E   /* button face */
#define C_BTN_TERM   0xFF2E4A2E   /* "End Task" face (green-tinted) */
#define C_BTN_KILL   0xFF5C1A1A   /* "Force Quit" face (red) */
#define C_BTN_LABEL  0xFFEEEEEE   /* button label text */
#define C_SEP        0xFF30363D   /* separator line */
#define C_STATUS_FG  0xFFB0B0B0   /* status bar text */
#define C_DEAD_FG    0xFFE05050   /* bright red for Force Quit highlight */

/* -------------------------------------------------------------------------
 * Per-row click user-data (one struct per pre-created row).
 * ----------------------------------------------------------------------- */
typedef struct {
    int row_index;   /* 0 .. MAX_ROWS-1 */
} row_ud_t;

/* -------------------------------------------------------------------------
 * Kill button user-data.
 * ----------------------------------------------------------------------- */
typedef struct {
    int sig;  /* SIGTERM or SIGKILL */
} kill_ud_t;

/* -------------------------------------------------------------------------
 * Global application state.
 * ----------------------------------------------------------------------- */
typedef struct {
    /* Process list buffer (PROC_CAP so we can fetch the TRUE total; the kernel
       fills up to PROC_CAP entries -- we only RENDER the first MAX_ROWS). */
    procinfo_t procs[PROC_CAP];
    int        proc_count;   /* TRUE live process count (may exceed MAX_ROWS) */

    /* Own PID -- never offered as a kill target */
    unsigned int own_pid;

    /* Selection */
    int selected;            /* -1 = none, else index into procs[] */

    /* Post-kill visual flag: index of row just killed, -1 = none */
    int killed_idx;

    /* Frame counter for throttled refresh */
    int frame_count;

    /* Status message */
    char status_buf[80];

    /* UI widgets */
    ui_widget_t *lbl_status;

    /* Per-row background panels and label widgets */
    ui_widget_t *row_panels[MAX_ROWS];
    ui_widget_t *row_labels[MAX_ROWS];

    /* Kill buttons (need to store so we can reference them; the toolkit
       owns the widgets -- we just need the row-select buttons' ud) */
    /* (row-select buttons are plain buttons with row_ud_t ud) */

} taskman_t;

/* Single file-static instance */
static taskman_t g_tm;

/* Per-row click user-data (static so pointers remain valid) */
static row_ud_t  g_row_ud[MAX_ROWS];

/* Kill button user-data */
static kill_ud_t g_ud_endtask;   /* SIGTERM */
static kill_ud_t g_ud_forcequit; /* SIGKILL */

/* -------------------------------------------------------------------------
 * Format helpers for the process list.
 * ----------------------------------------------------------------------- */

static const char *state_str(unsigned int s)
{
    switch (s) {
    case 0: return "new  ";
    case 1: return "ready";
    case 2: return "run  ";
    case 3: return "blk  ";
    case 4: return "dead ";
    default: return "?    ";
    }
}

/*
 * Build a row string:
 *   "PPPPP  NNNNNNNNNNNNNNNN  SSSSS  TTTTTTTT  CCCCCC"
 * pid (5), 2sp, name (16), 2sp, state (5), 2sp, cpu_ticks (8), 2sp,
 * ctx_switches (6). buf must be >= 64 bytes.
 */
static void fmt_row(char *buf, const procinfo_t *pi)
{
    char *p = buf;
    /* PID: right-aligned in 5 chars */
    tm_cat_uint(&p, (unsigned long)pi->pid, 5, ' ');
    *p++ = ' '; *p++ = ' ';
    /* Name: left-aligned, padded/truncated to 16 chars */
    int nlen = 0;
    while (pi->name[nlen] && nlen < 16) { *p++ = pi->name[nlen++]; }
    while (nlen++ < 16) *p++ = ' ';
    *p++ = ' '; *p++ = ' ';
    /* State: 5 chars */
    const char *ss = state_str(pi->state);
    while (*ss) *p++ = *ss++;
    *p++ = ' '; *p++ = ' ';
    /* CPU ticks: right-aligned in 8 chars (real per-process CPU time). */
    tm_cat_uint(&p, (unsigned long)pi->cpu_ticks, 8, ' ');
    *p++ = ' '; *p++ = ' ';
    /* Context switches: right-aligned in 6 chars (dispatch count). */
    tm_cat_uint(&p, (unsigned long)pi->ctx_switches, 6, ' ');
    *p = '\0';
}

/* -------------------------------------------------------------------------
 * Serial helpers.
 * ----------------------------------------------------------------------- */

static void serial_uint(unsigned long v)
{
    char buf[22];
    tm_utoa(v, buf);
    tm_write(buf);
}

static void serial_int(long v)
{
    if (v < 0) { tm_write("-"); v = -v; }
    serial_uint((unsigned long)v);
}

/* -------------------------------------------------------------------------
 * Refresh: call SYS_PROCLIST, update all row labels.
 * ----------------------------------------------------------------------- */
static void refresh_proclist(taskman_t *st)
{
    long cnt = sc(SYS_PROCLIST, (long)st->procs, (long)PROC_CAP, 0);
    if (cnt < 0) cnt = 0;
    if (cnt > PROC_CAP) cnt = PROC_CAP;
    st->proc_count = (int)cnt;                  /* TRUE total live process count */

    int shown = st->proc_count < MAX_ROWS ? st->proc_count : MAX_ROWS;

    /* Serial report -- the HONEST total now (was silently pinned at MAX_ROWS). */
    tm_write("[TASKMAN] ");
    serial_uint((unsigned long)st->proc_count);
    tm_write(" procs\n");

    /* Update row labels and APPLY the panel color live. Previously the computed
       color was discarded with (void)bg -- the selected row never highlighted,
       which made the task manager look bugged. ui_widget_set_bg (ui.c) sets
       w->bg, which UI_PANEL re-reads every frame; the highlight renders behind
       the legible label (panel child precedes label child within the list). */
    for (int i = 0; i < MAX_ROWS; i++) {
        if (i < shown) {
            char row_buf[64];
            fmt_row(row_buf, &st->procs[i]);
            ui_label_set_text(st->row_labels[i], row_buf);

            unsigned int bg;
            if (i == st->selected)        bg = C_ROW_SEL;
            else if (i == st->killed_idx) bg = C_ROW_DEAD;
            else                          bg = (i & 1) ? C_ROW_ODD : C_ROW_EVEN;
            ui_widget_set_bg(st->row_panels[i], bg);
        } else {
            ui_label_set_text(st->row_labels[i], "");
            ui_widget_set_bg(st->row_panels[i], (i & 1) ? C_ROW_ODD : C_ROW_EVEN);
        }
    }

    /* Honest cap indicator when idle (no selection / no just-killed row, so we
       don't clobber an action message): "N processes (showing K)". */
    if (st->selected < 0 && st->killed_idx < 0) {
        char *p = st->status_buf;
        tm_cat_uint(&p, (unsigned long)st->proc_count, 0, ' ');
        tm_cat(&p, " processes");
        if (st->proc_count > shown) {
            tm_cat(&p, " (showing ");
            tm_cat_uint(&p, (unsigned long)shown, 0, ' ');
            tm_cat(&p, ")");
        }
        ui_label_set_text(st->lbl_status, st->status_buf);
    }
}

/* -------------------------------------------------------------------------
 * Row click callback -- selects a row.
 * ----------------------------------------------------------------------- */
static void on_row_click(void *ud)
{
    row_ud_t *ru = (row_ud_t *)ud;
    taskman_t *st = &g_tm;
    int idx = ru->row_index;

    if (idx >= st->proc_count) return; /* blank row */

    st->selected   = idx;
    st->killed_idx = -1;

    /* Update status bar */
    char *p = st->status_buf;
    tm_cat(&p, "Selected: ");
    /* name */
    char tmp_name[33];
    tm_strncpy(tmp_name, st->procs[idx].name, 33);
    tm_cat(&p, tmp_name);
    tm_cat(&p, "  (PID ");
    tm_cat_uint(&p, (unsigned long)st->procs[idx].pid, 0, ' ');
    tm_cat(&p, ")");
    ui_label_set_text(st->lbl_status, st->status_buf);

    /* Immediately reflect the selection (don't wait up to TICK_PERIOD for the
       next refresh): recolor every row panel now. C_ROW_SEL on the selected
       row, killed/zebra otherwise. */
    for (int i = 0; i < MAX_ROWS; i++) {
        unsigned int bg = (i == st->selected)        ? C_ROW_SEL
                        : (i == st->killed_idx)      ? C_ROW_DEAD
                        : (i & 1) ? C_ROW_ODD : C_ROW_EVEN;
        ui_widget_set_bg(st->row_panels[i], bg);
    }
}

/* -------------------------------------------------------------------------
 * Kill callbacks -- "End Task" (SIGTERM=15) and "Force Quit" (SIGKILL=9).
 * ----------------------------------------------------------------------- */
static void do_kill(taskman_t *st, int sig)
{
    if (st->selected < 0 || st->selected >= st->proc_count) {
        /* Nothing selected */
        char *p = st->status_buf;
        tm_cat(&p, "No process selected.");
        ui_label_set_text(st->lbl_status, st->status_buf);
        return;
    }

    unsigned int tgt_pid = st->procs[st->selected].pid;

    /* Guard: never kill pid 0, pid 1, or self */
    if (tgt_pid == 0 || tgt_pid == 1 || tgt_pid == st->own_pid) {
        char *p = st->status_buf;
        tm_cat(&p, "Cannot kill system process.");
        ui_label_set_text(st->lbl_status, st->status_buf);
        return;
    }

    long rc = sc(SYS_KILL, (long)tgt_pid, (long)sig, 0);

    /* Serial log */
    tm_write("[TASKMAN] kill pid=");
    serial_uint((unsigned long)tgt_pid);
    tm_write(" sig=");
    serial_uint((unsigned long)sig);
    tm_write(" rc=");
    serial_int(rc);
    tm_write("\n");

    /* Update status */
    char *p = st->status_buf;
    if (rc == 0) {
        tm_cat(&p, sig == SIGKILL ? "Force quit sent to " : "End task sent to ");
        /* name */
        char tmp_name[33];
        tm_strncpy(tmp_name, st->procs[st->selected].name, 33);
        tm_cat(&p, tmp_name);
        /* Mark killed row for visual feedback */
        st->killed_idx = st->selected;
        st->selected   = -1;
    } else if (rc == -3) {
        tm_cat(&p, "Process not found (already gone).");
        st->killed_idx = st->selected;
        st->selected   = -1;
    } else {
        tm_cat(&p, "Kill failed (rc=");
        tm_cat_uint(&p, (unsigned long)(-rc), 0, ' ');
        tm_cat(&p, ")");
    }
    ui_label_set_text(st->lbl_status, st->status_buf);

    /* Immediately re-fetch so dead process disappears */
    refresh_proclist(st);
}

static void on_endtask(void *ud)
{
    (void)ud;
    do_kill(&g_tm, SIGTERM);
}

static void on_forcequit(void *ud)
{
    (void)ud;
    do_kill(&g_tm, SIGKILL);
}

/* -------------------------------------------------------------------------
 * Per-frame tick callback.
 * ----------------------------------------------------------------------- */
static void taskman_tick(void *ud)
{
    taskman_t *st = (taskman_t *)ud;

    st->frame_count++;
    if (st->frame_count >= TICK_PERIOD) {
        st->frame_count = 0;
        refresh_proclist(st);
    }

    sc(SYS_YIELD, 0, 0, 0);
}

/* -------------------------------------------------------------------------
 * Entry point.
 * ----------------------------------------------------------------------- */
void _start(void)
{
    tm_write("[TASKMAN] starting\n");

    /* Own PID */
    long own = sc(SYS_GETPID, 0, 0, 0);
    g_tm.own_pid     = (unsigned int)(own > 0 ? own : 0);
    g_tm.selected    = -1;
    g_tm.killed_idx  = -1;
    g_tm.proc_count  = 0;
    g_tm.frame_count = 0;

    /* Pre-fill status */
    char *sp = g_tm.status_buf;
    tm_cat(&sp, "Click a process to select, then End Task or Force Quit.");

    /* ------------------------------------------------------------------
     * Build the UI tree.
     * ------------------------------------------------------------------ */
    ui_app_t    *app  = ui_app_create("Task Manager", WIN_W, WIN_H);
    if (!app) {
        tm_write("[TASKMAN] ui_app_create failed\n");
        asm volatile("mov $60,%rax; mov $1,%rdi; syscall");
        __builtin_unreachable();
    }
    ui_widget_t *root = ui_app_root(app);

    /* ---- Title bar ---- */
    ui_widget_t *title_pnl = ui_panel(root, 0, 0, WIN_W, 34, C_TITLE_BG);
    ui_label(title_pnl, 12, 9, "Task Manager", C_TITLE_FG);
    {
        /* Own PID in title bar (right side) */
        char pid_buf[32];
        char *p = pid_buf;
        tm_cat(&p, "PID: ");
        tm_cat_uint(&p, (unsigned long)g_tm.own_pid, 0, ' ');
        ui_label(title_pnl, WIN_W - 90, 9, pid_buf, C_STATUS_FG);
    }

    /* ---- Column header row ---- */
    ui_widget_t *hdr_pnl = ui_panel(root, 8, 40, WIN_W - 16, 24, C_HDR_BG);
    ui_label(hdr_pnl, 8, 4,
             "  PID    NAME              STATE     TICKS   CTXSW",
             C_HDR_FG);

    /* Separator under header */
    ui_panel(root, 8, 64, WIN_W - 16, 1, C_SEP);

    /* ---- Process list panel ---- */
    int list_top  = 66;
    int list_h    = MAX_ROWS * ROW_H + ROW_YSTART * 2;
    ui_widget_t *list_pnl = ui_panel(root, 8, list_top, WIN_W - 16, list_h, C_LIST_BG);

    /* Pre-create MAX_ROWS rows */
    for (int i = 0; i < MAX_ROWS; i++) {
        int y = ROW_YSTART + i * ROW_H;

        /* Alternating background panel */
        unsigned int row_bg = (i & 1) ? C_ROW_ODD : C_ROW_EVEN;
        g_tm.row_panels[i] = ui_panel(list_pnl, 2, y - 2, WIN_W - 24, ROW_H, row_bg);

        /* Row label (text will be set in refresh) */
        g_tm.row_labels[i] = ui_label(list_pnl, 8, y, "", C_TEXT);

        /* Invisible clickable button over the row */
        g_row_ud[i].row_index = i;
        /* We overlay a transparent-feeling button by giving it the same bg
           color as the row panel -- the text on the button will be blank
           and the real label is behind it. Since the toolkit draws buttons
           on top of labels, we instead put a button *behind* by using a
           zero-height clickable region... but the toolkit doesn't expose
           z-order. The practical solution: make the row click button
           visually match the row (no text, same bg). The label is separate
           and shows above. But we can't layer within the same parent cleanly.
           Instead: put the click button as a child of list_pnl with matching
           bg and ZERO label text, so it acts as an invisible hit-area. */
        ui_button(list_pnl, 2, y - 2, WIN_W - 24, ROW_H, "",
                  on_row_click, (void *)&g_row_ud[i]);
    }

    /* Separator above bottom bar */
    int sep_y = list_top + list_h + 4;
    ui_panel(root, 8, sep_y, WIN_W - 16, 1, C_SEP);

    /* ---- Status bar ---- */
    int status_y = sep_y + 6;
    ui_widget_t *status_pnl = ui_panel(root, 8, status_y, WIN_W - 16, 20, C_WIN_BG);
    g_tm.lbl_status = ui_label(status_pnl, 6, 2, g_tm.status_buf, C_STATUS_FG);

    /* ---- Bottom buttons ---- */
    int btn_y    = status_y + 28;
    int btn_w    = 160;
    int btn_h    = 36;
    int btn_gap  = 20;
    int btns_x   = (WIN_W - (2 * btn_w + btn_gap)) / 2;

    /* "End Task" -- SIGTERM, green-tinted face */
    g_ud_endtask.sig = SIGTERM;
    ui_button(root, btns_x, btn_y, btn_w, btn_h,
              "End Task", on_endtask, (void *)&g_ud_endtask);

    /* "Force Quit" -- SIGKILL, red face */
    g_ud_forcequit.sig = SIGKILL;
    ui_button(root, btns_x + btn_w + btn_gap, btn_y, btn_w, btn_h,
              "Force Quit", on_forcequit, (void *)&g_ud_forcequit);

    /* ---- Register tick ---- */
    ui_app_set_tick(app, taskman_tick, &g_tm);

    /* ---- Initial process list fetch ---- */
    refresh_proclist(&g_tm);

    /* ---- Enter event loop (never returns) ---- */
    ui_app_run(app);

    __builtin_unreachable();
}

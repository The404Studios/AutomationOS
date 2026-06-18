/*
 * cockpit.c -- AGENT-COCKPIT-0: the human-facing surface for the agent (ring 3).
 * =============================================================================
 *
 * The agent cockpit. The user types a GOAL, clicks RUN, and watches the live
 * agent steps stream into a scroll log -- with Allow / Deny / grant-full /
 * STOP controls. It is the human in the loop for the Nemotron OS-automation
 * agent (sbin/agentd):
 *
 *     cockpit (this window)  <--SHM page-->  sbin/agentd  <--slirp-->  host LLM
 *                 (goal/control IN, status/step OUT)
 *
 * The contract is a single shared page (userspace/include/agentcockpit.h):
 *   - cockpit CREATES + OWNS the page (IPC_CREAT|0600), zeroes it, sets magic.
 *   - cockpit writes:  goal, goal_seq, stop, grant_full, confirm.
 *   - agentd  writes:  state, step, run_seq, tool, args, last.
 *
 * RUN copies the textbox into shm->goal, bumps goal_seq, resets confirm/stop,
 * then spawns agentd passing the goal as argv[1]:
 *     sc(SYS_SPAWN, (long)"sbin/agentd", (long)shm->goal, 0)
 * agentd uses argv[1] as the goal AND attaches THIS page for status/control.
 *
 * The per-frame tick callback reads state/step/tool/args/last, appends new
 * steps to the log as `step` advances, drives the status label, and enables
 * the Allow/Deny buttons while state == AC_STATE_CONFIRM.
 *
 * Launch arg `--proof`: auto-fill a goal, auto-click RUN, and print
 *   [COCKPIT] proof: posted goal seq=1
 * so a headless boot can verify the cockpit<->agentd seam without a human.
 *
 * Built like the ARGV-AWARE UI app filemanager: link crt0.o FIRST (it reads
 * the kernel-provided argc/argv off the stack and calls main(argc, argv)),
 * then ui.o + wl_client.o + bitfont.o + font2.o. The bare-_start UI apps
 * (claudechat, anthropic) have no argv; cockpit needs argv for --proof, so it
 * follows filemanager's crt0 + int main(argc,argv) convention instead.
 *
 *   cc userspace/apps/cockpit/cockpit.c /tmp/cockpit.o
 *   $LD /tmp/crt0.o /tmp/cockpit.o /tmp/ui.o /tmp/wlc.o /tmp/bf.o /tmp/font2.o \
 *       -o /tmp/cockpit.elf
 *
 * No libc: pure inline syscalls + the ui.h toolkit + fixed static buffers.
 */

#include "../../lib/ui/ui.h"
#include "../../include/agentcockpit.h"   /* the cockpit<->agentd SHM contract */

/* -- syscalls (raw ABI; no fs:0x28 canary under -fno-stack-protector) -------- */
#define SYS_EXIT       0
#define SYS_WRITE      3
#define SYS_SPAWN     16
#define SYS_SHMGET    18
#define SYS_SHMAT     19

#define IPC_CREAT  01000   /* matches the kernel shm flag (octal, == 0x200) */

static long sc(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s)); }

/* -- window geometry -------------------------------------------------------- */
#define WIN_W      640
#define WIN_H      480

#define PAD          12
#define GOAL_Y       40
#define GOAL_H       24
#define BTNROW_Y     72
#define LOG_Y       112
#define LOG_H       248                 /* scroll viewport height            */
#define STATUS_Y    (LOG_Y + LOG_H + 8) /* 368                               */
#define CTLROW_Y    (STATUS_Y + 28)     /* 396 -- Allow/Deny/grant row       */

#define LOG_W       (WIN_W - 2 * PAD)   /* 616                               */
#define CONTENT_H   4000                /* ~250 lines of scroll capacity     */
#define LINE_H        16
#define LINES_PER_BOX 16                /* == the toolkit's UI_MAX_CHILDREN  */
#define WRAP_COLS     74                /* chars per log line (~LOG_W/8)      */

/* Aether Dark palette */
#define COL_TEXT      0xFFFFFFFF
#define COL_MUTED     0xFFAEAEB2
#define COL_DIM       0xFF8E8E93
#define COL_LOG_BG    0xFF141416
#define COL_GREEN     0xFF7CE38B
#define COL_AMBER     0xFFFF9E64
#define COL_BLUE      0xFF9CDCFE
#define COL_RED       0xFFFF6B6B

/* -- app + widget state ----------------------------------------------------- */
static ui_app_t              *g_app;
static volatile agentcockpit_shm_t *g_shm;     /* the owned contract page     */

static ui_widget_t *g_goalbox;       /* ui_textbox: the goal input            */
static ui_widget_t *g_log;           /* ui_scroll: the live step transcript   */
static ui_widget_t *g_status;        /* one-line status label                 */
static ui_widget_t *g_allow_btn;     /* Allow caption (greyed unless CONFIRM) */
static ui_widget_t *g_deny_btn;      /* Deny  caption                         */

/* scroll bucketing (same scheme as claudechat: the toolkit caps EVERY widget
 * at UI_MAX_CHILDREN children, so bucket log lines into row CONTAINERS). */
static int          g_msg_y = 8;
static ui_widget_t *g_line_box = 0;
static int          g_box_lines = 0;
static int          g_box_y = 8;

/* tick bookkeeping */
static unsigned g_last_step = 0xFFFFFFFFu;  /* force first step to render      */
static unsigned g_last_state = 0xFFFFFFFFu;
static int      g_started = 0;              /* a run has been posted at least once */

/* -- tiny freestanding helpers ---------------------------------------------- */
static void u_to_dec(unsigned v, char *buf) {
    char tmp[12]; int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (v && n < 11) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int i = 0;
    while (n) buf[i++] = tmp[--n];
    buf[i] = 0;
}
/* append src to dst (dst must have room); returns chars written */
static int strapp(char *dst, int at, const char *src, int cap) {
    int i = at;
    for (; src && *src && i < cap - 1; src++) dst[i++] = *src;
    dst[i] = 0;
    return i;
}

/* -- log: append one already-short line to the scroll transcript ------------ */
static void emit_line(const char *s, unsigned int color) {
    if (g_msg_y > CONTENT_H - LINE_H) return;        /* full -- drop silently */
    if (!g_line_box || g_box_lines >= LINES_PER_BOX) {
        g_box_y    = g_msg_y;
        g_line_box = ui_panel(g_log, 0, g_box_y, LOG_W - 12,
                              LINES_PER_BOX * LINE_H, COL_LOG_BG);
        g_box_lines = 0;
        if (!g_line_box) return;                     /* scroll full -- stop   */
    }
    ui_label(g_line_box, 8, g_msg_y - g_box_y, s, color);
    g_box_lines++;
    g_msg_y += LINE_H;
}

/* Word-wrap + newline-split `text` into log lines, the first carrying prefix. */
static void log_line(const char *prefix, const char *text, unsigned int color) {
    char line[WRAP_COLS + 1];
    int  col = 0;
    for (const char *p = prefix; *p && col < WRAP_COLS; p++) line[col++] = *p;
    for (const char *p = text; *p; p++) {
        char c = *p;
        if (c == '\r') continue;
        if (c == '\n' || col >= WRAP_COLS) {
            line[col] = 0;
            emit_line(line, color);
            col = 0;
            if (c == '\n') continue;
        }
        line[col++] = c;
    }
    if (col > 0) { line[col] = 0; emit_line(line, color); }

    /* auto-scroll so the newest line is visible */
    int off = g_msg_y - (LOG_H - LINE_H);
    if (off < 0) off = 0;
    ui_scroll_set_offset(g_log, off);
}

/* -- the actual RUN: post a goal + spawn agentd ----------------------------- */
static void post_goal(const char *goal) {
    if (!g_shm) return;

    /* Copy goal into the page (bounded to the contract's goal[256]). */
    int i = 0;
    for (; goal && goal[i] && i < (int)sizeof(g_shm->goal) - 1; i++)
        g_shm->goal[i] = goal[i];
    g_shm->goal[i] = 0;

    /* Reset control fields, then bump goal_seq LAST so agentd sees a coherent
     * goal when it notices the new sequence number. */
    g_shm->confirm    = AC_CONFIRM_NONE;
    g_shm->stop       = 0;
    g_shm->state      = AC_STATE_IDLE;
    g_shm->step       = 0;
    g_shm->goal_seq  += 1;

    /* Reset our render bookkeeping so the new run's steps stream cleanly. */
    g_last_step  = 0xFFFFFFFFu;
    g_last_state = 0xFFFFFFFFu;
    g_started    = 1;

    log_line("RUN ", (const char *)g_shm->goal, COL_BLUE);
    ui_label_set_text(g_status, "Launching agentd...");

    /* Spawn agentd: argv[1] = the goal string (space-separated tokens). It also
     * attaches THIS page (same key) for status/control. */
    sc(SYS_SPAWN, (long)"sbin/agentd", (long)g_shm->goal, 0);

    out("[COCKPIT] proof: posted goal seq=");
    {
        char num[12];
        u_to_dec(g_shm->goal_seq, num);
        out(num);
    }
    out("\n");
}

/* -- button callbacks ------------------------------------------------------- */
static void on_run(void *ud) {
    (void)ud;
    const char *txt = ui_textbox_text(g_goalbox);
    if (!txt || !txt[0]) {
        ui_label_set_text(g_status, "Type a goal first.");
        return;
    }
    /* Snapshot before post (post_goal copies into the page immediately). */
    static char goal[256];
    int i = 0;
    for (; txt[i] && i < (int)sizeof(goal) - 1; i++) goal[i] = txt[i];
    goal[i] = 0;
    post_goal(goal);
}

static void on_stop(void *ud) {
    (void)ud;
    if (g_shm) g_shm->stop = 1;
    log_line("", "[STOP requested]", COL_RED);
    ui_label_set_text(g_status, "STOP requested.");
}

static void on_allow(void *ud) {
    (void)ud;
    if (!g_shm) return;
    if (g_shm->state != AC_STATE_CONFIRM) return;     /* only meaningful then  */
    g_shm->confirm = AC_CONFIRM_ALLOW;
    log_line("", "[allowed]", COL_GREEN);
    ui_label_set_text(g_status, "Allowed -- continuing.");
}

static void on_deny(void *ud) {
    (void)ud;
    if (!g_shm) return;
    if (g_shm->state != AC_STATE_CONFIRM) return;
    g_shm->confirm = AC_CONFIRM_DENY;
    log_line("", "[denied]", COL_AMBER);
    ui_label_set_text(g_status, "Denied -- skipping step.");
}

static void on_grant_full(int state, void *ud) {
    (void)ud;
    if (g_shm) g_shm->grant_full = state ? 1u : 0u;
}

/* -- per-frame tick: pull agentd status, stream new steps ------------------- */
static void on_tick(void *ud) {
    (void)ud;
    if (!g_shm) return;

    unsigned state = g_shm->state;
    unsigned step  = g_shm->step;

    /* Append a new step line whenever `step` advances (agentd bumps it per
     * tool). Snapshot tool/args into locals -- agentd may rewrite them. */
    if (step != g_last_step) {
        g_last_step = step;
        if (g_started && step > 0) {
            char buf[64];
            char num[12];
            int  at = 0;
            u_to_dec(step, num);
            at = strapp(buf, 0,  "step ", (int)sizeof(buf));
            at = strapp(buf, at, num,     (int)sizeof(buf));
            at = strapp(buf, at, ": ",    (int)sizeof(buf));

            char detail[256];
            int  d = 0;
            d = strapp(detail, 0, (const char *)g_shm->tool, (int)sizeof(detail));
            if (g_shm->args[0]) {
                d = strapp(detail, d, " ", (int)sizeof(detail));
                d = strapp(detail, d, (const char *)g_shm->args, (int)sizeof(detail));
            }
            (void)d;
            log_line(buf, detail, COL_TEXT);
            /* serial: prove the STATUS-back flow (agentd wrote step/tool; the cockpit
             * observed it) so the headless run_cockpit.sh can assert both directions. */
            out("[COCKPIT] step "); out(num); out(": "); out((const char *)g_shm->tool); out("\n");
        }
    }

    /* Drive the status label + Allow/Deny affordance on state transitions. */
    if (state != g_last_state) {
        g_last_state = state;
        switch (state) {
        case AC_STATE_IDLE:
            if (g_started) ui_label_set_text(g_status, "agentd idle.");
            break;
        case AC_STATE_RUNNING: {
            char line[300];
            char num[12];
            int  at = 0;
            u_to_dec(step, num);
            at = strapp(line, 0,  "running step ", (int)sizeof(line));
            at = strapp(line, at, num,             (int)sizeof(line));
            at = strapp(line, at, ": ",            (int)sizeof(line));
            at = strapp(line, at, (const char *)g_shm->tool, (int)sizeof(line));
            if (g_shm->args[0]) {
                at = strapp(line, at, " ", (int)sizeof(line));
                at = strapp(line, at, (const char *)g_shm->args, (int)sizeof(line));
            }
            (void)at;
            ui_label_set_text(g_status, line);
            ui_widget_set_fg(g_status, COL_MUTED);
            break;
        }
        case AC_STATE_CONFIRM: {
            char line[300];
            int  at = strapp(line, 0, "AWAITING CONFIRM: ", (int)sizeof(line));
            at = strapp(line, at, (const char *)g_shm->tool, (int)sizeof(line));
            if (g_shm->args[0]) {
                at = strapp(line, at, " ", (int)sizeof(line));
                at = strapp(line, at, (const char *)g_shm->args, (int)sizeof(line));
            }
            (void)at;
            ui_label_set_text(g_status, line);
            ui_widget_set_fg(g_status, COL_AMBER);
            /* enable + highlight the Allow/Deny buttons */
            ui_widget_set_bg(g_allow_btn, 0xFF1E6F3A);   /* live green */
            ui_widget_set_bg(g_deny_btn,  0xFF7A2B2B);   /* live red   */
            log_line("", line, COL_AMBER);
            break;
        }
        case AC_STATE_DONE:
            ui_label_set_text(g_status, "DONE");
            ui_widget_set_fg(g_status, COL_GREEN);
            ui_widget_set_bg(g_allow_btn, COL_LOG_BG);   /* grey out again */
            ui_widget_set_bg(g_deny_btn,  COL_LOG_BG);
            if (g_shm->last[0]) log_line("DONE ", (const char *)g_shm->last, COL_GREEN);
            else                log_line("", "[done]", COL_GREEN);
            break;
        case AC_STATE_STOPPED:
            ui_label_set_text(g_status, "STOPPED");
            ui_widget_set_fg(g_status, COL_RED);
            ui_widget_set_bg(g_allow_btn, COL_LOG_BG);
            ui_widget_set_bg(g_deny_btn,  COL_LOG_BG);
            log_line("", "[stopped]", COL_RED);
            break;
        default:
            break;
        }
    }
    /* Allow/Deny stay inert outside CONFIRM: their callbacks early-return unless
     * state==CONFIRM, and the state-change branch greys them on every exit from
     * CONFIRM -- so no per-frame work is needed here. */
}

/* -- argv: detect --proof (crt0 hands us a SysV argc/argv) ------------------ */
static int has_proof_arg(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a) continue;
        if (a[0]=='-'&&a[1]=='-'&&a[2]=='p'&&a[3]=='r'&&a[4]=='o'&&
            a[5]=='o'&&a[6]=='f'&&a[7]==0) return 1;
    }
    return 0;
}

/* -- entry ------------------------------------------------------------------ */
/* crt0-linked (like filemanager): the kernel lays out argc/argv on the user
 * stack and crt0 calls main(argc, argv). argv[1..] may carry --proof. */
int main(int argc, char **argv) {
    out("[COCKPIT] starting\n");

    /* ---- CREATE + OWN the contract page (IPC_CREAT|0600), zero it, then set
     * magic LAST so agentd only ever observes a fully-initialised page. ---- */
    {
        long id = sc(SYS_SHMGET, (long)AGENTCOCKPIT_SHM_KEY,
                     (long)AGENTCOCKPIT_SHM_SIZE, IPC_CREAT | 0600);
        if (id >= 0) {
            long p = sc(SYS_SHMAT, id, 0, 0);
            if (p > 0) {
                g_shm = (volatile agentcockpit_shm_t *)p;
                /* zero the whole page */
                volatile unsigned char *b = (volatile unsigned char *)p;
                for (unsigned k = 0; k < AGENTCOCKPIT_SHM_SIZE; k++) b[k] = 0;
                g_shm->magic = AGENTCOCKPIT_MAGIC;         /* publish LAST */
            }
        }
    }

    /* ---- Build the window ---- */
    g_app = ui_app_create("Agent Cockpit", WIN_W, WIN_H);
    ui_widget_t *root = ui_app_root(g_app);

    ui_label(root, PAD, 12, "Agent Cockpit", COL_TEXT);
    ui_label(root, PAD + 130, 14,
             g_shm ? "seam: ready" : "seam: SHM unavailable",
             g_shm ? COL_GREEN : COL_RED);

    /* Goal input + RUN / STOP. */
    g_goalbox = ui_textbox(root, PAD, GOAL_Y, LOG_W - 8, UI_TEXTBOX_MAXBUF - 1);
    ui_button(root, PAD,        BTNROW_Y, 100, 30, "RUN",  on_run,  0);
    ui_button(root, PAD + 112,  BTNROW_Y, 100, 30, "STOP", on_stop, 0);

    /* Live step log. */
    g_log = ui_scroll(root, PAD, LOG_Y, LOG_W, LOG_H, COL_LOG_BG, CONTENT_H);
    log_line("", "Type a goal, then press RUN. The agent's steps appear here.",
             COL_DIM);

    /* Status line. */
    g_status = ui_label(root, PAD, STATUS_Y,
                        g_shm ? "Ready." : "SHM page unavailable -- cannot run.",
                        COL_MUTED);

    /* Confirmation controls: Allow / Deny (inert until state==CONFIRM) +
     * grant-full checkbox. */
    g_allow_btn = ui_button(root, PAD,        CTLROW_Y, 100, 28, "Allow", on_allow, 0);
    g_deny_btn  = ui_button(root, PAD + 112,  CTLROW_Y, 100, 28, "Deny",  on_deny,  0);
    ui_widget_set_bg(g_allow_btn, COL_LOG_BG);    /* greyed until a CONFIRM */
    ui_widget_set_bg(g_deny_btn,  COL_LOG_BG);
    ui_checkbox(root, PAD + 232, CTLROW_Y + 6, "grant full (auto-allow)", 0,
                on_grant_full, 0);

    /* ---- --proof: auto-fill a goal + auto-RUN headlessly ---- */
    if (g_shm && has_proof_arg(argc, argv)) {
        const char *pg = "List /etc and read /etc/toolset0.txt";
        ui_textbox_set_text(g_goalbox, pg);
        post_goal(pg);
    }

    /* ---- Drive the per-frame status pump. ---- */
    ui_app_set_tick(g_app, on_tick, 0);

    ui_app_run(g_app);   /* never returns */
    return 0;            /* unreachable -- satisfies int main() */
}

/*
 * claudechat.c -- CLAUDE-APP-0: a GUI chat window for Claude (freestanding, ring 3).
 * ==================================================================================
 *
 * A real desktop app: a scrollable transcript + a single-line input box + a
 * Send button. On Send it talks to the host Claude broker over the proven
 * slirp seam (the SAME path sbin/claudehost uses):
 *
 *     claudechat (this window) --TCP 10.0.2.2:8432--> scripts/claude_broker.py
 *                                                   --HTTPS--> api.anthropic.com
 *
 * The API key lives ONLY on the host broker -- it never enters this image. If
 * the broker / network is absent, the reply is a clear "broker offline" line
 * (connect fails fast under slirp) so the window stays usable at zero cost.
 *
 * Built like the other UI apps (clock, aiconsole): links ui.o + wl_client.o +
 * bitfont.o + font2.o. The toolkit (ui.h) owns the event loop; the Send
 * callback does a bounded, yield-friendly round trip. NOTE: the round trip is
 * synchronous, so a LIVE reply (broker up + key) briefly freezes the window
 * until Claude answers; the common keyless case returns instantly.
 *
 * No libc: pure inline syscalls + fixed static buffers.
 */

#include "../../lib/ui/ui.h"

/* -- syscalls (6-arg raw ABI, identical to claudehost.c / nc.c) ------------- */
#define SYS_WRITE      3
#define SYS_YIELD     15
#define SYS_SOCKET    51
#define SYS_CONNECT   52
#define SYS_SEND      53
#define SYS_RECV      54
#define SYS_CLOSE_SK  55
#define SYS_SOCK_POLL 58
#define SOCK_STREAM    1
#define EAGAIN_NEG  (-11)

#define BROKER_IP   0x0A000202u   /* 10.0.2.2 = the QEMU slirp host */
#define BROKER_PORT 8432
#define REPLY_CAP   8192
#define RECV_MAX    4000000       /* bounded; Claude can take tens of seconds */

static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }

/* -- window geometry -------------------------------------------------------- */
#define WIN_W     560
#define WIN_H     420
#define VP_X        8
#define VP_Y        8
#define VP_W      (WIN_W - 16)        /* 544 */
#define VP_H      344
#define CONTENT_H 4000               /* scroll capacity (~250 lines)          */
#define LINE_H     16                /* one 8x16 row + a hair of leading       */
#define WRAP_COLS  64                /* chars per transcript line (~VP_W/8)    */

/* -- app state -------------------------------------------------------------- */
#define LINES_PER_BOX 16             /* == the toolkit's UI_MAX_CHILDREN cap     */

static ui_app_t    *g_app;
static ui_widget_t *g_transcript;    /* ui_scroll viewport                     */
static ui_widget_t *g_input;         /* ui_textbox                             */
static ui_widget_t *g_status;        /* one-line hint/status label             */
static int          g_msg_y = 8;     /* y of the next transcript line          */
static ui_widget_t *g_line_box = 0;  /* container holding the current run of lines */
static int          g_box_lines = 0; /* lines in g_line_box                     */
static int          g_box_y = 8;     /* y of g_line_box within the scroll       */

static char g_reply[REPLY_CAP];

/* Append one already-short line to the transcript at the running y.
 * The toolkit caps EVERY widget at UI_MAX_CHILDREN (16) children, so attaching
 * every line directly to the single g_transcript scroll would silently DROP
 * lines past the 16th (and auto-scroll into blank space). Instead bucket lines
 * into transparent row CONTAINERS of <= LINES_PER_BOX each: the scroll holds at
 * most CONTENT_H/(LINES_PER_BOX*LINE_H) (~16) containers, each <= 16 labels --
 * both within the cap, so the full ~250-line CONTENT_H is usable. */
static void emit_line(const char *s, unsigned int color) {
    if (g_msg_y > CONTENT_H - LINE_H) return;   /* full -- drop silently       */
    if (!g_line_box || g_box_lines >= LINES_PER_BOX) {
        g_box_y    = g_msg_y;
        g_line_box = ui_panel(g_transcript, 0, g_box_y, VP_W - 12,
                              LINES_PER_BOX * LINE_H, 0xFF141416);  /* == scroll bg */
        g_box_lines = 0;
        if (!g_line_box) return;                 /* scroll full -- stop cleanly */
    }
    ui_label(g_line_box, 8, g_msg_y - g_box_y, s, color);
    g_box_lines++;
    g_msg_y += LINE_H;
}

/* Word-wrap + newline-split `text` into transcript lines, the first carrying
 * `prefix` (e.g. "You: " / "Claude: "). Bounded; never allocates. */
static void add_message(const char *prefix, const char *text, unsigned int color) {
    char line[WRAP_COLS + 1];
    int  col = 0;
    /* seed the prefix into the first line */
    for (const char *p = prefix; *p && col < WRAP_COLS; p++) line[col++] = *p;

    for (const char *p = text; *p; p++) {
        char c = *p;
        if (c == '\r') continue;
        if (c == '\n' || col >= WRAP_COLS) {
            line[col] = 0;
            emit_line(line, color);
            col = 0;
            if (c == '\n') continue;     /* the newline itself is consumed     */
        }
        line[col++] = c;
    }
    if (col > 0) { line[col] = 0; emit_line(line, color); }

    /* auto-scroll so the newest line is visible */
    int off = g_msg_y - (VP_H - LINE_H);
    if (off < 0) off = 0;
    ui_scroll_set_offset(g_transcript, off);
}

/* Bounded, yield-friendly send-all (claudehost pattern). */
static long send_all(long fd, const char *b, long len) {
    long off = 0; int guard = 0;
    while (off < len) {
        long n = sc(SYS_SEND, fd, (long)(b + off), len - off, 0, 0);
        if (n > 0) { off += n; guard = 0; continue; }
        if (n == EAGAIN_NEG) { sc(SYS_YIELD, 0, 0, 0, 0, 0); if (++guard > 200000) break; continue; }
        return n;
    }
    return off;
}

/* Round-trip one prompt to the broker; fills g_reply, returns byte count (>=0)
 * or a negative code if the broker is unreachable. */
static long ask_broker(const char *prompt) {
    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) return -1;
    long cr = sc(SYS_CONNECT, fd, (long)BROKER_IP, BROKER_PORT, 0, 0);
    if (cr < 0) { sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0); return -2; }

    send_all(fd, prompt, (long)slen(prompt));
    send_all(fd, "\n", 1);

    long total = 0;
    for (long it = 0; it < RECV_MAX && total < REPLY_CAP - 1; it++) {
        sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
        long rn = sc(SYS_RECV, fd, (long)(g_reply + total), REPLY_CAP - 1 - total, 0, 0);
        if (rn > 0) { total += rn; continue; }
        if (rn == 0) break;                                   /* broker closed */
        if (rn == EAGAIN_NEG) { sc(SYS_YIELD, 0, 0, 0, 0, 0); continue; }
        break;
    }
    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    g_reply[total] = 0;
    return total;
}

/* -- Send button --------------------------------------------------------- */
static void on_send(void *ud) {
    (void)ud;
    const char *txt = ui_textbox_text(g_input);
    if (!txt || !txt[0]) return;

    /* Snapshot the prompt before we clear the box (ask_broker may yield). */
    static char prompt[UI_TEXTBOX_MAXBUF];
    int i = 0;
    for (; txt[i] && i < (int)sizeof(prompt) - 1; i++) prompt[i] = txt[i];
    prompt[i] = 0;

    add_message("You: ", prompt, 0xFF9CDCFE);     /* light blue                */
    ui_textbox_set_text(g_input, "");
    ui_label_set_text(g_status, "Claude is thinking...");

    long n = ask_broker(prompt);
    if (n == -2)
        add_message("Claude: ", "[broker offline] start `python3 scripts/claude_broker.py` "
                                "on the host and boot with -netdev user -device e1000.",
                    0xFFFF9E64);
    else if (n <= 0)
        add_message("Claude: ", "[no reply from broker]", 0xFFFF9E64);
    else
        add_message("Claude: ", g_reply, 0xFFD7FFD7);          /* soft green   */

    ui_label_set_text(g_status, "Type a message and press Send.");
}

void _start(void) {
    sc(SYS_WRITE, 1, (long)"[CLAUDECHAT] starting\n", 22, 0, 0);

    g_app = ui_app_create("Claude Chat", WIN_W, WIN_H);
    ui_widget_t *root = ui_app_root(g_app);

    /* Transcript: a dark scroll viewport filling the top. */
    g_transcript = ui_scroll(root, VP_X, VP_Y, VP_W, VP_H, 0xFF141416, CONTENT_H);

    /* Greeting (rendered into the transcript). */
    add_message("Claude: ", "Hi! I'm Claude, running as a native AutomationOS app. "
                            "Ask me anything and press Send. (Replies route through "
                            "the host broker; the API key never enters the OS.)",
                0xFFD7FFD7);

    /* Input row. */
    g_input  = ui_textbox(root, VP_X, VP_Y + VP_H + 8, VP_W - 96, UI_TEXTBOX_MAXBUF - 1);
    ui_button(root, VP_X + VP_W - 88, VP_Y + VP_H + 6, 88, 28, "Send", on_send, 0);

    g_status = ui_label(root, VP_X, VP_Y + VP_H + 34, "Type a message and press Send.",
                        0xFFAEAEB2);

    ui_app_run(g_app);   /* never returns */
}

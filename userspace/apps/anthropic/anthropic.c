/*
 * anthropic.c -- CLAUDE-APP-0: the Anthropic control panel (freestanding, ring 3).
 * =================================================================================
 *
 * A small desktop app that is the "home" for Claude on AutomationOS:
 *   - shows whether the host Claude broker is reachable (probes 10.0.2.2:8432),
 *   - shows the model the broker uses by default,
 *   - launches the Claude chat window, the AI console, and the claudehost test.
 *
 * The broker is the SAME slirp seam claudechat / claudehost use; the API key
 * never enters this image (it lives only on the host broker). Probing is a
 * cheap connect()/close() -- under slirp a missing broker fails fast, so the
 * panel stays responsive at zero cost.
 *
 * Built like the other UI apps: links ui.o + wl_client.o + bitfont.o + font2.o.
 * No libc: pure inline syscalls + fixed static buffers.
 */

#include "../../lib/ui/ui.h"

/* -- syscalls (6-arg raw ABI) ----------------------------------------------- */
#define SYS_WRITE      3
#define SYS_SPAWN     16
#define SYS_SOCKET    51
#define SYS_CONNECT   52
#define SYS_CLOSE_SK  55
#define SOCK_STREAM    1

#define BROKER_IP   0x0A000202u   /* 10.0.2.2 = the QEMU slirp host */
#define BROKER_PORT 8432

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
static void out(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s), 0, 0); }

/* -- app state -------------------------------------------------------------- */
static ui_widget_t *g_broker_lbl;

/* Probe the broker: connect then immediately close. Returns 1 if reachable. */
static int broker_online(void) {
    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) return 0;
    long cr = sc(SYS_CONNECT, fd, (long)BROKER_IP, BROKER_PORT, 0, 0);
    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    return cr >= 0;
}

static void refresh_status(void) {
    if (broker_online()) {
        ui_label_set_text(g_broker_lbl, "Broker: ONLINE  (10.0.2.2:8432)");
        ui_widget_set_fg(g_broker_lbl, 0xFF7CE38B);            /* green        */
    } else {
        ui_label_set_text(g_broker_lbl, "Broker: offline -- run claude_broker.py");
        ui_widget_set_fg(g_broker_lbl, 0xFFFF9E64);            /* amber        */
    }
}

/* -- button callbacks ------------------------------------------------------- */
static void cb_refresh(void *ud) { (void)ud; refresh_status(); }

/* Generic launcher: ud is the "sbin/<app>" path string. */
static void cb_spawn(void *ud) {
    const char *path = (const char *)ud;
    out("[ANTHROPIC] launch ");
    out(path);
    out("\n");
    sc(SYS_SPAWN, (long)path, 0, 0, 0, 0);
}

void _start(void) {
    out("[ANTHROPIC] starting\n");

    ui_app_t    *app  = ui_app_create("Anthropic", 380, 320);
    ui_widget_t *root = ui_app_root(app);

    ui_label(root, 16, 16, "Anthropic Control Panel", 0xFFFFFFFF);
    ui_label(root, 16, 40, "Claude, native on AutomationOS.", 0xFFAEAEB2);

    /* Broker status (probed now + via Refresh). */
    g_broker_lbl = ui_label(root, 16, 74, "Broker: checking...", 0xFFAEAEB2);
    ui_label(root, 16, 96, "Default model: claude-haiku-4-5", 0xFF8E8E93);
    ui_label(root, 16, 114, "(key stays on the host broker)", 0xFF8E8E93);

    /* Actions. */
    ui_button(root, 16, 148, 348, 34, "Open Claude Chat",  cb_spawn, (void *)"sbin/claudechat");
    ui_button(root, 16, 190, 348, 34, "Open AI Console",   cb_spawn, (void *)"sbin/aiconsole");
    ui_button(root, 16, 232, 348, 34, "Run claudehost test", cb_spawn, (void *)"sbin/claudehost");
    ui_button(root, 16, 274, 168, 34, "Refresh status",    cb_refresh, 0);

    refresh_status();

    ui_app_run(app);   /* never returns */
}

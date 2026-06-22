/*
 * netman.c -- Network Manager GUI app (freestanding, ring 3).
 * ===========================================================
 *
 * A windowed, ANIMATED WiFi manager for AutomationOS, built on the M4
 * retained-mode UI toolkit (userspace/lib/ui) over the M3 "Wayland-lite"
 * compositor stack and the 8x16 bitmap font. It presents a modern OS-style
 * network panel:
 *
 *   - Title "Network Manager" + an animated iOS-style Wi-Fi ui_toggle.
 *   - Live NIC config read from the kernel net stack each frame
 *     (SYS_NET_INFO=59): Status / MAC / IPv4 / Gateway.
 *   - A scrollable list of scanned WiFi networks (SYS_WLAN_SCAN=113 every
 *     ~3s). Each row shows an animated ui_signal_bars meter (derived from the
 *     dBm signal), a lock glyph for WPA2/WPA3 networks, and the SSID. The row
 *     for the currently-connected SSID gets an accent + checkmark.
 *   - Tapping a SECURED row opens a modal passphrase dialog: the window dims
 *     behind a rounded, shadowed panel with a masked text field and
 *     Connect/Cancel buttons. Tapping an OPEN row connects immediately.
 *   - On Connect: SYS_WLAN_CONNECT(114) then spawn sbin/wpasupp (SYS_SPAWN=16,
 *     args "<ssid> <security> <passphrase>") to run the 4-way + DHCP.
 *   - While SYS_WLAN_STATUS(115) reports ASSOCIATING/4WAY a ui_spinner +
 *     "Connecting to <ssid>..." is shown; on CONNECTED the spinner hides and
 *     the row is marked connected. The IPv4 label shows the DHCP lease once it
 *     lands.
 *   - A DNS resolve field (text input + Resolve button) at the bottom.
 *
 * No libc: pure inline syscalls + tiny freestanding helpers + the OS UI/wl/
 * font/dns libraries. Fixed-size buffers, bounded loops, integer-only (no
 * float -- the toolkit owns the Q8 easing).
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/netman/netman.c -o netman.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       netman.o ui.o wl_client.o bitfont.o font2.o dns.o -o build/netman
 *   objdump -d build/netman | grep fs:0x28   # MUST be empty (no canary)
 *
 * Serial output (fd 1, for the boot smoke test):
 *   [NETMAN] window created
 *   [NETMAN] link=UP ip=10.0.2.15        (or link=DOWN ip=0.0.0.0)
 *   [NETMAN] scan <N> networks           (when the scan count changes)
 *   [NETMAN] connect ssid=<ssid>
 *   [NETMAN] connected ssid=<ssid>
 */

#include "../../lib/ui/ui.h"
#include "../../lib/net/dns.h"
#include "../../../kernel/include/uapi/wlan.h"   /* header-only SYS_WLAN_* ABI */

/* -----------------------------------------------------------------------
 * Syscall numbers and inline syscall helper.
 * --------------------------------------------------------------------- */
#define SYS_WRITE       3
#define SYS_SPAWN       16   /* sc(16, path, args, ...) -> pid (kernel splits args) */
#define SYS_NET_INFO    59   /* sc(59,&net_info,...) -> 0 ok, <0 if net down */
#define SYS_WLAN_SCAN   113  /* sc(113, &bss[], max) -> count                */
#define SYS_WLAN_CONNECT 114 /* sc(114, &uapi_wlan_connect_t) -> 0/err       */
#define SYS_WLAN_STATUS 115  /* sc(115, &uapi_wlan_status_t) -> 0/err        */
#define SYS_WLAN_DIAG   118  /* sc(118, &uapi_wlan_diag_t) -> 0/err          */

/*
 * 6-arg raw inline syscall wrapper.
 *   nr -> rax ; args -> rdi, rsi, rdx, r10, r8 ; ret -> rax
 */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Kernel payload for SYS_NET_INFO -- MUST match kernel net_info_ext_t
 * (kernel/include/netif.h). ip / gateway are in HOST byte order.
 * --------------------------------------------------------------------- */
typedef struct {
    char           ifname[16];
    unsigned char  mac[6];
    unsigned char  _pad[2];
    unsigned int   ip;
    unsigned int   netmask;
    unsigned int   gateway;
    unsigned int   dns;
    unsigned char  up;
    unsigned char  dhcp_active;
    unsigned char  _pad2[6];
    unsigned long long tx_packets;
    unsigned long long rx_packets;
    unsigned long long tx_bytes;
    unsigned long long rx_bytes;
} net_info_t;

/* -----------------------------------------------------------------------
 * Freestanding helpers (no libc).
 * --------------------------------------------------------------------- */

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void serial_print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0);
}

/* Append NUL-terminated src to dst (already NUL-terminated). Returns new len. */
static int str_append(char *dst, int len, const char *src)
{
    while (*src) dst[len++] = *src++;
    dst[len] = '\0';
    return len;
}

/* Append a 1-3 digit unsigned decimal byte (0..255) at dst[len]. */
static int append_u8_dec(char *dst, int len, unsigned int v)
{
    char tmp[4];
    int  i = 0;
    if (v > 255) v = 255;
    do { tmp[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i > 0) dst[len++] = tmp[--i];
    return len;
}

/* Append a small signed decimal int at dst[len]. */
static int append_int(char *dst, int len, int v)
{
    char tmp[12];
    int  i = 0;
    unsigned int u;
    if (v < 0) { dst[len++] = '-'; u = (unsigned int)(-v); }
    else        u = (unsigned int)v;
    do { tmp[i++] = (char)('0' + (u % 10)); u /= 10; } while (u);
    while (i > 0) dst[len++] = tmp[--i];
    dst[len] = '\0';
    return len;
}

/* Append a 2-digit lowercase hex byte at dst[len]. */
static int append_u8_hex(char *dst, int len, unsigned int v)
{
    static const char H[] = "0123456789abcdef";
    dst[len++] = H[(v >> 4) & 0xF];
    dst[len++] = H[v & 0xF];
    return len;
}

/*
 * Format a host-order IPv4 address as "a.b.c.d" (NUL-terminated).
 * buf must be >= 16 bytes.  Big-endian octet order: byte 31..24 is "a".
 */
static void fmt_ipv4(char *buf, unsigned int ip)
{
    int len = 0;
    len = append_u8_dec(buf, len, (ip >> 24) & 0xFF);
    buf[len++] = '.';
    len = append_u8_dec(buf, len, (ip >> 16) & 0xFF);
    buf[len++] = '.';
    len = append_u8_dec(buf, len, (ip >> 8) & 0xFF);
    buf[len++] = '.';
    len = append_u8_dec(buf, len, ip & 0xFF);
    buf[len]   = '\0';
}

/*
 * Format a 6-byte MAC as "xx:xx:xx:xx:xx:xx" (NUL-terminated).
 * buf must be >= 18 bytes.
 */
static void fmt_mac(char *buf, const unsigned char mac[6])
{
    int len = 0;
    for (int i = 0; i < 6; i++) {
        if (i) buf[len++] = ':';
        len = append_u8_hex(buf, len, mac[i]);
    }
    buf[len] = '\0';
}

/* Compare two NUL-terminated strings for equality. */
static int str_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

/* -----------------------------------------------------------------------
 * Application state.
 * --------------------------------------------------------------------- */

#define MAX_NETS  8       /* networks displayed (>= UI list capacity headroom) */

/* Live status labels (updated by the tick callback). */
static ui_widget_t *g_link_label = 0;
static ui_widget_t *g_mac_label  = 0;
static ui_widget_t *g_ip_label   = 0;
static ui_widget_t *g_gw_label   = 0;
static ui_widget_t *g_wifi_toggle = 0;
static ui_widget_t *g_diag_label = 0;   /* radio bring-up diagnostics (no serial) */

/* Network list (scroll view) + per-row widgets. */
static ui_widget_t *g_list       = 0;   /* ui_scroll viewport            */
static ui_widget_t *g_row_panel[MAX_NETS];  /* container panel per row   */
static ui_widget_t *g_row_listrow[MAX_NETS];/* clickable list_row        */
static ui_widget_t *g_row_bars[MAX_NETS];   /* animated signal bars      */
static ui_widget_t *g_row_check[MAX_NETS];  /* connected-checkmark label */
static int          g_nrows = 0;        /* number of populated rows      */

/* Cached scan results (so a row click knows its SSID/security). */
static char          g_ssid[MAX_NETS][33];
static unsigned char g_sec[MAX_NETS];
static int           g_scan_count = -1; /* last reported count (-1 = none)*/

/* Connecting status. */
static ui_widget_t *g_spinner      = 0;   /* created/freed on demand     */
static ui_widget_t *g_conn_label   = 0;   /* "Connecting to ..." label   */
static char          g_connecting_ssid[33];
static int           g_connecting = 0;    /* 1 while a connect is in flight */
static char          g_connected_ssid[33];/* SSID we believe is connected   */

/* Modal passphrase dialog (created/freed on demand). */
static ui_widget_t *g_modal_root = 0;     /* whole modal subtree (dim+panel)*/
static ui_widget_t *g_modal_pass = 0;     /* masked textbox                 */
static int          g_modal_open = 0;
static char         g_modal_ssid[33];
static unsigned char g_modal_sec = 0;

/* Whether we've already emitted the one-shot smoke-test serial line. */
static int g_smoke_emitted = 0;

/* Scan pacing (frame counter; ~60fps -> ~180 frames is ~3s). */
static int g_frames = 0;

/* App handle (needed to attach the modal to the root on demand). */
static ui_app_t    *g_app  = 0;
static ui_widget_t *g_root = 0;

/* Colors (ARGB) consistent with the Aether Dark theme. */
#define COL_TITLE     0xFFFFFFFFu
#define COL_DIM       0xFFAEAEB2u
#define COL_ACCENT    0xFF0A84FFu
#define COL_GREEN     0xFF30D158u
#define COL_LOCK      0xFFFFD60Au
#define COL_DIMLAYER  0xFF0B0B0Du   /* opaque-dark "dim" backdrop          */
#define COL_DIALOG    0xFF2C2C2Eu   /* dialog panel fill                   */
#define COL_SHADOW    0xFF101012u   /* faux drop-shadow under the dialog   */
#define COL_FIELD     0xFF1C1C1Eu

/* -----------------------------------------------------------------------
 * Forward declarations.
 * --------------------------------------------------------------------- */
static void begin_connect(const char *ssid, unsigned char sec, const char *pass);
static void close_modal(void);

/* -----------------------------------------------------------------------
 * Read NIC info and refresh the status labels.
 * --------------------------------------------------------------------- */
static void refresh_status(void)
{
    net_info_t info;
    /* Zero the struct so DOWN renders sane (all-zero) addresses. */
    for (unsigned i = 0; i < sizeof(info); i++) ((unsigned char *)&info)[i] = 0;

    long rc = sc(SYS_NET_INFO, (long)&info, 0, 0, 0, 0);
    int  up = (rc == 0);

    char macbuf[18];
    char ipbuf[16];
    char gwbuf[16];

    if (up) {
        ui_label_set_text(g_link_label, "Status: UP");
        ui_widget_set_fg(g_link_label, COL_GREEN);
        fmt_mac(macbuf, info.mac);
        fmt_ipv4(ipbuf, info.ip);
        fmt_ipv4(gwbuf, info.gateway);
    } else {
        ui_label_set_text(g_link_label, "Status: DOWN (no NIC)");
        ui_widget_set_fg(g_link_label, COL_DIM);
        macbuf[0] = '-'; macbuf[1] = '\0';
        ipbuf[0]  = '-'; ipbuf[1]  = '\0';
        gwbuf[0]  = '-'; gwbuf[1]  = '\0';
    }

    /* "MAC: <mac>" / "IPv4: <ip>" / "Gateway: <gw>" into fixed buffers. */
    char line[40];
    int  len;

    len = str_append(line, 0, "MAC: ");
    str_append(line, len, macbuf);
    ui_label_set_text(g_mac_label, line);

    len = str_append(line, 0, "IPv4: ");
    str_append(line, len, ipbuf);
    ui_label_set_text(g_ip_label, line);

    len = str_append(line, 0, "Gateway: ");
    str_append(line, len, gwbuf);
    ui_label_set_text(g_gw_label, line);

    /* One-shot smoke-test line: [NETMAN] link=UP ip=10.0.2.15 */
    if (!g_smoke_emitted) {
        char sline[48];
        int  sl = str_append(sline, 0, "[NETMAN] link=");
        sl = str_append(sline, sl, up ? "UP" : "DOWN");
        sl = str_append(sline, sl, " ip=");
        sl = str_append(sline, sl, up ? ipbuf : "0.0.0.0");
        sline[sl++] = '\n';
        sline[sl]   = '\0';
        serial_print(sline);
        g_smoke_emitted = 1;
    }
}

/* -----------------------------------------------------------------------
 * Map a dBm signal level to 1..4 strength bars.
 * --------------------------------------------------------------------- */
static int dbm_to_bars(int dbm)
{
    if (dbm > -50) return 4;
    if (dbm > -60) return 3;
    if (dbm > -70) return 2;
    return 1;
}

/* -----------------------------------------------------------------------
 * Row click callbacks. Each row stores its index in its `ud` (as an int
 * encoded in a pointer); the callback recovers the index and acts on the
 * cached scan entry.
 * --------------------------------------------------------------------- */
static void on_row_click(void *ud)
{
    long idx = (long)ud;
    if (idx < 0 || idx >= g_nrows) return;
    if (g_modal_open) return;            /* modal eats interaction          */

    const char   *ssid = g_ssid[idx];
    unsigned char sec   = g_sec[idx];

    if (sec == UAPI_WLAN_SEC_OPEN) {
        /* Open network: connect immediately, no passphrase needed. */
        begin_connect(ssid, sec, "");
        return;
    }

    /* Secured: open the modal passphrase dialog. */
    {
        int i = 0;
        while (ssid[i] && i < 32) { g_modal_ssid[i] = ssid[i]; i++; }
        g_modal_ssid[i] = '\0';
    }
    g_modal_sec = sec;

    /* Build the modal subtree as the LAST child of root (rendered on top). */
    int W = 420, H = 440;     /* window size (pre-scale, see _start)         */
    g_modal_root = ui_panel(g_root, 0, 0, W, H, COL_DIMLAYER);   /* dim layer */
    if (!g_modal_root) return;

    /* Faux drop shadow (offset dark panel) then the dialog panel on top. */
    int dw = 300, dh = 180;
    int dx = (W - dw) / 2, dy = (H - dh) / 2;
    ui_panel(g_modal_root, dx + 6, dy + 8, dw, dh, COL_SHADOW);
    ui_widget_t *dlg = ui_panel(g_modal_root, dx, dy, dw, dh, COL_DIALOG);
    if (!dlg) { close_modal(); return; }

    /* Title + SSID. */
    ui_label(dlg, 16, 14, "Enter passphrase", COL_TITLE);
    {
        char tline[48];
        int  tl = str_append(tline, 0, "Network: ");
        tl = str_append(tline, tl, g_modal_ssid);
        ui_label(dlg, 16, 40, tline, COL_DIM);
    }

    /* Masked passphrase field. */
    g_modal_pass = ui_textbox(dlg, 16, 66, dw - 32, 63);
    if (g_modal_pass) ui_textbox_set_mask(g_modal_pass, 1);

    /* Connect / Cancel buttons (callbacks below). */
    extern void on_modal_connect(void *ud);
    extern void on_modal_cancel(void *ud);
    ui_button(dlg, 16,        dh - 44, 130, 30, "Connect", on_modal_connect, 0);
    ui_button(dlg, dw - 146,  dh - 44, 130, 30, "Cancel",  on_modal_cancel,  0);

    g_modal_open = 1;
}

/* -----------------------------------------------------------------------
 * Modal dialog button callbacks (external linkage so on_row_click can take
 * their addresses before they are defined).
 * --------------------------------------------------------------------- */
void on_modal_connect(void *ud)
{
    (void)ud;
    if (!g_modal_open) return;
    const char *pass = g_modal_pass ? ui_textbox_text(g_modal_pass) : "";
    char ssid[33];
    {
        int i = 0;
        while (g_modal_ssid[i] && i < 32) { ssid[i] = g_modal_ssid[i]; i++; }
        ssid[i] = '\0';
    }
    unsigned char sec = g_modal_sec;
    close_modal();
    begin_connect(ssid, sec, pass);
}

void on_modal_cancel(void *ud)
{
    (void)ud;
    close_modal();
}

static void close_modal(void)
{
    if (g_modal_root) {
        ui_widget_detach(g_root, g_modal_root);
        ui_widget_free_tree(g_modal_root);
        g_modal_root = 0;
        g_modal_pass = 0;
    }
    g_modal_open = 0;
}

/* -----------------------------------------------------------------------
 * Kick off a connect: SYS_WLAN_CONNECT then spawn the supplicant, and put
 * the UI into the "connecting" state (spinner + label).
 * --------------------------------------------------------------------- */
static void begin_connect(const char *ssid, unsigned char sec, const char *pass)
{
    /* Build the connect payload. */
    uapi_wlan_connect_t conn;
    for (unsigned i = 0; i < sizeof(conn); i++) ((unsigned char *)&conn)[i] = 0;

    int sl = 0;
    while (ssid[sl] && sl < 32) { conn.ssid[sl] = (uint8_t)ssid[sl]; sl++; }
    conn.ssid_len = (uint8_t)sl;
    conn.security = sec;
    if (sec != UAPI_WLAN_SEC_OPEN) {
        int pi = 0;
        while (pass[pi] && pi < 63) { conn.passphrase[pi] = pass[pi]; pi++; }
        conn.passphrase[pi] = '\0';
    }
    sc(SYS_WLAN_CONNECT, (long)&conn, 0, 0, 0, 0);

    /* Spawn the supplicant: "sbin/wpasupp" with "<ssid> <security> <pass>". */
    {
        char args[160];
        int  a = 0;
        a = str_append(args, a, ssid);
        args[a++] = ' ';
        a = append_int(args, a, (int)sec);
        if (sec != UAPI_WLAN_SEC_OPEN) {
            args[a++] = ' ';
            a = str_append(args, a, pass);
        }
        args[a] = '\0';
        sc(SYS_SPAWN, (long)"sbin/wpasupp", (long)args, 0, 0, 0);
    }

    /* Remember which SSID we are joining and flag the UI as connecting. */
    {
        int i = 0;
        while (ssid[i] && i < 32) { g_connecting_ssid[i] = ssid[i]; i++; }
        g_connecting_ssid[i] = '\0';
    }
    g_connecting = 1;
    g_connected_ssid[0] = '\0';

    /* Serial marker. */
    {
        char sline[64];
        int  s = str_append(sline, 0, "[NETMAN] connect ssid=");
        s = str_append(sline, s, ssid);
        sline[s++] = '\n';
        sline[s]   = '\0';
        serial_print(sline);
    }
}

/* -----------------------------------------------------------------------
 * Show / hide the connecting spinner + label.  The spinner self-animates
 * from the toolkit clock once created; we create it lazily and free it when
 * the connect settles so it stops spinning.
 * --------------------------------------------------------------------- */
static void show_connecting(const char *ssid)
{
    if (!g_spinner) {
        g_spinner = ui_spinner(g_root, 20, 388, 18);
    }
    if (!g_conn_label) {
        g_conn_label = ui_label(g_root, 48, 388, "", COL_ACCENT);
    }
    char line[56];
    int  l = str_append(line, 0, "Connecting to ");
    l = str_append(line, l, ssid);
    l = str_append(line, l, "...");
    ui_label_set_text(g_conn_label, line);
}

static void hide_connecting(void)
{
    if (g_spinner) {
        ui_widget_detach(g_root, g_spinner);
        ui_widget_free_tree(g_spinner);
        g_spinner = 0;
    }
    if (g_conn_label) {
        ui_label_set_text(g_conn_label, "");
    }
}

/* -----------------------------------------------------------------------
 * Poll SYS_WLAN_STATUS and update the connecting/connected UI.
 * --------------------------------------------------------------------- */
static void poll_wlan_status(void)
{
    if (!g_connecting && g_connected_ssid[0] == '\0') {
        hide_connecting();
        return;
    }

    uapi_wlan_status_t st;
    for (unsigned i = 0; i < sizeof(st); i++) ((unsigned char *)&st)[i] = 0;
    long rc = sc(SYS_WLAN_STATUS, (long)&st, 0, 0, 0, 0);
    if (rc != 0) { return; }

    if (st.state == UAPI_WLAN_ST_CONNECTED) {
        /* Settle: hide spinner, record connected SSID, mark the row. */
        if (g_connecting) {
            char sline[64];
            int  s = str_append(sline, 0, "[NETMAN] connected ssid=");
            s = str_append(sline, s, g_connecting_ssid);
            sline[s++] = '\n';
            sline[s]   = '\0';
            serial_print(sline);

            int i = 0;
            while (g_connecting_ssid[i] && i < 32) {
                g_connected_ssid[i] = g_connecting_ssid[i]; i++;
            }
            g_connected_ssid[i] = '\0';
            g_connecting = 0;
        }
        hide_connecting();

        /* Mark the matching row connected (accent + checkmark glyph). */
        for (int r = 0; r < g_nrows; r++) {
            int hit = str_eq(g_ssid[r], g_connected_ssid);
            if (g_row_listrow[r])
                ui_list_row_set_selected(g_row_listrow[r], hit ? 1 : 0);
            if (g_row_check[r])
                ui_label_set_text(g_row_check[r], hit ? "\x07" : "");
        }
    } else if (st.state == UAPI_WLAN_ST_ASSOCIATING ||
               st.state == UAPI_WLAN_ST_4WAY        ||
               st.state == UAPI_WLAN_ST_ASSOCIATED  ||
               st.state == UAPI_WLAN_ST_AUTHENTICATING) {
        /* In flight: keep the spinner + label visible. */
        if (g_connecting) show_connecting(g_connecting_ssid);
    }
}

/* -----------------------------------------------------------------------
 * Rebuild the network-list rows from the cached scan results. Each row is a
 * container panel holding a clickable list_row (full-width), animated signal
 * bars, a lock glyph for secured nets, and a connected-checkmark slot.
 *
 * Rows are created ONCE (first non-empty scan) and then their contents are
 * updated in place, so the list is stable and the modal/selection survive a
 * re-scan. The number of rows is fixed at the first populated count (bounded
 * by MAX_NETS), which is fine for the demo wifisim AP set.
 * --------------------------------------------------------------------- */
static void build_rows(int count)
{
    if (count > MAX_NETS) count = MAX_NETS;
    if (g_nrows != 0) return;       /* rows already built; updated in place  */
    if (count <= 0) return;

    int row_h = 36;
    for (int i = 0; i < count; i++) {
        /* Container panel for this row (1 child of the scroll list). */
        ui_widget_t *p = ui_panel(g_list, 4, i * row_h + 2, 372, row_h - 4,
                                  COL_FIELD);
        if (!p) break;
        g_row_panel[i] = p;

        /* Clickable list_row covers the whole panel; SSID as its caption. */
        ui_widget_t *lr = ui_list_row(p, 0, 0, 372, row_h - 4, g_ssid[i],
                                      on_row_click, (void *)(long)i);
        g_row_listrow[i] = lr;

        /* Animated signal bars on the left. */
        g_row_bars[i] = ui_signal_bars(p, 8, 8, dbm_to_bars(-65));

        /* Lock glyph for secured networks (label, drawn over the row). */
        if (g_sec[i] != UAPI_WLAN_SEC_OPEN)
            ui_label(p, 40, 8, "\x0E", COL_LOCK);   /* music-note-ish padlock */

        /* Connected-checkmark slot (filled in poll_wlan_status). */
        g_row_check[i] = ui_label(p, 352, 8, "", COL_GREEN);

        g_nrows++;
    }
}

/* -----------------------------------------------------------------------
 * Radio bring-up diagnostics (SYS_WLAN_DIAG=118). Surfaces the kernel IWL*
 * bring-up state ON-SCREEN so the user can see WHERE the radio stopped on the
 * real T410 -- no serial cable needed. Updated every tick.
 * --------------------------------------------------------------------- */
static void poll_diag(void)
{
    if (!g_diag_label) return;

    uapi_wlan_diag_t d;
    for (unsigned i = 0; i < sizeof(d); i++) ((unsigned char *)&d)[i] = 0;
    long r = sc(SYS_WLAN_DIAG, (long)&d, 0, 0, 0, 0);
    if (r != 0 || !d.present) {
        ui_label_set_text(g_diag_label, "Radio: no Wi-Fi backend (run iwlup on the T410)");
        return;
    }

    /* "Radio: <card> -- <msg> (nets=N)". d.msg is the actionable line the kernel
     * set (firmware missing / RF-kill ON / wlan0 LIVE / scan complete ...). */
    char line[180];
    int  l = 0;
    line[0] = '\0';
    l = str_append(line, l, "Radio: ");
    l = str_append(line, l, d.card[0] ? d.card : "Wi-Fi");
    if (d.msg[0]) {
        l = str_append(line, l, " -- ");
        l = str_append(line, l, d.msg);
    }
    if (d.last_scan_bss >= 0) {
        l = str_append(line, l, "  (nets=");
        l = append_int(line, l, (int)d.last_scan_bss);
        l = str_append(line, l, ")");
    }
    line[l] = '\0';
    ui_label_set_text(g_diag_label, line);
}

/* -----------------------------------------------------------------------
 * Run a WiFi scan and refresh the list. Called every ~3s from the tick.
 * --------------------------------------------------------------------- */
static void do_scan(void)
{
    uapi_wlan_bss_t bss[MAX_NETS];
    for (unsigned i = 0; i < sizeof(bss); i++) ((unsigned char *)bss)[i] = 0;

    long count = sc(SYS_WLAN_SCAN, (long)bss, MAX_NETS, 0, 0, 0);
    if (count < 0) count = 0;
    if (count > MAX_NETS) count = MAX_NETS;

    /* Cache SSID/security and update the per-row widgets. */
    for (int i = 0; i < (int)count; i++) {
        int n = bss[i].ssid_len;
        if (n > 32) n = 32;
        int j = 0;
        for (; j < n; j++) g_ssid[i][j] = (char)bss[i].ssid[j];
        g_ssid[i][j] = '\0';
        if (g_ssid[i][0] == '\0') {
            g_ssid[i][0] = '('; g_ssid[i][1] = '?'; g_ssid[i][2] = ')';
            g_ssid[i][3] = '\0';
        }
        g_sec[i] = bss[i].security;
    }

    /* Build the rows once; afterwards refresh captions + bars in place. */
    build_rows((int)count);
    for (int i = 0; i < g_nrows && i < (int)count; i++) {
        if (g_row_bars[i])
            ui_signal_bars_set(g_row_bars[i], dbm_to_bars(bss[i].signal));
        /* (list_row text is set at creation; SSIDs are stable in wifisim.) */
    }

    /* Report the count when it changes. */
    if ((int)count != g_scan_count) {
        g_scan_count = (int)count;
        char sline[40];
        int  s = str_append(sline, 0, "[NETMAN] scan ");
        s = append_int(sline, s, (int)count);
        s = str_append(sline, s, " networks");
        sline[s++] = '\n';
        sline[s]   = '\0';
        serial_print(sline);
    }
}

/* -----------------------------------------------------------------------
 * Wi-Fi master toggle: turning it off clears any connecting state; turning
 * it on triggers an immediate scan next tick.
 * --------------------------------------------------------------------- */
static void on_wifi_toggle(int state, void *ud)
{
    (void)ud;
    if (g_modal_open) return;            /* ignore stray clicks behind modal */
    if (!state) {
        /* Off: stop showing connect progress. */
        g_connecting = 0;
        g_connected_ssid[0] = '\0';
        hide_connecting();
        if (g_modal_open) close_modal();
        serial_print("[NETMAN] wifi off\n");
    } else {
        g_frames = 0;           /* force a scan on the next tick */
        serial_print("[NETMAN] wifi on\n");
    }
}

/* -----------------------------------------------------------------------
 * Per-frame tick: keep the NIC status live, pace WiFi scans, poll status.
 * --------------------------------------------------------------------- */
static void tick_cb(void *ud)
{
    (void)ud;
    refresh_status();

    int wifi_on = g_wifi_toggle ? ui_toggle_on(g_wifi_toggle) : 1;

    if (wifi_on) {
        /* Scan immediately on the first tick, then every ~180 frames (~3s). */
        if (g_frames == 0 || (g_frames % 180) == 0) do_scan();
        poll_wlan_status();
    }
    poll_diag();   /* keep the radio bring-up diagnostics line live (always) */
    g_frames++;
}

/* -----------------------------------------------------------------------
 * "Resolve" button: look up the hostname in the text field.
 * --------------------------------------------------------------------- */
static ui_widget_t *g_dns_input  = 0;
static ui_widget_t *g_dns_result = 0;

static void on_resolve(void *ud)
{
    (void)ud;

    const char *host = ui_textbox_text(g_dns_input);

    /* Empty field -> demonstrate the path with a hardcoded query. */
    if (!host || host[0] == '\0') host = "10.0.2.3";

    unsigned int ip = 0;
    int rc = dns_resolve(host, &ip);

    if (rc == 0) {
        char buf[40];
        char ipbuf[16];
        fmt_ipv4(ipbuf, ip);
        int len = str_append(buf, 0, "-> ");
        str_append(buf, len, ipbuf);
        ui_label_set_text(g_dns_result, buf);

        char sline[64];
        int  sl = str_append(sline, 0, "[NETMAN] resolve ");
        sl = str_append(sline, sl, host);
        sl = str_append(sline, sl, " -> ");
        sl = str_append(sline, sl, ipbuf);
        sline[sl++] = '\n';
        sline[sl]   = '\0';
        serial_print(sline);
    } else {
        ui_label_set_text(g_dns_result, "resolve failed");
        serial_print("[NETMAN] resolve failed\n");
    }
}

/* -----------------------------------------------------------------------
 * Entry point.
 * --------------------------------------------------------------------- */
void _start(void)
{
    /* Window: 420 x 440, titled "Network Manager". */
    ui_app_t    *app  = ui_app_create("Network Manager", 420, 440);
    ui_widget_t *root = ui_app_root(app);
    g_app  = app;
    g_root = root;

    serial_print("[NETMAN] window created\n");

    /*
     * Layout (all x/y relative to root = window origin):
     *
     *   y=14   "Network Manager"   title           [Wi-Fi toggle @ x=360]
     *   y=42   "Status: ..."       live link state
     *   y=64   "MAC: ..."          live
     *   y=84   "IPv4: ..."         live
     *   y=104  "Gateway: ..."      live
     *   y=128  "Networks"          section header
     *   y=148  [ scroll list of network rows, 372x200 ]
     *   y=388  [ spinner ] "Connecting to <ssid>..."  (on demand)
     *   y=410  [ dns field ] [Resolve]   "-> ..."
     */
    ui_label(root, 16, 14, "Network Manager", COL_TITLE);

    /* Animated Wi-Fi master toggle (top-right), starts ON. */
    g_wifi_toggle = ui_toggle(root, 360, 12, 1, on_wifi_toggle, 0);

    g_link_label = ui_label(root, 16, 42,  "Status: ...",  COL_GREEN);
    g_mac_label  = ui_label(root, 16, 64,  "MAC: ...",     COL_DIM);
    g_ip_label   = ui_label(root, 16, 84,  "IPv4: ...",    COL_DIM);
    g_gw_label   = ui_label(root, 16, 104, "Gateway: ...", COL_DIM);

    ui_label(root, 16, 128, "Networks", COL_TITLE);

    /* Scrollable network list. content_h allows up to MAX_NETS rows. */
    g_list = ui_scroll(root, 16, 148, 388, 200, COL_FIELD, MAX_NETS * 36 + 8);

    /* Radio bring-up diagnostics line: shows the kernel IWL* bring-up state
     * (detected / firmware alive / RF-kill / scanned / FAILED + reason) so the
     * user can diagnose the real T410 radio on-screen, no serial cable. */
    g_diag_label = ui_label(root, 16, 356, "Radio: ...", COL_DIM);

    /* DNS lookup row at the bottom. */
    g_dns_input  = ui_textbox(root, 16, 410, 200, 63);
    ui_button(root, 224, 408, 96, 24, "Resolve", on_resolve, 0);
    g_dns_result = ui_label(root, 324, 410, "DNS", COL_ACCENT);

    /* Populate the status immediately and emit the smoke-test line. */
    refresh_status();

    /* Keep everything live and pace the loop via the toolkit. */
    ui_app_set_tick(app, tick_cb, 0);

    ui_app_run(app);   /* never returns */
}

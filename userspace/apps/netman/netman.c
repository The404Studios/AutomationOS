/*
 * netman.c -- Network Manager GUI app (freestanding, ring 3).
 * ===========================================================
 *
 * A windowed network manager for AutomationOS, built on the M4 retained-mode
 * UI toolkit (userspace/lib/ui) over the M3 "Wayland-lite" compositor stack
 * and the 8x16 bitmap font.  It shows the live NIC configuration read from the
 * kernel network stack and offers a small DNS-lookup field.
 *
 * Window contents:
 *   - Title label "Network Manager".
 *   - Link status: "UP" if SYS_NET_INFO (59) returns 0, else "DOWN (no NIC)".
 *   - MAC address  (xx:xx:xx:xx:xx:xx).
 *   - IPv4 address (a.b.c.d, from the host-order int).
 *   - Gateway      (a.b.c.d, from the host-order int).
 *   - DNS lookup: a focusable text field + "Resolve" button.  On click the
 *     app calls dns_resolve() and shows the resolved IP, or "resolve failed".
 *
 * The NIC info is re-read each frame (via the toolkit tick hook) so the
 * status stays live if networking comes up after launch.
 *
 * No libc: pure inline syscalls + tiny freestanding helpers + the OS UI/wl/
 * font/dns libraries.  Fixed-size buffers only.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/netman/netman.c -o netman.o
 *   gcc ... -c userspace/lib/ui/ui.c         -o ui.o
 *   gcc ... -c userspace/lib/wl/wl_client.c  -o wl_client.o
 *   gcc ... -c userspace/lib/font/bitfont.c  -o bitfont.o
 *   gcc ... -c userspace/lib/net/dns.c       -o dns.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       netman.o ui.o wl_client.o bitfont.o dns.o -o build/netman
 *   objdump -d build/netman | grep fs:0x28   # MUST be empty (no canary)
 *
 * Serial output (fd 1, for the boot smoke test):
 *   [NETMAN] window created
 *   [NETMAN] link=UP ip=10.0.2.15        (or link=DOWN ip=0.0.0.0)
 */

#include "../../lib/ui/ui.h"
#include "../../lib/net/dns.h"

/* -----------------------------------------------------------------------
 * Syscall numbers and inline syscall helper.
 * --------------------------------------------------------------------- */
#define SYS_WRITE     3
#define SYS_NET_INFO  59   /* sc(59,&net_info,...) -> 0 ok, <0 if net down */

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

/* -----------------------------------------------------------------------
 * Application state.
 * --------------------------------------------------------------------- */

/* Live status labels (updated by the tick callback). */
static ui_widget_t *g_link_label = 0;
static ui_widget_t *g_mac_label  = 0;
static ui_widget_t *g_ip_label   = 0;
static ui_widget_t *g_gw_label   = 0;

/* DNS lookup widgets. */
static ui_widget_t *g_dns_input  = 0;   /* hostname text field          */
static ui_widget_t *g_dns_result = 0;   /* resolved IP / status label   */

/* Whether we've already emitted the one-shot smoke-test serial line. */
static int g_smoke_emitted = 0;

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
        fmt_mac(macbuf, info.mac);
        fmt_ipv4(ipbuf, info.ip);
        fmt_ipv4(gwbuf, info.gateway);
    } else {
        ui_label_set_text(g_link_label, "Status: DOWN (no NIC)");
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
 * Per-frame tick: keep the NIC status live.
 * --------------------------------------------------------------------- */
static void tick_cb(void *ud)
{
    (void)ud;
    refresh_status();
}

/* -----------------------------------------------------------------------
 * "Resolve" button: look up the hostname in the text field.
 * --------------------------------------------------------------------- */
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
    /* Window: 340 x 280, titled "Network Manager". */
    ui_app_t    *app  = ui_app_create("Network Manager", 340, 280);
    ui_widget_t *root = ui_app_root(app);

    serial_print("[NETMAN] window created\n");

    /*
     * Layout (all x/y relative to root = window origin):
     *
     *   y=14   "Network Manager"          -- white title
     *   y=42   "Status: ..."              -- live link state
     *   y=66   "MAC: ..."                 -- live
     *   y=88   "IPv4: ..."                -- live
     *   y=110  "Gateway: ..."             -- live
     *   y=140  "DNS lookup:"              -- section header
     *   y=160  [ text field ]  [Resolve]  -- input + button
     *   y=198  "-> ..."                   -- resolved result
     */
    ui_label(root, 16, 14, "Network Manager", 0xFFFFFFFF);

    g_link_label = ui_label(root, 16, 42,  "Status: ...",  0xFF30D158);
    g_mac_label  = ui_label(root, 16, 66,  "MAC: ...",     0xFFAEAEB2);
    g_ip_label   = ui_label(root, 16, 88,  "IPv4: ...",    0xFFAEAEB2);
    g_gw_label   = ui_label(root, 16, 110, "Gateway: ...", 0xFFAEAEB2);

    ui_label(root, 16, 140, "DNS lookup:", 0xFFFFFFFF);

    /* Hostname text field: click to focus, type, then press Resolve. */
    g_dns_input  = ui_textbox(root, 16, 160, 200, 63);

    ui_button(root, 224, 158, 96, 24, "Resolve", on_resolve, 0);

    g_dns_result = ui_label(root, 16, 198, "-> (enter a host)", 0xFF0A84FF);

    /* Populate the status immediately and emit the smoke-test line. */
    refresh_status();

    /* Keep the NIC status live and pace the loop via the toolkit. */
    ui_app_set_tick(app, tick_cb, 0);

    ui_app_run(app);   /* never returns */
}

/*
 * nettool.c -- Network tool GUI app (freestanding, ring 3).
 * ==========================================================
 *
 * Creates a 640x420 window titled "Network" via the M3 Wayland-lite client
 * library.  Renders a scrolling console (8x16 bitmap font) that shows:
 *   - On startup: link status, IP, MAC, gateway (from net_info), or a clear
 *     "networking not yet enabled" message if the driver is not wired.
 *   - An interactive command line with the following built-ins:
 *
 *       help                        list commands
 *       ip                          show current NIC configuration
 *       ping <ip>                   ICMP-style UDP probe (or "unavailable")
 *       udp <ip> <port> <text>      send a UDP datagram (net_sendto)
 *       tcp <ip> <port>             TCP connect + send a test line
 *       clear                       clear the screen
 *
 * Graceful degradation:
 *   Every network call checks net_available() / return codes.  When
 *   net_available() returns 0, the app prints "networking not yet enabled"
 *   and keeps running normally.  The UI is always usable; networking
 *   features degrade cleanly once the e1000 driver + socket syscalls are
 *   wired by the integrator.
 *
 * Serial diagnostics:
 *   Every command logs: [NET] cmd=<name> rc=<code>  (on SYS_WRITE fd=1).
 *
 * Build (flags DIRECT on cmdline -- NEVER via shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/nettool/nettool.c -o nettool.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/net/net.c -o net.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/wl/wl_client.c -o wl_client.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/font/bitfont.c -o bitfont.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       nettool.o net.o wl_client.o bitfont.o -o build/nettool
 *   objdump -d build/nettool | grep fs:0x28   # MUST be empty
 */

#include "../../lib/wl/wl_client.h"
#include "../../lib/font/bitfont.h"
#include "../../lib/net/net.h"

/* ---- syscall numbers ---- */
#define SYS_WRITE        3
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

/* ---- fixed-width types ---- */
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed int         i32;

/* ---- raw 6-arg inline syscall ---- */
static inline long sc(long n, long a1, long a2, long a3,
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

/* ---- window / grid geometry ---- */
#define WIN_W      640
#define WIN_H      420
#define MAX_COLS   (WIN_W / FONT_W)    /* 80 */
#define MAX_ROWS   (WIN_H / FONT_H)    /* 26 */

#define BG_COLOR      0xFF0D1117u   /* near-black background          */
#define FG_COLOR      0xFFCDD9E5u   /* light text                     */
#define CURSOR_COLOR  0xFF238636u   /* green cursor                   */
#define HEADER_COLOR  0xFF388BFDu   /* blue for status/header lines   */
#define ERROR_COLOR   0xFFF85149u   /* red for error lines            */
#define OK_COLOR      0xFF3FB950u   /* green for success lines        */
#define PROMPT_COLOR  0xFFD29922u   /* yellow prompt                  */

/* ---- freestanding string helpers ---- */
static unsigned int k_strlen(const char *s) {
    unsigned int n = 0; while (s[n]) n++; return n; }

static int k_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b; }

static int k_strncmp(const char *a, const char *b, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0; }

static int k_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0'; return i; }

static long k_atoi(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return -1;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v; }

/* serial print helpers */
static void serial_puts(const char *m) {
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0); }

static void serial_putnum(long n) {
    char b[24]; int i = 0;
    int neg = (n < 0); if (neg) n = -n;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    if (neg) sc(SYS_WRITE, 1, (long)"-", 1, 0, 0, 0);
    while (i > 0) { char ch = b[--i]; sc(SYS_WRITE, 1, (long)&ch, 1, 0, 0, 0); } }

/* structured serial log: [NET] cmd=<name> rc=<rc> */
static void serial_log(const char *cmd, int rc) {
    serial_puts("[NET] cmd=");
    serial_puts(cmd);
    serial_puts(" rc=");
    serial_putnum((long)rc);
    serial_puts("\n"); }

/* ---- key codes (evdev/Linux, as used by the compositor) ---- */
#define KEY_BACKSPACE  14
#define KEY_ENTER      28
#define KEY_LEFTSHIFT  42
#define KEY_RIGHTSHIFT 54
#define KEY_SPACE      57
#define KEY_UP         103
#define KEY_DOWN       108

static char keycode_to_ascii(int kc, int shift) {
    switch (kc) {
        case 2:  return shift ? '!' : '1';
        case 3:  return shift ? '@' : '2';
        case 4:  return shift ? '#' : '3';
        case 5:  return shift ? '$' : '4';
        case 6:  return shift ? '%' : '5';
        case 7:  return shift ? '^' : '6';
        case 8:  return shift ? '&' : '7';
        case 9:  return shift ? '*' : '8';
        case 10: return shift ? '(' : '9';
        case 11: return shift ? ')' : '0';
        case 12: return shift ? '_' : '-';
        case 13: return shift ? '+' : '=';
        case 16: return shift ? 'Q' : 'q';
        case 17: return shift ? 'W' : 'w';
        case 18: return shift ? 'E' : 'e';
        case 19: return shift ? 'R' : 'r';
        case 20: return shift ? 'T' : 't';
        case 21: return shift ? 'Y' : 'y';
        case 22: return shift ? 'U' : 'u';
        case 23: return shift ? 'I' : 'i';
        case 24: return shift ? 'O' : 'o';
        case 25: return shift ? 'P' : 'p';
        case 26: return shift ? '{' : '[';
        case 27: return shift ? '}' : ']';
        case 30: return shift ? 'A' : 'a';
        case 31: return shift ? 'S' : 's';
        case 32: return shift ? 'D' : 'd';
        case 33: return shift ? 'F' : 'f';
        case 34: return shift ? 'G' : 'g';
        case 35: return shift ? 'H' : 'h';
        case 36: return shift ? 'J' : 'j';
        case 37: return shift ? 'K' : 'k';
        case 38: return shift ? 'L' : 'l';
        case 39: return shift ? ':' : ';';
        case 40: return shift ? '"' : '\'';
        case 41: return shift ? '~' : '`';
        case 43: return shift ? '|' : '\\';
        case 44: return shift ? 'Z' : 'z';
        case 45: return shift ? 'X' : 'x';
        case 46: return shift ? 'C' : 'c';
        case 47: return shift ? 'V' : 'v';
        case 48: return shift ? 'B' : 'b';
        case 49: return shift ? 'N' : 'n';
        case 50: return shift ? 'M' : 'm';
        case 51: return shift ? '<' : ',';
        case 52: return shift ? '>' : '.';
        case 53: return shift ? '?' : '/';
        case KEY_SPACE: return ' ';
        default: return 0;
    }
}

/* =========================================================================
 * Character grid (scrolling console)
 * ========================================================================= */

static char   grid[MAX_ROWS][MAX_COLS];
static u32    grid_color[MAX_ROWS][MAX_COLS];   /* per-character ARGB color */
static int    g_cols = MAX_COLS;
static int    g_rows = MAX_ROWS;
static int    cur_row;
static int    cur_col;
static u32    cur_color = FG_COLOR;   /* color for next char */

static void grid_clear(void) {
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++) {
            grid[r][c] = ' ';
            grid_color[r][c] = FG_COLOR;
        }
    cur_row = 0; cur_col = 0;
}

static void grid_scroll_up(void) {
    for (int r = 1; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++) {
            grid[r-1][c]       = grid[r][c];
            grid_color[r-1][c] = grid_color[r][c];
        }
    for (int c = 0; c < g_cols; c++) {
        grid[g_rows-1][c]       = ' ';
        grid_color[g_rows-1][c] = FG_COLOR;
    }
}

static void grid_newline(void) {
    cur_col = 0; cur_row++;
    if (cur_row >= g_rows) { grid_scroll_up(); cur_row = g_rows - 1; }
}

static void grid_putchar_c(char ch, u32 color) {
    if (ch == '\n') { grid_newline(); return; }
    if (ch == '\t') { do { grid_putchar_c(' ', color); } while (cur_col % 4 != 0); return; }
    if (cur_col >= g_cols) grid_newline();
    grid[cur_row][cur_col]       = ch;
    grid_color[cur_row][cur_col] = color;
    cur_col++;
    if (cur_col >= g_cols) grid_newline();
}

static void grid_putchar(char ch)             { grid_putchar_c(ch, cur_color); }
static void grid_puts_c(const char *s, u32 color) {
    for (; *s; s++) grid_putchar_c(*s, color); }
static void grid_puts(const char *s)          { grid_puts_c(s, cur_color); }

static void grid_backspace(void) {
    if (cur_col > 0) { cur_col--; grid[cur_row][cur_col] = ' '; }
    else if (cur_row > 0) { cur_row--; cur_col = g_cols - 1; grid[cur_row][cur_col] = ' '; }
}

static void grid_put_unum(unsigned long n) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + n % 10); n /= 10; } while (n > 0);
    while (i > 0) grid_putchar(b[--i]); }

static void grid_put_num(long n) {
    if (n < 0) { grid_putchar('-'); grid_put_unum((unsigned long)-n); }
    else        grid_put_unum((unsigned long)n); }

/* =========================================================================
 * Render
 * ========================================================================= */

static void fill_rect(u32 *buf, u32 bw, u32 bh, u32 stride_px,
                      i32 x, i32 y, i32 w, i32 h, u32 color)
{
    i32 x1 = x < 0 ? 0 : x;
    i32 y1 = y < 0 ? 0 : y;
    i32 x2 = x + w; if (x2 > (i32)bw) x2 = (i32)bw;
    i32 y2 = y + h; if (y2 > (i32)bh) y2 = (i32)bh;
    if (x1 >= x2 || y1 >= y2) return;
    for (i32 yy = y1; yy < y2; yy++) {
        u32 *row = buf + (u32)yy * stride_px;
        for (i32 xx = x1; xx < x2; xx++) row[xx] = color;
    }
}

static void render(wl_window *win, u32 stride_px)
{
    fill_rect(win->pixels, win->w, win->h, stride_px,
              0, 0, (i32)win->w, (i32)win->h, BG_COLOR);

    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            char ch = grid[r][c];
            if (ch == ' ' || ch == 0) continue;
            font_draw_char(win->pixels, (int)stride_px,
                           (int)win->w, (int)win->h,
                           c * FONT_W, r * FONT_H,
                           ch, grid_color[r][c]);
        }
    }

    /* Cursor block */
    fill_rect(win->pixels, win->w, win->h, stride_px,
              cur_col * FONT_W, cur_row * FONT_H, FONT_W, FONT_H, CURSOR_COLOR);

    wl_commit(win);
}

/* =========================================================================
 * Command line input
 * ========================================================================= */

#define LINE_MAX  256
static char line_buf[LINE_MAX];
static int  line_len;
static int  shift_held;

/* History ring */
#define HIST_MAX   16
static char hist_ring[HIST_MAX][LINE_MAX];
static int  hist_count;
static int  hist_nav = -1;

static void hist_push(const char *line) {
    if (!line || !line[0]) return;
    int slot = hist_count % HIST_MAX;
    k_strlcpy(hist_ring[slot], line, LINE_MAX);
    hist_count++; }

static const char *hist_get(int offset) {
    if (offset < 0 || offset >= hist_count || offset >= HIST_MAX) return (void *)0;
    int idx = (hist_count - 1 - offset) % HIST_MAX;
    return hist_ring[idx]; }

static const char *PROMPT = "net> ";

static void shell_prompt(void) {
    cur_color = PROMPT_COLOR;
    grid_puts(PROMPT);
    cur_color = FG_COLOR; }

/* skip leading whitespace */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p; }

/* extract next whitespace-delimited token into out[cap], advance *pp */
static int next_token(const char **pp, char *out, int cap) {
    const char *s = skip_ws(*pp);
    int n = 0;
    while (*s && *s != ' ' && *s != '\t' && n < cap - 1) out[n++] = *s++;
    out[n] = '\0'; *pp = s; return n; }

/* =========================================================================
 * Network command implementations
 * ========================================================================= */

/*
 * Print the IP configuration from net_info, or a degraded message.
 */
static void cmd_ip(void)
{
    net_info_t info;
    int rc = net_info(&info);
    serial_log("ip", rc);

    if (rc == NET_ERR_UNAVAIL) {
        cur_color = ERROR_COLOR;
        grid_puts("networking not yet enabled (driver not wired)\n");
        cur_color = FG_COLOR;
        return;
    }
    if (rc < 0) {
        cur_color = ERROR_COLOR;
        grid_puts("net_info error: ");
        grid_puts(net_strerror(rc));
        grid_putchar('\n');
        cur_color = FG_COLOR;
        return;
    }

    char buf[20];

    /* link status */
    cur_color = info.link_up ? OK_COLOR : ERROR_COLOR;
    grid_puts(info.link_up ? "Link: UP\n" : "Link: DOWN\n");
    cur_color = FG_COLOR;

    /* IP */
    net_fmt_ip(buf, info.ip);
    grid_puts("  IP      : "); grid_puts(buf); grid_putchar('\n');

    /* netmask */
    net_fmt_ip(buf, info.netmask);
    grid_puts("  Netmask : "); grid_puts(buf); grid_putchar('\n');

    /* gateway */
    net_fmt_ip(buf, info.gateway);
    grid_puts("  Gateway : "); grid_puts(buf); grid_putchar('\n');

    /* MAC */
    char mbuf[20];
    net_fmt_mac(mbuf, info.mac);
    grid_puts("  MAC     : "); grid_puts(mbuf); grid_putchar('\n');
}

/*
 * ping <ip> -- send a UDP probe packet and wait briefly for any reply.
 * If networking is unavailable, prints "unavailable" gracefully.
 */
#define PING_BUF_LEN  32
#define PING_TIMEOUT_TRIES 50000

static char ping_buf[PING_BUF_LEN];
static const char *PING_PAYLOAD = "nettool-ping\n";

static void cmd_ping(const char *args)
{
    char tok[80];
    const char *p = args;
    if (!next_token(&p, tok, sizeof(tok))) {
        grid_puts("usage: ping <ip>\n");
        return;
    }

    int parsed = 0;
    u32 dest = net_parse_ip(tok, &parsed);
    if (!parsed) {
        grid_puts("ping: invalid IP address\n");
        serial_log("ping", NET_ERR_INVAL);
        return;
    }

    if (!net_available()) {
        cur_color = ERROR_COLOR;
        grid_puts("ping: networking not yet enabled\n");
        cur_color = FG_COLOR;
        serial_log("ping", NET_ERR_UNAVAIL);
        return;
    }

    /* Open a UDP socket, sendto dest:7 (echo port), poll for reply. */
    int fd = net_socket(NET_SOCK_UDP);
    if (fd < 0) {
        cur_color = ERROR_COLOR;
        grid_puts("ping: socket failed: ");
        grid_puts(net_strerror(fd));
        grid_putchar('\n');
        cur_color = FG_COLOR;
        serial_log("ping", fd);
        return;
    }

    char ipbuf[20];
    net_fmt_ip(ipbuf, dest);
    grid_puts("PING ");
    grid_puts(ipbuf);
    grid_puts(" (UDP echo port 7)...\n");

    int plen = (int)k_strlen(PING_PAYLOAD);
    int snd = net_sendto(fd, PING_PAYLOAD, plen, dest, 7);
    if (snd < 0) {
        cur_color = ERROR_COLOR;
        grid_puts("ping: send failed: ");
        grid_puts(net_strerror(snd));
        grid_putchar('\n');
        cur_color = FG_COLOR;
        net_close(fd);
        serial_log("ping", snd);
        return;
    }

    /* Poll briefly for a reply. */
    u32 src_ip = 0;
    u16 src_port = 0;
    int got = 0;
    for (int tries = 0; tries < PING_TIMEOUT_TRIES && !got; tries++) {
        int n = net_recvfrom(fd, ping_buf, PING_BUF_LEN, &src_ip, &src_port);
        if (n > 0) { got = n; break; }
        if (n != NET_ERR_AGAIN) break;
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    net_close(fd);

    if (got > 0) {
        char sibuf[20];
        net_fmt_ip(sibuf, src_ip);
        cur_color = OK_COLOR;
        grid_puts("reply from ");
        grid_puts(sibuf);
        grid_puts(" (");
        grid_put_unum((unsigned long)got);
        grid_puts(" bytes)\n");
        cur_color = FG_COLOR;
        serial_log("ping", 0);
    } else {
        cur_color = ERROR_COLOR;
        grid_puts("no reply (timeout)\n");
        cur_color = FG_COLOR;
        serial_log("ping", NET_ERR_TIMEDOUT);
    }
}

/*
 * udp <ip> <port> <text> -- send a UDP datagram.
 */
static void cmd_udp(const char *args)
{
    char tok_ip[80], tok_port[16], tok_text[200];
    const char *p = args;

    if (!next_token(&p, tok_ip, sizeof(tok_ip))   ||
        !next_token(&p, tok_port, sizeof(tok_port))) {
        grid_puts("usage: udp <ip> <port> <text>\n");
        serial_log("udp", NET_ERR_INVAL);
        return;
    }
    /* rest of line as text */
    const char *text = skip_ws(p);
    k_strlcpy(tok_text, text, sizeof(tok_text));
    if (!tok_text[0]) k_strlcpy(tok_text, "hello from nettool", sizeof(tok_text));

    int parsed = 0;
    u32 dest = net_parse_ip(tok_ip, &parsed);
    if (!parsed) {
        grid_puts("udp: invalid IP address\n");
        serial_log("udp", NET_ERR_INVAL);
        return;
    }

    long port = k_atoi(tok_port);
    if (port <= 0 || port > 65535) {
        grid_puts("udp: invalid port\n");
        serial_log("udp", NET_ERR_INVAL);
        return;
    }

    if (!net_available()) {
        cur_color = ERROR_COLOR;
        grid_puts("udp: networking not yet enabled\n");
        cur_color = FG_COLOR;
        serial_log("udp", NET_ERR_UNAVAIL);
        return;
    }

    int fd = net_socket(NET_SOCK_UDP);
    if (fd < 0) {
        cur_color = ERROR_COLOR;
        grid_puts("udp: socket failed: ");
        grid_puts(net_strerror(fd));
        grid_putchar('\n');
        cur_color = FG_COLOR;
        serial_log("udp", fd);
        return;
    }

    int tlen = (int)k_strlen(tok_text);
    int snd = net_sendto(fd, tok_text, tlen, dest, (u16)port);
    net_close(fd);

    if (snd >= 0) {
        char ibuf[20];
        net_fmt_ip(ibuf, dest);
        cur_color = OK_COLOR;
        grid_puts("sent ");
        grid_put_unum((unsigned long)snd);
        grid_puts(" bytes to ");
        grid_puts(ibuf);
        grid_putchar(':');
        grid_put_num(port);
        grid_putchar('\n');
        cur_color = FG_COLOR;
        serial_log("udp", snd);
    } else {
        cur_color = ERROR_COLOR;
        grid_puts("udp: sendto failed: ");
        grid_puts(net_strerror(snd));
        grid_putchar('\n');
        cur_color = FG_COLOR;
        serial_log("udp", snd);
    }
}

/*
 * tcp <ip> <port> -- connect a TCP socket and send a test line.
 */
#define TCP_RECV_BUF  256
#define TCP_TIMEOUT_TRIES 100000
static char tcp_rbuf[TCP_RECV_BUF];

static void cmd_tcp(const char *args)
{
    char tok_ip[80], tok_port[16];
    const char *p = args;

    if (!next_token(&p, tok_ip, sizeof(tok_ip)) ||
        !next_token(&p, tok_port, sizeof(tok_port))) {
        grid_puts("usage: tcp <ip> <port>\n");
        serial_log("tcp", NET_ERR_INVAL);
        return;
    }

    int parsed = 0;
    u32 dest = net_parse_ip(tok_ip, &parsed);
    if (!parsed) {
        grid_puts("tcp: invalid IP address\n");
        serial_log("tcp", NET_ERR_INVAL);
        return;
    }

    long port = k_atoi(tok_port);
    if (port <= 0 || port > 65535) {
        grid_puts("tcp: invalid port\n");
        serial_log("tcp", NET_ERR_INVAL);
        return;
    }

    if (!net_available()) {
        cur_color = ERROR_COLOR;
        grid_puts("tcp: networking not yet enabled\n");
        cur_color = FG_COLOR;
        serial_log("tcp", NET_ERR_UNAVAIL);
        return;
    }

    int fd = net_socket(NET_SOCK_TCP);
    if (fd < 0) {
        cur_color = ERROR_COLOR;
        grid_puts("tcp: socket failed: ");
        grid_puts(net_strerror(fd));
        grid_putchar('\n');
        cur_color = FG_COLOR;
        serial_log("tcp", fd);
        return;
    }

    char ibuf[20];
    net_fmt_ip(ibuf, dest);
    grid_puts("Connecting to ");
    grid_puts(ibuf);
    grid_putchar(':');
    grid_put_num(port);
    grid_puts("...\n");

    int rc = net_connect(fd, dest, (u16)port);
    if (rc < 0) {
        cur_color = ERROR_COLOR;
        grid_puts("tcp: connect failed: ");
        grid_puts(net_strerror(rc));
        grid_putchar('\n');
        cur_color = FG_COLOR;
        net_close(fd);
        serial_log("tcp", rc);
        return;
    }

    /* Send a test line. */
    const char *testline = "GET / HTTP/1.0\r\nHost: nettool\r\n\r\n";
    int snd = net_send(fd, testline, (int)k_strlen(testline));
    if (snd < 0) {
        cur_color = ERROR_COLOR;
        grid_puts("tcp: send failed: ");
        grid_puts(net_strerror(snd));
        grid_putchar('\n');
        cur_color = FG_COLOR;
        net_close(fd);
        serial_log("tcp", snd);
        return;
    }

    cur_color = OK_COLOR;
    grid_puts("Connected, sent test request (");
    grid_put_unum((unsigned long)snd);
    grid_puts(" bytes)\n");
    cur_color = FG_COLOR;

    /* Try to read a response header (a few hundred ms worth). */
    int total_recv = 0;
    for (int tries = 0; tries < TCP_TIMEOUT_TRIES && total_recv < 80; tries++) {
        int n = net_recv(fd, tcp_rbuf, TCP_RECV_BUF - 1);
        if (n > 0) {
            tcp_rbuf[n] = '\0';
            /* print first line only */
            char *nl = tcp_rbuf;
            int printed = 0;
            while (*nl && *nl != '\n' && *nl != '\r' && printed < 79) {
                grid_putchar_c(*nl, OK_COLOR);
                nl++; printed++;
            }
            if (printed) grid_putchar('\n');
            total_recv += n;
            break;
        }
        if (n != NET_ERR_AGAIN) break;
        sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
    if (!total_recv) {
        cur_color = ERROR_COLOR;
        grid_puts("(no response received)\n");
        cur_color = FG_COLOR;
    }

    net_close(fd);
    serial_log("tcp", 0);
}

static void cmd_help(void)
{
    cur_color = HEADER_COLOR;
    grid_puts("Network Tool commands:\n");
    cur_color = FG_COLOR;
    grid_puts("  help                     show this list\n");
    grid_puts("  ip                       show IP/MAC/gateway config\n");
    grid_puts("  ping <ip>                UDP probe (echo port 7)\n");
    grid_puts("  udp <ip> <port> [text]   send a UDP datagram\n");
    grid_puts("  tcp <ip> <port>          TCP connect + HTTP probe\n");
    grid_puts("  clear                    clear the screen\n");
}

/* =========================================================================
 * Startup banner
 * ========================================================================= */

static void show_banner(void)
{
    cur_color = HEADER_COLOR;
    grid_puts("Network Tool  --  v0.1\n");
    cur_color = FG_COLOR;
    grid_puts("----------------------------------------\n");

    if (!net_available()) {
        cur_color = ERROR_COLOR;
        grid_puts("Networking not yet enabled\n");
        grid_puts("(driver will be wired by integrator)\n");
        cur_color = FG_COLOR;
    } else {
        /* Show a quick IP summary on launch. */
        cmd_ip();
    }

    grid_puts("Type 'help' for commands.\n");
    grid_puts("----------------------------------------\n");
}

/* =========================================================================
 * Shell: dispatch
 * ========================================================================= */

static void shell_exec(const char *cmdline)
{
    const char *p = skip_ws(cmdline);
    if (!*p) return;

    char verb[32];
    next_token(&p, verb, sizeof(verb));
    const char *rest = skip_ws(p);

    if (k_streq(verb, "help")) {
        cmd_help();
    } else if (k_streq(verb, "ip")) {
        cmd_ip();
    } else if (k_streq(verb, "ping")) {
        cmd_ping(rest);
    } else if (k_streq(verb, "udp")) {
        cmd_udp(rest);
    } else if (k_streq(verb, "tcp")) {
        cmd_tcp(rest);
    } else if (k_streq(verb, "clear")) {
        grid_clear();
    } else {
        cur_color = ERROR_COLOR;
        grid_puts("unknown command: ");
        grid_puts(verb);
        grid_putchar('\n');
        cur_color = FG_COLOR;
    }
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

void _start(void)
{
    /* Connect to compositor. */
    if (wl_connect() != 0) {
        sc(SYS_WRITE, 1, (long)"[NET] wl_connect failed\n", 24, 0, 0, 0);
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    wl_window *win = wl_create_window(WIN_W, WIN_H, "Network");
    if (!win) {
        sc(SYS_WRITE, 1, (long)"[NET] wl_create_window failed\n", 30, 0, 0, 0);
        for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }

    /* Derive grid size from granted window. */
    g_cols = (int)(win->w / FONT_W);
    g_rows = (int)(win->h / FONT_H);
    if (g_cols > MAX_COLS) g_cols = MAX_COLS;
    if (g_rows > MAX_ROWS) g_rows = MAX_ROWS;

    grid_clear();
    show_banner();
    shell_prompt();
    render(win, win->stride / 4);

    int ev_kind, ev_a, ev_b, ev_c;
    shift_held = 0;
    hist_nav   = -1;

    for (;;) {
        int had_event = 0;

        while (wl_poll_event(win, &ev_kind, &ev_a, &ev_b, &ev_c)) {
            had_event = 1;
            if (ev_kind != WL_EVENT_KEY) continue;

            int kc      = ev_a;
            int pressed = ev_b;

            /* Track shift state. */
            if (kc == KEY_LEFTSHIFT || kc == KEY_RIGHTSHIFT) {
                shift_held = pressed;
                continue;
            }
            if (!pressed) continue;   /* ignore key-up for printing */

            if (kc == KEY_ENTER) {
                grid_putchar('\n');
                line_buf[line_len] = '\0';
                hist_push(line_buf);
                hist_nav = -1;
                shell_exec(line_buf);
                line_len = 0;
                line_buf[0] = '\0';
                shell_prompt();

            } else if (kc == KEY_BACKSPACE) {
                if (line_len > 0) {
                    line_len--;
                    line_buf[line_len] = '\0';
                    grid_backspace();
                }

            } else if (kc == KEY_UP) {
                /* Recall previous command. */
                int next_nav = hist_nav + 1;
                const char *h = hist_get(next_nav);
                if (h) {
                    /* erase current input */
                    while (line_len > 0) {
                        line_len--;
                        grid_backspace();
                    }
                    k_strlcpy(line_buf, h, LINE_MAX);
                    line_len = (int)k_strlen(line_buf);
                    grid_puts(line_buf);
                    hist_nav = next_nav;
                }

            } else if (kc == KEY_DOWN) {
                if (hist_nav > 0) {
                    int next_nav = hist_nav - 1;
                    const char *h = hist_get(next_nav);
                    if (h) {
                        while (line_len > 0) { line_len--; grid_backspace(); }
                        k_strlcpy(line_buf, h, LINE_MAX);
                        line_len = (int)k_strlen(line_buf);
                        grid_puts(line_buf);
                        hist_nav = next_nav;
                    }
                } else if (hist_nav == 0) {
                    while (line_len > 0) { line_len--; grid_backspace(); }
                    hist_nav = -1;
                }

            } else {
                char ch = keycode_to_ascii(kc, shift_held);
                if (ch && line_len < LINE_MAX - 1) {
                    line_buf[line_len++] = ch;
                    line_buf[line_len] = '\0';
                    grid_putchar(ch);
                    hist_nav = -1;
                }
            }
        }

        if (had_event)
            render(win, win->stride / 4);
        else
            sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}

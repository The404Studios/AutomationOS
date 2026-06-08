/*
 * net.c -- BSD-ish userspace networking library (freestanding, ring 3).
 * ======================================================================
 *
 * Implements the net.h API over the socket syscalls proposed for the kernel:
 *
 *   SYS_SOCKET   = 51  (type -> fd)
 *   SYS_CONNECT  = 52  (fd, ip, port)
 *   SYS_SEND     = 53  (fd, buf, len)
 *   SYS_RECV     = 54  (fd, buf, len)
 *   SYS_CLOSE_SK = 55  (fd)
 *   SYS_SENDTO   = 56  (fd, buf, len, ip, port)
 *   SYS_RECVFROM = 57  (fd, buf, len, &sock_addr_t)
 *   SYS_SOCK_POLL= 58  (fd)
 *   SYS_NET_INFO = 59  (fills net_info_t)
 *
 * Graceful degradation: every syscall that returns -ENOSYS is translated to
 * NET_ERR_UNAVAIL (-38).  net_available() caches this probe so the caller
 * can gate all network activity cleanly.
 *
 * Build (flags DIRECT on cmdline -- NEVER via a shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/net/net.c -o net.o
 */

#include "net.h"

/* ---- syscall numbers ---- */
#define SYS_WRITE     3
#define SYS_SOCKET    51
#define SYS_CONNECT   52
#define SYS_SEND      53
#define SYS_RECV      54
#define SYS_CLOSE_SK  55
#define SYS_SENDTO    56
#define SYS_RECVFROM  57
#define SYS_SOCK_POLL 58
#define SYS_NET_INFO  59

/* ENOSYS as returned by the kernel when the syscall isn't wired. */
#define ENOSYS_NEG  (-38)

/* ---- raw 6-arg inline syscall (rdi/rsi/rdx/r10/r8/r9) ---- */
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

/* ---- availability cache (0 = unchecked, 1 = available, -1 = unavailable) */
static int g_net_avail = 0;   /* 0 means "not yet probed" */

/* ---- internal helpers ---- */

/* Map a raw syscall return code to the net API convention.
 * If rc == ENOSYS_NEG we return NET_ERR_UNAVAIL; otherwise pass through. */
static int map_rc(long rc)
{
    if (rc == ENOSYS_NEG) return NET_ERR_UNAVAIL;
    return (int)rc;
}

/* ---- freestanding string helpers (no libc) ---- */

static unsigned int net_strlen(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

/* Write a digit string into buf (no NUL); returns chars written. */
static int fmt_u32_dec(char *buf, u32 v)
{
    char tmp[12];
    int i = 0;
    do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    return i;
}

/* Write two hex nibbles into buf; returns 2. */
static int fmt_hex2(char *buf, u8 v)
{
    const char *h = "0123456789abcdef";
    buf[0] = h[(v >> 4) & 0xF];
    buf[1] = h[v & 0xF];
    return 2;
}

/* =========================================================================
 * Public API implementations
 * ========================================================================= */

/* net_available ---------------------------------------------------------- */
int net_available(void)
{
    if (g_net_avail != 0)
        return g_net_avail > 0 ? 1 : 0;

    net_info_t tmp;
    /* zero the struct (no memset in freestanding; loop it) */
    u8 *p = (u8 *)&tmp;
    for (unsigned int i = 0; i < sizeof(tmp); i++) p[i] = 0;

    long rc = sc(SYS_NET_INFO, (long)&tmp, 0, 0, 0, 0, 0);
    if (rc == ENOSYS_NEG || rc < 0) {
        g_net_avail = -1;
        return 0;
    }
    g_net_avail = 1;
    return 1;
}

/* net_info --------------------------------------------------------------- */
int net_info(net_info_t *info)
{
    if (!info) return NET_ERR_INVAL;
    /* zero the struct */
    u8 *p = (u8 *)info;
    for (unsigned int i = 0; i < sizeof(*info); i++) p[i] = 0;

    long rc = sc(SYS_NET_INFO, (long)info, 0, 0, 0, 0, 0);
    if (rc == ENOSYS_NEG) {
        g_net_avail = -1;
        return NET_ERR_UNAVAIL;
    }
    if (rc == 0) g_net_avail = 1;
    return map_rc(rc);
}

/* net_socket ------------------------------------------------------------- */
int net_socket(int type)
{
    if (!net_available()) return NET_ERR_UNAVAIL;
    long rc = sc(SYS_SOCKET, (long)type, 0, 0, 0, 0, 0);
    return map_rc(rc);
}

/* net_connect ------------------------------------------------------------ */
int net_connect(int fd, u32 ip, u16 port)
{
    if (fd < 0) return NET_ERR_BADF;
    if (!net_available()) return NET_ERR_UNAVAIL;
    long rc = sc(SYS_CONNECT, (long)fd, (long)ip, (long)port, 0, 0, 0);
    return map_rc(rc);
}

/* net_send --------------------------------------------------------------- */
int net_send(int fd, const void *buf, int len)
{
    if (fd < 0) return NET_ERR_BADF;
    if (!buf || len <= 0) return NET_ERR_INVAL;
    if (!net_available()) return NET_ERR_UNAVAIL;
    long rc = sc(SYS_SEND, (long)fd, (long)buf, (long)len, 0, 0, 0);
    return map_rc(rc);
}

/* net_recv --------------------------------------------------------------- */
int net_recv(int fd, void *buf, int len)
{
    if (fd < 0) return NET_ERR_BADF;
    if (!buf || len <= 0) return NET_ERR_INVAL;
    if (!net_available()) return NET_ERR_UNAVAIL;
    long rc = sc(SYS_RECV, (long)fd, (long)buf, (long)len, 0, 0, 0);
    return map_rc(rc);
}

/* net_sendto ------------------------------------------------------------- */
int net_sendto(int fd, const void *buf, int len, u32 ip, u16 port)
{
    if (fd < 0) return NET_ERR_BADF;
    if (!buf || len <= 0) return NET_ERR_INVAL;
    if (!net_available()) return NET_ERR_UNAVAIL;
    long rc = sc(SYS_SENDTO, (long)fd, (long)buf, (long)len,
                 (long)ip, (long)port, 0);
    return map_rc(rc);
}

/* net_recvfrom ----------------------------------------------------------- */

/* Mirror of sock_addr_t from kernel/include/socket.h.  The kernel's
 * sys_sock_recvfrom copies this struct to the user pointer in a4.       */
typedef struct { u32 ip; u16 port; u16 _pad; } net_sock_addr_t;

int net_recvfrom(int fd, void *buf, int len, u32 *src_ip, u16 *src_port)
{
    if (fd < 0) return NET_ERR_BADF;
    if (!buf || len <= 0) return NET_ERR_INVAL;
    if (!net_available()) return NET_ERR_UNAVAIL;
    net_sock_addr_t addr;
    addr.ip = 0; addr.port = 0; addr._pad = 0;
    int want_addr = (src_ip || src_port) ? 1 : 0;
    long rc = sc(SYS_RECVFROM, (long)fd, (long)buf, (long)len,
                 want_addr ? (long)&addr : 0, 0, 0);
    if (rc > 0 && want_addr) {
        if (src_ip)   *src_ip   = addr.ip;
        if (src_port) *src_port = addr.port;
    }
    return map_rc(rc);
}

/* net_close -------------------------------------------------------------- */
int net_close(int fd)
{
    if (fd < 0) return NET_ERR_BADF;
    if (!net_available()) return NET_ERR_UNAVAIL;
    long rc = sc(SYS_CLOSE_SK, (long)fd, 0, 0, 0, 0, 0);
    return map_rc(rc);
}

/* net_poll --------------------------------------------------------------- */
int net_poll(int fd)
{
    if (fd < 0) return NET_ERR_BADF;
    if (!net_available()) return NET_ERR_UNAVAIL;
    long rc = sc(SYS_SOCK_POLL, (long)fd, 0, 0, 0, 0, 0);
    return map_rc(rc);
}

/* net_parse_ip ----------------------------------------------------------- */
u32 net_parse_ip(const char *s, int *parsed_out)
{
    if (parsed_out) *parsed_out = 0;
    if (!s) return 0;

    u32 ip = 0;
    int octets = 0;
    u32 cur = 0;
    int digits = 0;

    for (;; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (u32)(c - '0');
            if (cur > 255) return 0;   /* octet overflow */
            digits++;
        } else if (c == '.' || c == '\0') {
            if (!digits) return 0;     /* empty octet */
            ip = (ip << 8) | cur;
            cur = 0; digits = 0;
            octets++;
            if (c == '\0') break;
        } else {
            return 0;                  /* unexpected char */
        }
    }

    if (octets != 4) return 0;
    if (parsed_out) *parsed_out = 1;
    return ip;
}

/* net_fmt_ip ------------------------------------------------------------- */
void net_fmt_ip(char *out, u32 ip)
{
    /* writes "A.B.C.D\0" into out (caller must supply >= 16 bytes) */
    int pos = 0;
    u8 octets[4] = {
        (u8)((ip >> 24) & 0xFF),
        (u8)((ip >> 16) & 0xFF),
        (u8)((ip >>  8) & 0xFF),
        (u8)( ip        & 0xFF),
    };
    for (int i = 0; i < 4; i++) {
        pos += fmt_u32_dec(out + pos, octets[i]);
        if (i < 3) out[pos++] = '.';
    }
    out[pos] = '\0';
}

/* net_fmt_mac ------------------------------------------------------------ */
void net_fmt_mac(char *out, const u8 mac[6])
{
    /* writes "xx:xx:xx:xx:xx:xx\0" (18 bytes) */
    int pos = 0;
    for (int i = 0; i < 6; i++) {
        pos += fmt_hex2(out + pos, mac[i]);
        if (i < 5) out[pos++] = ':';
    }
    out[pos] = '\0';
}

/* net_strerror ----------------------------------------------------------- */
const char *net_strerror(int err)
{
    switch (err) {
        case NET_OK:          return "ok";
        case NET_ERR_UNAVAIL: return "networking not yet enabled";
        case NET_ERR_REFUSED: return "connection refused";
        case NET_ERR_TIMEDOUT:return "connection timed out";
        case NET_ERR_AGAIN:   return "no data (would block)";
        case NET_ERR_INVAL:   return "invalid argument";
        case NET_ERR_BADF:    return "bad socket fd";
        default:              return "error";
    }
}

/*
 * net.h -- BSD-ish userspace networking library (freestanding, ring 3).
 * =====================================================================
 *
 * Thin, gracefully-degrading wrapper over the SYS_SOCKET/CONNECT/SEND/...
 * family of kernel syscalls.  When those syscalls return -ENOSYS (i.e. the
 * e1000 driver + socket dispatch are not yet wired), every function returns
 * NET_ERR_UNAVAIL (-ENOSYS, -38) and the caller may call net_available() to
 * gate any network activity rather than crashing.
 *
 * Graceful degradation contract:
 *   - net_available() probes SYS_NET_INFO once and caches the result.
 *   - Every API function checks the cached availability flag first.
 *   - On -ENOSYS the function returns NET_ERR_UNAVAIL; the caller prints
 *     "networking not yet enabled" (or similar) instead of panicking.
 *
 * Build (flags passed DIRECTLY on the command line, NEVER via a variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/net/net.c -o net.o
 *
 * Link with nettool.o, wl_client.o, bitfont.o via userspace/userspace.ld:
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       nettool.o net.o wl_client.o bitfont.o -o build/nettool
 *   objdump -d build/nettool | grep fs:0x28   # MUST be empty
 */

#ifndef NET_H
#define NET_H

/* ---- fixed-width types (freestanding) ---- */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed long long   i64;

/* ---- socket types ---- */
#define NET_SOCK_TCP   1
#define NET_SOCK_UDP   2
#define NET_SOCK_RAW   3

/* ---- error codes ---- */
#define NET_OK           0
#define NET_ERR_UNAVAIL  (-38)   /* -ENOSYS: driver not wired yet          */
#define NET_ERR_REFUSED  (-111)  /* -ECONNREFUSED                          */
#define NET_ERR_TIMEDOUT (-110)  /* -ETIMEDOUT                             */
#define NET_ERR_AGAIN    (-11)   /* -EAGAIN: no data / would block         */
#define NET_ERR_INVAL    (-22)   /* -EINVAL: bad argument                  */
#define NET_ERR_BADF     (-9)    /* -EBADF: bad socket fd                  */

/*
 * net_info_t -- filled by net_info(); mirrors kernel net_info_ext_t
 * (kernel/include/netif.h).
 *
 * All addresses are in host byte order (u32 = 0xAABBCCDD for A.B.C.D).
 *
 * IMPORTANT: the field order and sizes must match the kernel struct EXACTLY
 * because SYS_NET_INFO does a raw copy_to_user of the whole struct.
 */
typedef struct {
    char ifname[16]; /* interface name, e.g. "eth0" (NETIF_NAME_MAX) */
    u8   mac[6];     /* hardware address    */
    u8   _pad[2];
    u32  ip;         /* our IPv4 address    */
    u32  netmask;    /* subnet mask         */
    u32  gateway;    /* default gateway     */
    u32  dns;        /* DNS server          */
    u8   up;         /* 1 = link up         */
    u8   dhcp_active;
    u8   _pad2[6];
    u64  tx_packets;
    u64  rx_packets;
    u64  tx_bytes;
    u64  rx_bytes;
} net_info_t;

/* ---- address helpers ---- */

/* Build a u32 IPv4 address from four octets (A.B.C.D -> host order). */
static inline u32 ip4(u8 a, u8 b, u8 c, u8 d) {
    return ((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d;
}

/* Network-to-host / host-to-network for 16-bit (byte-swap). */
static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }

/* Network-to-host for 32-bit. */
static inline u32 htonl(u32 v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) <<  8) |
           ((v & 0x00FF0000u) >>  8) | ((v & 0xFF000000u) >> 24);
}
static inline u32 ntohl(u32 v) { return htonl(v); }

/*
 * net_parse_ip -- parse a dotted-decimal IPv4 string (e.g. "192.168.1.1")
 * into a host-order u32.  Returns 0 if the string is invalid.
 * The result is the *address value*; 0.0.0.0 is also a valid parse (returns 0).
 * Caller may pass parsed != 0 to distinguish 0.0.0.0 from error.
 */
u32 net_parse_ip(const char *s, int *parsed_out);

/* ---- availability probe (cached after first call) ---- */

/*
 * net_available -- returns 1 if SYS_NET_INFO responds (driver wired + link),
 * 0 if the kernel returns -ENOSYS or another error.
 *
 * The result is cached after the first call (cheap to call repeatedly).
 */
int net_available(void);

/* ---- socket API ---- */

/*
 * net_socket -- create a socket.
 *   type  : NET_SOCK_TCP / NET_SOCK_UDP / NET_SOCK_RAW
 * Returns a non-negative fd on success, NET_ERR_UNAVAIL if not wired,
 * or another negative errno on failure.
 */
int net_socket(int type);

/*
 * net_connect -- connect TCP socket fd to host:port.
 *   ip   : host IPv4 address in host byte order (use ip4() or net_parse_ip())
 *   port : port in host byte order
 * Returns 0 on success, negative errno on failure.
 */
int net_connect(int fd, u32 ip, u16 port);

/*
 * net_send -- send len bytes from buf over connected socket fd.
 * Returns bytes sent (>= 0) on success, negative errno on failure.
 */
int net_send(int fd, const void *buf, int len);

/*
 * net_recv -- receive up to len bytes into buf from connected socket fd.
 * Non-blocking: returns NET_ERR_AGAIN if no data available.
 * Returns bytes received (>= 0) on success, negative errno on failure.
 */
int net_recv(int fd, void *buf, int len);

/*
 * net_sendto -- send a datagram to a specific destination (UDP/RAW).
 *   ip   : destination IPv4 in host byte order
 *   port : destination port in host byte order
 * Returns bytes sent on success, negative errno on failure.
 */
int net_sendto(int fd, const void *buf, int len, u32 ip, u16 port);

/*
 * net_recvfrom -- receive a datagram, filling *src_ip and *src_port.
 * Non-blocking: returns NET_ERR_AGAIN if nothing available.
 * Returns bytes received on success, negative errno on failure.
 */
int net_recvfrom(int fd, void *buf, int len, u32 *src_ip, u16 *src_port);

/*
 * net_close -- close a socket fd.
 * Returns 0 on success, negative errno on failure.
 */
int net_close(int fd);

/*
 * net_poll -- check whether socket fd has data ready to read.
 * Returns 1 if data is available, 0 if not, negative on error.
 */
int net_poll(int fd);

/*
 * net_info -- fill *info with the current NIC configuration.
 * Returns 0 on success, NET_ERR_UNAVAIL if the driver is not wired,
 * or another negative errno on error.
 */
int net_info(net_info_t *info);

/* ---- formatting helpers (serial diagnostics) ---- */

/*
 * net_fmt_ip -- write "A.B.C.D\0" into out (must be >= 16 bytes).
 * ip is in host byte order.
 */
void net_fmt_ip(char *out, u32 ip);

/*
 * net_fmt_mac -- write "xx:xx:xx:xx:xx:xx\0" into out (must be >= 18 bytes).
 */
void net_fmt_mac(char *out, const u8 mac[6]);

/*
 * net_strerror -- return a short string for a NET_ERR_* code.
 */
const char *net_strerror(int err);

#endif /* NET_H */

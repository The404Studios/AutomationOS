/*
 * CHANNEL-0 userspace wrappers (self-contained, header-only).
 * The terminal (and later the agent runtime) use these to create channels,
 * spawn children bound to them, and drain output. Kernel = dumb rings; the
 * holder (userspace) is the master + renderer. See kernel/include/channel.h.
 */
#ifndef USERSPACE_CHANNEL_H
#define USERSPACE_CHANNEL_H

/* syscall numbers (must match kernel/include/syscall.h) */
#define SYS_CH_CREATE   96
#define SYS_CH_WRITE    97
#define SYS_CH_READ     98
#define SYS_CH_WAIT     99
#define SYS_CH_CLOSE   100
#define SYS_SPAWN_EX   101
#define SYS_CH_SENDMSG 102
#define SYS_CH_RECVMSG 103
#define SYS_CH_GRANT   104
#define SYS_CH_ACCEPT  105
#define SYS_SPAWN_EX_ARGV 106

/* ch_create flags */
#define CH_BYTE       0x0001u
#define CH_MSG        0x0002u
#define CH_PAGE       4096u

/* ch_wait events */
#define CH_READABLE   0x01u
#define CH_WRITABLE   0x02u
#define CH_CLOSED     0x04u

/* self-contained syscall inline (uniquely named so it never clashes with an
 * includer's own wrapper). */
static inline long _ch_sc(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* Returns a master handle (>0) or <0 on error. */
static inline int ch_create(unsigned int flags, unsigned int cap) {
    return (int)_ch_sc(SYS_CH_CREATE, (long)flags, (long)cap, 0, 0, 0, 0);
}
/* Returns bytes written (may be < len if the ring is full), or <0. */
static inline int ch_write(int h, const void *buf, unsigned long len) {
    return (int)_ch_sc(SYS_CH_WRITE, (long)h, (long)buf, (long)len, 0, 0, 0);
}
/* Returns bytes read, 0 if empty (non-blocking), or <0. */
static inline int ch_read(int h, void *buf, unsigned long len) {
    return (int)_ch_sc(SYS_CH_READ, (long)h, (long)buf, (long)len, 0, 0, 0);
}
/* Returns ready-event bitmask (CH_READABLE|CH_WRITABLE|CH_CLOSED). */
static inline int ch_wait(int h, unsigned int events, unsigned long timeout_ms) {
    return (int)_ch_sc(SYS_CH_WAIT, (long)h, (long)events, (long)timeout_ms, 0, 0, 0);
}
static inline int ch_close(int h) {
    return (int)_ch_sc(SYS_CH_CLOSE, (long)h, 0, 0, 0, 0, 0);
}
/*
 * spawn_ex(path, args, stdin_h, stdout_h, stderr_h): like spawn, but binds the
 * child's fd0/1/2 to channel handles (0 = leave that fd unbound). The handles
 * are the caller's master handles; the kernel installs the child's slave end.
 * `args` is a space-separated args string (same ABI as the plain spawn arg2).
 * Returns the child pid (>0) or <0 on error.
 */
static inline long spawn_ex(const char *path, const char *args, int stdin_h, int stdout_h, int stderr_h) {
    return _ch_sc(SYS_SPAWN_EX, (long)path, (long)args, (long)stdin_h, (long)stdout_h, (long)stderr_h, 0);
}
/*
 * spawn_ex_argv(path, argv_buf, argv_len, ...): AGENT-RPC-0 P6d explicit VECTOR
 * spawn. argv_buf holds argv[1..] as NUL-separated bytes (length argv_len); each
 * entry is passed to the child INTACT (no whitespace split, no shell). argv[0] is
 * the explicit `path`. argv_len 0 => no extra args (== spawn_ex with "").
 */
static inline long spawn_ex_argv(const char *path, const void *argv_buf, unsigned argv_len,
                                 int stdin_h, int stdout_h, int stderr_h) {
    return _ch_sc(SYS_SPAWN_EX_ARGV, (long)path, (long)argv_buf, (long)argv_len,
                  (long)stdin_h, (long)stdout_h, (long)stderr_h);
}

/* ---- CHANNEL-0 P5b: typed message packets (CH_MSG channels) ----
 * Mirrors kernel/include/channel.h msg_packet_t exactly (16-byte header, no
 * padding). Byte channels keep using ch_write/ch_read (stream semantics). */
typedef struct ch_msg_hdr {
    unsigned short type;        /* message type (P6 assigns TOOL_RUN, ...) */
    unsigned short flags;
    unsigned int   len;         /* payload length in bytes                 */
    unsigned long  request_id;  /* correlate request <-> response          */
} ch_msg_hdr;
/* Send one packet. hdr->len payload bytes are taken from `payload`. Returns the
 * total framed bytes (>0), or <0: EMSGSIZE (-90) too big, EAGAIN (-11) ring
 * full, EINVAL (-22) misuse, EBADF (-9) bad handle/rights. */
static inline int ch_sendmsg(int h, const ch_msg_hdr *hdr, const void *payload) {
    return (int)_ch_sc(SYS_CH_SENDMSG, (long)h, (long)hdr, (long)payload, 0, 0, 0);
}
/* Receive one whole packet: fills *hdr and up to cap payload bytes. Returns the
 * payload length (>=0; 0 = empty-payload packet), or <0: EAGAIN (-11) no
 * message queued, EMSGSIZE (-90) cap too small (message left intact). */
static inline int ch_recvmsg(int h, ch_msg_hdr *hdr, void *payload, unsigned int cap) {
    return (int)_ch_sc(SYS_CH_RECVMSG, (long)h, (long)hdr, (long)payload, (long)cap, 0, 0);
}

/* ---- CHANNEL-0 P6c: one-shot read-only CH_BYTE capability transfer ----
 * ch_grant(handle, to_pid): offer a READ-only, MASTER-end capability to the
 * CH_BYTE channel behind `handle` to process `to_pid`. Returns a grant_id (>0)
 * or <0 (EBADF bad/owned handle, EPERM no READ right, EINVAL not CH_BYTE,
 * ENOSPC grant table full). ch_accept(grant_id): the target pid claims it once,
 * receiving a read-only local handle (>0), or <0 (EBADF bogus/stale/consumed id,
 * EPERM wrong pid, EMFILE local table full). */
static inline int ch_grant(int h, unsigned int to_pid) {
    return (int)_ch_sc(SYS_CH_GRANT, (long)h, (long)to_pid, 0, 0, 0, 0);
}
static inline int ch_accept(int grant_id) {
    return (int)_ch_sc(SYS_CH_ACCEPT, (long)grant_id, 0, 0, 0, 0, 0);
}

/* common kernel errno values userspace checks (negative, Linux ABI) */
#define CH_EPERM     (-1)
#define CH_EBADF     (-9)
#define CH_EAGAIN    (-11)
#define CH_EINVAL    (-22)
#define CH_EMFILE    (-24)
#define CH_ENOSPC    (-28)
#define CH_EMSGSIZE  (-90)

#endif /* USERSPACE_CHANNEL_H */

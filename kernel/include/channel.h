/*
 * CHANNEL-0 — capability-backed shared-ring channel primitive
 * ===========================================================
 * One kernel object = a handle to a pair of shared-memory rings. The rail for
 * console StdIO now, and IPC / AI tool-calls / async I/O / networking later.
 * Kernel stays dumb (rings + handles + rights); userspace holds the policy.
 * Full design: docs/superpowers/specs/2026-06-08-channel-0-design.md
 *
 * A channel has two SPSC rings:
 *   to_master : slave(child)  -> master(holder)   [child stdout]
 *   to_slave  : master(holder)-> slave(child)     [child stdin]
 * The creator holds the MASTER end; a spawned child's stdio binds the SLAVE end.
 */
#ifndef CHANNEL_H
#define CHANNEL_H

#include "types.h"

/* ch_create flags */
#define CH_BYTE       0x0001u   /* byte stream (stdio / terminal text)      */
#define CH_MSG        0x0002u   /* typed message packets (P5+)              */
#define CH_NONBLOCK   0x0004u

/* capability rights (checked on every op) */
#define CH_R_READ     0x01u
#define CH_R_WRITE    0x02u
#define CH_R_DUP      0x04u
#define CH_R_TRANSFER 0x08u
#define CH_R_SIGNAL   0x10u
#define CH_R_ADMIN    0x20u
#define CH_R_ALL      0x3Fu

/* ch_wait event bits */
#define CH_READABLE   0x01u
#define CH_WRITABLE   0x02u
#define CH_CLOSED     0x04u

/* channel ends */
#define CH_END_MASTER 0
#define CH_END_SLAVE  1

/* per-process handle table size (handle 0 is reserved = "none") */
#define CH_MAX_HANDLES 32

struct process;

/* A SPSC ring with power-of-2 capacity. head/tail are free-running counters;
 * index = counter & mask; used = head - tail (unsigned, wrap-correct). */
typedef struct ch_ring {
    uint8_t* buf;
    uint32_t cap;            /* power of 2, bytes        */
    uint32_t mask;           /* cap - 1                  */
    volatile uint32_t head;  /* producer advances        */
    volatile uint32_t tail;  /* consumer advances        */
} ch_ring_t;

typedef struct channel {
    ch_ring_t to_master;     /* child -> holder (child stdout) */
    ch_ring_t to_slave;      /* holder -> child (child stdin)  */
    uint32_t  flags;
    uint32_t  refcount;
    int       closed;
} channel_t;

/* ---- CHANNEL-0 P5: typed message framing (CH_MSG channels) ----
 * A message = this fixed 16-byte header, immediately followed on the ring by
 * `len` payload bytes. Writes are message-atomic (the whole frame commits or
 * nothing); reads return exactly one whole message. Byte channels (CH_BYTE,
 * all of TERMINAL-0) are untouched -- they keep using channel_write/read.
 * (No padding: 2+2+4+8 = 16, naturally aligned -- raw bytes round-trip the ring.)
 */
typedef struct msg_packet {
    uint16_t type;        /* message type (P6/AGENT-RPC-0 assigns TOOL_RUN, ...) */
    uint16_t flags;       /* per-message flags                                   */
    uint32_t len;         /* payload length in bytes (follows the header)        */
    uint64_t request_id;  /* correlate request <-> response                      */
} msg_packet_t;

/* ---- kernel API (used by the ch_* syscalls and by stdio/spawn routing) ---- */
channel_t* channel_alloc(uint32_t flags, uint32_t capacity);
void       channel_ref(channel_t* ch);
void       channel_unref(channel_t* ch);
int        channel_write(channel_t* ch, int end, const uint8_t* data, uint32_t len);
int        channel_read(channel_t* ch, int end, uint8_t* data, uint32_t len);
uint32_t   channel_available(channel_t* ch, int end);  /* bytes this end can read */

/* P5 message framing (CH_MSG only). channel_write_msg: commits header+payload
 * atomically; returns total framed bytes written (>0), or EMSGSIZE if
 * header+len can never fit the ring, EAGAIN if it momentarily won't fit, EINVAL
 * on misuse. channel_read_msg: returns ONE whole message -- fills *hdr + up to
 * payload_cap payload bytes, returns the payload length (>=0; 0 = valid empty
 * message), EAGAIN if no complete message is queued, EMSGSIZE if the caller's
 * buffer is smaller than the message (left intact to retry), EINVAL on misuse. */
int        channel_write_msg(channel_t* ch, int end, const msg_packet_t* hdr, const uint8_t* payload);
int        channel_read_msg (channel_t* ch, int end, msg_packet_t* hdr, uint8_t* payload, uint32_t payload_cap);

/* per-process handle table */
int        process_alloc_handle(struct process* p, channel_t* ch, int end, uint32_t rights);
channel_t* process_get_handle(struct process* p, int handle, int* end_out, uint32_t need_rights);
int        process_close_handle(struct process* p, int handle);
void       channel_cleanup_process(struct process* p);

/* boot self-test (P1): create a channel, write via slave, read via master */
void       channel_selftest(void);
void       channel_selftest_p2(void);                          /* P2 binding/rights test */
void       channel_selftest_p5(void);                          /* P5 CH_MSG framing test */

/* P2: install parent-supplied stdio channels into a freshly-built child (slave
 * end, narrowed rights). Called from elf_load_and_exec(); no-op for plain spawn. */
void       channel_install_spawn_stdio(struct process* child);

/* ---- syscall handlers (registered in syscall.c) ---- */
int64_t sys_ch_create(uint64_t flags, uint64_t capacity, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_ch_write (uint64_t handle, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_ch_read  (uint64_t handle, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_ch_wait  (uint64_t handle, uint64_t events, uint64_t timeout_ms, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_ch_close (uint64_t handle, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
/* P2: additive spawn that binds the child's fd0/fd1/fd2 to channel handles. */
int64_t sys_spawn_ex (uint64_t path, uint64_t args, uint64_t stdin_h, uint64_t stdout_h, uint64_t stderr_h, uint64_t a6);
/* P5b: explicit packet syscalls for CH_MSG channels (byte channels keep using
 * sys_ch_write/read -- stream semantics stay pure). sendmsg(handle, user hdr,
 * user payload); recvmsg(handle, user hdr-out, user payload-out, payload_cap). */
int64_t sys_ch_sendmsg(uint64_t handle, uint64_t hdr, uint64_t payload, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_ch_recvmsg(uint64_t handle, uint64_t hdr, uint64_t payload, uint64_t payload_cap, uint64_t a5, uint64_t a6);
/* P6c: one-shot, read-only CH_BYTE capability transfer. grant(handle, to_pid) ->
 * grant_id (>0); accept(grant_id) -> a read-only local handle for the target pid. */
int64_t sys_ch_grant (uint64_t handle, uint64_t to_pid, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_ch_accept(uint64_t grant_id, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* CHANNEL_H */

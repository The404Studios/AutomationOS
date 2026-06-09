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

/* ---- kernel API (used by the ch_* syscalls and by stdio/spawn routing) ---- */
channel_t* channel_alloc(uint32_t flags, uint32_t capacity);
void       channel_ref(channel_t* ch);
void       channel_unref(channel_t* ch);
int        channel_write(channel_t* ch, int end, const uint8_t* data, uint32_t len);
int        channel_read(channel_t* ch, int end, uint8_t* data, uint32_t len);
uint32_t   channel_available(channel_t* ch, int end);  /* bytes this end can read */

/* per-process handle table */
int        process_alloc_handle(struct process* p, channel_t* ch, int end, uint32_t rights);
channel_t* process_get_handle(struct process* p, int handle, int* end_out, uint32_t need_rights);
int        process_close_handle(struct process* p, int handle);
void       channel_cleanup_process(struct process* p);

/* boot self-test (P1): create a channel, write via slave, read via master */
void       channel_selftest(void);

/* ---- syscall handlers (registered in syscall.c) ---- */
int64_t sys_ch_create(uint64_t flags, uint64_t capacity, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_ch_write (uint64_t handle, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_ch_read  (uint64_t handle, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_ch_wait  (uint64_t handle, uint64_t events, uint64_t timeout_ms, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_ch_close (uint64_t handle, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* CHANNEL_H */

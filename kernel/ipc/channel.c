/*
 * CHANNEL-0 — capability-backed shared-ring channel primitive (P0/P1).
 * See kernel/include/channel.h and docs/superpowers/specs/2026-06-08-channel-0-design.md.
 *
 * Two SPSC rings per channel; the creator holds the MASTER end, a spawned
 * child's stdio binds the SLAVE end. Kernel stays dumb (rings + handles +
 * rights); all terminal/agent policy lives in userspace. Additive: nothing
 * uses a channel until a process opts in, so the default build is unchanged.
 */
#include "../include/syscall.h"
#include "../include/kernel.h"
#include "../include/sched.h"
#include "../include/mem.h"
#include "../include/string.h"
#include "../include/errno.h"
#include "../include/channel.h"

#define CH_PAGE     4096u
#define CH_MAX_CAP  (1u << 20)   /* 1 MiB per ring cap          */
#define CH_MAX_IO   65536u       /* max bytes per read/write op */

/* ---- SPSC ring (power-of-2 capacity; head/tail free-running counters) ---- */
static int ring_init(ch_ring_t* r, uint32_t cap) {
    uint32_t c = CH_PAGE;
    while (c < cap && c < CH_MAX_CAP) c <<= 1;
    void* p = pmm_alloc_pages(c / CH_PAGE);   /* identity-mapped: phys == usable ptr */
    if (!p) return -1;
    r->buf = (uint8_t*)p;
    r->cap = c;
    r->mask = c - 1;
    r->head = 0;
    r->tail = 0;
    return 0;
}
static void ring_free(ch_ring_t* r) {
    if (r->buf) { pmm_free_pages(r->buf, r->cap / CH_PAGE); r->buf = (uint8_t*)0; }
}
static int ring_write(ch_ring_t* r, const uint8_t* d, uint32_t len) {
    uint32_t freeb = r->cap - (r->head - r->tail);   /* unsigned wrap-correct */
    uint32_t n = (len < freeb) ? len : freeb;
    for (uint32_t i = 0; i < n; i++) r->buf[(r->head + i) & r->mask] = d[i];
    r->head += n;
    return (int)n;
}
static int ring_read(ch_ring_t* r, uint8_t* d, uint32_t len) {
    uint32_t used = r->head - r->tail;
    uint32_t n = (len < used) ? len : used;
    for (uint32_t i = 0; i < n; i++) d[i] = r->buf[(r->tail + i) & r->mask];
    r->tail += n;
    return (int)n;
}

/* ---- channel object ---- */
channel_t* channel_alloc(uint32_t flags, uint32_t capacity) {
    channel_t* ch = (channel_t*)kmalloc(sizeof(channel_t));
    if (!ch) return (channel_t*)0;
    memset(ch, 0, sizeof(*ch));
    if (ring_init(&ch->to_master, capacity) != 0) { kfree(ch); return (channel_t*)0; }
    if (ring_init(&ch->to_slave, capacity) != 0) { ring_free(&ch->to_master); kfree(ch); return (channel_t*)0; }
    ch->flags = flags;
    ch->refcount = 1;
    ch->closed = 0;
    return ch;
}
void channel_ref(channel_t* ch) { if (ch) ch->refcount++; }
void channel_unref(channel_t* ch) {
    if (!ch) return;
    if (ch->refcount > 0) ch->refcount--;
    if (ch->refcount == 0) {
        ring_free(&ch->to_master);
        ring_free(&ch->to_slave);
        kfree(ch);
    }
}
/* end = the end YOU hold. A slave writes the child's stdout (to_master); a
 * master writes the child's stdin (to_slave). */
int channel_write(channel_t* ch, int end, const uint8_t* data, uint32_t len) {
    if (!ch || !data) return -1;
    ch_ring_t* r = (end == CH_END_SLAVE) ? &ch->to_master : &ch->to_slave;
    return ring_write(r, data, len);
}
int channel_read(channel_t* ch, int end, uint8_t* data, uint32_t len) {
    if (!ch || !data) return -1;
    ch_ring_t* r = (end == CH_END_MASTER) ? &ch->to_master : &ch->to_slave;
    return ring_read(r, data, len);
}
uint32_t channel_available(channel_t* ch, int end) {
    if (!ch) return 0;
    ch_ring_t* r = (end == CH_END_MASTER) ? &ch->to_master : &ch->to_slave;
    return r->head - r->tail;
}

/* ---- CHANNEL-0 P5: message-atomic framing on CH_MSG channels ----
 * The frame is [16-byte msg_packet_t header][len payload bytes] in the same
 * SPSC ring, routed by end exactly like the byte path. A write places all bytes
 * then bumps head ONCE, so a consumer never sees a partial frame (and it is
 * SMP-safe in shape, not just cooperative-safe). A read peeks the header first
 * so an undersized-buffer caller gets EMSGSIZE with the message left intact. */
int channel_write_msg(channel_t* ch, int end, const msg_packet_t* hdr, const uint8_t* payload) {
    if (!ch || !hdr) return EINVAL;
    if (!(ch->flags & CH_MSG)) return EINVAL;              /* byte channels use channel_write */
    if (hdr->len && !payload) return EINVAL;
    uint32_t hsz   = (uint32_t)sizeof(msg_packet_t);
    uint32_t total = hsz + hdr->len;
    if (total < hsz) return EMSGSIZE;                      /* len overflow (wrap) -> too long */
    ch_ring_t* r = (end == CH_END_SLAVE) ? &ch->to_master : &ch->to_slave;
    if (total > r->cap) return EMSGSIZE;                   /* can NEVER fit this ring (hard) */
    uint32_t freeb = r->cap - (r->head - r->tail);
    if (total > freeb) return EAGAIN;                      /* momentarily full -> atomic, no partial */
    const uint8_t* hb = (const uint8_t*)hdr;
    for (uint32_t i = 0; i < hsz; i++)      r->buf[(r->head + i)        & r->mask] = hb[i];
    for (uint32_t j = 0; j < hdr->len; j++) r->buf[(r->head + hsz + j)  & r->mask] = payload[j];
    r->head += total;                                      /* single commit: frame appears atomically */
    return (int)total;
}
int channel_read_msg(channel_t* ch, int end, msg_packet_t* hdr, uint8_t* payload, uint32_t payload_cap) {
    if (!ch || !hdr) return EINVAL;
    if (!(ch->flags & CH_MSG)) return EINVAL;
    if (payload_cap && !payload) return EINVAL;
    ch_ring_t* r = (end == CH_END_MASTER) ? &ch->to_master : &ch->to_slave;
    uint32_t hsz  = (uint32_t)sizeof(msg_packet_t);
    uint32_t used = r->head - r->tail;
    if (used < hsz) return EAGAIN;                         /* no complete header -> no message */
    uint8_t* hb = (uint8_t*)hdr;                           /* peek header (do NOT consume yet) */
    for (uint32_t i = 0; i < hsz; i++) hb[i] = r->buf[(r->tail + i) & r->mask];
    if (used < hsz + hdr->len) return EAGAIN;              /* defensive: atomic writes prevent this */
    if (hdr->len > payload_cap) return EMSGSIZE;           /* buffer too small -> leave msg intact */
    r->tail += hsz;                                        /* commit: consume header ... */
    for (uint32_t j = 0; j < hdr->len; j++) payload[j] = r->buf[(r->tail + j) & r->mask];
    r->tail += hdr->len;                                   /* ... then payload */
    return (int)hdr->len;                                  /* payload bytes delivered (0 = empty msg) */
}

/* ---- per-process handle table (handle 0 reserved = "none") ---- */
int process_alloc_handle(struct process* p, channel_t* ch, int end, uint32_t rights) {
    if (!p || !ch) return -1;
    for (int h = 1; h < CH_MAX_HANDLES; h++) {
        if (p->ch_handles[h].ch == (void*)0) {
            p->ch_handles[h].ch = ch;
            p->ch_handles[h].end = (uint8_t)end;
            p->ch_handles[h].rights = rights;
            return h;
        }
    }
    return -1;   /* table full */
}
channel_t* process_get_handle(struct process* p, int handle, int* end_out, uint32_t need_rights) {
    if (!p || handle <= 0 || handle >= CH_MAX_HANDLES) return (channel_t*)0;
    channel_t* ch = (channel_t*)p->ch_handles[handle].ch;
    if (!ch) return (channel_t*)0;
    if (need_rights && (p->ch_handles[handle].rights & need_rights) != need_rights) return (channel_t*)0;
    if (end_out) *end_out = (int)p->ch_handles[handle].end;
    return ch;
}
int process_close_handle(struct process* p, int handle) {
    if (!p || handle <= 0 || handle >= CH_MAX_HANDLES) return -1;
    channel_t* ch = (channel_t*)p->ch_handles[handle].ch;
    if (!ch) return -1;
    p->ch_handles[handle].ch = (void*)0;
    p->ch_handles[handle].rights = 0;
    channel_unref(ch);
    return 0;
}
void channel_cleanup_process(struct process* p) {
    if (!p) return;
    for (int h = 1; h < CH_MAX_HANDLES; h++) {
        channel_t* ch = (channel_t*)p->ch_handles[h].ch;
        if (ch) { p->ch_handles[h].ch = (void*)0; p->ch_handles[h].rights = 0; channel_unref(ch); }
    }
}

/* ---- syscalls (negative errno on error, like the rest of the kernel) ---- */
int64_t sys_ch_create(uint64_t flags, uint64_t capacity, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    uint32_t cap = (uint32_t)capacity;
    if (cap == 0) cap = CH_PAGE;
    if (cap > CH_MAX_CAP) return EINVAL;
    channel_t* ch = channel_alloc((uint32_t)flags, cap);
    if (!ch) return ENOMEM;
    process_t* cur = process_get_current();
    int h = process_alloc_handle(cur, ch, CH_END_MASTER, CH_R_ALL);
    if (h < 0) { channel_unref(ch); return EMFILE; }
    return h;
}
int64_t sys_ch_write(uint64_t handle, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (len == 0) return 0;
    if (len > CH_MAX_IO) len = CH_MAX_IO;
    process_t* cur = process_get_current();
    int end;
    channel_t* ch = process_get_handle(cur, (int)handle, &end, CH_R_WRITE);
    if (!ch) return EBADF;
    uint8_t* kbuf = (uint8_t*)kmalloc(len);
    if (!kbuf) return ENOMEM;
    if (copy_from_user(kbuf, (const void*)buf, len) != COPY_SUCCESS) { kfree(kbuf); return EFAULT; }
    int n = channel_write(ch, end, kbuf, (uint32_t)len);
    kfree(kbuf);
    return n;
}
int64_t sys_ch_read(uint64_t handle, uint64_t buf, uint64_t len, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (len == 0) return 0;
    if (len > CH_MAX_IO) len = CH_MAX_IO;
    process_t* cur = process_get_current();
    int end;
    channel_t* ch = process_get_handle(cur, (int)handle, &end, CH_R_READ);
    if (!ch) return EBADF;
    uint8_t* kbuf = (uint8_t*)kmalloc(len);
    if (!kbuf) return ENOMEM;
    int n = channel_read(ch, end, kbuf, (uint32_t)len);
    if (n > 0 && copy_to_user((void*)buf, kbuf, n) != COPY_SUCCESS) { kfree(kbuf); return EFAULT; }
    kfree(kbuf);
    return n;
}
int64_t sys_ch_wait(uint64_t handle, uint64_t events, uint64_t timeout_ms, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)timeout_ms; (void)a4; (void)a5; (void)a6;
    /* P1: non-blocking readiness poll. Holders already yield each frame, so a
     * full block+timeout (via wait_object_block) is a later refinement -- this
     * stays bounded and never spins. */
    process_t* cur = process_get_current();
    int end;
    channel_t* ch = process_get_handle(cur, (int)handle, &end, 0);
    if (!ch) return EBADF;
    uint32_t ready = 0;
    if ((events & CH_READABLE) && channel_available(ch, end) > 0) ready |= CH_READABLE;
    if (events & CH_WRITABLE) ready |= CH_WRITABLE;
    if (ch->closed) ready |= CH_CLOSED;
    return (int64_t)ready;
}
int64_t sys_ch_close(uint64_t handle, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    process_t* cur = process_get_current();
    if (process_close_handle(cur, (int)handle) != 0) return EBADF;
    return 0;
}

/* ---- boot self-test (P1): prove the ring + both ends end-to-end ---- */
void channel_selftest(void) {
    channel_t* ch = channel_alloc(CH_BYTE, CH_PAGE);
    if (!ch) { kprintf("[CHAN] selftest FAIL: alloc\n"); return; }
    const char* msg = "PING";
    int w = channel_write(ch, CH_END_SLAVE, (const uint8_t*)msg, 4);  /* child -> holder */
    uint8_t out[8];
    memset(out, 0, sizeof(out));
    int r = channel_read(ch, CH_END_MASTER, out, sizeof(out));        /* holder reads it */
    int ok = (w == 4 && r == 4 && out[0]=='P' && out[1]=='I' && out[2]=='N' && out[3]=='G');
    kprintf("[CHAN] selftest %s (slave wrote %d, master read %d: '%s')\n",
            ok ? "PASS" : "FAIL", w, r, (char*)out);
    channel_unref(ch);
}

/* ===== CHANNEL-0 P2: bind a child's stdio to channels at spawn (additive) ===== */

/* Staged by sys_spawn_ex, consumed by channel_install_spawn_stdio() on the exec
 * success path. File-static: spawn is single-threaded on the cooperative core,
 * like exec.c's g_exec_spawn_args. */
struct exec_stdio_bind { channel_t* ch; int end; uint32_t rights; };
static struct exec_stdio_bind g_exec_stdio[3];

void channel_install_spawn_stdio(struct process* child) {
    if (!child) return;
    for (int fd = 0; fd < 3; fd++) {
        channel_t* ch = g_exec_stdio[fd].ch;
        if (!ch) continue;
        int h = process_alloc_handle(child, ch, g_exec_stdio[fd].end, g_exec_stdio[fd].rights);
        if (h > 0) child->stdio_chan[fd] = (uint8_t)h;   /* child fd -> this handle */
        else        channel_unref(ch);                   /* handle table full: don't leak the ref */
        g_exec_stdio[fd].ch = (channel_t*)0;             /* consumed (ref transferred or freed) */
    }
}

/*
 * sys_spawn_ex(path, args, stdin_h, stdout_h, stderr_h) -- additive spawn that
 * binds the child's fd0/fd1/fd2 to channels. The handles are the PARENT's master
 * handles (must hold CH_R_TRANSFER); the child receives the SLAVE end with
 * narrowed rights (stdin READ, stdout/stderr WRITE) -- the master end is never
 * leaked into the child. A 0 handle leaves that fd unbound (serial/ps2 fallback,
 * unchanged). SYS_SPAWN itself is untouched.
 */
int64_t sys_spawn_ex(uint64_t path, uint64_t args, uint64_t stdin_h, uint64_t stdout_h, uint64_t stderr_h, uint64_t a6) {
    (void)a6;
    process_t* cur = process_get_current();
    uint64_t hin[3] = { stdin_h, stdout_h, stderr_h };
    for (int fd = 0; fd < 3; fd++) {
        g_exec_stdio[fd].ch = (channel_t*)0;
        g_exec_stdio[fd].end = CH_END_SLAVE;
        g_exec_stdio[fd].rights = 0;
        if (!hin[fd]) continue;
        channel_t* ch = process_get_handle(cur, (int)hin[fd], (int*)0, CH_R_TRANSFER);
        if (!ch) {                                       /* invalid / insufficient rights */
            for (int j = 0; j < fd; j++)
                if (g_exec_stdio[j].ch) { channel_unref(g_exec_stdio[j].ch); g_exec_stdio[j].ch = (channel_t*)0; }
            return EBADF;
        }
        channel_ref(ch);                                 /* this ref is the child's (transferred on success) */
        g_exec_stdio[fd].ch = ch;
        g_exec_stdio[fd].rights = (fd == 0) ? CH_R_READ : CH_R_WRITE;
    }
    /* delegate to the normal spawn; on success elf_load_and_exec consumes the
     * bindings (channel_install_spawn_stdio), transferring the refs to the child. */
    extern int64_t sys_spawn(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
    int64_t pid = sys_spawn(path, args, 0, 0, 0, 0);
    /* spawn failed before install -> release the refs we staged (no leak) */
    for (int fd = 0; fd < 3; fd++)
        if (g_exec_stdio[fd].ch) { channel_unref(g_exec_stdio[fd].ch); g_exec_stdio[fd].ch = (channel_t*)0; }
    return pid;
}

/* P2 synthetic self-test: prove the binding/rights mechanism (slave-end handle +
 * narrowed rights enforced + invalid handle denied + clean teardown) without a
 * live spawn -- there is no current process at boot-time syscall_init. */
void channel_selftest_p2(void) {
    process_t* fake = (process_t*)kmalloc(sizeof(process_t));
    if (!fake) { kprintf("[CHAN] p2 selftest FAIL: kmalloc proc\n"); return; }
    memset(fake, 0, sizeof(*fake));
    channel_t* ch = channel_alloc(CH_BYTE, CH_PAGE);
    if (!ch) { kfree(fake); kprintf("[CHAN] p2 selftest FAIL: alloc ch\n"); return; }
    int hm = process_alloc_handle(fake, ch, CH_END_MASTER, CH_R_ALL);    /* master (full rights)        */
    channel_ref(ch);
    int hs = process_alloc_handle(fake, ch, CH_END_SLAVE, CH_R_WRITE);   /* child stdout (write-only)   */
    int em = -1, es = -1;
    channel_t* gm   = process_get_handle(fake, hm, &em, CH_R_READ);      /* master has ALL  -> ok       */
    channel_t* gsw  = process_get_handle(fake, hs, &es, CH_R_WRITE);     /* slave WRITE     -> ok       */
    channel_t* gsr  = process_get_handle(fake, hs, (int*)0, CH_R_READ);  /* slave lacks READ-> denied   */
    channel_t* gbad = process_get_handle(fake, 99, (int*)0, 0);          /* invalid handle  -> denied   */
    int ok = (hm > 0 && hs > 0 && hm != hs && gm == ch && gsw == ch &&
              gsr == (channel_t*)0 && gbad == (channel_t*)0 &&
              em == CH_END_MASTER && es == CH_END_SLAVE);
    kprintf("[CHAN] p2 selftest %s (m=%d s=%d; slave-READ denied=%d; bad-handle denied=%d)\n",
            ok ? "PASS" : "FAIL", hm, hs, gsr == (channel_t*)0, gbad == (channel_t*)0);
    channel_cleanup_process(fake);   /* unref both handles -> channel freed */
    kfree(fake);
}

/* ===== CHANNEL-0 P5 self-test: prove CH_MSG framing end-to-end (substrate) =====
 * Writes one typed packet on the slave end, reads it whole on the master end,
 * then exercises the two boundaries: an empty ring reads back EAGAIN, and a
 * frame larger than the ring is rejected with EMSGSIZE (not EAGAIN). No syscall
 * surface and no TOOL_RUN dispatch yet -- that is P5b / P6. */
void channel_selftest_p5(void) {
    channel_t* ch = channel_alloc(CH_MSG, CH_PAGE);
    if (!ch) { kprintf("[CHAN] p5 msg selftest FAIL: alloc\n"); return; }
    const char pl[] = "/bin/cc main.c";              /* sizeof-1 = exact payload length */
    uint32_t pllen  = (uint32_t)(sizeof(pl) - 1);    /* = 14 */
    msg_packet_t hdr; memset(&hdr, 0, sizeof(hdr));
    hdr.type = 0x55; hdr.flags = 0; hdr.len = pllen; hdr.request_id = 0xABCD1234ULL;

    int w = channel_write_msg(ch, CH_END_SLAVE, &hdr, (const uint8_t*)pl);   /* child -> holder */

    msg_packet_t got; memset(&got, 0, sizeof(got));
    uint8_t buf[64]; memset(buf, 0, sizeof(buf));
    int r = channel_read_msg(ch, CH_END_MASTER, &got, buf, sizeof(buf));     /* holder reads it */

    msg_packet_t g2; uint8_t b2[8];
    int empty = channel_read_msg(ch, CH_END_MASTER, &g2, b2, sizeof(b2));    /* nothing left -> EAGAIN */

    msg_packet_t big; memset(&big, 0, sizeof(big));
    big.len = ch->to_master.cap + 1;                                         /* frame > ring cap */
    int over = channel_write_msg(ch, CH_END_SLAVE, &big, (const uint8_t*)pl);/* -> EMSGSIZE (size checked first) */

    int ok = (w == (int)(sizeof(msg_packet_t) + pllen) &&
              r == (int)pllen &&
              got.type == 0x55 && got.len == pllen && got.request_id == 0xABCD1234ULL &&
              buf[0]=='/' && buf[1]=='b' && buf[2]=='i' && buf[3]=='n' &&
              empty == EAGAIN && over == EMSGSIZE);
    kprintf("[CHAN] p5 msg selftest %s (w=%d r=%d rid_ok=%d empty=EAGAIN:%d oversize=EMSGSIZE:%d payload='%s')\n",
            ok ? "PASS" : "FAIL", w, r, got.request_id == 0xABCD1234ULL,
            empty == EAGAIN, over == EMSGSIZE, (char*)buf);
    channel_unref(ch);
}

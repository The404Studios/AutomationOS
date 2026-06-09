/* msgtest -- CHANNEL-0 P5b proof: a userspace CH_MSG send/recv round-trip.
 *
 * Linked with crt0 (real main+argv). It self-spawns:
 *   parent (no arg): creates a CH_MSG channel, proves EAGAIN (recv on empty) +
 *     EMSGSIZE (oversize send) across the syscall boundary, spawns ITSELF as a
 *     child bound to the channel, sends a request packet, and reads the reply.
 *   child  (arg "c"): its fd0/fd1 are bound to the channel's SLAVE end (handles
 *     1 and 2, deterministic). It recvs the request and sends back a reply.
 *
 * The parent prints MSGTEST: PASS/FAIL to fd1 (serial -- the parent's stdio is
 * unbound). The child stays SILENT: its fd1 IS the channel, so a stray write
 * would inject raw bytes into the message ring.
 */
#include "../../lib/channel.h"

#define SYS_WRITE  3
#define SYS_YIELD 15

typedef unsigned long size_t;
static size_t slen(const char* s){ size_t n=0; while(s&&s[n]) n++; return n; }
static void out(const char* s){ _ch_sc(SYS_WRITE, 1, (long)s, (long)slen(s), 0,0,0); }
static void yield(void){ _ch_sc(SYS_YIELD, 0,0,0,0,0,0); }
static int  eq(const char* a, const char* b, unsigned n){ for(unsigned i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static void puthex(unsigned v){ char b[9]; const char* H="0123456789abcdef";
    for(int i=0;i<8;i++){ b[7-i]=H[v&0xF]; v>>=4; } b[8]=0; out(b); }

/* A freshly-exec'd child has an empty handle table, so channel_install_spawn_stdio
 * installs fd0 -> handle 1 (READ) and fd1 -> handle 2 (WRITE). See channel.c. */
#define CHILD_IN_H  1   /* fd0 slave end, READ  -> reads to_slave  (parent's request) */
#define CHILD_OUT_H 2   /* fd1 slave end, WRITE -> writes to_master (reply to parent)  */

static int child_mode(void) {
    ch_msg_hdr req; char buf[64]; int got = 0;
    for (long i = 0; i < 2000000 && !got; i++) {
        int r = ch_recvmsg(CHILD_IN_H, &req, buf, sizeof(buf));
        if (r >= 0) { got = 1; break; }
        if (r == CH_EAGAIN) { yield(); continue; }
        return 1;                         /* unexpected error */
    }
    if (!got) return 1;
    ch_msg_hdr rep; rep.type = 0x20; rep.flags = 0; rep.len = 4; rep.request_id = req.request_id;
    for (long i = 0; i < 2000000; i++) {
        int w = ch_sendmsg(CHILD_OUT_H, &rep, "PONG");
        if (w >= 0) return 0;
        if (w == CH_EAGAIN) { yield(); continue; }
        return 1;
    }
    return 1;
}

static int parent_mode(void) {
    int h = ch_create(CH_MSG, CH_PAGE);
    if (h <= 0) { out("MSGTEST: FAIL create\n"); return 1; }

    /* (a) EAGAIN: recv on an empty channel */
    ch_msg_hdr rh; char rb[64];
    int eagain_ok = (ch_recvmsg(h, &rh, rb, sizeof(rb)) == CH_EAGAIN);

    /* (b) EMSGSIZE: a frame larger than the syscall message cap (64 KiB) */
    ch_msg_hdr big; big.type=0; big.flags=0; big.len=70000; big.request_id=0;
    char tiny[8];
    int emsgsize_ok = (ch_sendmsg(h, &big, tiny) == CH_EMSGSIZE);

    /* (c) round-trip through a bound child (channel = child fd0 READ + fd1 WRITE) */
    long pid = spawn_ex("sbin/msgtest", "c", h, h, 0);
    if (pid <= 0) { out("MSGTEST: FAIL spawn\n"); ch_close(h); return 1; }

    ch_msg_hdr req; req.type=0x10; req.flags=0; req.len=4; req.request_id=0x1234abcd;
    int send_ok = (ch_sendmsg(h, &req, "PING") == (int)(sizeof(ch_msg_hdr) + 4));  /* 16+4 = 20 */

    ch_msg_hdr rep; rep.type=0; rep.len=0; rep.request_id=0; char pb[64]; int got = 0;
    for (long i = 0; i < 4000000 && !got; i++) {
        int r = ch_recvmsg(h, &rep, pb, sizeof(pb));
        if (r >= 0) { got = 1; break; }
        if (r == CH_EAGAIN) { yield(); continue; }
        break;
    }
    int rt_ok = (got && rep.type == 0x20 && rep.request_id == 0x1234abcd &&
                 rep.len == 4 && eq(pb, "PONG", 4));

    int ok = (eagain_ok && emsgsize_ok && send_ok && rt_ok);
    out("MSGTEST: "); out(ok ? "PASS" : "FAIL");
    out(" eagain=");    out(eagain_ok?"1":"0");
    out(" emsgsize=");  out(emsgsize_ok?"1":"0");
    out(" send=");      out(send_ok?"1":"0");
    out(" roundtrip="); out(rt_ok?"1":"0");
    out(" reply='"); if (got) { pb[rep.len < 63 ? rep.len : 63] = 0; out(pb); } out("'");
    out(" rid=0x"); puthex((unsigned)rep.request_id);
    out("\n");
    ch_close(h);
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && argv[1][0] == 'c') return child_mode();
    return parent_mode();
}

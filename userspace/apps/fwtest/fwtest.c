/*
 * fwtest.c -- FW-0 / PCAP-0 regression probe (freestanding, ring 3, own _start).
 * ==============================================================================
 *
 * Proves, through the REAL syscall surface, that
 *   (1) the packet filter's control plane works: STATUS, ADD, GET, DEL round-trip;
 *   (2) PCAP-0 privilege drops are ENFORCED by the kernel (not by cooperative
 *       userspace policy): a process that drops PCAP_NET_ADMIN|PCAP_NET_RAW is
 *       refused (EPERM == -1) on SYS_FW_CTL writes, raw SYS_NET_SEND/RECV and
 *       SYS_NET_CONFIG, yet may still READ firewall status;
 *   (3) a drop is INHERITED -- a grandchild forked from the dropper has the same
 *       denied mask and the same refusals, and has no way to clear it;
 *   (4) a drop is PER PROCESS -- after the dropper exits, this (privileged) parent
 *       can still change the rules.
 * The in-kernel packet logic itself (stateful return traffic, sanity drops,
 * ordering...) is proven by the kernel's boot "FW-SELFTEST" over crafted frames.
 *
 * Prints "FWTEST: PASS ..." / "FWTEST: FAIL ..." for smoke_boot.sh to gate on.
 */
#include "../../../kernel/include/uapi/fw.h"

#define SYS_EXIT        0
#define SYS_FORK        1
#define SYS_WRITE       3
#define SYS_WAITPID     6
#define SYS_YIELD       15
#define SYS_NET_SEND    68
#define SYS_NET_RECV    69
#define SYS_NET_CONFIG  89
#define SYS_FW_CTL      136
#define SYS_CAP_DROP    137
#define SYS_CAP_QUERY   138

#define PCAP_NET_RAW    (1L << 0)
#define PCAP_NET_ADMIN  (1L << 1)
#define EPERM_          (-1)

typedef unsigned long size_t;

static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}
#define sc3(n,a,b,c) sc6((n),(a),(b),(c),0,0)
#define sc1(n,a)     sc6((n),(a),0,0,0,0)
#define sc0(n)       sc6((n),0,0,0,0,0)

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static void die(int code) { sc3(SYS_EXIT, code, 0, 0); for (;;) sc0(SYS_YIELD); }

static void zero(void* p, size_t n) { volatile unsigned char* b = (volatile unsigned char*)p; while (n--) *b++ = 0; }

static long fw(uapi_fw_req_t* q) { return sc1(SYS_FW_CTL, (long)q); }

static long fw_add_rule(void) {
    uapi_fw_req_t q; zero(&q, sizeof(q));
    q.op = FW_OP_ADD;
    q.rule.dir = FW_DIR_IN; q.rule.action = FW_ACT_ACCEPT; q.rule.proto = 6;
    q.rule.sport_lo = 0; q.rule.sport_hi = 65535;
    q.rule.dport_lo = 4242; q.rule.dport_hi = 4242;
    return fw(&q);
}
static long fw_del_rule(long idx) {
    uapi_fw_req_t q; zero(&q, sizeof(q));
    q.op = FW_OP_DEL; q.index = (int)idx;
    return fw(&q);
}
static long fw_status(uapi_fw_req_t* q) { zero(q, sizeof(*q)); q->op = FW_OP_STATUS; return fw(q); }

/* The GRANDCHILD: inherited the dropped mask; every privileged op must still be refused. */
static void grandchild(void) {
    long denied = sc0(SYS_CAP_QUERY);
    int ok = 1;
    if ((denied & (PCAP_NET_ADMIN | PCAP_NET_RAW)) != (PCAP_NET_ADMIN | PCAP_NET_RAW)) { out("FWTEST:   [grandchild] mask NOT inherited (BAD)\n"); ok = 0; }
    if (fw_add_rule() != EPERM_) { out("FWTEST:   [grandchild] FW add NOT refused (BAD)\n"); ok = 0; }
    long after = sc1(SYS_CAP_DROP, 0);            /* "dropping nothing" must not clear anything */
    if (after != denied) { out("FWTEST:   [grandchild] cap_drop(0) changed the mask (BAD)\n"); ok = 0; }
    die(ok ? 0 : 1);
}

/* The CHILD: drops privilege, then proves the kernel refuses it. */
static void child(void) {
    int ok = 1;
    if (sc0(SYS_CAP_QUERY) != 0) { out("FWTEST:   [child] started with privileges already dropped (BAD)\n"); ok = 0; }

    long m = sc1(SYS_CAP_DROP, PCAP_NET_ADMIN | PCAP_NET_RAW);
    if ((m & (PCAP_NET_ADMIN | PCAP_NET_RAW)) != (PCAP_NET_ADMIN | PCAP_NET_RAW)) { out("FWTEST:   [child] cap_drop did not record the bits (BAD)\n"); ok = 0; }

    if (fw_add_rule() != EPERM_)  { out("FWTEST:   [child] FW ADD NOT refused (BAD)\n"); ok = 0; }
    else                            out("FWTEST:   [child] FW ADD refused EPERM (good)\n");

    uapi_fw_req_t st;
    if (fw_status(&st) != 0)      { out("FWTEST:   [child] FW STATUS (read) was refused (BAD)\n"); ok = 0; }
    else                            out("FWTEST:   [child] FW STATUS still readable (good)\n");

    unsigned char frame[64]; zero(frame, sizeof(frame));
    if (sc3(SYS_NET_SEND, (long)frame, 60, 0) != EPERM_) { out("FWTEST:   [child] raw NET_SEND NOT refused (BAD)\n"); ok = 0; }
    else                                                   out("FWTEST:   [child] raw NET_SEND refused EPERM (good)\n");
    if (sc3(SYS_NET_RECV, (long)frame, 64, 0) != EPERM_)  { out("FWTEST:   [child] raw NET_RECV NOT refused (BAD)\n"); ok = 0; }
    else                                                   out("FWTEST:   [child] raw NET_RECV refused EPERM (good)\n");
    unsigned char cfg[128]; zero(cfg, sizeof(cfg));
    if (sc1(SYS_NET_CONFIG, (long)cfg) != EPERM_)         { out("FWTEST:   [child] NET_CONFIG NOT refused (BAD)\n"); ok = 0; }
    else                                                   out("FWTEST:   [child] NET_CONFIG refused EPERM (good)\n");

    long g = sc0(SYS_FORK);
    if (g == 0) grandchild();
    if (g < 0)  { out("FWTEST:   [child] fork failed\n"); die(1); }
    int gs = 0; long w = 0;
    for (int t = 0; t < 2000000 && w != g; t++) { w = sc3(SYS_WAITPID, g, (long)&gs, 0); if (w != g) sc0(SYS_YIELD); }
    if (w != g || gs != 0) { out("FWTEST:   [child] grandchild did not confirm inherited refusal (BAD)\n"); ok = 0; }
    else                     out("FWTEST:   [grandchild] inherited the drop and was refused (good)\n");
    die(ok ? 0 : 1);
}

void _start(void) {
    out("FWTEST: start\n");
    int ok = 1;

    /* (1) control plane round-trip, as the privileged parent */
    uapi_fw_req_t st0, st1;
    if (fw_status(&st0) != 0 || st0.stats.enabled != 1) { out("FWTEST: FAIL (status unreadable or firewall not enabled)\n"); die(1); }
    long idx = fw_add_rule();
    if (idx < 0) { out("FWTEST: FAIL (privileged ADD refused)\n"); die(1); }
    uapi_fw_req_t g; zero(&g, sizeof(g)); g.op = FW_OP_GET; g.index = (int)idx;
    if (fw(&g) != 0 || g.rule.dport_lo != 4242 || g.rule.proto != 6 || g.rule.action != FW_ACT_ACCEPT) { out("FWTEST: FAIL (GET did not return the added rule)\n"); ok = 0; }
    if (fw_status(&st1) != 0 || st1.stats.rules != st0.stats.rules + 1) { out("FWTEST: FAIL (rule count did not grow)\n"); ok = 0; }
    if (fw_del_rule(idx) != 0) { out("FWTEST: FAIL (DEL failed)\n"); ok = 0; }
    if (fw_status(&st1) != 0 || st1.stats.rules != st0.stats.rules) { out("FWTEST: FAIL (rule count did not return)\n"); ok = 0; }

    /* (2)+(3) a child drops privilege; kernel must refuse it and its own child */
    long c = sc0(SYS_FORK);
    if (c == 0) child();
    if (c < 0) { out("FWTEST: FAIL (fork failed)\n"); die(1); }
    int cs = 0; long w = 0;
    for (int t = 0; t < 2000000 && w != c; t++) { w = sc3(SYS_WAITPID, c, (long)&cs, 0); if (w != c) sc0(SYS_YIELD); }
    if (w != c)  { out("FWTEST: FAIL (waitpid timeout)\n"); die(1); }
    if (cs != 0) { out("FWTEST: FAIL (privilege drop NOT enforced by the kernel)\n"); ok = 0; }

    /* (4) the drop was per-process: this parent is still privileged */
    long idx2 = fw_add_rule();
    if (idx2 < 0) { out("FWTEST: FAIL (parent lost privilege when the child dropped)\n"); ok = 0; }
    else (void)fw_del_rule(idx2);

    if (ok) out("FWTEST: PASS (fw ctl round-trip; PCAP drop enforced, inherited by grandchild, per-process)\n");
    die(ok ? 0 : 1);
}

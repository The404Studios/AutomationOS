/*
 * firewall.c -- FW-0: stateful IPv4 packet filter for AutomationOS.
 * ==================================================================
 *
 * Where it sits (kernel/net/net.c):
 *     NIC rx -> net_recv() -> fw_ingress() -> ARP / ICMP / TCP / UDP demux
 *     stack/raw tx -> net_send() -> fw_egress() -> NIC tx
 * Those two functions are the ONLY places frames cross the host boundary, so a
 * verdict here covers the in-kernel stack, the userspace stacks that ride
 * SYS_NET_SEND/RECV, and raw senders alike. Loopback (127/8) never reaches the
 * hooks and is always trusted. Non-IPv4 (ARP) is never filtered.
 *
 * Model
 *   - two chains (IN / OUT), ordered rule list per chain, FIRST MATCH WINS, then
 *     the chain's default policy. Defaults: IN=DROP, OUT=ACCEPT.
 *   - a connection tracker: anything this host initiates (TCP/UDP/ICMP echo)
 *     records a flow; inbound packets of that flow are RETURN TRAFFIC and are
 *     admitted without a rule. Unsolicited inbound is dropped unless a rule
 *     allows it.
 *   - built-ins (not rules, so a flush can never lock the host out of getting an
 *     address): DHCP server->client (UDP 67->68); ICMP echo-request (toggle,
 *     rate limited) and ICMP error messages (rate limited).
 *   - ingress sanity, always on while enabled: spoofed/martian sources, NULL /
 *     SYN+FIN / SYN+RST TCP scans, L4 fragments (the stack does not reassemble
 *     them, so letting them through would only hand truncated datagrams up).
 *
 * Management: SYS_FW_CTL (uapi/fw.h). Reads are open; every change requires
 * PCAP_NET_ADMIN (proc_caps.h) so a process that dropped privilege -- e.g. an
 * automation agent's tool children -- cannot rewrite the policy.
 *
 * Silent-drop contract: a dropped EGRESS frame reports success to the caller
 * (net_send returns len), exactly like a real filter -- returning an error made
 * TCP tear the whole connection down instead of simply timing out.
 */
#include "../include/firewall.h"
#include "../include/net.h"
#include "../include/proc_caps.h"
#include "../include/kernel.h"
#include "../include/drivers.h"     /* timer_get_ticks_ms */
#include "../include/spinlock.h"
#include "../include/string.h"
#include "../include/mem.h"         /* copy_from_user / copy_to_user */
#include "../include/errno.h"

#define CT_MAX            128
#define FW_ICMP_BURST     20          /* inbound ICMP admitted per window */
#define FW_LOG_BURST      8           /* log lines per window             */
#define FW_WINDOW_MS      1000u

/* TCP flag bits (byte 13 of the TCP header). */
#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_ACK 0x10

enum { CT_NEW = 1, CT_EST = 2, CT_CLOSING = 3 };

typedef struct { uapi_fw_rule_t r; uint64_t hits; } fw_rule_t;

typedef struct {
    uint32_t rip, lip;
    uint16_t rport, lport;
    uint8_t  proto, state, used, _pad;
    uint32_t last_ms;
} fw_ct_t;

typedef struct {
    uint32_t src, dst;                /* host byte order                       */
    uint16_t sport, dport;            /* ICMP echo: both = identifier          */
    uint8_t  proto, tcp_flags, icmp_type, frag;
    uint8_t  l4_ok;                   /* ports/flags/type are valid            */
} fw_pkt_t;

static struct {
    int       enabled;
    uint8_t   policy[2];              /* [FW_DIR_IN], [FW_DIR_OUT]             */
    uint8_t   allow_ping;
    fw_rule_t rules[FW_MAX_RULES];
    uint32_t  nrules;
    fw_ct_t   ct[CT_MAX];
    uint32_t  nct;
    uint64_t  in_accept, in_drop, out_accept, out_drop;
    uint64_t  ct_hits, ct_new, ct_evicted, bad_drop;
    uint32_t  icmp_win_ms, icmp_cnt;
    uint32_t  log_win_ms,  log_cnt;
    uint32_t  test_myip;              /* selftest override of our own address  */
    spinlock_t lock;
} g;

static inline uint32_t now_ms(void) { return (uint32_t)timer_get_ticks_ms(); }
static int g_quiet;                 /* selftest: suppress the per-packet log lines */

static uint32_t my_ip(void) { return g.test_myip ? g.test_myip : net_get_ip(); }

/* ------------------------------------------------------------------ */
/* Logging (rate limited so a scan cannot flood the serial line)        */
/* ------------------------------------------------------------------ */
static int log_ok(uint32_t now) {
    if (g_quiet) return 0;
    if ((uint32_t)(now - g.log_win_ms) >= FW_WINDOW_MS) { g.log_win_ms = now; g.log_cnt = 0; }
    return g.log_cnt++ < FW_LOG_BURST;
}

static void log_pkt(const char* tag, const fw_pkt_t* p, int rule, uint32_t now) {
    if (!log_ok(now)) return;
    kprintf("[FW] %s proto=%u %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u rule=%d\n", tag,
            (unsigned)p->proto,
            (unsigned)(p->src >> 24), (unsigned)((p->src >> 16) & 255),
            (unsigned)((p->src >> 8) & 255), (unsigned)(p->src & 255), (unsigned)p->sport,
            (unsigned)(p->dst >> 24), (unsigned)((p->dst >> 16) & 255),
            (unsigned)((p->dst >> 8) & 255), (unsigned)(p->dst & 255), (unsigned)p->dport, rule);
}

/* ------------------------------------------------------------------ */
/* Parsing                                                              */
/* ------------------------------------------------------------------ */
/* 0 = parsed IPv4; 1 = not IPv4 (caller accepts untouched); -1 = malformed IPv4. */
static int fw_parse(const uint8_t* frame, uint16_t len, fw_pkt_t* p) {
    memset(p, 0, sizeof(*p));
    if (len < ETH_HLEN) return 1;
    const eth_hdr_t* eh = (const eth_hdr_t*)frame;
    if (net_ntohs(eh->ethertype) != ETH_P_IP) return 1;
    uint16_t avail = (uint16_t)(len - ETH_HLEN);
    if (avail < sizeof(ipv4_hdr_t)) return -1;
    const ipv4_hdr_t* ip = (const ipv4_hdr_t*)(frame + ETH_HLEN);
    if ((ip->ver_ihl >> 4) != 4) return -1;
    uint16_t ihl = (uint16_t)((ip->ver_ihl & 0x0F) * 4);
    if (ihl < sizeof(ipv4_hdr_t) || ihl > avail) return -1;
    uint16_t tot = net_ntohs(ip->total_len);
    if (tot < ihl) return -1;
    if (tot > avail) tot = avail;

    p->src   = net_ntohl(ip->src);
    p->dst   = net_ntohl(ip->dst);
    p->proto = ip->proto;
    uint16_t fo = net_ntohs(ip->frag_off);
    p->frag  = (fo & 0x3FFF) != 0;                 /* offset != 0 or MF set */

    const uint8_t* l4 = (const uint8_t*)ip + ihl;
    uint16_t l4len = (uint16_t)(tot - ihl);
    if ((fo & 0x1FFF) != 0) return 0;              /* non-first fragment: no L4 header */

    if (p->proto == IPPROTO_TCP && l4len >= 20) {
        p->sport = (uint16_t)((l4[0] << 8) | l4[1]);
        p->dport = (uint16_t)((l4[2] << 8) | l4[3]);
        p->tcp_flags = l4[13];
        p->l4_ok = 1;
    } else if (p->proto == IPPROTO_UDP && l4len >= 8) {
        p->sport = (uint16_t)((l4[0] << 8) | l4[1]);
        p->dport = (uint16_t)((l4[2] << 8) | l4[3]);
        p->l4_ok = 1;
    } else if (p->proto == IPPROTO_ICMP && l4len >= 8) {
        p->icmp_type = l4[0];
        if (l4[0] == ICMP_ECHO_REQUEST || l4[0] == ICMP_ECHO_REPLY) {
            p->sport = p->dport = (uint16_t)((l4[4] << 8) | l4[5]);   /* identifier */
        }
        p->l4_ok = 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Connection tracker                                                   */
/* ------------------------------------------------------------------ */
static uint32_t ct_ttl_ms(const fw_ct_t* e) {
    if (e->proto == IPPROTO_TCP)
        return e->state == CT_EST ? 900000u : (e->state == CT_CLOSING ? 60000u : 30000u);   /* closing: tolerate half-close */
    if (e->proto == IPPROTO_UDP)
        return e->state == CT_EST ? 60000u : 30000u;
    return 10000u;                                  /* ICMP echo */
}

static int ct_expired(const fw_ct_t* e, uint32_t now) {
    return (uint32_t)(now - e->last_ms) > ct_ttl_ms(e);
}

static fw_ct_t* ct_find(uint8_t proto, uint32_t rip, uint32_t lip,
                        uint16_t rport, uint16_t lport, uint32_t now) {
    for (int i = 0; i < CT_MAX; i++) {
        fw_ct_t* e = &g.ct[i];
        if (!e->used) continue;
        if (e->proto == proto && e->rip == rip && e->lip == lip &&
            e->rport == rport && e->lport == lport) {
            if (ct_expired(e, now)) { e->used = 0; g.nct--; return NULL; }
            return e;
        }
    }
    return NULL;
}

static fw_ct_t* ct_alloc(uint32_t now) {
    fw_ct_t* oldest = &g.ct[0];
    for (int i = 0; i < CT_MAX; i++) {
        fw_ct_t* e = &g.ct[i];
        if (!e->used) { g.nct++; return e; }
        if (ct_expired(e, now)) { g.ct_evicted++; return e; }           /* recycle in place */
        if ((uint32_t)(now - e->last_ms) > (uint32_t)(now - oldest->last_ms)) oldest = e;
    }
    g.ct_evicted++;                                                     /* table full: drop the LRU flow */
    return oldest;
}

/* Record/refresh the flow of an ACCEPTED outbound packet. */
static void ct_note_out(const fw_pkt_t* p, uint32_t now) {
    if (!p->l4_ok || p->frag) return;
    if (p->dst == 0xFFFFFFFFu || (p->dst >> 28) == 0xE) return;         /* bcast/mcast: replies come from elsewhere */
    int track = (p->proto == IPPROTO_TCP || p->proto == IPPROTO_UDP ||
                 (p->proto == IPPROTO_ICMP && p->icmp_type == ICMP_ECHO_REQUEST));
    if (!track) return;

    fw_ct_t* e = ct_find(p->proto, p->dst, p->src, p->dport, p->sport, now);
    if (!e) {
        e = ct_alloc(now);
        e->used = 1; e->proto = p->proto;
        e->rip = p->dst; e->lip = p->src; e->rport = p->dport; e->lport = p->sport;
        /* TCP SYN -> NEW. A non-SYN with no flow is a connection that predates the filter
         * (or one we evicted): treat as established rather than severing it. */
        e->state = (p->proto == IPPROTO_TCP && !(p->tcp_flags & F_SYN)) ? CT_EST : CT_NEW;
        g.ct_new++;
    }
    if (p->proto == IPPROTO_TCP && (p->tcp_flags & (F_FIN | F_RST))) e->state = CT_CLOSING;
    e->last_ms = now;
}

/* ------------------------------------------------------------------ */
/* Rules                                                                */
/* ------------------------------------------------------------------ */
static int rule_match(const uapi_fw_rule_t* r, int dir, const fw_pkt_t* p) {
    if (r->dir != dir) return 0;
    if (r->proto != FW_PROTO_ANY && r->proto != p->proto) return 0;
    if ((p->src & r->src_mask) != (r->src & r->src_mask)) return 0;
    if ((p->dst & r->dst_mask) != (r->dst & r->dst_mask)) return 0;
    int any_ports = (r->sport_lo == 0 && r->sport_hi == 65535 &&
                     r->dport_lo == 0 && r->dport_hi == 65535);
    if (p->proto == IPPROTO_TCP || p->proto == IPPROTO_UDP) {
        if (!any_ports) {
            if (!p->l4_ok) return 0;
            if (p->sport < r->sport_lo || p->sport > r->sport_hi) return 0;
            if (p->dport < r->dport_lo || p->dport > r->dport_hi) return 0;
        }
    } else if (!any_ports) {
        return 0;                                    /* port-constrained rule can't match a portless proto */
    }
    return 1;
}

static int eval_rules(int dir, const fw_pkt_t* p, uint32_t now, int* verdict) {
    for (uint32_t i = 0; i < g.nrules; i++) {
        if (!rule_match(&g.rules[i].r, dir, p)) continue;
        g.rules[i].hits++;
        *verdict = g.rules[i].r.action == FW_ACT_DROP ? FW_VERDICT_DROP : FW_VERDICT_ACCEPT;
        if (g.rules[i].r.flags & FW_RULE_LOG)
            log_pkt(*verdict == FW_VERDICT_DROP ? "RULE-DROP" : "RULE-ACCEPT", p, (int)i, now);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Verdicts (lock held)                                                 */
/* ------------------------------------------------------------------ */
static int icmp_budget(uint32_t now) {
    if ((uint32_t)(now - g.icmp_win_ms) >= FW_WINDOW_MS) { g.icmp_win_ms = now; g.icmp_cnt = 0; }
    return g.icmp_cnt++ < FW_ICMP_BURST;
}

/* 1 = sanity violation (drop). */
static int ingress_sane_violation(const fw_pkt_t* p, uint32_t myip) {
    if ((p->src >> 24) == 127 || (p->dst >> 24) == 127) return 1;        /* martian on a real wire */
    if (myip && p->src == myip) return 1;                                /* spoofed as us          */
    if (p->src == 0xFFFFFFFFu || (p->src >> 28) == 0xE) return 1;        /* bcast/mcast source     */
    if (p->src == 0 && !(p->proto == IPPROTO_UDP && p->l4_ok && p->dport == 68)) return 1;
    if (p->frag && p->proto != IPPROTO_ICMP) return 1;                   /* no L4 reassembly       */
    if (p->proto == IPPROTO_TCP && p->l4_ok) {
        uint8_t f = p->tcp_flags;
        if (f == 0) return 1;                                            /* NULL scan              */
        if ((f & (F_SYN | F_FIN)) == (F_SYN | F_FIN)) return 1;
        if ((f & (F_SYN | F_RST)) == (F_SYN | F_RST)) return 1;
    }
    return 0;
}

static int verdict_in(const fw_pkt_t* p, uint32_t now, uint32_t myip) {
    if (ingress_sane_violation(p, myip)) { g.bad_drop++; log_pkt("BAD-DROP", p, -1, now); return FW_VERDICT_DROP; }

    /* Return traffic of a flow we started. */
    if (p->l4_ok && !p->frag) {
        int tracked = (p->proto == IPPROTO_TCP || p->proto == IPPROTO_UDP ||
                       (p->proto == IPPROTO_ICMP && p->icmp_type == ICMP_ECHO_REPLY));
        if (tracked) {
            fw_ct_t* e = ct_find(p->proto, p->src, p->dst, p->sport, p->dport, now);
            if (e) {
                if (p->proto == IPPROTO_TCP) {
                    if (p->tcp_flags & (F_FIN | F_RST)) e->state = CT_CLOSING;
                    else if (e->state == CT_NEW && (p->tcp_flags & F_ACK)) e->state = CT_EST;
                } else if (e->state == CT_NEW) {
                    e->state = CT_EST;
                }
                e->last_ms = now;
                g.ct_hits++;
                return FW_VERDICT_ACCEPT;
            }
        }
    }

    /* Built-ins. */
    if (p->proto == IPPROTO_UDP && p->l4_ok && p->sport == 67 && p->dport == 68)
        return FW_VERDICT_ACCEPT;                                        /* DHCP server -> client */
    if (p->proto == IPPROTO_ICMP && p->l4_ok) {
        int echo_req = (p->icmp_type == ICMP_ECHO_REQUEST);
        int err      = (p->icmp_type == ICMP_DEST_UNREACH || p->icmp_type == ICMP_TIME_EXCEEDED);
        if ((echo_req && g.allow_ping) || err)
            return icmp_budget(now) ? FW_VERDICT_ACCEPT : FW_VERDICT_DROP;
    }

    /* Rules, then policy. */
    int v;
    if (eval_rules(FW_DIR_IN, p, now, &v)) return v;
    if (g.policy[FW_DIR_IN] == FW_ACT_DROP) { log_pkt("POLICY-DROP", p, -1, now); return FW_VERDICT_DROP; }
    return FW_VERDICT_ACCEPT;
}

static int verdict_out(const fw_pkt_t* p, uint32_t now) {
    int v;
    if (!eval_rules(FW_DIR_OUT, p, now, &v))
        v = (g.policy[FW_DIR_OUT] == FW_ACT_DROP) ? FW_VERDICT_DROP : FW_VERDICT_ACCEPT;
    if (v == FW_VERDICT_ACCEPT) ct_note_out(p, now);
    return v;
}

/* ------------------------------------------------------------------ */
/* Public datapath                                                      */
/* ------------------------------------------------------------------ */
int fw_ingress(const uint8_t* frame, uint16_t len) {
    if (!g.enabled) return FW_VERDICT_ACCEPT;
    fw_pkt_t p;
    int pr = fw_parse(frame, len, &p);
    if (pr == 1) return FW_VERDICT_ACCEPT;                               /* ARP etc. */

    uint64_t fl; spin_lock_irqsave(&g.lock, &fl);
    uint32_t now = now_ms();
    int v;
    if (pr < 0) { g.bad_drop++; v = FW_VERDICT_DROP; }
    else        v = verdict_in(&p, now, my_ip());
    if (v == FW_VERDICT_ACCEPT) g.in_accept++; else g.in_drop++;
    spin_unlock_irqrestore(&g.lock, fl);
    return v;
}

int fw_egress(const uint8_t* frame, uint16_t len) {
    if (!g.enabled) return FW_VERDICT_ACCEPT;
    fw_pkt_t p;
    int pr = fw_parse(frame, len, &p);
    if (pr == 1) return FW_VERDICT_ACCEPT;

    uint64_t fl; spin_lock_irqsave(&g.lock, &fl);
    int v;
    if (pr < 0) { g.bad_drop++; v = FW_VERDICT_DROP; }
    else        v = verdict_out(&p, now_ms());
    if (v == FW_VERDICT_ACCEPT) g.out_accept++; else g.out_drop++;
    spin_unlock_irqrestore(&g.lock, fl);
    return v;
}

/* ------------------------------------------------------------------ */
/* Control                                                              */
/* ------------------------------------------------------------------ */
static void reset_defaults(void) {
    g.nrules = 0;
    memset(g.rules, 0, sizeof(g.rules));
    memset(g.ct, 0, sizeof(g.ct));
    g.nct = 0;
    g.policy[FW_DIR_IN]  = FW_ACT_DROP;
    g.policy[FW_DIR_OUT] = FW_ACT_ACCEPT;
    g.allow_ping = 1;
    g.icmp_cnt = g.log_cnt = 0;
#ifdef FW_OPEN
    g.policy[FW_DIR_IN] = FW_ACT_ACCEPT;      /* test/dev profile: stateful bookkeeping on, inbound open */
#endif
}

void fw_init(void) {
    spin_lock_init(&g.lock);
    g.enabled = 1;
    reset_defaults();
    kprintf("[FW] firewall up: policy IN=%s OUT=%s, stateful, ICMP-echo=%s\n",
            g.policy[FW_DIR_IN] == FW_ACT_DROP ? "DROP" : "ACCEPT",
            g.policy[FW_DIR_OUT] == FW_ACT_DROP ? "DROP" : "ACCEPT",
            g.allow_ping ? "on" : "off");
}

static int rule_valid(uapi_fw_rule_t* r) {
    if (r->dir > FW_DIR_OUT || r->action > FW_ACT_DROP) return 0;
    if (r->proto != FW_PROTO_ANY && r->proto != IPPROTO_ICMP &&
        r->proto != IPPROTO_TCP && r->proto != IPPROTO_UDP) return 0;
    if (r->flags & ~FW_RULE_LOG) return 0;
    if (r->sport_lo == 0 && r->sport_hi == 0) r->sport_hi = 65535;      /* convenience: 0-0 == any */
    if (r->dport_lo == 0 && r->dport_hi == 0) r->dport_hi = 65535;
    if (r->sport_lo > r->sport_hi || r->dport_lo > r->dport_hi) return 0;
    return 1;
}

static void fill_stats(uapi_fw_stats_t* s) {
    memset(s, 0, sizeof(*s));
    s->enabled = (uint32_t)g.enabled; s->rules = g.nrules; s->ct_entries = g.nct;
    s->policy_in = g.policy[FW_DIR_IN]; s->policy_out = g.policy[FW_DIR_OUT];
    s->allow_ping = g.allow_ping;
    s->in_accept = g.in_accept;   s->in_drop = g.in_drop;
    s->out_accept = g.out_accept; s->out_drop = g.out_drop;
    s->ct_hits = g.ct_hits; s->ct_new = g.ct_new; s->ct_evicted = g.ct_evicted;
    s->bad_drop = g.bad_drop;
}

/* Returns >= 0 on success (ADD/INSERT: the index), negative errno otherwise. Lock held. */
static int fw_ctl_locked(uapi_fw_req_t* q) {
    switch (q->op) {
    case FW_OP_STATUS: fill_stats(&q->stats); return 0;
    case FW_OP_GET:
        if (q->index < 0 || (uint32_t)q->index >= g.nrules) return ERANGE;
        q->rule = g.rules[q->index].r; q->hits = g.rules[q->index].hits; return 0;
    case FW_OP_ADD:
    case FW_OP_INSERT: {
        if (!rule_valid(&q->rule)) return EINVAL;
        if (g.nrules >= FW_MAX_RULES) return ENOSPC;
        uint32_t at = g.nrules;
        if (q->op == FW_OP_INSERT) {
            if (q->index < 0 || (uint32_t)q->index > g.nrules) return ERANGE;
            at = (uint32_t)q->index;
            for (uint32_t i = g.nrules; i > at; i--) g.rules[i] = g.rules[i - 1];
        }
        g.rules[at].r = q->rule; g.rules[at].hits = 0;
        g.nrules++;
        return (int)at;
    }
    case FW_OP_DEL:
        if (q->index < 0 || (uint32_t)q->index >= g.nrules) return ERANGE;
        for (uint32_t i = (uint32_t)q->index; i + 1 < g.nrules; i++) g.rules[i] = g.rules[i + 1];
        g.nrules--; memset(&g.rules[g.nrules], 0, sizeof(g.rules[0]));
        return 0;
    case FW_OP_FLUSH:
        g.nrules = 0; memset(g.rules, 0, sizeof(g.rules)); return 0;
    case FW_OP_SET_POLICY: {
        uint32_t dir = q->arg & 0xFF, act = (q->arg >> 8) & 0xFF;
        if (dir > FW_DIR_OUT || act > FW_ACT_DROP) return EINVAL;
        g.policy[dir] = (uint8_t)act; return 0;
    }
    case FW_OP_ENABLE:   g.enabled = q->arg ? 1 : 0; return 0;
    case FW_OP_SET_PING: g.allow_ping = q->arg ? 1 : 0; return 0;
    case FW_OP_RESET:    reset_defaults(); return 0;
    default: return EINVAL;
    }
}

int64_t sys_fw_ctl(uint64_t req_ptr, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (req_ptr == 0) return EINVAL;
    uapi_fw_req_t q;
    if (copy_from_user(&q, (const void*)req_ptr, sizeof(q)) != COPY_SUCCESS) return EFAULT;

    int write = (q.op != FW_OP_STATUS && q.op != FW_OP_GET);
    if (write && !proc_cap_check(PCAP_NET_ADMIN)) {
        kprintf("[FW] SYS_FW_CTL op=%u denied: caller dropped CAP_NET_ADMIN\n", (unsigned)q.op);
        return EPERM;
    }

    uint64_t fl; spin_lock_irqsave(&g.lock, &fl);
    int r = fw_ctl_locked(&q);
    spin_unlock_irqrestore(&g.lock, fl);

    if (r < 0) return r;
    if (q.op == FW_OP_STATUS || q.op == FW_OP_GET) {
        if (copy_to_user((void*)req_ptr, &q, sizeof(q)) != COPY_SUCCESS) return EFAULT;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Boot self-test: crafted frames through the REAL parse/verdict path    */
/* ------------------------------------------------------------------ */
#define T_ME    0x0A00020Fu   /* 10.0.2.15 */
#define T_PEER  0x5DB8D822u   /* 93.184.216.34 */
#define T_DNS   0x0A000203u   /* 10.0.2.3  */
#define T_ATT   0x06060606u   /* 6.6.6.6   */

static uint16_t mk_frame(uint8_t* b, uint8_t proto, uint32_t src, uint32_t dst,
                         uint16_t sport, uint16_t dport, uint8_t tcpflags,
                         uint8_t icmp_type, uint16_t frag_off) {
    memset(b, 0, 80);
    eth_hdr_t* eh = (eth_hdr_t*)b;
    eh->ethertype = net_htons(ETH_P_IP);
    ipv4_hdr_t* ip = (ipv4_hdr_t*)(b + ETH_HLEN);
    ip->ver_ihl = 0x45; ip->ttl = 64; ip->proto = proto;
    ip->src = net_htonl(src); ip->dst = net_htonl(dst);
    ip->frag_off = net_htons(frag_off);
    uint8_t* l4 = b + ETH_HLEN + 20;
    uint16_t l4len = (proto == IPPROTO_TCP) ? 20 : 8;
    if (proto == IPPROTO_ICMP) {
        l4[0] = icmp_type; l4[4] = (uint8_t)(sport >> 8); l4[5] = (uint8_t)sport;
    } else {
        l4[0] = (uint8_t)(sport >> 8); l4[1] = (uint8_t)sport;
        l4[2] = (uint8_t)(dport >> 8); l4[3] = (uint8_t)dport;
        if (proto == IPPROTO_TCP) { l4[12] = 0x50; l4[13] = tcpflags; }
    }
    ip->total_len = net_htons((uint16_t)(20 + l4len));
    return (uint16_t)(ETH_HLEN + 20 + l4len);
}

static int g_t_pass, g_t_fail;
static const char* g_t_first;
static void t_expect(const char* name, int got, int want) {
    if (got == want) g_t_pass++;
    else { g_t_fail++; if (!g_t_first) g_t_first = name; kprintf("[FW] SELFTEST FAIL: %s (got %d want %d)\n", name, got, want); }
}

static int t_in (uint8_t proto, uint32_t s, uint32_t d, uint16_t sp, uint16_t dp, uint8_t fl, uint8_t it, uint16_t fo) {
    uint8_t b[80]; uint16_t n = mk_frame(b, proto, s, d, sp, dp, fl, it, fo); return fw_ingress(b, n);
}
static int t_out(uint8_t proto, uint32_t s, uint32_t d, uint16_t sp, uint16_t dp, uint8_t fl, uint8_t it) {
    uint8_t b[80]; uint16_t n = mk_frame(b, proto, s, d, sp, dp, fl, it, 0); return fw_egress(b, n);
}
static void t_rule(uint8_t dir, uint8_t act, uint8_t proto, uint16_t dport) {
    uapi_fw_req_t q; memset(&q, 0, sizeof(q));
    q.op = FW_OP_ADD; q.rule.dir = dir; q.rule.action = act; q.rule.proto = proto;
    q.rule.sport_hi = 65535; q.rule.dport_lo = dport; q.rule.dport_hi = dport;
    uint64_t fl; spin_lock_irqsave(&g.lock, &fl); (void)fw_ctl_locked(&q); spin_unlock_irqrestore(&g.lock, fl);
}
static void t_ctl(uint32_t op, uint32_t arg) {
    uapi_fw_req_t q; memset(&q, 0, sizeof(q)); q.op = op; q.arg = arg;
    uint64_t fl; spin_lock_irqsave(&g.lock, &fl); (void)fw_ctl_locked(&q); spin_unlock_irqrestore(&g.lock, fl);
}

int fw_selftest(void) {
    const int ACC = FW_VERDICT_ACCEPT, DRP = FW_VERDICT_DROP;
    int saved_enabled = g.enabled;
    g_t_pass = g_t_fail = 0; g_t_first = NULL;
    g.test_myip = T_ME; g.enabled = 1; g_quiet = 1;
    t_ctl(FW_OP_RESET, 0);
#ifdef FW_OPEN
    t_ctl(FW_OP_SET_POLICY, FW_DIR_IN | (FW_ACT_DROP << 8));    /* the test asserts the secure default */
#endif

    /* 1. default-deny unsolicited inbound; rule opens exactly one port */
    t_expect("in-syn-drop-default", t_in(IPPROTO_TCP, T_ATT, T_ME, 5555, 22, F_SYN, 0, 0), DRP);
    t_rule(FW_DIR_IN, FW_ACT_ACCEPT, IPPROTO_TCP, 22);
    t_expect("in-syn-rule-allow",   t_in(IPPROTO_TCP, T_ATT, T_ME, 5555, 22, F_SYN, 0, 0), ACC);
    t_expect("in-syn-other-port",   t_in(IPPROTO_TCP, T_ATT, T_ME, 5555, 23, F_SYN, 0, 0), DRP);

    /* 2. stateful return traffic: outbound SYN admits ONLY its own reply */
    t_expect("out-syn-accept",      t_out(IPPROTO_TCP, T_ME, T_PEER, 40000, 443, F_SYN, 0), ACC);
    t_expect("in-synack-return",    t_in(IPPROTO_TCP, T_PEER, T_ME, 443, 40000, F_SYN | F_ACK, 0, 0), ACC);
    t_expect("in-data-return",      t_in(IPPROTO_TCP, T_PEER, T_ME, 443, 40000, F_ACK, 0, 0), ACC);
    t_expect("in-wrong-rport",      t_in(IPPROTO_TCP, T_PEER, T_ME, 444, 40000, F_ACK, 0, 0), DRP);
    t_expect("in-wrong-rip",        t_in(IPPROTO_TCP, T_ATT,  T_ME, 443, 40000, F_ACK, 0, 0), DRP);

    /* 3. UDP (DNS) */
    t_expect("out-udp-dns",         t_out(IPPROTO_UDP, T_ME, T_DNS, 5353, 53, 0, 0), ACC);
    t_expect("in-udp-dns-reply",    t_in(IPPROTO_UDP, T_DNS, T_ME, 53, 5353, 0, 0, 0), ACC);
    t_expect("in-udp-spoof-reply",  t_in(IPPROTO_UDP, T_ATT, T_ME, 53, 5353, 0, 0, 0), DRP);

    /* 4. DHCP built-in survives a flush; ICMP echo toggle + tracking */
    t_ctl(FW_OP_FLUSH, 0);
    t_expect("in-dhcp-builtin",     t_in(IPPROTO_UDP, 0x0A000202u, 0xFFFFFFFFu, 67, 68, 0, 0, 0), ACC);
    t_expect("in-ping-default-on",  t_in(IPPROTO_ICMP, T_ATT, T_ME, 1, 0, 0, ICMP_ECHO_REQUEST, 0), ACC);
    t_ctl(FW_OP_SET_PING, 0);
    t_expect("in-ping-off",         t_in(IPPROTO_ICMP, T_ATT, T_ME, 1, 0, 0, ICMP_ECHO_REQUEST, 0), DRP);
    t_expect("out-echo-accept",     t_out(IPPROTO_ICMP, T_ME, T_PEER, 7, 0, 0, ICMP_ECHO_REQUEST), ACC);
    t_expect("in-echo-reply-match", t_in(IPPROTO_ICMP, T_PEER, T_ME, 7, 0, 0, ICMP_ECHO_REPLY, 0), ACC);
    t_expect("in-echo-reply-stray", t_in(IPPROTO_ICMP, T_PEER, T_ME, 8, 0, 0, ICMP_ECHO_REPLY, 0), DRP);
    t_ctl(FW_OP_SET_PING, 1);

    /* 5. ingress sanity */
    t_expect("sane-own-src",        t_in(IPPROTO_TCP, T_ME, T_ME, 1, 2, F_SYN, 0, 0), DRP);
    t_expect("sane-loopback-src",   t_in(IPPROTO_TCP, 0x7F000001u, T_ME, 1, 2, F_SYN, 0, 0), DRP);
    t_expect("sane-null-scan",      t_in(IPPROTO_TCP, T_ATT, T_ME, 1, 2, 0, 0, 0), DRP);
    t_expect("sane-synfin",         t_in(IPPROTO_TCP, T_ATT, T_ME, 1, 2, F_SYN | F_FIN, 0, 0), DRP);
    t_expect("sane-synrst",         t_in(IPPROTO_TCP, T_ATT, T_ME, 1, 2, F_SYN | F_RST, 0, 0), DRP);
    t_expect("sane-udp-fragment",   t_in(IPPROTO_UDP, T_ATT, T_ME, 1, 2, 0, 0, 0x2000), DRP);
    t_expect("sane-bcast-src",      t_in(IPPROTO_UDP, 0xFFFFFFFFu, T_ME, 1, 2, 0, 0, 0), DRP);

    /* 6. egress rules + first-match-wins ordering */
    t_rule(FW_DIR_OUT, FW_ACT_DROP, IPPROTO_TCP, 25);
    t_expect("out-rule-drop-smtp",  t_out(IPPROTO_TCP, T_ME, T_PEER, 41000, 25, F_SYN, 0), DRP);
    t_expect("out-other-ok",        t_out(IPPROTO_TCP, T_ME, T_PEER, 41001, 587, F_SYN, 0), ACC);
    t_ctl(FW_OP_FLUSH, 0);
    t_rule(FW_DIR_IN, FW_ACT_DROP,   IPPROTO_TCP, 80);
    t_rule(FW_DIR_IN, FW_ACT_ACCEPT, IPPROTO_TCP, 80);
    t_expect("in-first-match-wins", t_in(IPPROTO_TCP, T_ATT, T_ME, 9, 80, F_SYN, 0, 0), DRP);

    /* 7. conntrack expiry */
    t_ctl(FW_OP_FLUSH, 0);
    (void)t_out(IPPROTO_UDP, T_ME, T_DNS, 6000, 53, 0, 0);
    for (int i = 0; i < CT_MAX; i++) if (g.ct[i].used) g.ct[i].last_ms -= 120000u;   /* age past every TTL */
    t_expect("ct-expired-drops",    t_in(IPPROTO_UDP, T_DNS, T_ME, 53, 6000, 0, 0, 0), DRP);

    /* 8. non-IP is never filtered; disabled = pass-through */
    {
        uint8_t arp[60]; memset(arp, 0, sizeof(arp));
        ((eth_hdr_t*)arp)->ethertype = net_htons(ETH_P_ARP);
        t_expect("arp-untouched",   fw_ingress(arp, sizeof(arp)), ACC);
    }
    t_ctl(FW_OP_ENABLE, 0);
    t_expect("disabled-passthru",   t_in(IPPROTO_TCP, T_ATT, T_ME, 1, 2, F_SYN, 0, 0), ACC);

    /* restore production state */
    t_ctl(FW_OP_RESET, 0);
    g.test_myip = 0; g_quiet = 0;
    g.enabled = saved_enabled;
    g.in_accept = g.in_drop = g.out_accept = g.out_drop = 0;
    g.ct_hits = g.ct_new = g.ct_evicted = g.bad_drop = 0;

    if (g_t_fail == 0) {
        kprintf("FW-SELFTEST: PASS tests=%d\n", g_t_pass);
        return 0;
    }
    kprintf("FW-SELFTEST: FAIL failed=%d of %d first=%s\n", g_t_fail, g_t_pass + g_t_fail,
            g_t_first ? g_t_first : "?");
    return -1;
}

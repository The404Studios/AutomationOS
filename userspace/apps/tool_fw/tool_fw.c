/* tool_fw -- TOOLSET-FW-0: the kernel packet filter as a typed, GATED agent tool.
 *
 * One source, two programs (the privilege boundary is decided at BUILD time, so a
 * hostile model can never talk a read-only tool into a write):
 *   -DTOOL_FW_RO  -> sbin/tool_fwstat : verbs  status | list           (agent tool "fw_status",
 *                                       AUTO class -- pure observation)
 *   (default)     -> sbin/tool_fw     : verbs  allow | block | del | policy | ping
 *                                       (agent tool "firewall", CONFIRM class -- the operator
 *                                       approves every call in the cockpit)
 *
 * argv is what agentd passes (the tool's args string, tab-split). The model is HOSTILE TEXT,
 * so every token is strictly parsed and the verbs the agent may use are deliberately a
 * SUBSET of `fwctl`. NOT reachable from the agent, on purpose:
 *   flush | reset | enable | disable     -- the master switch / mass-delete stay with the human
 *   policy in accept                     -- would open the whole host to the network
 *   policy out drop                      -- would sever the agent's own link to its brain
 *   allow/block of a port range wider than 16, or of port 0 -- no blanket openings
 * Anything refused prints `DENY <why>` to fd1 (the observation the model must see) and exits 2.
 *
 * Kernel enforcement sits underneath: SYS_FW_CTL writes need PCAP_NET_ADMIN, so even if this
 * file were subverted a process that dropped the capability still cannot change the policy.
 *
 * Freestanding ring 3 (crt0 -> main(argc,argv)); NO libc; fd1 = the result channel.
 */
#include "../../../kernel/include/uapi/fw.h"

#define SYS_WRITE  3
#define SYS_FW_CTL 136
#define FD_OUT     1

typedef unsigned long size_t;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static unsigned slen(const char* s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(SYS_WRITE, FD_OUT, (long)s, (long)slen(s)); }
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void out_u(u64 v) {
    char b[24]; int i = 23; b[i] = 0; if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    out(&b[i]);
}
static void zero(void* p, size_t n) { volatile u8* b = (volatile u8*)p; while (n--) *b++ = 0; }
static long fw(uapi_fw_req_t* q) { return sc(SYS_FW_CTL, (long)q, 0, 0); }

static int parse_u(const char* s, u32* out_v) {
    if (!s || !*s) return 0;
    u32 v = 0;
    for (; *s; s++) { if (*s < '0' || *s > '9') return 0; v = v * 10 + (u32)(*s - '0'); if (v > 65535u * 4u) return 0; }
    *out_v = v; return 1;
}
static int parse_ip(const char* s, u32* ip, u32* mask) {
    u32 parts[4]; int n = 0; u32 cur = 0; int digits = 0; u32 prefix = 32;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') { cur = cur * 10 + (u32)(*s - '0'); digits++; if (cur > 255) return 0; }
        else if (*s == '.' || *s == '/' || *s == 0) {
            if (!digits || n > 3) return 0;
            parts[n++] = cur; cur = 0; digits = 0;
            if (*s == '/') { u32 p; if (!parse_u(s + 1, &p) || p > 32 || p < 8) return 0; prefix = p; break; }  /* no /0../7 */
            if (*s == 0) break;
        } else return 0;
    }
    if (n != 4) return 0;
    *mask = 0xFFFFFFFFu << (32 - prefix);
    *ip = ((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]) & *mask;
    return 1;
}

static int deny(const char* why) { out("DENY "); out(why); out("\n"); return 2; }

static int do_status(void) {
    uapi_fw_req_t q; zero(&q, sizeof(q)); q.op = FW_OP_STATUS;
    if (fw(&q) < 0) { out("ERR fw status\n"); return 2; }
    const uapi_fw_stats_t* s = &q.stats;
    out("FW enabled="); out_u(s->enabled);
    out(" policy_in="); out(s->policy_in == FW_ACT_DROP ? "drop" : "accept");
    out(" policy_out="); out(s->policy_out == FW_ACT_DROP ? "drop" : "accept");
    out(" rules="); out_u(s->rules); out(" flows="); out_u(s->ct_entries);
    out(" in_drop="); out_u(s->in_drop); out(" bad_drop="); out_u(s->bad_drop); out("\n");
    return 0;
}
static int do_list(void) {
    uapi_fw_req_t q; zero(&q, sizeof(q)); q.op = FW_OP_STATUS;
    if (fw(&q) < 0) { out("ERR fw status\n"); return 2; }
    u32 n = q.stats.rules;
    for (u32 i = 0; i < n && i < FW_MAX_RULES; i++) {
        uapi_fw_req_t g; zero(&g, sizeof(g)); g.op = FW_OP_GET; g.index = (int)i;
        if (fw(&g) < 0) { out("ERR fw get\n"); return 2; }
        out("RULE "); out_u(i); out(g.rule.dir == FW_DIR_IN ? " in " : " out ");
        out(g.rule.action == FW_ACT_DROP ? "drop " : "accept ");
        out(g.rule.proto == 6 ? "tcp" : g.rule.proto == 17 ? "udp" : g.rule.proto == 1 ? "icmp" : "any");
        out(" dport="); out_u(g.rule.dport_lo); if (g.rule.dport_hi != g.rule.dport_lo) { out("-"); out_u(g.rule.dport_hi); }
        out(" src="); if (g.rule.src_mask) { out_u(g.rule.src >> 24); out("."); out_u((g.rule.src >> 16) & 255); out("."); out_u((g.rule.src >> 8) & 255); out("."); out_u(g.rule.src & 255); } else out("any");
        out(" hits="); out_u(g.hits); out("\n");
    }
    if (!n) out("RULES none\n");
    return 0;
}

#ifndef TOOL_FW_RO
static int report(long r) {
    if (r >= 0) return 0;
    if (r == -1)  return deny("kernel refused: this process has no CAP_NET_ADMIN");
    if (r == -28) return deny("rule table full");
    if (r == -34) return deny("no such rule");
    return deny("kernel rejected the request");
}
static int do_allow_block(int allow, int argc, char** argv) {
    /* allow|block <tcp|udp> <port|lo-hi> [from <addr[/len>=8]>] */
    if (argc < 4) return deny("usage: allow|block <tcp|udp> <port> [from <addr>]");
    uapi_fw_req_t q; zero(&q, sizeof(q)); q.op = FW_OP_ADD;
    q.rule.dir = FW_DIR_IN; q.rule.action = allow ? FW_ACT_ACCEPT : FW_ACT_DROP;
    q.rule.sport_lo = 0; q.rule.sport_hi = 65535;
    if (streq(argv[2], "tcp")) q.rule.proto = 6; else if (streq(argv[2], "udp")) q.rule.proto = 17;
    else return deny("proto must be tcp or udp");
    u32 lo = 0, hi = 0; const char* p = argv[3]; char a[8]; int i = 0;
    while (*p && *p != '-' && i < 7) a[i++] = *p++;
    a[i] = 0;
    if (!parse_u(a, &lo) || lo == 0 || lo > 65535) return deny("bad port");
    hi = lo;
    if (*p == '-') { if (!parse_u(p + 1, &hi) || hi < lo || hi > 65535) return deny("bad port range"); }
    if (hi - lo >= 16) return deny("port range too wide for the agent (max 16)");
    q.rule.dport_lo = (u16)lo; q.rule.dport_hi = (u16)hi;
    if (argc >= 5) {
        if (argc != 6 || !streq(argv[4], "from")) return deny("expected: from <addr>");
        if (!parse_ip(argv[5], &q.rule.src, &q.rule.src_mask)) return deny("bad address (prefix must be /8../32)");
    } else if (allow) {
        /* an unrestricted inbound allow is a LAN/Internet exposure decision: keep it visible */
        out("NOTE inbound allow from ANY source\n");
    }
    long r = fw(&q);
    if (report(r)) return 2;
    out(allow ? "ALLOW rule " : "BLOCK rule "); out_u((u64)r); out("\n");
    return 0;
}
static int do_del(int argc, char** argv) {
    u32 i; if (argc < 3 || !parse_u(argv[2], &i) || i >= FW_MAX_RULES) return deny("usage: del <index>");
    uapi_fw_req_t q; zero(&q, sizeof(q)); q.op = FW_OP_DEL; q.index = (int)i;
    if (report(fw(&q))) return 2;
    out("DELETED rule "); out_u(i); out("\n"); return 0;
}
static int do_policy(int argc, char** argv) {
    if (argc < 4) return deny("usage: policy <in|out> <accept|drop>");
    u32 dir = streq(argv[2], "in") ? FW_DIR_IN : streq(argv[2], "out") ? FW_DIR_OUT : 9;
    u32 act = streq(argv[3], "accept") ? FW_ACT_ACCEPT : streq(argv[3], "drop") ? FW_ACT_DROP : 9;
    if (dir == 9 || act == 9) return deny("usage: policy <in|out> <accept|drop>");
    if (dir == FW_DIR_IN  && act == FW_ACT_ACCEPT) return deny("policy in accept would expose the whole host (human-only: fwctl)");
    if (dir == FW_DIR_OUT && act == FW_ACT_DROP)   return deny("policy out drop would sever the agent's own link (human-only: fwctl)");
    uapi_fw_req_t q; zero(&q, sizeof(q)); q.op = FW_OP_SET_POLICY; q.arg = dir | (act << 8);
    if (report(fw(&q))) return 2;
    out("POLICY set\n"); return 0;
}
static int do_ping(int argc, char** argv) {
    if (argc < 3 || (!streq(argv[2], "on") && !streq(argv[2], "off"))) return deny("usage: ping <on|off>");
    uapi_fw_req_t q; zero(&q, sizeof(q)); q.op = FW_OP_SET_PING; q.arg = streq(argv[2], "on") ? 1 : 0;
    if (report(fw(&q))) return 2;
    out("PING set\n"); return 0;
}
#endif

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) return deny("no verb");
    const char* v = argv[1];
    if (streq(v, "status")) return do_status();
    if (streq(v, "list"))   return do_list();
#ifndef TOOL_FW_RO
    if (streq(v, "allow"))  return do_allow_block(1, argc, argv);
    if (streq(v, "block"))  return do_allow_block(0, argc, argv);
    if (streq(v, "del"))    return do_del(argc, argv);
    if (streq(v, "policy")) return do_policy(argc, argv);
    if (streq(v, "ping"))   return do_ping(argc, argv);
#endif
    return deny("verb not available to the agent");
}

/*
 * fwctl.c -- management tool for the kernel packet filter (FW-0).
 * =================================================================
 * Freestanding ring-3 tool, linked with crt0 (main(argc, argv)). Talks to the
 * kernel through SYS_FW_CTL (uapi/fw.h). Reading (status/list) is open to any
 * process; every change needs PCAP_NET_ADMIN, which a sandboxed/automation
 * process may have dropped -- the kernel then answers EPERM and we say so.
 *
 *   fwctl status
 *   fwctl list
 *   fwctl allow  <tcp|udp> <port|lo-hi> [from <addr[/len]>]     open an inbound service
 *   fwctl block  <tcp|udp> <port|lo-hi> [from <addr[/len]>]     refuse it explicitly
 *   fwctl add <in|out> <accept|drop> [proto tcp|udp|icmp|any] [src A[/L]] [dst A[/L]]
 *             [sport R] [dport R] [log] [at N]
 *   fwctl del <index>
 *   fwctl flush                      remove every rule (policies unchanged)
 *   fwctl policy <in|out> <accept|drop>
 *   fwctl ping <on|off>              answer inbound ICMP echo (rate limited)
 *   fwctl enable | disable           master switch (the stateless-allow-all escape hatch)
 *   fwctl reset                      factory defaults: no rules, IN=drop OUT=accept, ping on
 *
 * Addresses are dotted quads with an optional /prefix ("any" = everything).
 * Port ranges are N or N-M ("any" = all).
 */
#include "../../../kernel/include/uapi/fw.h"

#define SYS_EXIT   0
#define SYS_WRITE  3
#define SYS_FW_CTL 136

typedef unsigned long size_t;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

static inline long sc3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void put(const char* s) { sc3(SYS_WRITE, 1, (long)s, (long)slen(s)); }
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void put_u(u64 v) {
    char b[24]; int i = 23; b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    put(&b[i]);
}
static void put_pad(const char* s, int w) { int n = (int)slen(s); put(s); while (n++ < w) put(" "); }
static void put_ip(u32 ip, u32 mask) {
    if (mask == 0) { put("any"); return; }
    put_u(ip >> 24); put("."); put_u((ip >> 16) & 255); put("."); put_u((ip >> 8) & 255); put("."); put_u(ip & 255);
    int len = 0; for (u32 m = mask; m & 0x80000000u; m <<= 1) len++;
    if (len != 32) { put("/"); put_u((u64)len); }
}
static void put_ports(u16 lo, u16 hi) {
    if (lo == 0 && hi == 65535) { put("*"); return; }
    put_u(lo); if (hi != lo) { put("-"); put_u(hi); }
}

static void zero(void* p, size_t n) { volatile u8* b = (volatile u8*)p; while (n--) *b++ = 0; }

static int parse_u(const char* s, u32* out) {
    if (!*s) return 0;
    u32 v = 0;
    for (; *s; s++) { if (*s < '0' || *s > '9') return 0; v = v * 10 + (u32)(*s - '0'); if (v > 0xFFFFFFu) return 0; }
    *out = v; return 1;
}
static int parse_ip(const char* s, u32* ip, u32* mask) {
    if (streq(s, "any")) { *ip = 0; *mask = 0; return 1; }
    u32 parts[4]; int n = 0; u32 cur = 0; int digits = 0; u32 prefix = 32;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') { cur = cur * 10 + (u32)(*s - '0'); digits++; if (cur > 255) return 0; }
        else if (*s == '.' || *s == '/' || *s == 0) {
            if (!digits || n > 3) return 0;
            parts[n++] = cur; cur = 0; digits = 0;
            if (*s == '/') { u32 p = 0; if (!parse_u(s + 1, &p) || p > 32) return 0; prefix = p; break; }
            if (*s == 0) break;
        } else return 0;
    }
    if (n != 4) return 0;
    *ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    *mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
    *ip &= *mask ? *mask : 0xFFFFFFFFu;
    if (*mask == 0) *ip = 0;
    return 1;
}
static int parse_ports(const char* s, u16* lo, u16* hi) {
    if (streq(s, "any")) { *lo = 0; *hi = 65535; return 1; }
    char a[8]; int i = 0;
    while (*s && *s != '-' && i < 7) a[i++] = *s++;
    a[i] = 0;
    u32 l, h;
    if (!parse_u(a, &l) || l > 65535) return 0;
    if (*s == '-') { if (!parse_u(s + 1, &h) || h > 65535 || h < l) return 0; } else h = l;
    *lo = (u16)l; *hi = (u16)h; return 1;
}
static int parse_proto(const char* s, u8* p) {
    if (streq(s, "tcp")) *p = 6; else if (streq(s, "udp")) *p = 17;
    else if (streq(s, "icmp")) *p = 1; else if (streq(s, "any")) *p = 0; else return 0;
    return 1;
}
static const char* proto_name(u8 p) { return p == 6 ? "tcp" : p == 17 ? "udp" : p == 1 ? "icmp" : "any"; }

static long fw(uapi_fw_req_t* q) { return sc3(SYS_FW_CTL, (long)q, 0, 0); }

static int report(long r, const char* what) {
    if (r >= 0) return 0;
    put("fwctl: "); put(what); put(": ");
    put(r == -1 ? "permission denied (this process dropped CAP_NET_ADMIN)"
        : r == -22 ? "invalid argument" : r == -28 ? "rule table full" : r == -34 ? "no such rule" : "failed");
    put("\n");
    return 2;
}

static int cmd_status(void) {
    uapi_fw_req_t q; zero(&q, sizeof(q)); q.op = FW_OP_STATUS;
    int e = report(fw(&q), "status"); if (e) return e;
    const uapi_fw_stats_t* s = &q.stats;
    put("firewall      : "); put(s->enabled ? "ENABLED" : "DISABLED (allow-all)"); put("\n");
    put("policy        : in="); put(s->policy_in == FW_ACT_DROP ? "drop" : "accept");
    put(" out="); put(s->policy_out == FW_ACT_DROP ? "drop" : "accept"); put("\n");
    put("icmp echo     : "); put(s->allow_ping ? "answered (rate limited)" : "ignored"); put("\n");
    put("rules         : "); put_u(s->rules); put("   tracked flows: "); put_u(s->ct_entries); put("\n");
    put("inbound       : accepted "); put_u(s->in_accept); put("  dropped "); put_u(s->in_drop); put("\n");
    put("outbound      : accepted "); put_u(s->out_accept); put("  dropped "); put_u(s->out_drop); put("\n");
    put("return traffic: "); put_u(s->ct_hits); put(" admitted via "); put_u(s->ct_new); put(" tracked flows ("); put_u(s->ct_evicted); put(" recycled)\n");
    put("sanity drops  : "); put_u(s->bad_drop); put(" (spoof/scan/fragment)\n");
    return 0;
}

static int cmd_list(void) {
    uapi_fw_req_t q; zero(&q, sizeof(q)); q.op = FW_OP_STATUS;
    int e = report(fw(&q), "status"); if (e) return e;
    u32 n = q.stats.rules;
    put("#   dir act    proto src                 dst                 sport        dport        hits\n");
    for (u32 i = 0; i < n; i++) {
        uapi_fw_req_t g; zero(&g, sizeof(g)); g.op = FW_OP_GET; g.index = (int)i;
        if (report(fw(&g), "get")) return 2;
        put_pad("", 0); put_u(i); put(i < 10 ? "   " : "  ");
        put_pad(g.rule.dir == FW_DIR_IN ? "in" : "out", 4);
        put_pad(g.rule.action == FW_ACT_DROP ? "drop" : "accept", 7);
        put_pad(proto_name(g.rule.proto), 6);
        put_ip(g.rule.src, g.rule.src_mask); put("  ");
        put_ip(g.rule.dst, g.rule.dst_mask); put("  ");
        put_ports(g.rule.sport_lo, g.rule.sport_hi); put("  ");
        put_ports(g.rule.dport_lo, g.rule.dport_hi); put("  ");
        put_u(g.hits); if (g.rule.flags & FW_RULE_LOG) put("  [log]"); put("\n");
    }
    if (!n) put("(no rules)\n");
    return 0;
}

static int usage(void) {
    put("usage: fwctl status|list|flush|reset|enable|disable\n"
        "       fwctl allow|block <tcp|udp> <port|lo-hi> [from <addr[/len]>]\n"
        "       fwctl add <in|out> <accept|drop> [proto P] [src A] [dst A] [sport R] [dport R] [log] [at N]\n"
        "       fwctl del <index>   fwctl policy <in|out> <accept|drop>   fwctl ping <on|off>\n");
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 2) return cmd_status();
    const char* c = argv[1];
    uapi_fw_req_t q; zero(&q, sizeof(q));

    if (streq(c, "status")) return cmd_status();
    if (streq(c, "list"))   return cmd_list();
    if (streq(c, "flush"))  { q.op = FW_OP_FLUSH;  return report(fw(&q), "flush"); }
    if (streq(c, "reset"))  { q.op = FW_OP_RESET;  return report(fw(&q), "reset"); }
    if (streq(c, "enable")) { q.op = FW_OP_ENABLE; q.arg = 1; return report(fw(&q), "enable"); }
    if (streq(c, "disable")){ q.op = FW_OP_ENABLE; q.arg = 0; return report(fw(&q), "disable"); }
    if (streq(c, "ping")) {
        if (argc < 3) return usage();
        q.op = FW_OP_SET_PING; q.arg = streq(argv[2], "on") ? 1 : 0;
        if (!streq(argv[2], "on") && !streq(argv[2], "off")) return usage();
        return report(fw(&q), "ping");
    }
    if (streq(c, "del")) {
        u32 i; if (argc < 3 || !parse_u(argv[2], &i)) return usage();
        q.op = FW_OP_DEL; q.index = (int)i; return report(fw(&q), "del");
    }
    if (streq(c, "policy")) {
        if (argc < 4) return usage();
        u32 dir = streq(argv[2], "in") ? FW_DIR_IN : streq(argv[2], "out") ? FW_DIR_OUT : 9;
        u32 act = streq(argv[3], "accept") ? FW_ACT_ACCEPT : streq(argv[3], "drop") ? FW_ACT_DROP : 9;
        if (dir == 9 || act == 9) return usage();
        q.op = FW_OP_SET_POLICY; q.arg = dir | (act << 8); return report(fw(&q), "policy");
    }

    /* allow / block sugar and the general `add` */
    q.op = FW_OP_ADD;
    q.rule.sport_lo = 0; q.rule.sport_hi = 65535; q.rule.dport_lo = 0; q.rule.dport_hi = 65535;
    int i = 2, at = -1;
    if (streq(c, "allow") || streq(c, "block")) {
        if (argc < 4) return usage();
        q.rule.dir = FW_DIR_IN; q.rule.action = streq(c, "allow") ? FW_ACT_ACCEPT : FW_ACT_DROP;
        if (!parse_proto(argv[2], &q.rule.proto) || q.rule.proto == 0 || q.rule.proto == 1) return usage();
        if (!parse_ports(argv[3], &q.rule.dport_lo, &q.rule.dport_hi)) return usage();
        i = 4;
    } else if (streq(c, "add")) {
        if (argc < 4) return usage();
        q.rule.dir = streq(argv[2], "in") ? FW_DIR_IN : streq(argv[2], "out") ? FW_DIR_OUT : 9;
        q.rule.action = streq(argv[3], "accept") ? FW_ACT_ACCEPT : streq(argv[3], "drop") ? FW_ACT_DROP : 9;
        if (q.rule.dir == 9 || q.rule.action == 9) return usage();
        i = 4;
    } else return usage();

    for (; i < argc; i++) {
        const char* k = argv[i];
        if (streq(k, "log")) { q.rule.flags |= FW_RULE_LOG; continue; }
        if (i + 1 >= argc) return usage();
        const char* v = argv[++i];
        if      (streq(k, "proto")) { if (!parse_proto(v, &q.rule.proto)) return usage(); }
        else if (streq(k, "src") || streq(k, "from")) { if (!parse_ip(v, &q.rule.src, &q.rule.src_mask)) return usage(); }
        else if (streq(k, "dst") || streq(k, "to"))   { if (!parse_ip(v, &q.rule.dst, &q.rule.dst_mask)) return usage(); }
        else if (streq(k, "sport")) { if (!parse_ports(v, &q.rule.sport_lo, &q.rule.sport_hi)) return usage(); }
        else if (streq(k, "dport")) { if (!parse_ports(v, &q.rule.dport_lo, &q.rule.dport_hi)) return usage(); }
        else if (streq(k, "at"))    { u32 n; if (!parse_u(v, &n)) return usage(); at = (int)n; }
        else return usage();
    }
    if (at >= 0) { q.op = FW_OP_INSERT; q.index = at; }
    long r = fw(&q);
    if (report(r, "add")) return 2;
    put("rule "); put_u((u64)r); put(" added\n");
    return 0;
}

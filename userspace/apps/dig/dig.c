/*
 * dig.c -- DNS lookup tool (freestanding, ring 3).
 * ================================================
 *
 * A from-scratch `dig`-style A-record resolver for AutomationOS userspace.
 * NO libc, NO stdio, NO malloc, NO standard headers -- everything is built on
 * inline syscalls, fixed-size local/static buffers and a handful of small
 * static helpers.  Diagnostics + results go to fd 1 (stdout/console/serial)
 * via SYS_WRITE, exactly like nc.c / ping.c / wget.c.
 *
 * Unlike nc/ping/wget (which call the dns.c library's dns_resolve), this tool
 * crafts the DNS A-record query ON THE WIRE itself and parses the response
 * by hand, to show the full UDP DNS round-trip:
 *
 *   1. open a UDP socket            -> SYS_SOCKET(SOCK_DGRAM)
 *   2. build a standard DNS query   (12-byte header + QNAME labels + QTYPE=A +
 *                                     QCLASS=IN), RD=1, fresh transaction id
 *   3. SYS_SENDTO it to 10.0.2.3:53 (the QEMU slirp DNS resolver, host order)
 *   4. pump the NIC + SYS_RECVFROM in a bounded poll loop (5 s wall-clock cap)
 *   5. parse the reply: validate header (QR/OPCODE/RCODE), skip the question,
 *      walk each answer record skipping the NAME (handling the 0xC0 compression
 *      pointer), and read the first A/IN record's 4-byte RDATA address.
 *   6. print the resolved IPv4 as A.B.C.D.
 *
 * A dotted-quad argument ("a.b.c.d") is recognised and printed directly with
 * NO network activity at all.
 *
 * Usage (linked with crt0.o -> int main(int argc, char **argv)):
 *     dig HOST        resolve HOST's first A record, print "HOST has address X"
 *     dig             (argc <= 1) print a short usage line
 *
 * Bounded: the receive loop has BOTH an iteration cap AND a 5-second wall-clock
 * deadline (SYS_GET_TICKS_MS), with a SYS_YIELD between dry polls, so an
 * unresponsive resolver can never hang the tool.
 *
 * Build (flags passed DIRECTLY on the command line, NEVER via a variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/dig/dig.c -o /tmp/dig.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/crt0.o /tmp/dig.o -o /tmp/dig.elf
 *   objdump -d /tmp/dig.o | grep fs:0x28   # MUST be empty (no stack canary)
 *
 * NOTE: dig links ONLY crt0.o (no dns.o) -- it is fully self-contained.
 */

/* ---- syscall numbers (per AutomationOS ABI; identical to dns.c/nc.c) ---- */
#define SYS_WRITE        3    /* write(fd, buf, len)         fd1 = stdout      */
#define SYS_YIELD        15   /* cooperative yield                            */
#define SYS_GET_TICKS_MS 40   /* returns current time in milliseconds         */
#define SYS_SOCKET       51   /* socket(type) -> fd                           */
#define SYS_CLOSE_SK     55   /* close(fd) -> 0                               */
#define SYS_SENDTO       56   /* sendto(fd, buf, len, ip_host, port) -> bytes */
#define SYS_RECVFROM     57   /* recvfrom(fd, buf, len, &addr, 0) -> bytes    */
#define SYS_SOCK_POLL    58   /* pump the NIC RX/timers; call before each recv */

/* ---- socket type ------------------------------------------------------- */
#define SOCK_DGRAM       2

/* ---- well-known fd ----------------------------------------------------- */
#define FD_STDOUT        1

/* ---- DNS environment (QEMU slirp), host byte order --------------------- */
#define DNS_SERVER       0x0A000203u  /* 10.0.2.3 */
#define DNS_PORT         53

/*
 * Receive bound: an iteration cap (matching dns.c's DNS_POLL_MAX magnitude)
 * PLUS a 5-second wall-clock deadline.  Whichever fires first ends the wait,
 * so the tool always returns.
 */
#define POLL_MAX         200000
#define TIMEOUT_MS       5000

/* ===================================================================== */
/* Raw 6-arg inline syscall (rdi/rsi/rdx/r10/r8) -- copied verbatim from  */
/* nc.c / ping.c / wget.c / dns.c.                                        */
/* ===================================================================== */
static long sc(long n, long a1, long a2, long a3, long a4, long a5)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

/* ===================================================================== */
/* Tiny freestanding helpers (no libc) -- same style as the model apps.  */
/* ===================================================================== */

static unsigned int d_strlen(const char *s)
{
    unsigned int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* ---- stdout (fd1) print helpers (serial / console) -------------------- */

static void out_write(const char *buf, unsigned int len)
{
    if (len == 0) return;
    sc(SYS_WRITE, FD_STDOUT, (long)buf, (long)len, 0, 0);
}

static void out_puts(const char *s)
{
    out_write(s, d_strlen(s));
}

/* Print an unsigned decimal number to fd1. */
static void out_unum(unsigned long v)
{
    char b[24];
    int i = 0;
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    char rev[24];
    int j = 0;
    while (i > 0) rev[j++] = b[--i];
    out_write(rev, (unsigned int)j);
}

/* Print a signed decimal number to fd1. */
static void out_num(long v)
{
    if (v < 0) { out_puts("-"); out_unum((unsigned long)(-v)); }
    else        out_unum((unsigned long)v);
}

/* Print a host-order IPv4 address as A.B.C.D to fd1. */
static void out_ip(unsigned int ip)
{
    out_unum((ip >> 24) & 0xFFu); out_puts(".");
    out_unum((ip >> 16) & 0xFFu); out_puts(".");
    out_unum((ip >>  8) & 0xFFu); out_puts(".");
    out_unum( ip        & 0xFFu);
}

/* Current tick timestamp via SYS_GET_TICKS_MS (negative -> 0). */
static unsigned int get_ticks_ms(void)
{
    long t = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0);
    return (unsigned int)(t < 0 ? 0 : t);
}

/* ===================================================================== */
/* DNS dotted-quad fast path (no network), mirrors dns.c parse_dotted_quad */
/* ===================================================================== */
static int d_is_digit(char c) { return c >= '0' && c <= '9'; }

static int parse_dotted_quad(const char *s, unsigned int *out)
{
    unsigned int ip = 0;
    int octets = 0;
    unsigned int cur = 0;
    int digits = 0;

    for (;; s++) {
        char c = *s;
        if (d_is_digit(c)) {
            cur = cur * 10u + (unsigned int)(c - '0');
            if (cur > 255u) return 0;
            digits++;
            if (digits > 3) return 0;
        } else if (c == '.' || c == '\0') {
            if (!digits) return 0;
            ip = (ip << 8) | cur;
            cur = 0; digits = 0;
            octets++;
            if (c == '\0') break;
            if (octets > 3) return 0;
        } else {
            return 0;
        }
    }
    if (octets != 4) return 0;
    *out = ip;
    return 1;
}

/* ===================================================================== */
/* DNS wire helpers                                                      */
/* ===================================================================== */

/* sock_addr_t filled by SYS_RECVFROM out_addr (ip + port in HOST order). */
typedef struct {
    unsigned int   ip;
    unsigned short port;
    unsigned short _pad;
} sock_addr_t;

/*
 * Encode a hostname into DNS QNAME label format at `out`.
 * "www.example.com" -> 3 w w w 7 e x a m p l e 3 c o m 0
 * Returns the number of bytes written, or 0 on error (label/name too long).
 * `cap` is the available space in out.  (Same algorithm as dns.c encode_qname.)
 */
static int encode_qname(const char *host, unsigned char *out, int cap)
{
    int pos = 0;
    const char *seg = host;

    for (;;) {
        int len = 0;
        while (seg[len] && seg[len] != '.') len++;

        if (len == 0) break;                    /* empty label terminates */
        if (len > 63) return 0;                 /* label too long          */
        if (pos + 1 + len >= cap) return 0;     /* +room for terminating 0 */

        out[pos++] = (unsigned char)len;
        for (int i = 0; i < len; i++) out[pos++] = (unsigned char)seg[i];

        seg += len;
        if (*seg == '.') {
            seg++;
            if (*seg == '\0') break;             /* trailing dot terminates */
        } else {
            break;
        }
    }

    if (pos + 1 > cap) return 0;
    out[pos++] = 0;                              /* root label */
    return pos;
}

/* Read a big-endian 16-bit value at msg[off], or -1 if out of bounds. */
static int read_be16(const unsigned char *msg, int len, int off)
{
    if (off < 0 || off + 1 >= len) return -1;
    return ((int)msg[off] << 8) | (int)msg[off + 1];
}

/*
 * Skip a DNS name starting at offset `off` within a `len`-byte message.
 * Handles label sequences AND compression pointers (0xC0 prefix).
 * Returns the offset of the byte FOLLOWING the name, or -1 on a malformed
 * name / out-of-bounds read.  (Same logic as dns.c skip_name.)
 */
static int skip_name(const unsigned char *msg, int len, int off)
{
    int guard = 0;
    while (off >= 0 && off < len) {
        unsigned char b = msg[off];

        if ((b & 0xC0) == 0xC0) {                /* compression pointer */
            if (off + 1 >= len) return -1;
            return off + 2;                      /* 2-byte pointer ends name */
        }
        if (b == 0) return off + 1;              /* root label -> end of name */

        off += 1 + b;                            /* ordinary label */
        if (++guard > 128) return -1;            /* runaway / loop guard */
    }
    return -1;
}

/*
 * Parse a DNS response `resp` of length `rlen` issued with transaction id `id`.
 * Validates the header, skips the question, then walks the answer section
 * (skipping each NAME via skip_name, which follows the 0xC0 compression
 * pointer) and returns the FIRST A/IN record's 4-byte address in host byte
 * order via *out_ip.  Returns 0 on success, negative on any failure.
 */
static int parse_a_record(const unsigned char *resp, int rlen,
                          unsigned short id, unsigned int *out_ip)
{
    if (rlen < 12) return -1;                    /* too short for a header */

    if (read_be16(resp, rlen, 0) != (int)id) return -2;   /* wrong txid */

    unsigned char flags_hi = resp[2];
    unsigned char flags_lo = resp[3];
    if ((flags_hi & 0x80) == 0) return -3;       /* QR must be 1 (response) */
    if ((flags_hi & 0x78) != 0) return -4;       /* OPCODE must be 0 (QUERY) */
    if ((flags_lo & 0x0F) != 0) return -5;       /* RCODE must be 0 (NOERROR) */

    int qdcount = read_be16(resp, rlen, 4);
    int ancount = read_be16(resp, rlen, 6);
    if (qdcount < 0 || ancount <= 0) return -6;  /* no answers */

    /* ---- skip the question section(s): QNAME + QTYPE(2) + QCLASS(2) ---- */
    int off = 12;
    for (int q = 0; q < qdcount; q++) {
        off = skip_name(resp, rlen, off);
        if (off < 0) return -7;
        off += 4;
        if (off > rlen) return -7;
    }

    /* ---- walk the answer records, return the first A/IN address ---- */
    int iter_max = ancount > 64 ? 64 : ancount;  /* cap vs crafted ancount */
    for (int a = 0; a < iter_max; a++) {
        off = skip_name(resp, rlen, off);        /* handles 0xC0 pointer */
        if (off < 0) return -8;

        int type     = read_be16(resp, rlen, off);
        int klass    = read_be16(resp, rlen, off + 2);
        int rdlength = read_be16(resp, rlen, off + 8);
        if (type < 0 || klass < 0 || rdlength < 0) return -9;

        int rdata = off + 10;                    /* skip TYPE,CLASS,TTL,RDLEN */
        if (rdata + rdlength > rlen) return -10;

        if (type == 1 /* A */ && klass == 1 /* IN */ && rdlength == 4) {
            *out_ip = ((unsigned int)resp[rdata]     << 24) |
                      ((unsigned int)resp[rdata + 1] << 16) |
                      ((unsigned int)resp[rdata + 2] <<  8) |
                      ((unsigned int)resp[rdata + 3]);
            return 0;
        }
        off = rdata + rdlength;                  /* skip non-A RR (AAAA/CNAME) */
    }

    return -11;                                  /* no A record found */
}

/* ===================================================================== */
/* Live DNS query: build + send + bounded-poll + parse.                  */
/* Returns 0 and sets *out_ip on success; negative on failure.           */
/* ===================================================================== */
static int dns_query_a(const char *host, unsigned int *out_ip)
{
    unsigned int hlen = d_strlen(host);
    if (hlen == 0 || hlen > 253) return -22;     /* bad name length */

    long fd = sc(SYS_SOCKET, SOCK_DGRAM, 0, 0, 0, 0);
    if (fd < 0) return -1;

    unsigned char query[300];
    unsigned char resp[512];
    sock_addr_t from;

    /* Fresh-ish transaction id from the NIC pump return + name mix. */
    unsigned int seed = (unsigned int)sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
    for (unsigned int i = 0; i < hlen; i++)
        seed = seed * 31u + (unsigned char)host[i];
    unsigned short id = (unsigned short)(seed ^ (seed >> 16) ^ 0xA53Cu);
    if (id == 0) id = 0x1234;

    /* ---- build the 12-byte DNS header ---- */
    int qlen = 0;
    query[qlen++] = (unsigned char)(id >> 8);
    query[qlen++] = (unsigned char)(id & 0xFF);
    query[qlen++] = 0x01;                        /* flags hi: RD=1 */
    query[qlen++] = 0x00;                        /* flags lo       */
    query[qlen++] = 0x00; query[qlen++] = 0x01;  /* QDCOUNT = 1 */
    query[qlen++] = 0x00; query[qlen++] = 0x00;  /* ANCOUNT = 0 */
    query[qlen++] = 0x00; query[qlen++] = 0x00;  /* NSCOUNT = 0 */
    query[qlen++] = 0x00; query[qlen++] = 0x00;  /* ARCOUNT = 0 */

    /* ---- QNAME labels ---- */
    int nlen = encode_qname(host, query + qlen, (int)sizeof(query) - qlen - 4);
    if (nlen <= 0) { sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0); return -22; }
    qlen += nlen;

    /* ---- QTYPE = A (1), QCLASS = IN (1) ---- */
    query[qlen++] = 0x00; query[qlen++] = 0x01;
    query[qlen++] = 0x00; query[qlen++] = 0x01;

    /* ---- send to 10.0.2.3:53 (host order ip + port) ---- */
    long sent = sc(SYS_SENDTO, fd, (long)query, qlen,
                   (long)DNS_SERVER, DNS_PORT);
    if (sent < 0) { sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0); return -2; }

    /* ---- bounded poll loop: iteration cap + 5 s wall-clock deadline ---- */
    unsigned int start = get_ticks_ms();
    int rc = -110;                               /* default: timeout */
    for (int it = 0; it < POLL_MAX; it++) {
        sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);        /* pump the NIC */

        long n = sc(SYS_RECVFROM, fd, (long)resp, (long)sizeof(resp),
                    (long)&from, 0);
        if (n > 0) {
            rc = parse_a_record(resp, (int)n, id, out_ip);
            break;
        }

        /* 5-second wall-clock cap (handles the case where a reply never comes). */
        if (get_ticks_ms() - start >= TIMEOUT_MS) { rc = -110; break; }

        sc(SYS_YIELD, 0, 0, 0, 0, 0);            /* let NIC/timers progress */
    }

    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    return rc;
}

/* ===================================================================== */
/* Entry point -- crt0 turns the return value into SYS_EXIT.             */
/* ===================================================================== */
int main(int argc, char **argv)
{
    if (argc <= 1 || !argv[1] || !argv[1][0]) {
        out_puts("usage: dig HOST\n");
        return 1;
    }

    const char *host = argv[1];

    /* Dotted-quad fast path: print it directly, no network. */
    unsigned int ip = 0;
    if (parse_dotted_quad(host, &ip)) {
        out_puts(host);
        out_puts(" has address ");
        out_ip(ip);
        out_puts("\n");
        return 0;
    }

    out_puts("dig: querying ");
    out_ip(DNS_SERVER);
    out_puts(":");
    out_num(DNS_PORT);
    out_puts(" for A record of ");
    out_puts(host);
    out_puts("\n");

    int r = dns_query_a(host, &ip);
    if (r != 0) {
        out_puts("dig: lookup failed for '");
        out_puts(host);
        out_puts("' (rc=");
        out_num(r);
        out_puts(r == -110 ? ", timeout)\n" : ")\n");
        return 2;
    }

    out_puts(host);
    out_puts(" has address ");
    out_ip(ip);
    out_puts("\n");
    return 0;
}

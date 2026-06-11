/*
 * tcpconnect.c -- TCP connectivity test tool (freestanding, ring 3).
 * ===================================================================
 *
 * Takes an IP address (dotted-quad) and port number as argv, creates a
 * SOCK_STREAM socket, and attempts a TCP connect.  Prints "connected" on
 * success or an error message on failure.
 *
 * Usage:
 *   tcpconnect <ip> <port>        -- connect to ip:port via TCP
 *   tcpconnect                    -- run built-in self-test (no network)
 *
 * Examples:
 *   tcpconnect 10.0.2.2 80       -- connect to QEMU gateway port 80
 *   tcpconnect 93.184.216.34 80  -- connect to example.com
 *
 * NO libc, NO stdio, NO malloc, NO standard headers.
 * Inline syscalls + fixed buffers + own helpers only.
 *
 * Build (flags DIRECT on cmdline -- NEVER via shell variable):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/tcpconnect/tcpconnect.c -o tcpconnect.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       userspace/crt0.o tcpconnect.o -o build/tcpconnect
 *   objdump -d build/tcpconnect | grep fs:0x28   # MUST be empty
 */

/* ---- syscall numbers (must match kernel/include/syscall.h) ---- */
#define SYS_EXIT       0
#define SYS_WRITE      3
#define SYS_SOCKET    51
#define SYS_CONNECT   52
#define SYS_CLOSE_SK  55

/* Socket type (must match kernel/include/socket.h). */
#define SOCK_STREAM    1

/* ---- 6-argument inline syscall ---- */
static inline long sc(long n, long a1, long a2, long a3,
                      long a4, long a5, long a6)
{
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                   "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny freestanding helpers ---- */

static unsigned long k_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *m)
{
    sc(SYS_WRITE, 1, (long)m, (long)k_strlen(m), 0, 0, 0);
}

static void print_ch(char c)
{
    sc(SYS_WRITE, 1, (long)&c, 1, 0, 0, 0);
}

static void print_dec(unsigned long v)
{
    char buf[20];
    int  i = 0;
    if (v == 0) { print_ch('0'); return; }
    do {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (i > 0) print_ch(buf[--i]);
}

static void print_ip(unsigned int ip)
{
    print_dec((ip >> 24) & 0xFFu);
    print_ch('.');
    print_dec((ip >> 16) & 0xFFu);
    print_ch('.');
    print_dec((ip >>  8) & 0xFFu);
    print_ch('.');
    print_dec(ip & 0xFFu);
}

/* Translate a negative errno to a short reason string (freestanding). */
static const char *rc_str(long e) {
    if (e >= 0)   return "ok";
    long v = -e;
    switch (v) {
        case  11: return "would block";
        case  12: return "out of memory";
        case  22: return "invalid argument";
        case 104: return "connection reset by peer";
        case 110: return "connection timed out";
        case 111: return "connection refused";
        case 113: return "no route to host";
        default:  return "error";
    }
}

/* ---- dotted-quad parser (mirrors dig.c / dns.c) ---- */

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int parse_dotted_quad(const char *s, unsigned int *out)
{
    unsigned int ip = 0;
    int octets = 0;
    unsigned int cur = 0;
    int digits = 0;

    for (;; s++) {
        char c = *s;
        if (is_digit(c)) {
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

/* ---- port parser (mirrors tcping.c) ---- */

static long parse_port(const char *s)
{
    if (!s || !*s) return -1;
    long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 65535) return -1;
    }
    if (v <= 0) return -1;
    return v;
}

/* ---- self-test (offline, no network) ---- */

static int selftest(void)
{
    int pass = 1;
    unsigned int ip;

    /* Test 1: valid dotted-quad parse. */
    if (!parse_dotted_quad("10.0.2.2", &ip)) {
        print("  FAIL: parse 10.0.2.2\n");
        pass = 0;
    } else if (ip != 0x0A000202u) {
        print("  FAIL: 10.0.2.2 value\n");
        pass = 0;
    }

    /* Test 2: valid dotted-quad parse (255.255.255.255). */
    if (!parse_dotted_quad("255.255.255.255", &ip)) {
        print("  FAIL: parse 255.255.255.255\n");
        pass = 0;
    } else if (ip != 0xFFFFFFFFu) {
        print("  FAIL: 255.255.255.255 value\n");
        pass = 0;
    }

    /* Test 3: invalid (octet > 255). */
    if (parse_dotted_quad("256.0.0.1", &ip)) {
        print("  FAIL: accepted 256.0.0.1\n");
        pass = 0;
    }

    /* Test 4: invalid (too few octets). */
    if (parse_dotted_quad("10.0.2", &ip)) {
        print("  FAIL: accepted 10.0.2\n");
        pass = 0;
    }

    /* Test 5: port parse. */
    if (parse_port("80") != 80) {
        print("  FAIL: parse port 80\n");
        pass = 0;
    }
    if (parse_port("0") != -1) {
        print("  FAIL: accepted port 0\n");
        pass = 0;
    }
    if (parse_port("70000") != -1) {
        print("  FAIL: accepted port 70000\n");
        pass = 0;
    }
    if (parse_port("abc") != -1) {
        print("  FAIL: accepted port abc\n");
        pass = 0;
    }

    return pass ? 0 : 1;
}

/* ---- entry point ---- */

int main(int argc, char **argv)
{
    /* Self-test mode: no arguments. */
    if (argc <= 1) {
        int rc = selftest();
        if (rc == 0) {
            print("TCPCONNECT SELFTEST: PASS\n");
            return 0;
        } else {
            print("TCPCONNECT SELFTEST: FAIL\n");
            return 1;
        }
    }

    /* Live mode: tcpconnect <ip> <port> */
    if (argc < 3) {
        print("usage: tcpconnect <ip> <port>\n");
        return 1;
    }

    /* Parse IP address. */
    unsigned int ip;
    if (!parse_dotted_quad(argv[1], &ip)) {
        print("tcpconnect: bad IP address: ");
        print(argv[1]);
        print("\n");
        return 1;
    }

    /* Parse port. */
    long port = parse_port(argv[2]);
    if (port < 0) {
        print("tcpconnect: bad port: ");
        print(argv[2]);
        print("\n");
        return 1;
    }

    /* Create TCP socket. */
    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0, 0);
    if (fd < 0) {
        print("tcpconnect: cannot create socket: ");
        print(rc_str(fd));
        print("\n");
        return 1;
    }

    /* Connect. */
    print("tcpconnect: connecting to ");
    print_ip(ip);
    print_ch(':');
    print_dec((unsigned long)port);
    print(" ...\n");

    long rc = sc(SYS_CONNECT, fd, (long)ip, port, 0, 0, 0);
    if (rc < 0) {
        print("tcpconnect: ");
        print(argv[1]);
        print(":");
        print(argv[2]);
        print(": ");
        print(rc_str(rc));
        print("\n");
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0, 0);
        return 1;
    }

    print("tcpconnect: connected to ");
    print_ip(ip);
    print_ch(':');
    print_dec((unsigned long)port);
    print("\n");

    /* Close the socket. */
    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0, 0);

    return 0;
}

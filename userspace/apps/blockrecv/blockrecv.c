/*
 * blockrecv.c -- NET-BLOCK-0 proof: blocking accept + blocking recv that do
 * NOT busy-spin. Two instances are spawned by init (BLOCKRECV_TEST=1); the
 * first binds the port and becomes the SERVER, the second's bind fails so it
 * becomes the CLIENT. They rendezvous over loopback (127.0.0.1):
 *
 *   SERVER: setsockopt(SO_BLOCKING) on the listener, blocking accept() (parks
 *           until the client connects -- proves blocking accept), SYS_SLEEP
 *           500 ms, then send "NETBLK-OK".
 *   CLIENT: connect (bounded retry until the server listens), setsockopt
 *           SO_BLOCKING, then a SINGLE blocking recv() that parks ~500 ms
 *           until the server sends -- proves blocking recv returns the data
 *           (not SOCK_EAGAIN) after waiting.
 *
 * The zero-CPU evidence is emitted KERNEL-side: the blocking loop prints
 *   [NETBLOCK] <recv|accept> blocked slices=<n> ms=<m>
 * where n ~= m/POLL_SLICE_MS (~100 slices for a 500 ms wait), i.e. it slept
 * in timed slices, NOT a 200000-iteration busy spin. The check script asserts
 * the client saw the marker AND slices is small.
 *
 * Bare _start, no libc (the cpu1hello/batchdemo house pattern).
 */

#define SYS_EXIT       0
#define SYS_WRITE      3
#define SYS_SLEEP      9
#define SYS_YIELD     15
#define SYS_SOCKET    51
#define SYS_CONNECT   52
#define SYS_SEND      53
#define SYS_RECV      54
#define SYS_CLOSE_SK  55
#define SYS_BIND      76
#define SYS_LISTEN    77
#define SYS_ACCEPT    78
#define SYS_GET_TICKS_MS 40
#define SYS_SETSOCKOPT 125

#define SOCK_STREAM   1
#define SOL_SOCKET    1
#define SO_BLOCKING   22
#define SOCK_EAGAIN  -11

#define BR_PORT      47700
#define LOOPBACK_IP  0x7F000001u    /* 127.0.0.1 host order */

static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile("syscall" : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

static unsigned long slen(const char* s){ unsigned long n=0; while(s[n]) n++; return n; }
static void print(const char* m){ sc(SYS_WRITE, 1, (long)m, (long)slen(m), 0, 0); }

static char* u2s(char* p, unsigned long v){
    char t[24]; int i=0;
    if(!v) t[i++]='0';
    while(v){ t[i++]='0'+(v%10); v/=10; }
    while(i) *p++=t[--i];
    return p;
}

/* setsockopt(fd, SOL_SOCKET, SO_BLOCKING, 1) -- pass &value via a1..a5 layout
 * matching sys_sock_setsockopt(s, level, optname, optval*, optlen). */
static void set_blocking(int fd){
    int one = 1;
    sc(SYS_SETSOCKOPT, fd, SOL_SOCKET, SO_BLOCKING, (long)&one, (long)sizeof(int));
}

int main(void){
    int fd = (int)sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) { print("BLOCKRECV: FAIL socket\n"); sc(SYS_EXIT,0,0,0,0,0); }

    long bound = sc(SYS_BIND, fd, BR_PORT, 0, 0, 0);
    if (bound == 0) {
        /* ---- SERVER role ---- */
        sc(SYS_LISTEN, fd, 4, 0, 0, 0);
        set_blocking(fd);                    /* blocking accept */
        print("BLOCKRECV: server listening, blocking accept\n");
        int cli = (int)sc(SYS_ACCEPT, fd, 0, 0, 0, 0);
        if (cli < 0) { print("BLOCKRECV: FAIL accept\n"); sc(SYS_EXIT,0,0,0,0,0); }
        sc(SYS_SLEEP, 500, 0, 0, 0, 0);      /* make the client's recv wait */
        sc(SYS_SEND, cli, (long)"NETBLK-OK", 9, 0, 0);
        sc(SYS_CLOSE_SK, cli, 0, 0, 0, 0);
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
        print("BLOCKRECV: server sent marker\n");
        sc(SYS_EXIT, 0, 0, 0, 0, 0);
    }

    /* ---- CLIENT role (bind failed: the server already owns the port) ---- */
    /* connect with a bounded retry until the server is listening. */
    int connected = 0;
    for (int tries = 0; tries < 200; tries++) {
        if (sc(SYS_CONNECT, fd, (long)LOOPBACK_IP, BR_PORT, 0, 0) == 0) { connected = 1; break; }
        sc(SYS_YIELD, 0, 0, 0, 0, 0);
    }
    if (!connected) { print("BLOCKRECV: FAIL connect\n"); sc(SYS_EXIT,0,0,0,0,0); }

    set_blocking(fd);                        /* blocking recv */
    char buf[32];
    long t0 = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0);
    int n = (int)sc(SYS_RECV, fd, (long)buf, sizeof(buf), 0, 0);
    long t1 = sc(SYS_GET_TICKS_MS, 0, 0, 0, 0, 0);

    const char* want = "NETBLK-OK";
    int match = (n == 9);
    for (int i = 0; i < 9 && match; i++) if (buf[i] != want[i]) match = 0;

    /* Diagnostic marker: report n, whether the bytes matched, wall time, and
     * the received bytes (printable) so a mismatch is visible in the log. */
    char line[128]; char* p = line;
    const char* s = "BLOCKRECV: "; while(*s) *p++=*s++;
    s = match ? "PASS got=NETBLK-OK n=" : "FAIL n="; while(*s) *p++=*s++;
    p = u2s(p, (unsigned long)(n < 0 ? (unsigned long)(-n) : (unsigned long)n));
    s = (n<0) ? "(neg)" : ""; while(*s) *p++=*s++;
    s = " waited_ms="; while(*s) *p++=*s++;
    p = u2s(p, (unsigned long)(t1 - t0));
    s = " data="; while(*s) *p++=*s++;
    for (int i = 0; i < n && i < 12; i++) {
        char c = buf[i];
        *p++ = (c >= 32 && c < 127) ? c : '.';
    }
    *p++='\n'; *p=0;
    print(line);

    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    sc(SYS_EXIT, 0, 0, 0, 0, 0);
    return 0;   /* unreachable (SYS_EXIT above); satisfies main's signature */
}

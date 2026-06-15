// pollselftest -- POLL-SELECT-0 (B10) proof. Self-contained (no libs, own
// _start, direct syscalls). Spawned by init; the smoke greps "POLLSELFTEST
// RESULT: PASS".
//
// Proves, on a real boot:
//   [1] poll() reports a ready (readable) fd                  -> a regular file
//   [2] select() reports a ready (readable) fd                -> same file
//   [3] poll() honors a timeout on a NOT-ready fd             -> an idle socket
//   [4] poll() over a MIXED set distinguishes ready/not-ready -> file + socket
//   [5] epoll LEVEL re-reports a still-ready fd; EDGE reports once
//
// Readiness here is REAL (fd_poll_state): files are always readable, an
// unconnected TCP socket is not. (No pipes in this OS -- CHANNEL-0 replaces
// them -- so the "mixed" set is file + socket rather than pipe + socket.)

typedef unsigned long size_t;

#define SYS_EXIT          0
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_SOCKET        51
#define SYS_EPOLL_CREATE  73
#define SYS_EPOLL_CTL     74
#define SYS_EPOLL_WAIT    75
#define SYS_POLL          111
#define SYS_SELECT        112

#define O_RDONLY     0
#define SOCK_STREAM  1

#define POLLIN   0x001
#define POLLOUT  0x004

#define EPOLLIN   0x001
#define EPOLLET   0x80000000u
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_MOD 2

static inline long sc6(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}
#define sc5(n,a,b,c,d,e) sc6((n),(a),(b),(c),(d),(e))
#define sc4(n,a,b,c,d)   sc6((n),(a),(b),(c),(d),0)
#define sc3(n,a,b,c)     sc6((n),(a),(b),(c),0,0)
#define sc1(n,a)         sc6((n),(a),0,0,0,0)

static size_t slen(const char* s){ size_t n=0; while(s[n]) n++; return n; }
static void out(const char* s){ sc3(SYS_WRITE, 1, (long)s, (long)slen(s)); }

struct pollfd { int fd; short events; short revents; };
struct epoll_event { unsigned int events; unsigned long data; };   /* 16 bytes: data@8 */
struct timeval { long tv_sec; long tv_usec; };
typedef struct { unsigned long w[4]; } fdset_t;                     /* 256 fds */

static void FD_ZERO(fdset_t* s){ s->w[0]=s->w[1]=s->w[2]=s->w[3]=0; }
static void FD_SET(int fd, fdset_t* s){ s->w[fd>>6] |= (1UL<<(fd&63)); }
static int  FD_ISSET(int fd, fdset_t* s){ return (s->w[fd>>6]>>(fd&63))&1; }

void _start(void) {
    out("POLLSELFTEST: start\n");
    int ok = 1;

    // A regular file is always poll-readable. /sbin/init is present in the initrd.
    long fd = sc3(SYS_OPEN, (long)"/sbin/init", O_RDONLY, 0);
    if (fd < 0) { out("POLLSELFTEST: open /sbin/init FAILED\n");
                  out("POLLSELFTEST RESULT: FAIL\n"); sc1(SYS_EXIT, 1); for(;;){} }

    // --- [1] poll() a ready (readable) fd: returns 1, POLLIN set, immediately ---
    {
        struct pollfd p = { (int)fd, POLLIN, 0 };
        long r = sc3(SYS_POLL, (long)&p, 1, 0 /*timeout=0 => no block*/);
        if (r == 1 && (p.revents & POLLIN)) out("POLLSELFTEST: [1] PASS poll_ready_file=1\n");
        else { out("POLLSELFTEST: [1] FAIL\n"); ok = 0; }
    }

    // --- [2] select() the same fd in readfds: returns >=1, fd still set ---
    {
        fdset_t rs; FD_ZERO(&rs); FD_SET((int)fd, &rs);
        struct timeval tv = { 0, 0 };
        long r = sc5(SYS_SELECT, (int)fd + 1, (long)&rs, 0, 0, (long)&tv);
        if (r >= 1 && FD_ISSET((int)fd, &rs)) out("POLLSELFTEST: [2] PASS select_ready_file=1\n");
        else { out("POLLSELFTEST: [2] FAIL\n"); ok = 0; }
    }

    // An unconnected TCP socket is NOT readable.
    long sfd = sc1(SYS_SOCKET, SOCK_STREAM);
    if (sfd < 0) { out("POLLSELFTEST: socket() FAILED\n");
                   out("POLLSELFTEST RESULT: FAIL\n"); sc1(SYS_EXIT, 1); for(;;){} }

    // --- [3] poll() honors a timeout on a not-ready fd: returns 0 after ~50ms ---
    {
        struct pollfd p = { (int)sfd, POLLIN, 0 };
        long r = sc3(SYS_POLL, (long)&p, 1, 50 /*ms*/);
        if (r == 0 && p.revents == 0) out("POLLSELFTEST: [3] PASS poll_timeout_idle_socket=1\n");
        else { out("POLLSELFTEST: [3] FAIL\n"); ok = 0; }
    }

    // --- [4] mixed set: the file is ready, the socket is not ---
    {
        struct pollfd pm[2] = { { (int)fd, POLLIN, 0 }, { (int)sfd, POLLIN, 0 } };
        long r = sc3(SYS_POLL, (long)pm, 2, 0);
        if (r == 1 && (pm[0].revents & POLLIN) && pm[1].revents == 0)
            out("POLLSELFTEST: [4] PASS mixed_file_ready_socket_not=1\n");
        else { out("POLLSELFTEST: [4] FAIL\n"); ok = 0; }
    }

    // --- [5] epoll level-trigger re-reports; edge-trigger reports once ---
    {
        long epfd = sc1(SYS_EPOLL_CREATE, 1);
        struct epoll_event ev; ev.events = EPOLLIN; ev.data = fd;
        sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_ADD, fd, (long)&ev);

        struct epoll_event got[4];
        long n1 = sc4(SYS_EPOLL_WAIT, epfd, (long)got, 4, 0);   // level: ready
        long n2 = sc4(SYS_EPOLL_WAIT, epfd, (long)got, 4, 0);   // level: STILL ready
        int level_ok = (n1 >= 1 && n2 >= 1);

        struct epoll_event ev2; ev2.events = EPOLLIN | EPOLLET; ev2.data = fd;
        sc4(SYS_EPOLL_CTL, epfd, EPOLL_CTL_MOD, fd, (long)&ev2);
        long e1 = sc4(SYS_EPOLL_WAIT, epfd, (long)got, 4, 0);   // edge: new -> reports
        long e2 = sc4(SYS_EPOLL_WAIT, epfd, (long)got, 4, 0);   // edge: no change -> 0
        int edge_ok = (e1 >= 1 && e2 == 0);

        if (level_ok && edge_ok)
            out("POLLSELFTEST: [5] PASS epoll_level_rereports=1 epoll_edge_once=1\n");
        else { out("POLLSELFTEST: [5] FAIL\n"); ok = 0; }
    }

    sc1(SYS_CLOSE, fd);
    out(ok ? "POLLSELFTEST RESULT: PASS\n" : "POLLSELFTEST RESULT: FAIL\n");
    sc1(SYS_EXIT, ok ? 0 : 1);
    for (;;) {}
}

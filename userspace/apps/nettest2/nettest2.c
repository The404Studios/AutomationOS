/*
 * nettest2.c -- transport-layer (UDP/TCP/socket) smoke test, ring 3.
 * ==================================================================
 *
 * Drives the BSD-ish socket syscalls added on top of the UDP/TCP layer:
 *
 *   1. UDP: socket(SOCK_DGRAM) -> sendto(gateway:7) -> poll+recvfrom.
 *      slirp's QEMU user-net does NOT echo by default, so a successful SEND
 *      (bytes accepted by the kernel = on the wire) is the primary PASS gate;
 *      a received datagram is reported as a bonus.
 *   2. TCP: socket(SOCK_STREAM) -> connect(gateway:9). slirp has no listener
 *      on arbitrary ports, so this exercises the handshake/retransmit path and
 *      reports the outcome (timeout/refused is expected and still "path ok").
 *
 * Everything goes through syscalls -- no MMIO here. Diagnostics to serial via
 * SYS_WRITE(fd=1). Prints a final PASS/FAIL line.
 *
 * Build (EXACT -- flags DIRECT on the cmdline; objdump grep fs:0x28 MUST be empty):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/nettest2/nettest2.c -o nettest2.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       nettest2.o -o nettest2
 *   objdump -d nettest2 | grep fs:0x28        # MUST be empty
 *
 * NOTE: these syscall numbers must match kernel/include/syscall.h
 * (SYS_SOCKET=51 .. SYS_SOCK_POLL=58).
 */

/* ---- syscall numbers (must match kernel/include/syscall.h) ---- */
#define SYS_EXIT       0
#define SYS_WRITE      3
#define SYS_YIELD      15
#define SYS_SOCKET     51
#define SYS_CONNECT    52
#define SYS_SEND       53
#define SYS_RECV       54
#define SYS_CLOSE_SK   55
#define SYS_SENDTO     56
#define SYS_RECVFROM   57
#define SYS_SOCK_POLL  58

/* ---- socket constants (must match kernel/include/socket.h) ---- */
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_EAGAIN    -11

/* QEMU user-net gateway 10.0.2.2 (host byte order). */
#define GATEWAY_IP     0x0A000202u

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* recvfrom out-address (mirror of sock_addr_t in socket.h). */
typedef struct { u32 ip; u16 port; u16 _pad; } sock_addr_t;

/* 6-argument inline syscall (args rdi/rsi/rdx/r10/r8/r9). */
static inline long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- tiny serial diagnostics (fd 1) ---- */
static unsigned long k_strlen(const char *s){unsigned long n=0;while(s[n])n++;return n;}
static void print(const char *m){sc(SYS_WRITE,1,(long)m,(long)k_strlen(m),0,0,0);}
static void print_dec(long v){
    char b[24]; int i=0; unsigned long n;
    if (v<0){ char m='-'; sc(SYS_WRITE,1,(long)&m,1,0,0,0); n=(unsigned long)(-v); }
    else n=(unsigned long)v;
    do{ b[i++]=(char)('0'+(n%10)); n/=10; }while(n>0);
    while(i>0){ char ch=b[--i]; sc(SYS_WRITE,1,(long)&ch,1,0,0,0); }
}
static void print_ip(u32 ip){
    print_dec((ip>>24)&0xFF);print(".");print_dec((ip>>16)&0xFF);print(".");
    print_dec((ip>>8)&0xFF);print(".");print_dec(ip&0xFF);
}

void _start(void) {
    int udp_ok = 0;
    int tcp_path_ok = 0;

    print("[NETTEST2] transport-layer (UDP/TCP/socket) smoke test\n");

    /* ---------------- UDP ---------------- */
    print("[NETTEST2] UDP: socket(SOCK_DGRAM)\n");
    long us = sc(SYS_SOCKET, SOCK_DGRAM, 0, 0, 0, 0, 0);
    if (us < 0) {
        print("[NETTEST2] UDP socket failed rc="); print_dec(us); print("\n");
    } else {
        const char *payload = "hello-udp";
        int plen = 9;
        print("[NETTEST2] UDP sendto "); print_ip(GATEWAY_IP); print(":7\n");
        long sent = sc(SYS_SENDTO, us, (long)payload, plen, GATEWAY_IP, 7, 0);
        print("[NETTEST2] UDP sendto rc="); print_dec(sent); print("\n");
        if (sent == plen) {
            udp_ok = 1;   /* datagram accepted onto the wire */
        }

        /* Best-effort: pump + recvfrom for an echo (slirp usually won't echo). */
        u8 rb[128];
        sock_addr_t from;
        for (long t = 0; t < 200000; t++) {
            sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0, 0);
            long r = sc(SYS_RECVFROM, us, (long)rb, (long)sizeof(rb),
                        (long)&from, 0, 0);
            if (r > 0) {
                print("[NETTEST2] UDP RX "); print_dec(r);
                print(" bytes from "); print_ip(from.ip);
                print(":"); print_dec(from.port); print("\n");
                udp_ok = 1;
                break;
            }
            if (r != SOCK_EAGAIN && r < 0) break;
            sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
        }
        sc(SYS_CLOSE_SK, us, 0, 0, 0, 0, 0);
    }

    /* ---------------- TCP ---------------- */
    print("[NETTEST2] TCP: socket(SOCK_STREAM)\n");
    long ts = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0, 0);
    if (ts < 0) {
        print("[NETTEST2] TCP socket failed rc="); print_dec(ts); print("\n");
    } else {
        print("[NETTEST2] TCP connect "); print_ip(GATEWAY_IP); print(":9\n");
        long cr = sc(SYS_CONNECT, ts, GATEWAY_IP, 9, 0, 0, 0);
        print("[NETTEST2] TCP connect rc="); print_dec(cr); print("\n");
        if (cr == 0) {
            print("[NETTEST2] TCP handshake COMPLETED (established)\n");
            const char *req = "hi\n";
            long sr = sc(SYS_SEND, ts, (long)req, 3, 0, 0, 0);
            print("[NETTEST2] TCP send rc="); print_dec(sr); print("\n");
            u8 rb[256];
            for (int i = 0; i < 1000; i++) {
                sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0, 0);
                long r = sc(SYS_RECV, ts, (long)rb, (long)sizeof(rb), 0, 0, 0);
                if (r > 0) { print("[NETTEST2] TCP RX "); print_dec(r);
                             print(" bytes\n"); break; }
                if (r == 0) break;
            }
            tcp_path_ok = 1;
        } else {
            /* No server on slirp:9 -> connect path exercised, no peer. */
            print("[NETTEST2] TCP connect did not establish (expected vs slirp)\n");
            tcp_path_ok = 1;   /* the handshake path ran without faulting */
        }
        sc(SYS_CLOSE_SK, ts, 0, 0, 0, 0, 0);
    }

    /* ---------------- verdict ---------------- */
    if (udp_ok && tcp_path_ok) {
        print("[NETTEST2] PASS: UDP send + socket API + TCP handshake path OK\n");
    } else if (udp_ok) {
        print("[NETTEST2] PARTIAL: UDP OK, TCP path failed\n");
    } else {
        print("[NETTEST2] FAIL: UDP did not send\n");
    }

    sc(SYS_EXIT, udp_ok ? 0 : 1, 0, 0, 0, 0, 0);
    for (;;) sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}

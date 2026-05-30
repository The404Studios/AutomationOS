/*
 * libtest.c -- boot-time self-test harness for misc userspace libraries.
 * ======================================================================
 * Runs deterministic, network-free self-tests of the JSON parser and the DHCP
 * client's packet build/parse logic. Prints one summary line the smoke test
 * gates on. Freestanding, own _start, no libc.
 */
#include "../../lib/json/json.h"        /* json_selftest()     */
#include "../../lib/net/dhcp.h"         /* dhcp_selftest()     */
#include "../../lib/imgcodec/imgcodec.h" /* imgcodec_selftest() */

#define SYS_EXIT  0
#define SYS_WRITE 3

static long sc(long n, long a1, long a2, long a3) {
    long r;
    asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}
static unsigned long slen(const char* s){unsigned long n=0;while(s[n])n++;return n;}
static void print(const char* m){ sc(SYS_WRITE, 1, (long)m, (long)slen(m)); }

void _start(void) {
    int json = json_selftest();   /* 0 = pass */
    int dhcp = dhcp_selftest();
    int img  = imgcodec_selftest();

    print(json == 0 ? "[LIBTEST] json: PASS\n"     : "[LIBTEST] json: FAIL\n");
    print(dhcp == 0 ? "[LIBTEST] dhcp: PASS\n"     : "[LIBTEST] dhcp: FAIL\n");
    print(img  == 0 ? "[LIBTEST] imgcodec: PASS\n" : "[LIBTEST] imgcodec: FAIL\n");

    if (json == 0 && dhcp == 0 && img == 0)
        print("LIBTEST: PASS (json+dhcp+imgcodec KATs)\n");
    else
        print("LIBTEST: FAIL\n");

    sc(SYS_EXIT, 0, 0, 0);
    for (;;) {}
}

/*
 * nicup.c -- E1000-PCH-0B: trigger the DEFERRED PCH NIC bring-up (ring 3).
 * ========================================================================
 *
 * The T410's 82577LM defers its risky ME-shared-MDIO initialization out of
 * boot (E1000-PCH-0A): the kernel probes it safely and waits. Running this
 * tool from the desktop invokes the deferred bring-up via SYS_NET_CONFIG's
 * NIC_BRINGUP flag. If the ME holds the MDIO semaphore the kernel ABORTS
 * cleanly and this tool can simply be re-run; a stall now costs a retry on
 * a live machine with serial, never the boot.
 *
 * On QEMU (classic NIC, already up at boot) or a machine with nothing
 * deferred this is a clean diagnostic no-op.
 *
 * After a successful bring-up: run `dhcpc` to get a real lease, then ping.
 *
 * NO libc, NO stdio. Inline syscalls + fixed buffers only (house pattern).
 */

#include "../../../kernel/include/uapi/net.h"

#define SYS_EXIT       0
#define SYS_WRITE      3
#define SYS_NET_CONFIG 89

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

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    uapi_net_config_t req;
    char *p = (char *)&req;
    for (unsigned i = 0; i < sizeof(req); i++) p[i] = 0;
    req.ifname[0] = 'e'; req.ifname[1] = 't';
    req.ifname[2] = 'h'; req.ifname[3] = '0';
    req.flags = NET_CONFIG_FLAG_NIC_BRINGUP | NET_CONFIG_FLAG_UP;

    print("nicup: triggering deferred NIC bring-up...\n");
    long r = sc(SYS_NET_CONFIG, (long)&req, 0, 0, 0, 0, 0);
    if (r == 0) {
        print("nicup: OK -- NIC attached (watch serial for the E1000PCH ladder).\n");
        print("nicup: next: run dhcpc for a lease, then ping the gateway.\n");
        return 0;
    }
    if (r == 95 || r == -95) {   /* ENOTSUP: nothing deferred */
        print("nicup: nothing to do (no deferred NIC -- already up, or not a PCH part).\n");
        return 0;
    }
    print("nicup: bring-up ABORTED (ME busy or reset failed) -- safe to re-run.\n");
    return 1;
}

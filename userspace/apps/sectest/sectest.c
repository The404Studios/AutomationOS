/*
 * sectest.c - Seccomp enforcement end-to-end test (ring 3)
 * ========================================================
 *
 * Proves that the kernel's seccomp syscall filter ACTUALLY ENFORCES:
 *
 *   1. Calls SYS_GETPID once before any filter   -> expected to succeed.
 *   2. Installs a seccomp BPF filter via SYS_SECCOMP whose policy is an
 *      ALLOW-LIST of { SYS_WRITE, SYS_EXIT }; every other syscall returns
 *      errno EPERM (SECCOMP_RET_ERRNO|EPERM).
 *   3. Calls SYS_GETPID again -> the kernel must DENY it and return -EPERM,
 *      because GETPID is not on the allow-list.
 *   4. Prints "[SECTEST] correctly DENIED" on success, or
 *      "[SECTEST] FAIL: allowed" if the syscall slipped through.
 *
 * SYS_WRITE and SYS_EXIT remain on the allow-list so the test can still print
 * its verdict and terminate cleanly after the filter is active.
 *
 * Freestanding: no libc, no headers. Built with:
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 -c sectest.c -o sectest.o
 *   ld -T userspace/userspace.ld -e _start sectest.o -o sectest
 */

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

/* ---- Syscall numbers (must match kernel/include/syscall.h) ---- */
#define SYS_EXIT     0
#define SYS_WRITE    3
#define SYS_GETPID   8
#define SYS_SECCOMP  41   /* NEW: proposed SYS_SECCOMP slot */

/* ---- Generic syscall stub (per task brief) ---- */
static inline long sc(long n, long a1, long a2, long a3) {
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

/* ---- Minimal output helpers ---- */
static unsigned slen(const char* s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}
static void put(const char* s) {
    sc(SYS_WRITE, 1, (long)s, (long)slen(s));
}

/* ===========================================================
 * Seccomp ABI mirror (must match kernel/include/seccomp.h)
 * =========================================================== */

struct bpf_insn {
    uint16_t code;
    uint8_t  jt;
    uint8_t  jf;
    uint32_t k;
};

/* sock_fprog: 2-byte len + 6 bytes pad, then 8-byte pointer (16 bytes). */
struct sock_fprog {
    uint16_t len;
    uint16_t _pad[3];
    const struct bpf_insn* filter;
};

/* seccomp operations */
#define SECCOMP_SET_MODE_STRICT 0
#define SECCOMP_SET_MODE_FILTER 1

/* BPF opcode bits */
#define BPF_LD   0x00
#define BPF_W    0x00
#define BPF_ABS  0x20
#define BPF_JMP  0x05
#define BPF_JEQ  0x10
#define BPF_K    0x00
#define BPF_RET  0x06

/* seccomp_data field offsets */
#define OFF_NR   0

/* SECCOMP_RET_* actions */
#define SECCOMP_RET_ALLOW  0x7fff0000u
#define SECCOMP_RET_ERRNO  0x00050000u
#define EPERM_VAL          1u   /* returned to userspace as -1 */

#define BPF_STMT(_c, _k)            { (uint16_t)(_c), 0, 0, (uint32_t)(_k) }
#define BPF_JUMP(_c, _k, _jt, _jf)  { (uint16_t)(_c), (uint8_t)(_jt), (uint8_t)(_jf), (uint32_t)(_k) }

/*
 * Allow-list filter:
 *   load nr
 *   if nr == SYS_WRITE -> ALLOW
 *   if nr == SYS_EXIT  -> ALLOW
 *   else               -> ERRNO(EPERM)   (denies SYS_GETPID, etc.)
 */
static const struct bpf_insn allowlist[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),                 /* [0] A = nr        */
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_WRITE, 0, 1),       /* [1] ==WRITE?      */
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),              /* [2] allow         */
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_EXIT, 0, 1),       /* [3] ==EXIT?       */
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),              /* [4] allow         */
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM_VAL),  /* [5] deny -> EPERM */
};

void _start(void) {
    put("[SECTEST] seccomp end-to-end test starting\n");

    /* Step 1: GETPID works before any filter is installed. */
    long pid_before = sc(SYS_GETPID, 0, 0, 0);
    if (pid_before < 0) {
        put("[SECTEST] FAIL: GETPID failed before filter install\n");
        sc(SYS_EXIT, 1, 0, 0);
    }
    put("[SECTEST] GETPID allowed before filter (ok)\n");

    /* Step 2: install the allow-list filter on ourselves. */
    struct sock_fprog fprog;
    fprog.len = (uint16_t)(sizeof(allowlist) / sizeof(allowlist[0]));
    fprog._pad[0] = fprog._pad[1] = fprog._pad[2] = 0;
    fprog.filter = allowlist;

    long rc = sc(SYS_SECCOMP, SECCOMP_SET_MODE_FILTER, 0, (long)&fprog);
    if (rc != 0) {
        put("[SECTEST] FAIL: SYS_SECCOMP install returned error\n");
        sc(SYS_EXIT, 2, 0, 0);
    }
    put("[SECTEST] filter installed (WRITE+EXIT allowed, others EPERM)\n");

    /* Step 3: GETPID must now be DENIED by the kernel. */
    long pid_after = sc(SYS_GETPID, 0, 0, 0);

    /* Step 4: verdict. A denied syscall returns -EPERM (-1). A real pid is
     * >= 0, so a non-negative result means enforcement FAILED. */
    if (pid_after < 0) {
        put("[SECTEST] correctly DENIED\n");
        sc(SYS_EXIT, 0, 0, 0);
    } else {
        put("[SECTEST] FAIL: allowed\n");
        sc(SYS_EXIT, 3, 0, 0);
    }

    /* unreachable */
    for (;;) { }
}

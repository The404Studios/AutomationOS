/*
 * batchtest.c -- Ring-3 test for SYS_BATCH_SUBMIT (82)
 *
 * ABI (from kernel/core/syscall/batch.c):
 *
 *   int64_t sys_batch_submit(ring_ptr, count, 0, 0, 0, 0)
 *
 *   ring_ptr  : pointer to a batch_ring_t in userspace
 *   count     : number of entries to execute (must be <= sq_size AND cq_size)
 *   Returns   : number of syscalls executed on success, negative errno on error
 *
 *   batch_ring_t layout:
 *     syscall_request_t* sq;   // submission queue (userspace writes)
 *     int64_t*           cq;   // completion queue (kernel writes results)
 *     uint32_t           sq_size;
 *     uint32_t           cq_size;
 *
 *   syscall_request_t layout:
 *     int32_t  syscall_num;    // SYS_* number
 *     uint32_t reserved;       // padding (set to 0)
 *     uint64_t args[6];        // syscall arguments
 *
 * Test strategy:
 *   Build a batch of 3 SYS_WRITE calls to stdout (fd 1), each printing one
 *   short label.  Submit via SYS_BATCH_SUBMIT.  Verify the return value
 *   equals 3 (all three entries executed).  Check each CQ slot >= 0
 *   (write succeeded).  Print BATCHTEST: PASS or FAIL.
 *
 * Build:
 *   x86_64-elf-gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *     -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign \
 *     -O2 -o batchtest batchtest.c
 */

/* ── fixed-width types (no libc) ─────────────────────────────────────── */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned long      size_t;

/* ── syscall numbers ─────────────────────────────────────────────────── */
#define SYS_EXIT         0
#define SYS_WRITE        3
#define SYS_BATCH_SUBMIT 82

/* ── batch ABI structs (must exactly match kernel/core/syscall/batch.c) ─ */
typedef struct {
    int32_t  syscall_num;   /* SYS_* number */
    uint32_t reserved;      /* padding, set 0 */
    uint64_t args[6];       /* syscall arguments */
} syscall_request_t;

typedef struct {
    syscall_request_t *sq;   /* submission queue */
    int64_t           *cq;   /* completion queue  */
    uint32_t           sq_size;
    uint32_t           cq_size;
} batch_ring_t;

/* ── 6-argument inline syscall wrapper ───────────────────────────────── */
static inline int64_t sc(uint64_t nr,
                          uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6)
{
    int64_t ret;
    register uint64_t r10 asm("r10") = a4;
    register uint64_t r8  asm("r8")  = a5;
    register uint64_t r9  asm("r9")  = a6;
    asm volatile (
        "syscall"
        : "=a"(ret)
        : "0"(nr), "D"(a1), "S"(a2), "d"(a3),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* ── tiny helpers ────────────────────────────────────────────────────── */
static void write_str(const char *s)
{
    size_t len = 0;
    while (s[len]) len++;
    sc(SYS_WRITE, 1, (uint64_t)s, len, 0, 0, 0);
}

static char *i64toa(int64_t v, char *buf, int bufsz)
{
    char tmp[24];
    int neg = 0, i = 0, j;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { tmp[i++] = '0'; }
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) tmp[i++] = '-';
    j = 0;
    if (i >= bufsz) i = bufsz - 1;
    while (i-- > 0) buf[j++] = tmp[i];
    buf[j] = '\0';
    return buf;
}

/* ── batch payload strings ───────────────────────────────────────────── */
static const char msg0[] = "[batch-entry-0]\n";
static const char msg1[] = "[batch-entry-1]\n";
static const char msg2[] = "[batch-entry-2]\n";
#define MSG0_LEN 16
#define MSG1_LEN 16
#define MSG2_LEN 16

#define BATCH_COUNT 3

/* ── test ────────────────────────────────────────────────────────────── */
void _start(void)
{
    char numbuf[24];
    int64_t ret;

    /* Static allocation -- no heap needed */
    static syscall_request_t sq[BATCH_COUNT];
    static int64_t           cq[BATCH_COUNT];

    /* Entry 0: write msg0 to stdout */
    sq[0].syscall_num = SYS_WRITE;
    sq[0].reserved    = 0;
    sq[0].args[0]     = 1;                   /* fd = stdout */
    sq[0].args[1]     = (uint64_t)msg0;
    sq[0].args[2]     = MSG0_LEN;
    sq[0].args[3]     = 0;
    sq[0].args[4]     = 0;
    sq[0].args[5]     = 0;

    /* Entry 1: write msg1 to stdout */
    sq[1].syscall_num = SYS_WRITE;
    sq[1].reserved    = 0;
    sq[1].args[0]     = 1;
    sq[1].args[1]     = (uint64_t)msg1;
    sq[1].args[2]     = MSG1_LEN;
    sq[1].args[3]     = 0;
    sq[1].args[4]     = 0;
    sq[1].args[5]     = 0;

    /* Entry 2: write msg2 to stdout */
    sq[2].syscall_num = SYS_WRITE;
    sq[2].reserved    = 0;
    sq[2].args[0]     = 1;
    sq[2].args[1]     = (uint64_t)msg2;
    sq[2].args[2]     = MSG2_LEN;
    sq[2].args[3]     = 0;
    sq[2].args[4]     = 0;
    sq[2].args[5]     = 0;

    /* Initialise CQ to a sentinel so we can detect un-written slots */
    cq[0] = cq[1] = cq[2] = -999;

    /* Build ring descriptor */
    static batch_ring_t ring;
    ring.sq      = sq;
    ring.cq      = cq;
    ring.sq_size = BATCH_COUNT;
    ring.cq_size = BATCH_COUNT;

    write_str("BATCHTEST: submitting batch of 3 SYS_WRITE entries\n");

    ret = sc(SYS_BATCH_SUBMIT,
             (uint64_t)&ring,   /* ring_ptr */
             BATCH_COUNT,       /* count    */
             0, 0, 0, 0);

    write_str("BATCHTEST: SYS_BATCH_SUBMIT returned ");
    write_str(i64toa(ret, numbuf, sizeof numbuf));
    write_str("\n");

    if (ret < 0) {
        write_str("BATCHTEST: FAIL (batch_submit error=");
        write_str(i64toa(ret, numbuf, sizeof numbuf));
        write_str(")\n");
        sc(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    }

    if (ret != BATCH_COUNT) {
        write_str("BATCHTEST: FAIL (expected executed=3, got ");
        write_str(i64toa(ret, numbuf, sizeof numbuf));
        write_str(")\n");
        sc(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    }

    /* Verify each completion slot shows a non-negative result */
    int all_ok = 1;
    for (int i = 0; i < BATCH_COUNT; i++) {
        if (cq[i] < 0) {
            write_str("BATCHTEST: FAIL (cq[");
            char ibuf[4]; ibuf[0] = (char)('0' + i); ibuf[1] = '\0';
            write_str(ibuf);
            write_str("]= ");
            write_str(i64toa(cq[i], numbuf, sizeof numbuf));
            write_str(")\n");
            all_ok = 0;
        }
    }

    if (all_ok) {
        write_str("BATCHTEST: PASS\n");
        sc(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    } else {
        sc(SYS_EXIT, 1, 0, 0, 0, 0, 0);
    }

    /* unreachable */
    while (1) {}
}

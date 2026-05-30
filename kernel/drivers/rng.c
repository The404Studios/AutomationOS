/*
 * rng.c -- Kernel Random Number Generator
 * =========================================
 *
 * Strategy
 * --------
 *  1. CPUID leaf 1 ECX[30]: if set, use RDRAND via inline asm carry-flag
 *     retry loop (up to RNG_RDRAND_RETRIES per word).
 *  2. Fallback: xorshift128+ seeded from:
 *       - two rdtsc samples (before and after a short spin)
 *       - RTC seconds / minutes registers (port 0x70/0x71)
 *       - a compile-time constant mixed in so two boots on the same tick
 *         are still distinguishable at early init.
 *
 * If RDRAND fails all retries for a single word, we mix the PRNG result
 * into the RDRAND value to avoid returning 0.
 *
 * Build check (from project root, inside WSL Arch/Ubuntu):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -nostdinc -fno-pic -fno-pie \
 *       -fno-stack-protector -mno-red-zone -mcmodel=kernel \
 *       -DSYSCALL_QUIET -DSCHEDULER_QUIET -DCONTEXT_SWITCH_QUIET \
 *       -Wno-unused-variable -Wno-unused-function \
 *       -Wno-builtin-declaration-mismatch \
 *       -Wno-implicit-function-declaration \
 *       -Wno-int-conversion -Wno-incompatible-pointer-types \
 *       -Ikernel/include -Ikernel/include/compat \
 *       -c kernel/drivers/rng.c -o /tmp/rng.o
 */

#include "../include/rng.h"
#include "../include/kernel.h"
#include "../include/x86_64.h"
#include "../include/vfs.h"
#include "../include/mem.h"

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static int  g_has_rdrand = 0;   /* 1 = RDRAND available */
static int  g_rng_ready  = 0;   /* 1 = rng_init() completed */

/* xorshift128+ state (two 64-bit words) */
static uint64_t g_xs_s0 = 0x123456789ABCDEF0ULL;
static uint64_t g_xs_s1 = 0xFEDCBA9876543210ULL;

/* -------------------------------------------------------------------------
 * Low-level helpers (no libc, freestanding)
 * ---------------------------------------------------------------------- */

/* rdtsc: returns 64-bit time-stamp counter */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * CPUID wrapper.
 * leaf -> EAX; returns ECX in *ecx_out, EDX in *edx_out.
 */
static inline void cpuid(uint32_t leaf,
                         uint32_t *eax_out, uint32_t *ecx_out,
                         uint32_t *edx_out)
{
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(leaf), "c"(0));
    if (eax_out) *eax_out = eax;
    if (ecx_out) *ecx_out = ecx;
    if (edx_out) *edx_out = edx;
}

/* Read RTC register <reg> via CMOS ports 0x70/0x71. */
static inline uint8_t rtc_read(uint8_t reg)
{
    outb(0x70, reg & 0x7F); /* bit 7 = NMI disable, leave it clear */
    return inb(0x71);
}

/* -------------------------------------------------------------------------
 * xorshift128+ PRNG
 * ---------------------------------------------------------------------- */

/*
 * xorshift128+ step.
 * Very fast, passes BigCrush.  Not cryptographically secure.
 */
static uint64_t xs128plus(void)
{
    uint64_t x = g_xs_s0;
    uint64_t y = g_xs_s1;
    g_xs_s0 = y;
    x ^= x << 23;
    x ^= x >> 17;
    x ^= y;
    x ^= y >> 26;
    g_xs_s1 = x;
    return g_xs_s0 + g_xs_s1;
}

/* -------------------------------------------------------------------------
 * RDRAND wrapper (carry-flag retry loop)
 * ---------------------------------------------------------------------- */

/*
 * Try to read a 64-bit value from RDRAND.
 * Returns 1 on success (value placed in *out), 0 if all retries exhausted.
 *
 * The RDRAND instruction sets CF=1 on success, CF=0 if the hardware
 * entropy pool is temporarily empty.  Intel recommends up to 10 retries.
 */
static int rdrand64(uint64_t *out)
{
    for (int i = 0; i < RNG_RDRAND_RETRIES; i++) {
        uint64_t val;
        uint8_t  ok;
        asm volatile(
            "rdrand %0\n\t"
            "setc   %1\n\t"
            : "=r"(val), "=qm"(ok)
            :
            : "cc"
        );
        if (ok) {
            *out = val;
            return 1;
        }
        /* Short pause hint to let the entropy conditioner refill */
        asm volatile("pause");
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * rng_init() -- call once at kernel startup (after serial is up so we can
 *               print diagnostics, before any rng_u64 caller).
 */
void rng_init(void)
{
    /* --- Probe RDRAND via CPUID leaf 1, ECX bit 30 --- */
    uint32_t ecx = 0;
    cpuid(1, (void*)0, &ecx, (void*)0);
    g_has_rdrand = (ecx >> 30) & 1;

    /* --- Seed xorshift128+ regardless (RDRAND can fail transiently) --- */

    /* Sample TSC twice with a short spin between them */
    uint64_t t0 = rdtsc();
    /* A tiny spin so the two samples are different */
    for (volatile int i = 0; i < 1024; i++)
        asm volatile("pause");
    uint64_t t1 = rdtsc();

    /* Read RTC seconds and minutes (BCD or binary; we just need entropy) */
    uint8_t rtc_sec = rtc_read(0x00); /* seconds */
    uint8_t rtc_min = rtc_read(0x02); /* minutes */
    uint8_t rtc_hr  = rtc_read(0x04); /* hours   */

    /* Mix everything into the two state words using a simple hash step */
    uint64_t seed0 = t0 ^ (t1 << 17) ^ (t1 >> 47);
    uint64_t seed1 = ((uint64_t)rtc_sec        ) |
                     ((uint64_t)rtc_min << 8   ) |
                     ((uint64_t)rtc_hr  << 16  ) |
                     0xDEADBEEF00000000ULL;

    /* Splitmix64 finaliser to spread bits */
    seed0 += 0x9E3779B97F4A7C15ULL;
    seed0  = (seed0 ^ (seed0 >> 30)) * 0xBF58476D1CE4E5B9ULL;
    seed0  = (seed0 ^ (seed0 >> 27)) * 0x94D049BB133111EBULL;
    seed0 ^= seed0 >> 31;

    seed1 += 0x6C62272E07BB0142ULL;
    seed1  = (seed1 ^ (seed1 >> 30)) * 0xBF58476D1CE4E5B9ULL;
    seed1  = (seed1 ^ (seed1 >> 27)) * 0x94D049BB133111EBULL;
    seed1 ^= seed1 >> 31;

    g_xs_s0 = seed0 ? seed0 : 0xDEADC0DEDEADC0DEULL; /* must not be 0 */
    g_xs_s1 = seed1 ? seed1 : 0xCAFEBABECAFEBABEULL;

    /* Warm up: discard the first few outputs */
    for (int i = 0; i < 16; i++)
        (void)xs128plus();

    g_rng_ready = 1;

    kprintf("[RNG] Initialized: hardware RDRAND %s\n",
            g_has_rdrand ? "YES" : "NO (xorshift128+ fallback)");
}

/*
 * rng_u64() -- return 64 random bits.
 */
uint64_t rng_u64(void)
{
    if (!g_rng_ready) {
        /* Emergency: called before rng_init().  Best effort: TSC. */
        return rdtsc() ^ 0x5555555555555555ULL;
    }

    if (g_has_rdrand) {
        uint64_t val;
        if (rdrand64(&val)) {
            return val;
        }
        /* RDRAND exhausted all retries -- mix PRNG to avoid returning 0 */
        return xs128plus() ^ 0xA5A5A5A5A5A5A5A5ULL;
    }

    return xs128plus();
}

/*
 * rng_bytes() -- fill buf with n random bytes.
 */
void rng_bytes(void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t   remaining = n;

    while (remaining >= 8) {
        uint64_t v = rng_u64();
        p[0] = (uint8_t)(v       );
        p[1] = (uint8_t)(v >>  8 );
        p[2] = (uint8_t)(v >> 16 );
        p[3] = (uint8_t)(v >> 24 );
        p[4] = (uint8_t)(v >> 32 );
        p[5] = (uint8_t)(v >> 40 );
        p[6] = (uint8_t)(v >> 48 );
        p[7] = (uint8_t)(v >> 56 );
        p += 8;
        remaining -= 8;
    }

    if (remaining > 0) {
        uint64_t v = rng_u64();
        for (size_t i = 0; i < remaining; i++) {
            p[i] = (uint8_t)(v & 0xFF);
            v >>= 8;
        }
    }
}

/*
 * rng_has_hardware() -- 1 if RDRAND is present, 0 for PRNG fallback.
 */
int rng_has_hardware(void)
{
    return g_has_rdrand;
}

/* -------------------------------------------------------------------------
 * /dev/random VFS device node
 *
 * NOTE: this function wires the RNG to the VFS as a character device.
 *       It must be called AFTER vfs_init() and after the /dev directory
 *       exists in the ramfs.  The caller (kernel/kernel.c or equivalent)
 *       is responsible for the call; we do NOT touch vfs.c.
 *
 * Wiring summary (report for the integration agent):
 *   - Add `#include "drivers/rng.h"` in kernel.c (or wherever rng_init is
 *     called).
 *   - Call rng_init() early in kernel_main, before the scheduler.
 *   - After vfs_mkdir("/dev", 0755) succeeds, call rng_register_devnode().
 *   - That creates /dev/random with rng_dev_read as the read fop.
 *   - SYS_RANDOM (41): add `#define SYS_RANDOM 41` in syscall.h,
 *     implement sys_random() in handlers.c (copies rng_bytes into user
 *     buffer after validating the pointer), register in syscall_table[41].
 * ---------------------------------------------------------------------- */

/* -----------------------------------------------------------------------
 * /dev/random file operations
 * -------------------------------------------------------------------- */

static ssize_t rng_dev_read(vfs_file_t *file, void *buf, size_t count)
{
    (void)file;
    if (!buf || count == 0)
        return -22; /* EINVAL */
    rng_bytes(buf, count);
    return (ssize_t)count;
}

/* Write to /dev/random: accepted (could be used to add entropy) */
static ssize_t rng_dev_write(vfs_file_t *file, const void *buf, size_t count)
{
    (void)file; (void)buf;
    return (ssize_t)count;
}

static int rng_dev_open(vfs_inode_t *inode, vfs_file_t *file)
{
    (void)inode; (void)file;
    return 0;
}

static int rng_dev_close(vfs_file_t *file)
{
    (void)file;
    return 0;
}

static vfs_file_ops_t rng_fops = {
    .read  = rng_dev_read,
    .write = rng_dev_write,
    .open  = rng_dev_open,
    .close = rng_dev_close,
    .lseek = NULL,
};

/*
 * rng_register_devnode() -- create /dev/random in the VFS.
 *
 * Uses the same dentry-array insertion pattern as dev_input_link_node().
 * Must be called after vfs_init() and after /dev exists as a ramfs dir.
 *
 * Integration agent wiring (do NOT edit vfs.c):
 *   In kernel.c / init sequence, after vfs_mkdir("/dev", 0755):
 *     rng_init();
 *     rng_register_devnode();
 *
 * Returns 0 on success, negative on failure.
 */
int rng_register_devnode(void)
{
    /* Resolve /dev -- it must already exist */
    vfs_inode_t *dev_dir = vfs_path_lookup("/dev");
    if (!dev_dir) {
        kprintf("[RNG] /dev not found -- call after vfs_mkdir(\"/dev\",...)\n");
        return -2; /* ENOENT */
    }
    if (!(dev_dir->type & VFS_TYPE_DIR)) {
        kprintf("[RNG] /dev is not a directory\n");
        return -20; /* ENOTDIR */
    }

    /* Allocate an inode for /dev/random.
     * vfs_inode_alloc() needs a superblock; pass NULL and initialise
     * manually -- the ramfs alloc_inode tolerates NULL sb in some builds,
     * or we fall back to a direct kmalloc matching the struct size. */
    vfs_inode_t *inode = (vfs_inode_t *)kmalloc(sizeof(vfs_inode_t));
    if (!inode) {
        kprintf("[RNG] kmalloc(vfs_inode_t) failed\n");
        return -12; /* ENOMEM */
    }
    /* Zero-initialise manually (no memset in scope at link time) */
    {
        uint8_t *p = (uint8_t *)inode;
        for (size_t i = 0; i < sizeof(vfs_inode_t); i++) p[i] = 0;
    }

    inode->type         = VFS_TYPE_DEVICE;
    inode->mode         = 0666;
    inode->ref_count    = 1;
    inode->flags        = 0;
    inode->ops          = NULL;
    inode->private_data = (void *)&rng_fops; /* char-dev fops pointer */

    /* Insert into the /dev dentry array (ramfs layout) */
    if (!dev_dir->private_data) {
        dev_dir->private_data = kmalloc(sizeof(vfs_dentry_t *) * 16);
        if (!dev_dir->private_data) {
            kfree(inode);
            return -12;
        }
        {
            uint8_t *p = (uint8_t *)dev_dir->private_data;
            for (size_t i = 0; i < sizeof(vfs_dentry_t *) * 16; i++) p[i] = 0;
        }
        dev_dir->data_capacity = 16;
    }

    vfs_dentry_t *dentry = vfs_dentry_alloc("random");
    if (!dentry) {
        kfree(inode);
        return -12;
    }
    dentry->inode = inode;
    vfs_inode_get(inode); /* dentry holds a reference */

    vfs_dentry_t **entries = (vfs_dentry_t **)dev_dir->private_data;
    uint64_t cap = dev_dir->data_capacity;
    for (uint64_t i = 0; i < cap; i++) {
        if (!entries[i]) {
            entries[i] = dentry;
            dev_dir->size++;
            kprintf("[RNG] /dev/random registered\n");
            return 0;
        }
    }

    /* Directory full */
    kprintf("[RNG] /dev directory full, cannot register /dev/random\n");
    dentry->inode = NULL;
    vfs_inode_put(inode);
    vfs_dentry_free(dentry);
    kfree(inode);
    return -28; /* ENOSPC */
}

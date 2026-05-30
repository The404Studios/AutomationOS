/*
 * blk.c -- userspace block-device tool for the from-scratch x86_64 OS.
 * ====================================================================
 *
 * FREESTANDING userspace ELF (ring 3, NO libc). Pure inline syscalls + our
 * own string/number helpers. Single self-contained file. crt0-linked: the
 * entry point is _start (crt0.asm), which parses argc/argv off the kernel
 * stack and calls main(argc, argv).
 *
 * Wraps the kernel block-device syscalls (kernel/core/syscall/handlers.c):
 *
 *   SYS_BLK_READ  (49): sc(49, lba, count, ubuf) -> 0 on success, < 0 errno
 *   SYS_BLK_WRITE (50): sc(50, lba, count, ubuf) -> 0 on success, < 0 errno
 *
 * Both operate on 512-byte sectors. `count` is a sector count; `ubuf` must be
 * at least count*512 bytes. The kernel bounds count (<= 256 sectors), bounces
 * through a kmalloc'd staging buffer, and validates ubuf via copy_to/from_user.
 *
 * --------------------------------------------------------------------------
 * USAGE (argv provided by crt0):
 *
 *   blk read  LBA            read 1 sector at LBA, hexdump the first 32 bytes
 *   blk write LBA STRING     write STRING into sector LBA (NUL-padded to 512)
 *
 * SELF-TEST (argc <= 1): if a disk is present, write a known 16-byte pattern
 * to a HIGH lba (2048, away from any filesystem), read it back, and verify it
 * matches -> "BLK SELFTEST: PASS". If no disk is present (SYS_BLK_* return
 * -ENODEV), print "BLK SELFTEST: SKIP (no disk)". The default smoke run has no
 * disk attached, so SKIP is the expected outcome there.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/blk/blk.c -o blk.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       crt0.o blk.o -o build/blk
 *   objdump -d build/blk | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified against kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_EXIT       0
#define SYS_WRITE      3
#define SYS_BLK_READ   49   /* sc(49, lba, count, ubuf) -> 0 or -errno */
#define SYS_BLK_WRITE  50   /* sc(50, lba, count, ubuf) -> 0 or -errno */

#define FD_STDOUT      1

#define SECTOR_SIZE    512
#define SELFTEST_LBA   2048ULL  /* 1 MiB into the disk; clear of any FS */

/* The kernel returns -ENODEV (-19) when no usable SATA disk is present. */
#define ENODEV_NEG     (-19)

typedef unsigned long long u64;
typedef unsigned int       u32;
typedef unsigned char      u8;

/* -----------------------------------------------------------------------
 * Inline syscall helper (3 args is enough for all calls here).
 * RAX=number, RDI=a1, RSI=a2, RDX=a3.
 * --------------------------------------------------------------------- */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* lba + count are 64-bit; pass them through directly (kernel reads rdi/rsi). */
static inline long blk_read(u64 lba, u64 count, void *buf)
{
    return sc(SYS_BLK_READ, (long)lba, (long)count, (long)buf);
}
static inline long blk_write(u64 lba, u64 count, const void *buf)
{
    return sc(SYS_BLK_WRITE, (long)lba, (long)count, (long)buf);
}

/* =======================================================================
 *  Freestanding helpers.
 * ======================================================================= */
static unsigned long s_strlen(const char *s)
{
    unsigned long n = 0;
    while (s && s[n]) n++;
    return n;
}

static void out(const char *s)
{
    sc(SYS_WRITE, FD_STDOUT, (long)s, (long)s_strlen(s));
}

/* Print an unsigned decimal directly to stdout. */
static void out_u(u64 val)
{
    char tmp[24];
    int  i = 0;
    if (val == 0) { out("0"); return; }
    while (val > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (int)(val % 10ULL));
        val /= 10ULL;
    }
    char rev[24];
    int  j = 0;
    while (i-- > 0) rev[j++] = tmp[i];
    rev[j] = '\0';
    out(rev);
}

/* Print a byte as two lowercase hex digits. */
static void out_hex8(u8 b)
{
    static const char hx[] = "0123456789abcdef";
    char s[3];
    s[0] = hx[(b >> 4) & 0xF];
    s[1] = hx[b & 0xF];
    s[2] = '\0';
    out(s);
}

/*
 * Parse a non-negative decimal integer from `s` into a u64. On success returns
 * 1 and stores the value in *out_val; on malformed/empty input returns 0.
 */
static int parse_u64(const char *s, u64 *out_val)
{
    if (!s || !*s) return 0;
    u64 v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10ULL + (u64)(*p - '0');
    }
    *out_val = v;
    return 1;
}

/* strcmp returning 0 on equal (freestanding). */
static int s_eq(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return (*a == *b);
}

/* =======================================================================
 *  I/O sector buffers in .bss (linker aligns .bss; safe for syscall bounce).
 * ======================================================================= */
static u8 g_buf[SECTOR_SIZE];

/* =======================================================================
 *  USAGE text.
 * ======================================================================= */
static void usage(void)
{
    out("usage: blk read LBA | blk write LBA STRING\n");
    out("  read  LBA         read 1 sector, hexdump first 32 bytes\n");
    out("  write LBA STRING  write STRING into sector LBA\n");
}

/* Hexdump the first `n` bytes of `buf` as space-separated hex. */
static void hexdump(const u8 *buf, int n)
{
    for (int i = 0; i < n; i++) {
        out_hex8(buf[i]);
        out((i + 1 < n) ? " " : "\n");
    }
}

/* =======================================================================
 *  blk read LBA -- read one sector, hexdump first 32 bytes.
 * ======================================================================= */
static int do_read(const char *lba_str)
{
    u64 lba;
    if (!parse_u64(lba_str, &lba)) {
        out("blk: invalid lba: "); out(lba_str); out("\n");
        return 1;
    }

    for (int i = 0; i < SECTOR_SIZE; i++) g_buf[i] = 0;

    long r = blk_read(lba, 1, g_buf);
    if (r != 0) {
        out("blk: read failed at lba "); out_u(lba);
        out(" (rc="); out_u((u64)(-r)); out(")\n");
        return 1;
    }

    out("blk: sector "); out_u(lba); out(" [0..31]: ");
    hexdump(g_buf, 32);
    return 0;
}

/* =======================================================================
 *  blk write LBA STRING -- write STRING into sector LBA (NUL-padded).
 * ======================================================================= */
static int do_write(const char *lba_str, const char *str)
{
    u64 lba;
    if (!parse_u64(lba_str, &lba)) {
        out("blk: invalid lba: "); out(lba_str); out("\n");
        return 1;
    }

    for (int i = 0; i < SECTOR_SIZE; i++) g_buf[i] = 0;
    unsigned long n = s_strlen(str);
    if (n > SECTOR_SIZE) n = SECTOR_SIZE;
    for (unsigned long i = 0; i < n; i++) g_buf[i] = (u8)str[i];

    long r = blk_write(lba, 1, g_buf);
    if (r != 0) {
        out("blk: write failed at lba "); out_u(lba);
        out(" (rc="); out_u((u64)(-r)); out(")\n");
        return 1;
    }

    out("blk: wrote "); out_u((u64)n); out(" bytes to sector "); out_u(lba);
    out("\n");
    return 0;
}

/* =======================================================================
 *  Self-test: round-trip a known 16-byte pattern at a HIGH lba.
 *
 *  - No disk present (SYS_BLK_* return -ENODEV) -> SKIP (expected in smoke).
 *  - Disk present, round-trip matches            -> PASS.
 *  - Disk present, I/O error or mismatch         -> FAIL (exit 1).
 * ======================================================================= */
static int selftest(void)
{
    static const u8 pattern[16] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33,
        0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB
    };

    out("BLK SELFTEST: begin\n");

    /* Build the write buffer: pattern in the first 16 bytes, rest zeroed. */
    for (int i = 0; i < SECTOR_SIZE; i++) g_buf[i] = 0;
    for (int i = 0; i < 16; i++) g_buf[i] = pattern[i];

    long w = blk_write(SELFTEST_LBA, 1, g_buf);
    if (w == ENODEV_NEG) {
        out("BLK SELFTEST: SKIP (no disk)\n");
        return 0;
    }
    if (w != 0) {
        out("BLK SELFTEST: FAIL (write rc="); out_u((u64)(-w)); out(")\n");
        return 1;
    }

    /* Read it back into a fresh buffer. */
    for (int i = 0; i < SECTOR_SIZE; i++) g_buf[i] = 0;
    long r = blk_read(SELFTEST_LBA, 1, g_buf);
    if (r == ENODEV_NEG) {
        /* Vanished mid-test -- treat as no disk. */
        out("BLK SELFTEST: SKIP (no disk)\n");
        return 0;
    }
    if (r != 0) {
        out("BLK SELFTEST: FAIL (read rc="); out_u((u64)(-r)); out(")\n");
        return 1;
    }

    for (int i = 0; i < 16; i++) {
        if (g_buf[i] != pattern[i]) {
            out("BLK SELFTEST: FAIL (mismatch at byte "); out_u((u64)i);
            out(")\n");
            return 1;
        }
    }

    out("BLK SELFTEST: PASS\n");
    return 0;
}

/* =======================================================================
 *  Entry point. crt0.asm calls main(argc, argv).
 * ======================================================================= */
int main(int argc, char **argv)
{
    if (argc <= 1) {
        return selftest();
    }

    if (argc >= 3 && s_eq(argv[1], "read")) {
        return do_read(argv[2]);
    }
    if (argc >= 4 && s_eq(argv[1], "write")) {
        return do_write(argv[2], argv[3]);
    }

    usage();
    return 1;
}

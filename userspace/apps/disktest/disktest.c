/**
 * disktest - userspace AHCI/SATA block I/O smoke test
 * ===================================================
 *
 * Writes a known pattern to a scratch sector via SYS_BLK_WRITE, reads it back
 * via SYS_BLK_READ, and verifies the round-trip. Pure syscalls, no libc.
 *
 * Requires the kernel to expose:
 *     SYS_BLK_READ  (41): rdi=dev, rsi=lba_lo32, rdx=lba_hi32, r10=count, r8=buf
 *     SYS_BLK_WRITE (42): rdi=dev, rsi=lba_lo32, rdx=lba_hi32, r10=count, r8=buf
 *   returning 0 on success, negative on error.
 *
 * (The kernel side is sketched in the REPORT; this app is the consumer.)
 *
 * Build (freestanding, matching userspace/Makefile test_minimal recipe):
 *   gcc -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector \
 *       -mno-red-zone -fno-pic -fno-pie -c disktest.c -o disktest.o
 *   ld -nostdlib -static -n -T ../../test_program.ld disktest.o -o disktest
 */

/* --- syscall numbers (must match kernel/include/syscall.h) --------------- */
#define SYS_EXIT       0
#define SYS_WRITE      3   /* fd, buf, count */
#define SYS_GET_TICKS_MS 40
#define SYS_BLK_READ   41  /* PROPOSED */
#define SYS_BLK_WRITE  42  /* PROPOSED */

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned char  u8;

/* --- raw syscall (System V-ish: rax=nr, rdi/rsi/rdx/r10/r8/r9 args) ------ */
static inline long syscall5(long nr, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile(
        "syscall\n"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline long sys_write(int fd, const void* buf, u64 count) {
    return syscall5(SYS_WRITE, fd, (long)buf, (long)count, 0, 0);
}
static inline void sys_exit(int code) {
    syscall5(SYS_EXIT, code, 0, 0, 0, 0);
    __builtin_unreachable();
}

/*
 * LBA is 64-bit; pass it as two 32-bit halves to keep the ABI register-clean.
 * dev=device index (0 == first SATA disk).
 */
static inline long sys_blk_read(u32 dev, u64 lba, u32 count, void* buf) {
    return syscall5(SYS_BLK_READ, dev,
                    (long)(u32)(lba & 0xFFFFFFFF),
                    (long)(u32)(lba >> 32), count, (long)buf);
}
static inline long sys_blk_write(u32 dev, u64 lba, u32 count, const void* buf) {
    return syscall5(SYS_BLK_WRITE, dev,
                    (long)(u32)(lba & 0xFFFFFFFF),
                    (long)(u32)(lba >> 32), count, (long)buf);
}

/* --- tiny string helpers ------------------------------------------------- */
static u64 slen(const char* s) { u64 n = 0; while (s[n]) n++; return n; }
static void puts_(const char* s) { sys_write(1, s, slen(s)); }

/* --- test buffers (in .bss, page-ish aligned by linker .bss ALIGN(4K)) --- */
static u8 wbuf[512];
static u8 rbuf[512];

#define SCRATCH_LBA  2048   /* 1 MiB into the disk; clear of any superblock */

void _start(void) {
    puts_("disktest: AHCI block I/O round-trip\n");

    /* Fill a recognizable pattern. */
    for (int i = 0; i < 512; i++) wbuf[i] = (u8)(i ^ 0xA5);
    /* A signature at the start so a human can eyeball it on disk too. */
    const char sig[] = "DISKTEST-OK!";
    for (unsigned i = 0; i < sizeof(sig); i++) wbuf[i] = sig[i];

    long w = sys_blk_write(0, SCRATCH_LBA, 1, wbuf);
    if (w != 0) { puts_("disktest: WRITE failed\n"); sys_exit(1); }
    puts_("disktest: wrote sector\n");

    for (int i = 0; i < 512; i++) rbuf[i] = 0;
    long r = sys_blk_read(0, SCRATCH_LBA, 1, rbuf);
    if (r != 0) { puts_("disktest: READ failed\n"); sys_exit(2); }
    puts_("disktest: read sector\n");

    int mismatch = -1;
    for (int i = 0; i < 512; i++) {
        if (rbuf[i] != wbuf[i]) { mismatch = i; break; }
    }
    if (mismatch >= 0) {
        puts_("disktest: MISMATCH - data did not round-trip\n");
        sys_exit(3);
    }

    puts_("disktest: PASS - sector write/read round-trip OK\n");
    sys_exit(0);
}

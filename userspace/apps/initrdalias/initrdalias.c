/* initrdalias -- INITRD-ALIAS-0: the big-image / mmap-heavy reader.
 *
 * THE BUG THIS PROVES DEAD: user ELF images load at VA 0x800000 inside the
 * identity-mapped region; this image carries a 16 MiB .bss pad so it spans
 * past VA 16 MiB -- exactly over the initrd rescue copy. Pre-fix, the
 * loader's private zero pages SHADOW the initrd identity mapping in this
 * process's page tables, so every kernel read of initrd-backed file data on
 * this CR3 returns this image's own zeroes (exact byte count, all-zero
 * content). Post-fix the kernel reads the initrd through the DIRECT MAP
 * (supervisor-only, shared into every CR3, unshadowable) and the bytes are
 * exact in EVERY process.
 *
 * The test, in this order:
 *   1. touch the pad ends (keeps the array live; forces the >16 MiB span)
 *   2. two 4 MiB anonymous mmaps (the "mmap-heavy" characterization)
 *   3. read /etc/imgtest/t.png; require exact length, NOT all-zero, and a
 *      byte-exact match against the embedded generated fixture
 *   4. spawn the PRISTINE control (sbin/initrdp, tiny image) and fold its
 *      exit code in -- both halves must see the SAME bytes
 *
 * Verdict (the INITRD-ALIAS-0 acceptance line):
 *   INITRD-ALIAS: PASS pristine_read=1 mmapheavy_read=1 same_bytes=1 zero_bug_gone=1
 * (browser_file_img is proven by browser2's own BROWSER2-IMG-FILE line.)
 */
#include "../browser2/b2_img_fixtures.h"

#define SYS_EXIT     0
#define SYS_READ     2
#define SYS_WRITE    3
#define SYS_OPEN     4
#define SYS_CLOSE    5
#define SYS_WAITPID  6
#define SYS_YIELD    15
#define SYS_SPAWN    16
#define SYS_MMAP     37
#define WNOHANG      1

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

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s), 0, 0, 0); }

/* The whole point: force this image's VA span across the initrd region
 * (8 MiB base + 16 MiB pad -> image top ~24 MiB > initrd end ~22 MiB).
 * VOLATILE is load-bearing: a plain static array that is written but never
 * read is dead-store-eliminated wholesale at -O2 (first build shipped with
 * memsz=0x100 -- no pad, no shadow, false PASS pre-fix). A volatile object
 * cannot be elided, so the 16 MiB lands in p_memsz and the loader maps it. */
#define PAD_BYTES (16u * 1024u * 1024u)
static volatile unsigned char big_pad[PAD_BYTES];

static unsigned char buf[256];

int main(void)
{
    /* 1. keep the pad live (an unreferenced static array would be dropped). */
    big_pad[0] = 1;
    big_pad[PAD_BYTES - 1] = 1;

    /* 2. mmap-heavy: two 4 MiB anonymous regions, first bytes touched. */
    int mmaps_ok = 1;
    for (int i = 0; i < 2; i++) {
        long p = sc(SYS_MMAP, 0, 4 * 1024 * 1024, 3, 0x22, 0, 0);
        if (p <= 0) { mmaps_ok = 0; break; }
        *(volatile unsigned char *)p = 1;
    }

    /* 3. read the initrd-backed fixture on THIS (shadow-prone) CR3. */
    long fd = sc(SYS_OPEN, (long)"/etc/imgtest/t.png", 0, 0, 0, 0, 0);
    long total = 0;
    if (fd >= 0) {
        for (int g = 0; g < 64 && total < (long)sizeof(buf); g++) {
            long n = sc(SYS_READ, fd, (long)(buf + total),
                        (long)sizeof(buf) - total, 0, 0, 0);
            if (n <= 0) break;
            total += n;
        }
        sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }

    int nonzero = 0;
    for (long i = 0; i < total; i++) if (buf[i]) { nonzero = 1; break; }

    int len_ok = (total == (long)sizeof(b2fix_t_png));
    int same   = len_ok;
    if (same)
        for (long i = 0; i < total; i++)
            if (buf[i] != b2fix_t_png[i]) { same = 0; break; }

    int mmapheavy_read = (mmaps_ok && len_ok && nonzero);
    int zero_bug_gone  = (len_ok && nonzero);   /* count right AND data real */

    /* 4. the pristine control: tiny image, same file, same expected bytes. */
    int pristine = 0;
    long pid = sc(SYS_SPAWN, (long)"sbin/initrdp", 0, 0, 0, 0, 0);
    if (pid > 0) {
        int st = -1;
        for (long i = 0; i < 8000000; i++) {
            long w = sc(SYS_WAITPID, pid, (long)&st, WNOHANG, 0, 0, 0);
            if (w != 0) break;
            sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
        }
        pristine = (st == 0);
    }

    int ok = (pristine && mmapheavy_read && same && zero_bug_gone);
    puts1("INITRD-ALIAS: ");
    puts1(ok ? "PASS" : "FAIL");
    puts1(" pristine_read=");  puts1(pristine ? "1" : "0");
    puts1(" mmapheavy_read="); puts1(mmapheavy_read ? "1" : "0");
    puts1(" same_bytes=");     puts1(same ? "1" : "0");
    puts1(" zero_bug_gone=");  puts1(zero_bug_gone ? "1" : "0");
    puts1("\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0, 0, 0, 0);
    return ok ? 0 : 1;
}

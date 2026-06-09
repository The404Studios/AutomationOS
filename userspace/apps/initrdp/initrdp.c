/* initrdp -- INITRD-ALIAS-0 PRISTINE reader.
 *
 * A deliberately TINY-image process (small .bss, no mmap): its user image
 * ends far below VA 16 MiB, so nothing it maps can shadow the initrd. It
 * reads /etc/imgtest/t.png from the VFS and byte-compares against the
 * embedded generated fixture (the same bytes build_all staged into the
 * initrd). This is the control half of the aliasing regression pair --
 * sbin/initrdalias (the big-image half) spawns it and folds its exit code
 * into the INITRD-ALIAS verdict.
 *
 * exit 0 = bytes exact; exit 1 = any mismatch/failure.
 */
#include "../browser2/b2_img_fixtures.h"

#define SYS_EXIT   0
#define SYS_READ   2
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5

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

static unsigned char buf[256];

int main(void)
{
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

    int len_ok  = (total == (long)sizeof(b2fix_t_png));
    int same    = len_ok;
    if (same)
        for (long i = 0; i < total; i++)
            if (buf[i] != b2fix_t_png[i]) { same = 0; break; }

    puts1(same ? "INITRDP: PASS pristine_read=1 same_bytes=1\n"
               : "INITRDP: FAIL\n");
    sc(SYS_EXIT, same ? 0 : 1, 0, 0, 0, 0, 0);
    return same ? 0 : 1;
}

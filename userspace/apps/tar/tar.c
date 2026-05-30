/*
 * tar.c -- minimal POSIX ustar archiver for the from-scratch x86_64 OS.
 * =====================================================================
 *
 * FREESTANDING ring-3 userspace, NO libc. Pure inline syscalls + tiny
 * self-contained mem/str helpers. Output to fd 1 (serial + VGA).
 *
 * The on-disk format is byte-for-byte the same 512-byte POSIX ustar header
 * the kernel's own initrd parser consumes (see kernel/init/initrd.c):
 *
 *   struct tar_header {
 *     char name[100]; char mode[8]; char uid[8]; char gid[8];
 *     char size[12];  char mtime[12]; char checksum[8]; char typeflag;
 *     char linkname[100]; char magic[6] ("ustar"); char version[2] ("00");
 *     char uname[32]; char gname[32]; char devmajor[8]; char devminor[8];
 *     char prefix[155]; char padding[12];
 *   }  // exactly 512 bytes, packed
 *
 *   - typeflag '0' = regular file, '5' = directory
 *   - size  : octal, NUL/space terminated within the 12-byte field
 *   - magic : "ustar\0", version "00"
 *   - file data follows the header, padded up to a 512-byte boundary
 *   - the archive ends with two all-zero 512-byte blocks
 *   - the checksum is the unsigned sum of all 512 header bytes with the
 *     checksum field itself treated as 8 ASCII spaces, written back as a
 *     6-digit zero-padded octal number followed by NUL and a space.
 *
 * So archives produced here are valid initrd images, and this tool extracts
 * any archive the kernel would accept.
 *
 * Usage:
 *   tar -cf ARCHIVE PATH...     create
 *   tar -xf ARCHIVE [DESTDIR]   extract (DESTDIR default ".")
 *   tar -tf ARCHIVE             list
 *   tar selftest                run the built-in round-trip self test
 *
 * NOTE ON argv: this kernel does NOT pass argv to spawned programs. SYS_SPAWN
 * (kernel/core/syscall/handlers.c) copies only the path and elf_load_and_exec()
 * builds a fresh stack with no argc/argv; every userspace `_start(void)` takes
 * no arguments. Therefore _start() below CANNOT read a command line and instead
 * runs the self-test unconditionally. All real tar logic lives in callable
 * functions (tar_create / tar_extract / tar_list) so a future argv-carrying
 * exec, or another in-kernel caller, can drive them directly.
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at fs:0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/tar/tar.c -o /tmp/tar.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       /tmp/tar.o -o /tmp/tar.elf
 *   objdump -d /tmp/tar.elf | grep 'fs:0x28'   # must produce no output
 */

#include "tar.h"

/* ======================================================================
 * Syscall numbers (verified against kernel/include/syscall.h).
 * ==================================================================== */
#define SYS_EXIT      0
#define SYS_READ      2
#define SYS_WRITE     3
#define SYS_OPEN      4
#define SYS_CLOSE     5
#define SYS_OPENDIR   30
#define SYS_READDIR   31
#define SYS_CLOSEDIR  32
#define SYS_STAT      33
#define SYS_MKDIR     67

/* O_* flags (kernel/include/vfs.h). */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

/* dirent type byte (kernel/include/vfs.h). */
#define DT_DIR    4
#define DT_REG    8
#define NAME_MAX_ 256

/* The kernel copies sizeof(struct dirent) bytes into our buffer in
 * sys_readdir(); mirror that layout exactly. */
typedef struct {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[NAME_MAX_];
} k_dirent_t;

/* vfs_stat_t layout (kernel/include/vfs.h). st_mode is the bare permission
 * mode WITHOUT POSIX S_IFDIR/S_IFREG type bits (kernel stores type
 * separately and does not expose it via stat), so directory detection is
 * done by probing SYS_OPENDIR, not by inspecting st_mode. */
typedef struct {
    unsigned long long st_dev;
    unsigned long long st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned int       st_uid;
    unsigned int       st_gid;
    unsigned long long st_rdev;
    unsigned long long st_size;
    unsigned long long st_blksize;
    unsigned long long st_blocks;
    unsigned long long st_atime;
    unsigned long long st_mtime;
    unsigned long long st_ctime;
} k_stat_t;

/* ======================================================================
 * Inline syscall helpers (3- and 6-arg variants).
 * ==================================================================== */
static inline long sc3(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* ======================================================================
 * Freestanding mem/str helpers.
 * ==================================================================== */
static unsigned long t_strlen(const char *s) {
    unsigned long n = 0; while (s[n]) n++; return n;
}

static void t_memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    for (unsigned long i = 0; i < n; i++) p[i] = (unsigned char)c;
}

static void t_memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    for (unsigned long i = 0; i < n; i++) dp[i] = sp[i];
}

static int t_memeq(const void *a, const void *b, unsigned long n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (unsigned long i = 0; i < n; i++) if (pa[i] != pb[i]) return 0;
    return 1;
}

static int t_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Copy src into dst (cap incl NUL); returns length written (excl NUL). */
static int t_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* ---- fd-1 diagnostics ---- */
static void out(const char *s) { sc3(SYS_WRITE, 1, (long)s, (long)t_strlen(s)); }
static void outn(const char *s, long n) { sc3(SYS_WRITE, 1, (long)s, n); }

static void out_unum(unsigned long n) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + (n % 10)); n /= 10; } while (n > 0);
    while (i > 0) { char c = b[--i]; sc3(SYS_WRITE, 1, (long)&c, 1); }
}

/* ======================================================================
 * Octal <-> integer.
 * ==================================================================== */

/* Parse up to len octal digits (stops at NUL or non-octal); leading spaces
 * are skipped, exactly like the kernel's octal_to_int(). */
static unsigned long oct_to_u64(const char *s, int len) {
    unsigned long v = 0;
    int i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    for (; i < len && s[i]; i++) {
        if (s[i] < '0' || s[i] > '7') break;
        v = v * 8 + (unsigned long)(s[i] - '0');
    }
    return v;
}

/* Write val as a `width`-char zero-padded octal string into buf, then a
 * trailing NUL at buf[width-1] is OVERWRITTEN by the caller's field rules.
 * Here we fill exactly `digits` octal chars right-justified, zero-padded. */
static void u64_to_oct(char *buf, unsigned long val, int digits) {
    for (int i = digits - 1; i >= 0; i--) {
        buf[i] = (char)('0' + (val & 7));
        val >>= 3;
    }
}

/* ======================================================================
 * ustar header construction + checksum.
 * ==================================================================== */

/* The standard ustar checksum: sum of all 512 header bytes, with the 8-byte
 * checksum field counted as ASCII spaces (0x20). Returned as unsigned. */
static unsigned long ustar_checksum(const tar_header_t *h) {
    const unsigned char *p = (const unsigned char *)h;
    unsigned long sum = 0;
    for (int i = 0; i < 512; i++) {
        if (i >= 148 && i < 156) sum += (unsigned char)' '; /* checksum field */
        else sum += p[i];
    }
    return sum;
}

/* Fill a zeroed header for `name` (NUL-terminated, <=99 chars used), of the
 * given size and typeflag, then compute + write the checksum field. */
static void build_header(tar_header_t *h, const char *name,
                         unsigned long size, char typeflag, unsigned mode) {
    t_memset(h, 0, sizeof(*h));

    /* name[100] -- truncate silently if longer (no prefix splitting). */
    int n = 0;
    while (name[n] && n < 99) { h->name[n] = name[n]; n++; }
    h->name[n] = '\0';

    /* mode/uid/gid as 7-octal-digit + NUL (8-byte fields). */
    u64_to_oct(h->mode, mode & 07777, 7); h->mode[7] = '\0';
    u64_to_oct(h->uid, 0, 7); h->uid[7] = '\0';
    u64_to_oct(h->gid, 0, 7); h->gid[7] = '\0';

    /* size: 11 octal digits + NUL (12-byte field). */
    u64_to_oct(h->size, size, 11); h->size[11] = '\0';

    /* mtime: 11 octal digits + NUL. No RTC dependency -> 0. */
    u64_to_oct(h->mtime, 0, 11); h->mtime[11] = '\0';

    h->typeflag = typeflag;

    /* magic "ustar\0", version "00" -- matches kernel validate_tar_header(). */
    h->magic[0] = 'u'; h->magic[1] = 's'; h->magic[2] = 't';
    h->magic[3] = 'a'; h->magic[4] = 'r'; h->magic[5] = '\0';
    h->version[0] = '0'; h->version[1] = '0';

    /* checksum: 6 octal digits, NUL, space -- the canonical GNU/POSIX form. */
    unsigned long ck = ustar_checksum(h);
    u64_to_oct(h->checksum, ck, 6);
    h->checksum[6] = '\0';
    h->checksum[7] = ' ';
}

/* Validate magic exactly like the kernel does. */
static int header_valid(const tar_header_t *h) {
    return h->magic[0] == 'u' && h->magic[1] == 's' && h->magic[2] == 't' &&
           h->magic[3] == 'a' && h->magic[4] == 'r';
}

static int is_zero_block(const unsigned char *b) {
    for (int i = 0; i < 512; i++) if (b[i]) return 0;
    return 1;
}

/* ======================================================================
 * Output sink: write all archive bytes to a single open fd.
 * ==================================================================== */
static int write_all(int fd, const void *buf, unsigned long n) {
    const char *p = (const char *)buf;
    unsigned long off = 0;
    while (off < n) {
        long w = sc3(SYS_WRITE, fd, (long)(p + off), (long)(n - off));
        if (w <= 0) return -1;
        off += (unsigned long)w;
    }
    return 0;
}

static int read_full(int fd, void *buf, unsigned long n) {
    char *p = (char *)buf;
    unsigned long off = 0;
    while (off < n) {
        long r = sc3(SYS_READ, fd, (long)(p + off), (long)(n - off));
        if (r <= 0) break;
        off += (unsigned long)r;
    }
    return (int)off;   /* bytes actually read */
}

/* ======================================================================
 * Path detection: is this path a directory?
 *
 * st_mode does not carry the POSIX type bit on this kernel, so we probe by
 * trying SYS_OPENDIR. Success => directory. We also stat() to tell exists
 * vs not-exist. Returns: 1 dir, 0 regular file, -1 does not exist.
 *
 * `path` is passed straight to the syscalls: sys_stat/sys_opendir use
 * copy_user_string(), which copies only up to the terminating NUL, so any
 * NUL-terminated caller string is safe (no fixed-size scratch needed).
 * ==================================================================== */
static int path_kind(const char *path) {
    k_stat_t st;
    if (sc3(SYS_STAT, (long)path, (long)&st, 0) != 0) return -1;
    long dfd = sc3(SYS_OPENDIR, (long)path, 0, 0);
    if (dfd >= 0) { sc3(SYS_CLOSEDIR, dfd, 0, 0); return 1; }
    return 0;
}

/* ======================================================================
 * CREATE
 * ==================================================================== */

/* shared 4KB I/O scratch (kept off the small user stack). The archive-name
 * scratch g_namebuf is used only by the non-recursive top-level callers and
 * the extract walker, never across the create recursion. */
static char g_iobuf[4096] __attribute__((aligned(16)));
static char g_namebuf[512] __attribute__((aligned(16)));

static int tar_add_file(int afd, const char *fullpath, const char *arcname) {
    k_stat_t st;
    if (sc3(SYS_STAT, (long)fullpath, (long)&st, 0) != 0) {
        out("tar: cannot stat '"); out(fullpath); out("'\n");
        return -1;
    }
    unsigned long size = (unsigned long)st.st_size;
    unsigned mode = st.st_mode ? (unsigned)st.st_mode : 0644u;

    tar_header_t h;
    build_header(&h, arcname, size, '0', mode);
    if (write_all(afd, &h, 512) != 0) return -1;

    long fd = sc3(SYS_OPEN, (long)fullpath, O_RDONLY, 0);
    if (fd < 0) {
        out("tar: cannot open '"); out(fullpath); out("'\n");
        return -1;
    }
    unsigned long written = 0;
    for (;;) {
        long r = sc3(SYS_READ, fd, (long)g_iobuf, (long)sizeof(g_iobuf));
        if (r <= 0) break;
        if (write_all(afd, g_iobuf, (unsigned long)r) != 0) {
            sc3(SYS_CLOSE, fd, 0, 0);
            return -1;
        }
        written += (unsigned long)r;
    }
    sc3(SYS_CLOSE, fd, 0, 0);

    /* pad file data up to the next 512 boundary with zeros */
    unsigned long pad = (512 - (written % 512)) % 512;
    if (pad) {
        char z[512];
        t_memset(z, 0, pad);
        if (write_all(afd, z, pad) != 0) return -1;
    }

    out("  "); out(arcname); out(" ("); out_unum(written); out(" bytes)\n");
    return 0;
}

/* Forward decl for recursion. */
static int tar_add_path(int afd, const char *fullpath, const char *arcname);

static int tar_add_dir(int afd, const char *fullpath, const char *arcname) {
    /* emit the directory header (typeflag '5', size 0). The kernel parser
     * happily creates dirs from these. We normalize the stored name to end
     * with '/' -- conventional for ustar directory entries. */
    char dname[101];
    int dl = t_strlcpy(dname, arcname, (int)sizeof(dname));
    if (dl > 0 && dname[dl - 1] != '/' && dl < (int)sizeof(dname) - 1) {
        dname[dl] = '/'; dname[dl + 1] = '\0';
    }
    k_stat_t st;
    unsigned mode = 0755u;
    if (sc3(SYS_STAT, (long)fullpath, (long)&st, 0) == 0 && st.st_mode)
        mode = (unsigned)st.st_mode;

    tar_header_t h;
    build_header(&h, dname, 0, '5', mode);
    if (write_all(afd, &h, 512) != 0) return -1;
    out("  "); out(dname); out("\n");

    /* recurse into entries. All scratch is on the stack so it is private to
     * this recursion level -- nested tar_add_dir calls cannot clobber the
     * parent's childpath while it is still in use as the recursion argument. */
    long dfd = sc3(SYS_OPENDIR, (long)fullpath, 0, 0);
    if (dfd < 0) return 0;   /* empty/unreadable dir: header already written */

    k_dirent_t de;
    char childpath[512];
    char carc[256];
    for (;;) {
        long r = sc3(SYS_READDIR, dfd, (long)&de, 0);
        if (r != 0) break;
        de.d_name[NAME_MAX_ - 1] = '\0';
        const char *nm = de.d_name;
        if (nm[0] == '\0') continue;
        if (t_streq(nm, ".") || t_streq(nm, "..")) continue;

        /* build child full path: fullpath + "/" + nm */
        int n = 0;
        const char *fp = fullpath;
        while (*fp && n < (int)sizeof(childpath) - 1) childpath[n++] = *fp++;
        if (n == 0 || childpath[n - 1] != '/')
            if (n < (int)sizeof(childpath) - 1) childpath[n++] = '/';
        const char *cn = nm;
        while (*cn && n < (int)sizeof(childpath) - 1) childpath[n++] = *cn++;
        childpath[n] = '\0';

        /* build child archive name: arcname (no trailing /) + "/" + nm */
        int m = t_strlcpy(carc, arcname, (int)sizeof(carc));
        if (m > 0 && carc[m - 1] == '/') m--;        /* drop trailing slash */
        if (m < (int)sizeof(carc) - 1) carc[m++] = '/';
        int k = 0;
        while (nm[k] && m < (int)sizeof(carc) - 1) carc[m++] = nm[k++];
        carc[m] = '\0';

        if (tar_add_path(afd, childpath, carc) != 0) {
            sc3(SYS_CLOSEDIR, dfd, 0, 0);
            return -1;
        }
    }
    sc3(SYS_CLOSEDIR, dfd, 0, 0);
    return 0;
}

static int tar_add_path(int afd, const char *fullpath, const char *arcname) {
    int kind = path_kind(fullpath);
    if (kind < 0) {
        out("tar: '"); out(fullpath); out("' not found\n");
        return -1;
    }
    if (kind == 1) return tar_add_dir(afd, fullpath, arcname);
    return tar_add_file(afd, fullpath, arcname);
}

/* Derive a clean archive name from a path: strip a single leading "./" and
 * all leading '/'. The result is what gets stored in the header (relative). */
static const char *arcname_of(const char *path) {
    const char *p = path;
    if (p[0] == '.' && p[1] == '/') p += 2;
    while (*p == '/') p++;
    if (*p == '\0') p = path;   /* path was "/" -- keep something */
    return p;
}

/*
 * tar_create -- write a ustar archive at `archive` containing each of the
 * `count` paths in `paths` (files walked shallow, dirs walked recursively).
 * Returns 0 on success, -1 on any error.
 */
int tar_create(const char *archive, const char *const *paths, int count) {
    long afd = sc3(SYS_OPEN, (long)archive, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (afd < 0) {
        out("tar: cannot create archive '"); out(archive); out("'\n");
        return -1;
    }

    int rc = 0;
    for (int i = 0; i < count; i++) {
        /* arcname_of returns a pointer into paths[i] (stable for this call);
         * tar_add_dir consumes it before any recursion, so no copy needed. */
        const char *arc = arcname_of(paths[i]);
        if (tar_add_path((int)afd, paths[i], arc) != 0) { rc = -1; break; }
    }

    /* two zero terminator blocks */
    if (rc == 0) {
        char z[1024];
        t_memset(z, 0, sizeof(z));
        if (write_all((int)afd, z, sizeof(z)) != 0) rc = -1;
    }

    sc3(SYS_CLOSE, afd, 0, 0);
    return rc;
}

/* ======================================================================
 * LIST / EXTRACT
 * ==================================================================== */

/* Make every parent directory of `path` (a file path) exist. Walks the
 * components and SYS_MKDIRs each prefix; the kernel mkdir is recursive but
 * we do it explicitly so an archive without dir entries still extracts. */
static void make_parents(const char *path) {
    char tmp[512];
    int n = t_strlcpy(tmp, path, (int)sizeof(tmp));
    /* find last '/' */
    int last = -1;
    for (int i = 0; i < n; i++) if (tmp[i] == '/') last = i;
    if (last <= 0) return;      /* file in cwd or root */
    tmp[last] = '\0';
    sc3(SYS_MKDIR, (long)tmp, 0755, 0);
}

/* Join destdir + "/" + name into out[cap]. If name is absolute it is taken
 * as-is when destdir is empty/".". */
static void join_path(char *out, int cap, const char *destdir, const char *name) {
    int n = 0;
    int have_dest = destdir && destdir[0] &&
                    !(destdir[0] == '.' && destdir[1] == '\0');
    if (have_dest) {
        const char *d = destdir;
        while (*d && n < cap - 1) out[n++] = *d++;
        if (n == 0 || out[n - 1] != '/')
            if (n < cap - 1) out[n++] = '/';
    }
    /* strip leading "./" and leading slashes from name when prefixing dest */
    const char *nm = name;
    if (have_dest) {
        if (nm[0] == '.' && nm[1] == '/') nm += 2;
        while (*nm == '/') nm++;
    }
    while (*nm && n < cap - 1) out[n++] = *nm++;
    out[n] = '\0';
}

/*
 * Internal walker shared by list + extract. If `extract` is 0 it only prints
 * entries; if 1 it creates dirs/files under `destdir`.
 */
static int tar_walk(const char *archive, const char *destdir, int extract) {
    long afd = sc3(SYS_OPEN, (long)archive, O_RDONLY, 0);
    if (afd < 0) {
        out("tar: cannot open archive '"); out(archive); out("'\n");
        return -1;
    }

    int rc = 0;
    int empty_run = 0;
    for (;;) {
        tar_header_t h;
        int got = read_full((int)afd, &h, 512);
        if (got == 0) break;               /* clean EOF */
        if (got < 512) break;              /* truncated */

        if (is_zero_block((const unsigned char *)&h)) {
            if (++empty_run >= 2) break;    /* end of archive */
            continue;
        }
        empty_run = 0;

        if (!header_valid(&h)) {
            out("tar: invalid header, stopping\n");
            rc = -1; break;
        }

        h.name[99] = '\0';
        unsigned long size = oct_to_u64(h.size, sizeof(h.size));
        char type = h.typeflag;

        if (!extract) {
            /* list: "f/d  SIZE  NAME" */
            char tc = (type == '5') ? 'd' : 'f';
            char tb[2] = { tc, ' ' };
            outn(tb, 2);
            out(" "); out_unum(size); out("  ");
            out(h.name); out("\n");
        }

        if (type == '5') {
            if (extract) {
                join_path(g_namebuf, (int)sizeof(g_namebuf), destdir, h.name);
                /* drop any trailing slash before mkdir */
                int L = (int)t_strlen(g_namebuf);
                if (L > 1 && g_namebuf[L - 1] == '/') g_namebuf[L - 1] = '\0';
                sc3(SYS_MKDIR, (long)g_namebuf, 0755, 0);
            }
            /* directories carry no data; no padding to skip */
            continue;
        }

        /* regular file ('0' or '\0') */
        if (extract) {
            join_path(g_namebuf, (int)sizeof(g_namebuf), destdir, h.name);
            make_parents(g_namebuf);
            long fd = sc3(SYS_OPEN, (long)g_namebuf,
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                out("tar: cannot create '"); out(g_namebuf); out("'\n");
                rc = -1;
                /* still must consume the data + padding to stay aligned */
            }
            unsigned long remaining = size;
            while (remaining > 0) {
                unsigned long chunk = remaining < sizeof(g_iobuf)
                                    ? remaining : sizeof(g_iobuf);
                int r = read_full((int)afd, g_iobuf, chunk);
                if (r <= 0) { rc = -1; remaining = 0; break; }
                if (fd >= 0) {
                    if (write_all((int)fd, g_iobuf, (unsigned long)r) != 0) rc = -1;
                }
                remaining -= (unsigned long)r;
                if ((unsigned long)r < chunk) break;  /* short read: truncated */
            }
            if (fd >= 0) sc3(SYS_CLOSE, fd, 0, 0);

            /* skip padding to next 512 boundary */
            unsigned long pad = (512 - (size % 512)) % 512;
            if (pad) {
                char skip[512];
                read_full((int)afd, skip, pad);
            }
        } else {
            /* list mode: seek past data by reading + discarding (no lseek). */
            unsigned long skip_total = size + ((512 - (size % 512)) % 512);
            while (skip_total > 0) {
                unsigned long chunk = skip_total < sizeof(g_iobuf)
                                    ? skip_total : sizeof(g_iobuf);
                int r = read_full((int)afd, g_iobuf, chunk);
                if (r <= 0) break;
                skip_total -= (unsigned long)r;
                if ((unsigned long)r < chunk) break;
            }
        }
    }

    sc3(SYS_CLOSE, afd, 0, 0);
    return rc;
}

int tar_list(const char *archive) {
    return tar_walk(archive, ".", 0);
}

int tar_extract(const char *archive, const char *destdir) {
    if (!destdir) destdir = ".";
    return tar_walk(archive, destdir, 1);
}

/* ======================================================================
 * SELF-TEST
 *
 * 1. mkdir /tmp/tartest
 * 2. write /tmp/tartest/a.txt and b.txt with known content
 * 3. tar_create -> /tmp/test.tar
 * 4. tar_list (visual)
 * 5. tar_extract -> /tmp/tarout/
 * 6. read back /tmp/tarout/tartest/a.txt + b.txt, compare to originals
 * 7. print TAR SELFTEST: PASS / FAIL and SYS_EXIT
 * ==================================================================== */

static const char *A_CONTENT = "hello from a.txt\nline two\n";
static const char *B_CONTENT = "BBBB contents of b\n12345\n";

static int write_file(const char *path, const char *content) {
    long fd = sc3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    unsigned long len = t_strlen(content);
    int rc = write_all((int)fd, content, len);
    sc3(SYS_CLOSE, fd, 0, 0);
    return rc;
}

/* Read whole file into buf[cap]; returns bytes read or -1. */
static long read_file(const char *path, char *buf, int cap) {
    long fd = sc3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long total = 0;
    while (total < cap) {
        long r = sc3(SYS_READ, fd, (long)(buf + total), (long)(cap - total));
        if (r <= 0) break;
        total += r;
    }
    sc3(SYS_CLOSE, fd, 0, 0);
    return total;
}

static int content_matches(const char *path, const char *expect) {
    static char rb[4096];
    long n = read_file(path, rb, (int)sizeof(rb));
    if (n < 0) return 0;
    unsigned long el = t_strlen(expect);
    if ((unsigned long)n != el) return 0;
    return t_memeq(rb, expect, el);
}

static void selftest(void) {
    out("TAR: selftest begin\n");

    /* 1+2: create source tree */
    sc3(SYS_MKDIR, (long)"/tmp/tartest", 0755, 0);
    if (write_file("/tmp/tartest/a.txt", A_CONTENT) != 0 ||
        write_file("/tmp/tartest/b.txt", B_CONTENT) != 0) {
        out("TAR SELFTEST: FAIL (could not write source files)\n");
        sc3(SYS_EXIT, 1, 0, 0);
        for (;;) {}
    }

    /* 3: create archive of the two files (explicit paths) */
    const char *paths[2] = { "/tmp/tartest/a.txt", "/tmp/tartest/b.txt" };
    if (tar_create("/tmp/test.tar", paths, 2) != 0) {
        out("TAR SELFTEST: FAIL (create failed)\n");
        sc3(SYS_EXIT, 1, 0, 0);
        for (;;) {}
    }

    /* 4: list (informational) */
    out("TAR: listing /tmp/test.tar:\n");
    tar_list("/tmp/test.tar");

    /* 5: extract to /tmp/tarout/ */
    sc3(SYS_MKDIR, (long)"/tmp/tarout", 0755, 0);
    if (tar_extract("/tmp/test.tar", "/tmp/tarout") != 0) {
        out("TAR SELFTEST: FAIL (extract failed)\n");
        sc3(SYS_EXIT, 1, 0, 0);
        for (;;) {}
    }

    /*
     * 6: read back. The stored archive names are "tmp/tartest/a.txt"
     * (leading slash stripped by arcname_of), so under destdir /tmp/tarout
     * the files land at /tmp/tarout/tmp/tartest/a.txt.
     */
    int ok = content_matches("/tmp/tarout/tmp/tartest/a.txt", A_CONTENT) &&
             content_matches("/tmp/tarout/tmp/tartest/b.txt", B_CONTENT);

    if (ok) out("TAR SELFTEST: PASS\n");
    else    out("TAR SELFTEST: FAIL (content mismatch after round-trip)\n");

    sc3(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {}
}

/* ======================================================================
 * Entry point.
 *
 * crt0 (userspace/crt0.asm) provides _start and calls main(argc, argv).
 *
 *   tar                      -> run the built-in round-trip self-test
 *   tar -cf ARCHIVE path...  -> create
 *   tar -xf ARCHIVE [destdir]-> extract (destdir default "/")
 *   tar -tf ARCHIVE          -> list
 *
 * The self-test path SYS_EXITs from within selftest(); the argv paths return
 * an exit code from main (crt0 forwards it to SYS_EXIT). 0 = ok, 1 = usage.
 * ==================================================================== */
static void usage(void) {
    out("usage: tar -cf ARCHIVE path... | -xf ARCHIVE [destdir] | "
        "-tf ARCHIVE\n");
}

int main(int argc, char **argv) {
    /* No operands -> run the smoke-gated self-test (prints TAR SELFTEST:). */
    if (argc <= 1) {
        out("[TAR] no argv -> running selftest\n");
        selftest();          /* SYS_EXITs internally */
        return 0;            /* not reached */
    }

    const char *flag = argv[1];

    /* -cf ARCHIVE path...  : need at least one path. */
    if (t_streq(flag, "-cf")) {
        if (argc < 4) { usage(); return 1; }
        const char *archive = argv[2];
        const char *const *paths = (const char *const *)&argv[3];
        int count = argc - 3;
        return tar_create(archive, paths, count) == 0 ? 0 : 1;
    }

    /* -xf ARCHIVE [destdir] : destdir defaults to "/". */
    if (t_streq(flag, "-xf")) {
        if (argc < 3) { usage(); return 1; }
        const char *archive = argv[2];
        const char *destdir = (argc >= 4) ? argv[3] : "/";
        return tar_extract(archive, destdir) == 0 ? 0 : 1;
    }

    /* -tf ARCHIVE : list. */
    if (t_streq(flag, "-tf")) {
        if (argc < 3) { usage(); return 1; }
        return tar_list(argv[2]) == 0 ? 0 : 1;
    }

    usage();
    return 1;
}

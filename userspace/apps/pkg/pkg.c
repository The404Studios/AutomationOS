/*
 * pkg.c -- minimal package manager for the from-scratch x86_64 OS.
 * ================================================================
 *
 * FREESTANDING userspace ELF (no libc). Pure inline syscalls + own helpers.
 *
 * A "package" is a POSIX *ustar* tar archive (512-byte headers, matching
 * kernel/init/initrd.c EXACTLY) containing:
 *
 *     manifest          a tiny text file (see format below)
 *     files/...         the payload files, laid out under "files/"
 *
 * The manifest is line-oriented "key=value" text:
 *
 *     name=hello
 *     version=1.0
 *     files=/bin/hello,/etc/hello.conf
 *
 *   - name     : package name (used for the install DB filename)
 *   - version  : free-form version string (informational)
 *   - files    : comma-separated list of ABSOLUTE install paths. The Nth
 *                path receives the contents of the Nth "files/<...>" payload
 *                member, in archive order. (Order matters; this keeps the
 *                manifest tiny and avoids per-file name mapping.)
 *
 * Install destination mapping:
 *   For each archive member whose name begins with "files/", the payload is
 *   written to the corresponding entry of the manifest "files=" list (by
 *   position). Parent directories of each destination are created via
 *   SYS_MKDIR. The set of installed destination paths is recorded, one per
 *   line, in the install DB:  /var/lib/pkg/<name>.list
 *
 * Commands (when argv is available -- see ARGV note below):
 *   pkg install PKG.tar   extract payload, write DB list
 *   pkg remove  NAME      unlink each file in the DB list, then drop the list
 *   pkg list              print installed package names (read /var/lib/pkg/)
 *   pkg update            stub: prints "no remote configured"
 *   pkg selftest          build a tiny ustar pkg in /tmp/test.tar, install it,
 *                         verify, remove it, verify, print PASS/FAIL
 *
 * --------------------------------------------------------------------------
 * ARGV AVAILABILITY
 * --------------------------------------------------------------------------
 * Confirmed by reading kernel/fs/exec.c + kernel/core/syscall/handlers.c:
 *   * SYS_SPAWN (sys_spawn) copies ONLY the ELF path and calls
 *     elf_load_and_exec(elf_data, elf_size, name) -- no argv is threaded.
 *   * elf_load_and_exec() sets up a bare stack (initial RSP near the top of
 *     a 64KB anon region) and never pushes argc/argv/envp.
 * => Spawned programs receive NO argv. `_start(void)` gets nothing.
 *
 * Therefore _start() runs the SELF-TEST unconditionally. All package-manager
 * logic lives in callable functions (pkg_install / pkg_remove / pkg_list /
 * pkg_update / pkg_selftest) so a future argv-aware launcher (or a host build)
 * can dispatch on argv[1] without touching the logic.
 *
 * Build (flags DIRECTLY on the command line; never via a shell var, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/pkg/pkg.c -o pkg.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       pkg.o -o build/pkg
 *   objdump -d build/pkg | grep fs:0x28   # MUST be empty
 */

/* =========================================================================
 *  Syscall numbers (verified against kernel/include/syscall.h)
 * ========================================================================= */
#define SYS_EXIT      0
#define SYS_READ      2
#define SYS_WRITE     3
#define SYS_OPEN      4
#define SYS_CLOSE     5
#define SYS_OPENDIR   30
#define SYS_READDIR   31
#define SYS_CLOSEDIR  32
#define SYS_STAT      33
#define SYS_UNLINK    34
#define SYS_MKDIR     67

/* O_* flags (kernel/include/vfs.h) */
#define O_RDONLY      0x0000
#define O_WRONLY      0x0001
#define O_CREAT       0x0040
#define O_TRUNC       0x0200

/* dirent layout mirrors kernel `struct dirent` (kernel/include/vfs.h).
 * The kernel copies sizeof(struct dirent) bytes into our buffer. */
#define NAME_MAX_  256
struct k_dirent {
    unsigned long long d_ino;             /* uint64_t */
    long long          d_off;             /* int64_t  */
    unsigned short     d_reclen;          /* uint16_t */
    unsigned char      d_type;            /* uint8_t  */
    char               d_name[NAME_MAX_]; /* NUL-terminated filename */
};

typedef unsigned long  ulong_t;
typedef unsigned char  u8_t;

/* =========================================================================
 *  Inline syscall (3 args is enough for everything we use).
 * ========================================================================= */
static inline long sc3(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* =========================================================================
 *  Freestanding string / number helpers (self-contained).
 * ========================================================================= */
static ulong_t p_strlen(const char *s) { ulong_t n = 0; while (s[n]) n++; return n; }

static int p_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Copy src into dst (cap including NUL). Returns length written (excl NUL). */
static int p_strlcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static void p_memset(void *d, int c, ulong_t n) {
    u8_t *p = (u8_t *)d;
    for (ulong_t i = 0; i < n; i++) p[i] = (u8_t)c;
}

static void p_memcpy(void *d, const void *s, ulong_t n) {
    u8_t *dp = (u8_t *)d; const u8_t *sp = (const u8_t *)s;
    for (ulong_t i = 0; i < n; i++) dp[i] = sp[i];
}

/* "starts with" test. */
static int p_starts(const char *s, const char *pre) {
    while (*pre) { if (*s != *pre) return 0; s++; pre++; }
    return 1;
}

/* =========================================================================
 *  Output to fd 1.
 * ========================================================================= */
static void out(const char *s) { sc3(SYS_WRITE, 1, (long)s, (long)p_strlen(s)); }
static void outn(const char *s, long n) { sc3(SYS_WRITE, 1, (long)s, n); }

/* =========================================================================
 *  ustar octal field parse (matches initrd.c octal_to_int).
 *  Parses up to `len` octal digits; stops at NUL or non-octal char.
 * ========================================================================= */
static ulong_t octal_to_int(const char *str, int len) {
    ulong_t value = 0;
    for (int i = 0; i < len && str[i]; i++) {
        if (str[i] >= '0' && str[i] <= '7')
            value = value * 8 + (ulong_t)(str[i] - '0');
    }
    return value;
}

/* Write `val` as zero-padded octal (no NUL) into buf[0..width-1]. ustar
 * size/mode fields are octal ASCII; the size field is 12 bytes (11 digits +
 * NUL by convention, but we write `width-1` digits then a NUL terminator). */
static void int_to_octal(char *buf, ulong_t val, int width) {
    /* width includes the trailing NUL slot: write width-1 digits then NUL. */
    int digits = width - 1;
    for (int i = digits - 1; i >= 0; i--) {
        buf[i] = (char)('0' + (int)(val & 7));
        val >>= 3;
    }
    buf[digits] = '\0';
}

/* =========================================================================
 *  ustar 512-byte header (field-for-field identical to initrd.c).
 * ========================================================================= */
typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];      /* "ustar" */
    char version[2];    /* "00"    */
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed)) tar_header_t;

/* Validate a ustar header's magic + version (mirrors initrd.c). */
static int tar_valid(const tar_header_t *h) {
    if (h->magic[0] != 'u' || h->magic[1] != 's' || h->magic[2] != 't' ||
        h->magic[3] != 'a' || h->magic[4] != 'r')
        return 0;
    if (h->version[0] != '0' || h->version[1] != '0')
        return 0;
    return 1;
}

static int is_empty_block(const u8_t *p) {
    for (int i = 0; i < 512; i++) if (p[i]) return 0;
    return 1;
}

/* Compute + write the ustar header checksum. The checksum is the unsigned
 * sum of all 512 header bytes with the checksum field treated as spaces,
 * stored as a 6-digit octal string followed by NUL + space. */
static void tar_set_checksum(tar_header_t *h) {
    for (int i = 0; i < 8; i++) h->checksum[i] = ' ';
    ulong_t sum = 0;
    const u8_t *p = (const u8_t *)h;
    for (int i = 0; i < 512; i++) sum += p[i];
    /* 6 octal digits, NUL, space */
    for (int i = 5; i >= 0; i--) { h->checksum[i] = (char)('0' + (int)(sum & 7)); sum >>= 3; }
    h->checksum[6] = '\0';
    h->checksum[7] = ' ';
}

/* =========================================================================
 *  Buffers (capped ~64KB as instructed).
 * ========================================================================= */
#define ARCH_MAX    65536    /* whole package archive */
#define MANI_MAX     4096    /* manifest text */
#define PATH_MAX_   256
#define MAXFILES     32      /* max payload files per package */

static u8_t  g_arch[ARCH_MAX] __attribute__((aligned(16)));
static char  g_mani[MANI_MAX] __attribute__((aligned(16)));

/* Parsed manifest. */
static char  g_name[128];
static char  g_version[64];
static char  g_dests[MAXFILES][PATH_MAX_];   /* from files= list, in order */
static int   g_ndests;

/* =========================================================================
 *  File I/O helpers.
 * ========================================================================= */

/* Path buffers must have >= MAX_PATH_LEN(4096) readable bytes behind them for
 * copy_from_user; our path strings are short so we zero a 4096 scratch and
 * copy into it before any path syscall. */
#define KPATH 4096
static char g_kpath[KPATH] __attribute__((aligned(16)));

static const char *kpath(const char *p) {
    p_memset(g_kpath, 0, KPATH);
    p_strlcpy(g_kpath, p, KPATH);
    return g_kpath;
}

/* Read a whole file into buf (cap bytes). Returns bytes read, or -1. */
static long read_file(const char *path, u8_t *buf, long cap) {
    long fd = sc3(SYS_OPEN, (long)kpath(path), O_RDONLY, 0);
    if (fd < 0) return -1;
    long total = 0;
    for (;;) {
        long room = cap - total;
        if (room <= 0) break;
        long n = sc3(SYS_READ, fd, (long)(buf + total), room);
        if (n <= 0) break;
        total += n;
    }
    sc3(SYS_CLOSE, fd, 0, 0);
    return total;
}

/* Write buf (n bytes) to path, creating/truncating. Returns 0 / -1. */
static int write_file(const char *path, const u8_t *buf, long n) {
    long fd = sc3(SYS_OPEN, (long)kpath(path), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    long off = 0;
    while (off < n) {
        long w = sc3(SYS_WRITE, fd, (long)(buf + off), n - off);
        if (w <= 0) { sc3(SYS_CLOSE, fd, 0, 0); return -1; }
        off += w;
    }
    sc3(SYS_CLOSE, fd, 0, 0);
    return 0;
}

/* Does a file (or dir) exist? Uses SYS_STAT. Returns 1/0. The kernel stat
 * buffer is small; a 256-byte scratch is plenty. */
static char g_statbuf[256] __attribute__((aligned(16)));
static int path_exists(const char *path) {
    long r = sc3(SYS_STAT, (long)kpath(path), (long)g_statbuf, 0);
    return r >= 0;
}

/* Create every parent directory of an absolute file path. SYS_MKDIR is
 * recursive in this kernel, but we mkdir each prefix to be safe & explicit. */
static void mkdir_parents(const char *filepath) {
    char acc[PATH_MAX_];
    int n = 0;
    /* iterate, and at each '/' (after the first char) mkdir the prefix */
    for (int i = 0; filepath[i] && n < PATH_MAX_ - 1; i++) {
        if (filepath[i] == '/' && i > 0) {
            acc[n] = '\0';
            sc3(SYS_MKDIR, (long)kpath(acc), 0755, 0);   /* ignore EEXIST */
        }
        acc[n++] = filepath[i];
    }
    /* trailing component is the file itself -> not created here */
}

/* =========================================================================
 *  Manifest parsing.
 *  Fills g_name, g_version, g_dests[]/g_ndests from MANI text (NUL not
 *  required; we are given the length).
 * ========================================================================= */
static void parse_manifest(const char *txt, long len) {
    g_name[0] = '\0';
    g_version[0] = '\0';
    g_ndests = 0;

    long i = 0;
    while (i < len) {
        /* extract one line [i, eol) */
        long start = i;
        while (i < len && txt[i] != '\n') i++;
        long eol = i;
        if (i < len) i++;   /* skip '\n' */

        /* trim trailing CR */
        long e = eol;
        if (e > start && txt[e - 1] == '\r') e--;

        /* find '=' */
        long eq = start;
        while (eq < e && txt[eq] != '=') eq++;
        if (eq >= e) continue;   /* no '=' -> skip */

        int klen = (int)(eq - start);
        const char *val = txt + eq + 1;
        long vlen = e - (eq + 1);

        if (klen == 4 && p_starts(txt + start, "name") && txt[start + 4] == '=') {
            int n = (int)(vlen < (long)sizeof(g_name) - 1 ? vlen : (long)sizeof(g_name) - 1);
            p_memcpy(g_name, val, n); g_name[n] = '\0';
        } else if (klen == 7 && p_starts(txt + start, "version")) {
            int n = (int)(vlen < (long)sizeof(g_version) - 1 ? vlen : (long)sizeof(g_version) - 1);
            p_memcpy(g_version, val, n); g_version[n] = '\0';
        } else if (klen == 5 && p_starts(txt + start, "files")) {
            /* comma-separated list of absolute dest paths */
            long j = 0;
            while (j < vlen && g_ndests < MAXFILES) {
                long cs = j;
                while (j < vlen && val[j] != ',') j++;
                long clen = j - cs;
                if (j < vlen) j++;   /* skip ',' */
                if (clen <= 0) continue;
                int n = (int)(clen < PATH_MAX_ - 1 ? clen : PATH_MAX_ - 1);
                p_memcpy(g_dests[g_ndests], val + cs, n);
                g_dests[g_ndests][n] = '\0';
                g_ndests++;
            }
        }
    }
}

/* =========================================================================
 *  Install DB:  /var/lib/pkg/<name>.list
 * ========================================================================= */
#define DB_DIR "/var/lib/pkg"

static void db_path_for(char *out, int cap, const char *name) {
    /* "/var/lib/pkg/<name>.list" */
    int n = p_strlcpy(out, DB_DIR "/", cap);
    n += p_strlcpy(out + n, name, cap - n);
    p_strlcpy(out + n, ".list", cap - n);
}

static void db_ensure_dir(void) {
    sc3(SYS_MKDIR, (long)kpath("/var"), 0755, 0);
    sc3(SYS_MKDIR, (long)kpath("/var/lib"), 0755, 0);
    sc3(SYS_MKDIR, (long)kpath(DB_DIR), 0755, 0);
}

/* =========================================================================
 *  Core: install from an in-memory ustar archive (buf,len).
 *  Returns 0 on success, negative on error.
 * ========================================================================= */
static int install_from_buf(const u8_t *buf, long len) {
    /* Pass 1: locate the "manifest" member and parse it. */
    int have_manifest = 0;
    long off = 0;
    int empty = 0;
    while (off + 512 <= len) {
        const tar_header_t *h = (const tar_header_t *)(buf + off);
        if (is_empty_block((const u8_t *)h)) {
            if (++empty >= 2) break;
            off += 512; continue;
        }
        empty = 0;
        if (!tar_valid(h)) break;

        ulong_t fsize = octal_to_int(h->size, sizeof(h->size));
        const char *nm = h->name;
        if (nm[0] == '.' && nm[1] == '/') nm += 2;   /* strip "./" */

        if (p_streq(nm, "manifest") && h->typeflag != '5') {
            long n = (long)fsize;
            if (n > MANI_MAX - 1) n = MANI_MAX - 1;
            if (off + 512 + n <= len) {
                p_memcpy(g_mani, buf + off + 512, n);
                g_mani[n] = '\0';
                parse_manifest(g_mani, n);
                have_manifest = 1;
            }
        }

        ulong_t pad = (512 - (fsize % 512)) % 512;
        off += 512 + fsize + pad;
    }

    if (!have_manifest) { out("pkg: archive has no /manifest\n"); return -1; }
    if (!g_name[0])     { out("pkg: manifest missing name=\n");   return -1; }

    out("pkg: installing "); out(g_name);
    if (g_version[0]) { out(" "); out(g_version); }
    out("\n");

    db_ensure_dir();

    /* Build the DB list incrementally into g_mani-sized scratch. */
    static char db_list[8192];
    int db_len = 0;
    db_list[0] = '\0';

    /* Pass 2: extract each "files/..." member to its dest (by position). */
    int payload_idx = 0;
    int installed = 0;
    off = 0; empty = 0;
    while (off + 512 <= len) {
        const tar_header_t *h = (const tar_header_t *)(buf + off);
        if (is_empty_block((const u8_t *)h)) {
            if (++empty >= 2) break;
            off += 512; continue;
        }
        empty = 0;
        if (!tar_valid(h)) break;

        ulong_t fsize = octal_to_int(h->size, sizeof(h->size));
        const char *nm = h->name;
        if (nm[0] == '.' && nm[1] == '/') nm += 2;   /* strip "./" */

        int is_payload = p_starts(nm, "files/") && h->typeflag != '5';

        if (is_payload) {
            if (payload_idx >= g_ndests) {
                out("pkg: more payload files than manifest files= entries\n");
            } else {
                const char *dest = g_dests[payload_idx];
                const u8_t *data = buf + off + 512;
                long n = (long)fsize;
                if (off + 512 + n > len) {
                    out("pkg: truncated payload for "); out(dest); out("\n");
                } else {
                    mkdir_parents(dest);
                    if (write_file(dest, data, n) == 0) {
                        out("  -> "); out(dest); out("\n");
                        installed++;
                        /* append dest + '\n' to DB list */
                        int dl = (int)p_strlen(dest);
                        if (db_len + dl + 1 < (int)sizeof(db_list)) {
                            p_memcpy(db_list + db_len, dest, dl);
                            db_len += dl;
                            db_list[db_len++] = '\n';
                            db_list[db_len] = '\0';
                        }
                    } else {
                        out("pkg: failed to write "); out(dest); out("\n");
                    }
                }
            }
            payload_idx++;
        }

        ulong_t pad = (512 - (fsize % 512)) % 512;
        off += 512 + fsize + pad;
    }

    /* Write the install DB list file. */
    char dbp[PATH_MAX_];
    db_path_for(dbp, sizeof(dbp), g_name);
    if (write_file(dbp, (const u8_t *)db_list, db_len) != 0) {
        out("pkg: WARNING: could not write DB "); out(dbp); out("\n");
    }

    out("pkg: installed ");
    {
        char nb[16]; int i = 0, v = installed;
        if (v == 0) nb[i++] = '0';
        char tmp[16]; int t = 0;
        while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t > 0) nb[i++] = tmp[--t];
        nb[i] = '\0';
        out(nb);
    }
    out(" file(s)\n");
    return 0;
}

/* =========================================================================
 *  Public command: pkg install PKG.tar
 * ========================================================================= */
static int pkg_install(const char *tarpath) {
    long n = read_file(tarpath, g_arch, ARCH_MAX);
    if (n < 0) { out("pkg: cannot open package "); out(tarpath); out("\n"); return -1; }
    if (n < 512) { out("pkg: package too small\n"); return -1; }
    return install_from_buf(g_arch, n);
}

/* =========================================================================
 *  Public command: pkg remove NAME
 * ========================================================================= */
static int pkg_remove(const char *name) {
    char dbp[PATH_MAX_];
    db_path_for(dbp, sizeof(dbp), name);

    long n = read_file(dbp, g_arch, ARCH_MAX);
    if (n < 0) { out("pkg: package not installed: "); out(name); out("\n"); return -1; }

    out("pkg: removing "); out(name); out("\n");

    /* Each line of the list is one installed file path; unlink it. */
    long i = 0;
    int removed = 0;
    while (i < n) {
        long s = i;
        while (i < n && g_arch[i] != '\n') i++;
        long e = i;
        if (i < n) i++;   /* skip '\n' */
        if (e == s) continue;

        char fpath[PATH_MAX_];
        int len = (int)(e - s);
        if (len > PATH_MAX_ - 1) len = PATH_MAX_ - 1;
        p_memcpy(fpath, g_arch + s, len);
        fpath[len] = '\0';

        long r = sc3(SYS_UNLINK, (long)kpath(fpath), 0, 0);
        if (r >= 0) { out("  x "); out(fpath); out("\n"); removed++; }
        else        { out("  ! could not remove "); out(fpath); out("\n"); }
    }

    /* Drop the DB list itself. */
    sc3(SYS_UNLINK, (long)kpath(dbp), 0, 0);
    out("pkg: removed "); out(name); out("\n");
    (void)removed;
    return 0;
}

/* =========================================================================
 *  Public command: pkg list  (read directory /var/lib/pkg, strip ".list")
 * ========================================================================= */
static int pkg_list(void) {
    long dfd = sc3(SYS_OPENDIR, (long)kpath(DB_DIR), 0, 0);
    if (dfd < 0) { out("pkg: no packages installed\n"); return 0; }

    struct k_dirent de;
    int count = 0;
    out("Installed packages:\n");
    for (;;) {
        long r = sc3(SYS_READDIR, dfd, (long)&de, 0);
        if (r != 0) break;   /* 0 = entry filled; <0 = end/error */
        de.d_name[NAME_MAX_ - 1] = '\0';
        if (de.d_name[0] == '\0') continue;
        if (p_streq(de.d_name, ".") || p_streq(de.d_name, "..")) continue;

        /* show only "*.list" entries, with the suffix stripped */
        int L = (int)p_strlen(de.d_name);
        if (L > 5 && p_streq(de.d_name + L - 5, ".list")) {
            char nm[PATH_MAX_];
            int n = L - 5;
            if (n > PATH_MAX_ - 1) n = PATH_MAX_ - 1;
            p_memcpy(nm, de.d_name, n); nm[n] = '\0';
            out("  "); out(nm); out("\n");
            count++;
        }
    }
    sc3(SYS_CLOSEDIR, dfd, 0, 0);
    if (count == 0) out("  (none)\n");
    return 0;
}

/* =========================================================================
 *  Public command: pkg update  (stub -- no network)
 * ========================================================================= */
static int pkg_update(void) {
    out("no remote configured\n");
    return 0;
}

/* =========================================================================
 *  Build a tiny in-memory ustar package into `dst` (cap bytes).
 *  Members: "manifest" + "files/hello.txt". Returns total bytes, or -1.
 * ========================================================================= */
static long build_test_tar(u8_t *dst, long cap,
                           const char *manifest, long mlen,
                           const char *payload_name, const char *payload,
                           long plen) {
    long off = 0;

    /* helper to emit one member */
    /* member 1: manifest */
    struct memb { const char *name; const char *data; long len; } members[2] = {
        { "manifest", manifest, mlen },
        { payload_name, payload, plen },
    };

    for (int m = 0; m < 2; m++) {
        if (off + 512 > cap) return -1;
        tar_header_t *h = (tar_header_t *)(dst + off);
        p_memset(h, 0, 512);
        p_strlcpy(h->name, members[m].name, sizeof(h->name));
        p_strlcpy(h->mode, "0000644", sizeof(h->mode));
        p_strlcpy(h->uid, "0000000", sizeof(h->uid));
        p_strlcpy(h->gid, "0000000", sizeof(h->gid));
        int_to_octal(h->size, (ulong_t)members[m].len, 12);
        p_strlcpy(h->mtime, "00000000000", sizeof(h->mtime));
        h->typeflag = '0';
        h->magic[0] = 'u'; h->magic[1] = 's'; h->magic[2] = 't';
        h->magic[3] = 'a'; h->magic[4] = 'r'; h->magic[5] = '\0';
        h->version[0] = '0'; h->version[1] = '0';
        tar_set_checksum(h);
        off += 512;

        /* data + zero padding to 512 */
        if (off + members[m].len > cap) return -1;
        p_memcpy(dst + off, members[m].data, members[m].len);
        off += members[m].len;
        long pad = (512 - (members[m].len % 512)) % 512;
        if (off + pad > cap) return -1;
        p_memset(dst + off, 0, pad);
        off += pad;
    }

    /* two trailing zero blocks (end of archive) */
    if (off + 1024 > cap) return -1;
    p_memset(dst + off, 0, 1024);
    off += 1024;
    return off;
}

/* =========================================================================
 *  Self-test.
 *
 *  1. Build a ustar package (manifest + files/hello.txt) into /tmp/test.tar.
 *  2. install it -> /tmp/pkg_selftest_hello.txt should appear.
 *  3. Verify the target file's contents AND that the DB .list was written.
 *  4. remove the package -> the target file should be gone.
 *  Prints "PKG SELFTEST: PASS" or "...: FAIL ...".
 * ========================================================================= */
static const char SELF_DEST[] = "/tmp/pkg_selftest_hello.txt";
static const char SELF_BODY[] = "hello from pkg selftest\n";

static int pkg_selftest(void) {
    out("pkg: running selftest\n");

    /* Manifest: one file, mapping files/hello.txt -> SELF_DEST */
    static char manifest[256];
    int mn = 0;
    mn += p_strlcpy(manifest + mn, "name=pkgtest\n", sizeof(manifest) - mn);
    mn += p_strlcpy(manifest + mn, "version=1.0\n", sizeof(manifest) - mn);
    mn += p_strlcpy(manifest + mn, "files=", sizeof(manifest) - mn);
    mn += p_strlcpy(manifest + mn, SELF_DEST, sizeof(manifest) - mn);
    mn += p_strlcpy(manifest + mn, "\n", sizeof(manifest) - mn);

    long plen = (long)p_strlen(SELF_BODY);

    /* Build into a dedicated buffer, then write /tmp/test.tar. */
    static u8_t tarbuf[8192] __attribute__((aligned(16)));
    long tlen = build_test_tar(tarbuf, sizeof(tarbuf),
                               manifest, mn,
                               "files/hello.txt", SELF_BODY, plen);
    if (tlen < 0) { out("PKG SELFTEST: FAIL (build tar)\n"); return -1; }

    if (write_file("/tmp/test.tar", tarbuf, tlen) != 0) {
        out("PKG SELFTEST: FAIL (write /tmp/test.tar)\n"); return -1;
    }

    /* Clean any stale target before install. */
    sc3(SYS_UNLINK, (long)kpath(SELF_DEST), 0, 0);

    /* Install from the file we just wrote (exercises the real read path). */
    if (pkg_install("/tmp/test.tar") != 0) {
        out("PKG SELFTEST: FAIL (install)\n"); return -1;
    }

    /* Verify the installed file exists with the correct contents. */
    static u8_t verify[256];
    long vn = read_file(SELF_DEST, verify, sizeof(verify));
    if (vn != plen) { out("PKG SELFTEST: FAIL (installed size mismatch)\n"); return -1; }
    for (long i = 0; i < plen; i++) {
        if (verify[i] != (u8_t)SELF_BODY[i]) {
            out("PKG SELFTEST: FAIL (installed contents mismatch)\n"); return -1;
        }
    }

    /* Verify the DB list was written. */
    char dbp[PATH_MAX_];
    db_path_for(dbp, sizeof(dbp), "pkgtest");
    if (!path_exists(dbp)) { out("PKG SELFTEST: FAIL (no DB list)\n"); return -1; }

    /* List (sanity; output is informational). */
    pkg_list();

    /* Remove and verify the installed file is gone. */
    if (pkg_remove("pkgtest") != 0) {
        out("PKG SELFTEST: FAIL (remove)\n"); return -1;
    }
    if (path_exists(SELF_DEST)) {
        out("PKG SELFTEST: FAIL (file still present after remove)\n"); return -1;
    }
    if (path_exists(dbp)) {
        out("PKG SELFTEST: FAIL (DB list still present after remove)\n"); return -1;
    }

    out("PKG SELFTEST: PASS\n");
    return 0;
}

/* =========================================================================
 *  argv-aware dispatcher (kept for a future launcher / host build).
 *  Not reachable from _start under this kernel (no argv -- see header note).
 * ========================================================================= */
int pkg_main(int argc, char **argv) {
    if (argc < 2) {
        out("usage: pkg <install PKG.tar | remove NAME | list | update | selftest>\n");
        return 1;
    }
    const char *cmd = argv[1];
    if (p_streq(cmd, "install")) {
        if (argc < 3) { out("pkg install: missing PKG.tar\n"); return 1; }
        return pkg_install(argv[2]) == 0 ? 0 : 1;
    } else if (p_streq(cmd, "remove")) {
        if (argc < 3) { out("pkg remove: missing NAME\n"); return 1; }
        return pkg_remove(argv[2]) == 0 ? 0 : 1;
    } else if (p_streq(cmd, "list")) {
        return pkg_list();
    } else if (p_streq(cmd, "update")) {
        return pkg_update();
    } else if (p_streq(cmd, "selftest")) {
        return pkg_selftest() == 0 ? 0 : 1;
    }
    out("pkg: unknown command\n");
    return 1;
}

/* =========================================================================
 *  Entry point.
 *
 *  crt0 (userspace/crt0.asm) provides _start, parses the stack into
 *  argc/argv, and calls main(argc, argv); the return value becomes the
 *  process exit code. argv layout: argv[0]=pkg, argv[1]=subcommand,
 *  argv[2]=arg -- exactly what pkg_main() expects.
 *
 *  With arguments (argc > 1) we dispatch via pkg_main so that
 *  `pkg install X.tar`, `pkg remove NAME`, `pkg list`, etc. work. With no
 *  arguments (argc <= 1) we run the SELF-TEST, which still prints
 *  "PKG SELFTEST: PASS/FAIL" (smoke-gated).
 * ========================================================================= */
int main(int argc, char **argv) {
    if (argc > 1) {
        return pkg_main(argc, argv);
    }
    out("[PKG] no argv -> running selftest\n");
    pkg_selftest();
    return 0;
}

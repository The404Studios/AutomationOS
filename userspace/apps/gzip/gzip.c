/*
 * gzip.c -- gzip / gunzip for the from-scratch x86_64 OS.
 * =======================================================
 *
 * FREESTANDING userspace ELF (no libc): pure inline syscalls + our own
 * helpers, mirroring userspace/apps/sed/sed.c and userspace/apps/pkg/pkg.c.
 * crt0.asm provides _start, parses the kernel stack into argc/argv and calls
 * main(argc, argv); main's return value becomes the process exit code. A
 * built-in self-test runs when argc <= 1 so the program stays verifiable.
 *
 * The DEFLATE/INFLATE core lives in userspace/lib/deflate/deflate.c, which is
 * compiled separately and linked alongside gzip.o. CRC-32 is provided there
 * too (gzip and DEFLATE both need it). gzip.c only adds the RFC 1952 gzip
 * container framing + file I/O.
 *
 * MODES (decided by argv[0] basename OR a -d flag):
 *   gzip   FILE       -> compress FILE  -> FILE.gz   (original removed? no --
 *                        we keep it; the OS has no `--keep` convention yet)
 *   gunzip FILE.gz    -> decompress FILE.gz -> FILE  (strips ".gz")
 *   gzip -d FILE.gz   -> same as gunzip
 *   gzip -c FILE      -> write compressed output to stdout instead of a file
 *   gunzip -c FILE.gz -> write decompressed output to stdout
 *
 * gzip container (RFC 1952), little-endian:
 *   byte 0:  0x1f                magic 1
 *   byte 1:  0x8b                magic 2
 *   byte 2:  0x08                CM = 8 (DEFLATE)
 *   byte 3:  0x00                FLG = 0 (no extra fields/name/comment)
 *   bytes 4-7: MTIME = 0         (modification time; 0 = not set)
 *   byte 8:  0x00                XFL = 0
 *   byte 9:  0xff                OS  = 255 (unknown)
 *   ...      DEFLATE stream
 *   last 8:  CRC32 (LE) then ISIZE (LE) = uncompressed size mod 2^32
 *
 * Build (flags DIRECTLY on the command line -- never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/gzip/gzip.c -o gzip.o
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/deflate/deflate.c -o deflate.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       gzip.o deflate.o -o build/gzip
 *   objdump -d build/gzip | grep fs:0x28   # MUST be empty
 *
 * INTEGRATOR NOTE: gzip.c must be linked with deflate.o. The build installs a
 * single binary; both `gzip` and `gunzip` behaviour are selected at runtime
 * via argv[0]'s basename or the -d flag, so one binary covers both names
 * (symlink/alias `gunzip` -> `gzip` if the FS supports it).
 */

#include "../../lib/deflate/deflate.h"

/* =========================================================================
 *  Syscall numbers (verified against kernel/include/syscall.h via sed.c/pkg.c)
 * ========================================================================= */
#define SYS_EXIT    0
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_UNLINK  34

/* open() flags (kernel/include/vfs.h). */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_CREAT     0x0040
#define O_TRUNC     0x0200

/* sys_open copies MAX_PATH_LEN (4096) bytes from the path pointer. */
#define KPATH_MAX   4096

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long      size_t;

/* =========================================================================
 *  Inline syscall (3 args is enough for everything we use).
 * ========================================================================= */
static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* =========================================================================
 *  Freestanding string / memory helpers.
 * ========================================================================= */
static size_t g_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int g_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static size_t g_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static void g_memset(void *d, int c, size_t n)
{
    u8 *p = (u8 *)d;
    for (size_t i = 0; i < n; i++) p[i] = (u8)c;
}

/* Does `s` end with `suf`? */
static int g_endswith(const char *s, const char *suf)
{
    size_t ls = g_strlen(s), lf = g_strlen(suf);
    if (lf > ls) return 0;
    for (size_t i = 0; i < lf; i++)
        if (s[ls - lf + i] != suf[i]) return 0;
    return 1;
}

/* Last path component of `p` (the basename). */
static const char *basename_of(const char *p)
{
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

/* =========================================================================
 *  Output to fd 1.
 * ========================================================================= */
static void out(const char *s)  { sc(SYS_WRITE, 1, (long)s, (long)g_strlen(s)); }
static void out_n(const char *s, long n) { sc(SYS_WRITE, 1, (long)s, n); }

/* =========================================================================
 *  Buffers. The kernel stack is tiny, so all large buffers are static .bss.
 * ========================================================================= */
#define IN_MAX   (256 * 1024)   /* max uncompressed payload we handle  */
#define OUT_MAX  (320 * 1024)   /* compressed can briefly exceed input (stored) */

static u8 g_in[IN_MAX]   __attribute__((aligned(16)));
static u8 g_out[OUT_MAX] __attribute__((aligned(16)));

static char g_path[KPATH_MAX] __attribute__((aligned(16)));   /* safe path buffer */

/* =========================================================================
 *  File I/O (mirrors sed.c slurp/spill).
 * ========================================================================= */

/* Read whole file into buf (cap bytes). Returns bytes read, -1 open fail,
 * -2 if larger than cap. Path routed through g_path for copy_from_user. */
static long read_all(const char *path, u8 *buf, long cap)
{
    g_memset(g_path, 0, KPATH_MAX);
    g_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_RDONLY, 0);
    if (fd < 0) return -1;

    long total = 0;
    for (;;) {
        long room = cap - total;
        if (room <= 0) {
            u8 extra;
            long n = sc(SYS_READ, fd, (long)&extra, 1);
            sc(SYS_CLOSE, fd, 0, 0);
            return (n > 0) ? -2 : total;
        }
        long n = sc(SYS_READ, fd, (long)(buf + total), room);
        if (n <= 0) break;
        total += n;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return total;
}

/* Write buf[0..n) to path (truncate/create). Returns 0 / -1. */
static int write_all(const char *path, const u8 *buf, long n)
{
    g_memset(g_path, 0, KPATH_MAX);
    g_strlcpy(g_path, path, KPATH_MAX);
    long fd = sc(SYS_OPEN, (long)g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    long off = 0;
    while (off < n) {
        long w = sc(SYS_WRITE, fd, (long)(buf + off), n - off);
        if (w <= 0) { sc(SYS_CLOSE, fd, 0, 0); return -1; }
        off += w;
    }
    sc(SYS_CLOSE, fd, 0, 0);
    return 0;
}

/* =========================================================================
 *  gzip container helpers.
 * ========================================================================= */
#define GZ_MAGIC1   0x1f
#define GZ_MAGIC2   0x8b
#define GZ_METHOD   0x08   /* DEFLATE */

static void put_le32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

static u32 get_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/*
 * Wrap a DEFLATE stream in a gzip container. `payload`/`plen` is the raw
 * uncompressed data (needed for CRC + ISIZE); `def`/`dlen` is the DEFLATE
 * stream. Writes the framed gzip into out[] (cap bytes). Returns total bytes,
 * or -1 if it doesn't fit.
 */
static long gz_wrap(const u8 *payload, long plen,
                    const u8 *def, long dlen,
                    u8 *out, long cap)
{
    long need = 10 + dlen + 8;
    if (need > cap) return -1;

    long op = 0;
    out[op++] = GZ_MAGIC1;
    out[op++] = GZ_MAGIC2;
    out[op++] = GZ_METHOD;
    out[op++] = 0x00;          /* FLG = 0 */
    put_le32(out + op, 0); op += 4;   /* MTIME = 0 */
    out[op++] = 0x00;          /* XFL = 0 */
    out[op++] = 0xFF;          /* OS  = 255 (unknown) */

    for (long i = 0; i < dlen; i++) out[op++] = def[i];

    u32 crc   = deflate_crc32(payload, plen);
    u32 isize = (u32)((unsigned long)plen & 0xFFFFFFFFu);
    put_le32(out + op, crc);   op += 4;
    put_le32(out + op, isize); op += 4;
    return op;
}

/*
 * Locate the start of the DEFLATE stream inside a gzip file and return its
 * offset, or -1 on a malformed header. Parses the FLG-driven optional fields
 * (FEXTRA / FNAME / FCOMMENT / FHCRC).
 */
static long gz_payload_offset(const u8 *buf, long len)
{
    if (len < 18) return -1;             /* 10 header + min 8 trailer */
    if (buf[0] != GZ_MAGIC1 || buf[1] != GZ_MAGIC2) return -1;
    if (buf[2] != GZ_METHOD) return -1;
    u8 flg = buf[3];
    long off = 10;

    if (flg & 0x04) {                    /* FEXTRA */
        if (off + 2 > len) return -1;
        long xlen = (long)buf[off] | ((long)buf[off + 1] << 8);
        off += 2 + xlen;
    }
    if (flg & 0x08) {                    /* FNAME (NUL-terminated) */
        while (off < len && buf[off] != 0) off++;
        off++;
    }
    if (flg & 0x10) {                    /* FCOMMENT (NUL-terminated) */
        while (off < len && buf[off] != 0) off++;
        off++;
    }
    if (flg & 0x02) off += 2;            /* FHCRC */

    if (off >= len - 8) return -1;       /* nothing left before the trailer */
    return off;
}

/* =========================================================================
 *  Compress: read FILE, DEFLATE, wrap in gzip, write to dst (or stdout).
 * ========================================================================= */
static int do_compress(const char *infile, const char *outfile, int to_stdout)
{
    long n = read_all(infile, g_in, IN_MAX);
    if (n == -1) { out("gzip: cannot open '"); out(infile); out("'\n"); return 1; }
    if (n == -2) { out("gzip: input too large (>256KB)\n"); return 1; }

    /* Deflate into a temporary region at the front of g_out, then wrap it.
     * We deflate into a dedicated static buffer to keep payload + framing
     * separate from the deflate output. */
    static u8 g_def[OUT_MAX] __attribute__((aligned(16)));
    long dlen = deflate_compress(g_in, n, g_def, sizeof(g_def));
    if (dlen < 0) { out("gzip: compression buffer overflow\n"); return 1; }

    long total = gz_wrap(g_in, n, g_def, dlen, g_out, OUT_MAX);
    if (total < 0) { out("gzip: output buffer overflow\n"); return 1; }

    if (to_stdout) { out_n((const char *)g_out, total); return 0; }

    /* dst path = infile + ".gz" unless an explicit outfile was supplied. */
    char dst[KPATH_MAX];
    if (outfile) {
        g_strlcpy(dst, outfile, sizeof(dst));
    } else {
        size_t l = g_strlcpy(dst, infile, sizeof(dst));
        g_strlcpy(dst + l, ".gz", sizeof(dst) - l);
    }

    if (write_all(dst, g_out, total) != 0) {
        out("gzip: cannot write '"); out(dst); out("'\n"); return 1;
    }
    out("gzip: "); out(infile); out(" -> "); out(dst); out("\n");
    return 0;
}

/* =========================================================================
 *  Decompress: read FILE.gz, verify header, INFLATE, check CRC+ISIZE, write.
 * ========================================================================= */
static int do_decompress(const char *infile, const char *outfile, int to_stdout)
{
    long n = read_all(infile, g_in, IN_MAX);
    if (n == -1) { out("gunzip: cannot open '"); out(infile); out("'\n"); return 1; }
    if (n == -2) { out("gunzip: input too large (>256KB)\n"); return 1; }
    if (n < 18)  { out("gunzip: not in gzip format (too short)\n"); return 1; }

    long doff = gz_payload_offset(g_in, n);
    if (doff < 0) { out("gunzip: '"); out(infile); out("' is not a valid gzip file\n"); return 1; }

    /* DEFLATE stream runs from doff to n-8 (the last 8 bytes are CRC+ISIZE). */
    long dlen = (n - 8) - doff;
    long got = inflate_decompress(g_in + doff, dlen, g_out, OUT_MAX);
    if (got == -1) { out("gunzip: output too large (>320KB)\n"); return 1; }
    if (got < 0)   { out("gunzip: corrupt DEFLATE stream\n"); return 1; }

    /* Verify CRC-32 and ISIZE from the trailer. */
    u32 want_crc   = get_le32(g_in + (n - 8));
    u32 want_isize = get_le32(g_in + (n - 4));
    u32 have_crc   = deflate_crc32(g_out, got);
    if (have_crc != want_crc) {
        out("gunzip: CRC32 mismatch (corrupt data)\n"); return 1;
    }
    if (want_isize != (u32)((unsigned long)got & 0xFFFFFFFFu)) {
        out("gunzip: ISIZE mismatch (corrupt data)\n"); return 1;
    }

    if (to_stdout) { out_n((const char *)g_out, got); return 0; }

    /* dst path = strip a trailing ".gz" from infile, else append ".out". */
    char dst[KPATH_MAX];
    if (outfile) {
        g_strlcpy(dst, outfile, sizeof(dst));
    } else if (g_endswith(infile, ".gz")) {
        size_t l = g_strlen(infile) - 3;
        for (size_t i = 0; i < l && i < sizeof(dst) - 1; i++) dst[i] = infile[i];
        dst[l < sizeof(dst) - 1 ? l : sizeof(dst) - 1] = '\0';
    } else {
        size_t l = g_strlcpy(dst, infile, sizeof(dst));
        g_strlcpy(dst + l, ".out", sizeof(dst) - l);
    }

    if (write_all(dst, g_out, got) != 0) {
        out("gunzip: cannot write '"); out(dst); out("'\n"); return 1;
    }
    out("gunzip: "); out(infile); out(" -> "); out(dst); out("\n");
    return 0;
}

/* =========================================================================
 *  argv-aware dispatcher.
 *
 *  Mode selection:
 *    1. If argv[0]'s basename starts with "gunzip" -> decompress mode.
 *    2. A `-d` flag forces decompress; `-c` writes to stdout.
 *    3. Otherwise compress.
 * ========================================================================= */
static int gzip_main(int argc, char **argv)
{
    const char *prog = (argc > 0 && argv[0]) ? basename_of(argv[0]) : "gzip";

    int decompress = 0;
    int to_stdout  = 0;

    /* default mode from program name */
    if (prog[0] == 'g' && prog[1] == 'u') decompress = 1;   /* "gunzip" */

    int ai = 1;
    while (ai < argc && argv[ai] && argv[ai][0] == '-' && argv[ai][1]) {
        if (g_streq(argv[ai], "-d"))      decompress = 1;
        else if (g_streq(argv[ai], "-c")) to_stdout = 1;
        else if (g_streq(argv[ai], "-z")) decompress = 0;
        else { out("gzip: unknown option: "); out(argv[ai]); out("\n"); return 1; }
        ai++;
    }

    if (ai >= argc || !argv[ai]) {
        out("usage: gzip [-d] [-c] FILE   (gunzip FILE.gz decompresses)\n");
        return 1;
    }
    const char *infile  = argv[ai++];
    const char *outfile = (ai < argc && argv[ai]) ? argv[ai] : 0;

    if (decompress) return do_decompress(infile, outfile, to_stdout);
    return do_compress(infile, outfile, to_stdout);
}

/* =========================================================================
 *  SELF-TEST (argc <= 1).
 *
 *  In-memory round trip:
 *    1. Compress a known byte string with deflate_compress().
 *    2. Wrap it as gzip, then unwrap + inflate it.
 *    3. Verify the inflated bytes match the original byte-for-byte.
 *    4. Verify CRC-32 of the original matches the gzip trailer's CRC.
 *  Prints "GZIP SELFTEST: PASS" or "GZIP SELFTEST: FAIL <reason>".
 * ========================================================================= */

static int bytes_eq(const u8 *a, long alen, const u8 *b, long blen)
{
    if (alen != blen) return 0;
    for (long i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int selftest(void)
{
    out("GZIP SELFTEST: begin\n");

    /* A sample with literals AND repeats so LZ77 + Huffman actually engage,
     * plus enough variety that the round trip is meaningful. */
    static const u8 sample[] =
        "The quick brown fox jumps over the lazy dog. "
        "The quick brown fox jumps over the lazy dog. "
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA "
        "abcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabc "
        "gzip deflate inflate huffman lz77 crc32 round-trip test 1234567890";
    long slen = (long)g_strlen((const char *)sample);

    /* 1) compress (raw DEFLATE). */
    static u8 def[OUT_MAX] __attribute__((aligned(16)));
    long dlen = deflate_compress(sample, slen, def, sizeof(def));
    if (dlen < 0) { out("GZIP SELFTEST: FAIL <deflate overflow>\n"); return 1; }

    /* 2) wrap as gzip, then parse it back. */
    static u8 gz[OUT_MAX] __attribute__((aligned(16)));
    long gzlen = gz_wrap(sample, slen, def, dlen, gz, sizeof(gz));
    if (gzlen < 0) { out("GZIP SELFTEST: FAIL <gz wrap overflow>\n"); return 1; }

    long doff = gz_payload_offset(gz, gzlen);
    if (doff < 0) { out("GZIP SELFTEST: FAIL <bad gz header>\n"); return 1; }

    /* 3) inflate the embedded DEFLATE stream and compare. */
    static u8 res[OUT_MAX] __attribute__((aligned(16)));
    long rlen = inflate_decompress(gz + doff, (gzlen - 8) - doff, res, sizeof(res));
    if (rlen < 0) {
        out("GZIP SELFTEST: FAIL <inflate err ");
        char d[4]; d[0] = '-'; d[1] = (char)('0' - rlen); d[2] = '>'; d[3] = '\n';
        out_n(d, 4);
        return 1;
    }
    if (!bytes_eq(sample, slen, res, rlen)) {
        out("GZIP SELFTEST: FAIL <round-trip mismatch>\n");
        return 1;
    }

    /* 4) verify CRC-32 stored in the trailer matches a fresh CRC. */
    u32 trailer_crc = get_le32(gz + (gzlen - 8));
    u32 fresh_crc   = deflate_crc32(sample, slen);
    if (trailer_crc != fresh_crc) {
        out("GZIP SELFTEST: FAIL <crc mismatch>\n");
        return 1;
    }
    u32 trailer_isize = get_le32(gz + (gzlen - 4));
    if (trailer_isize != (u32)slen) {
        out("GZIP SELFTEST: FAIL <isize mismatch>\n");
        return 1;
    }

    /* 5) bonus: a stored-fallback / incompressible-ish edge case (single byte
     *    and empty input) must still round-trip. */
    {
        static const u8 one[] = "Z";
        static u8 d1[64], r1[64];
        long dl = deflate_compress(one, 1, d1, sizeof(d1));
        long rl = (dl < 0) ? -1 : inflate_decompress(d1, dl, r1, sizeof(r1));
        if (rl != 1 || r1[0] != 'Z') { out("GZIP SELFTEST: FAIL <1-byte>\n"); return 1; }

        static u8 d0[64], r0[64];
        long dl0 = deflate_compress((const u8 *)"", 0, d0, sizeof(d0));
        long rl0 = (dl0 < 0) ? -1 : inflate_decompress(d0, dl0, r0, sizeof(r0));
        if (rl0 != 0) { out("GZIP SELFTEST: FAIL <empty>\n"); return 1; }
    }

    /* Report the achieved size so the integrator can eyeball the ratio. */
    out("GZIP SELFTEST: ");
    {
        /* tiny decimal printer */
        char nb[24]; int i = 0; long v = slen;
        char tmp[24]; int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t > 0) nb[i++] = tmp[--t];
        nb[i] = '\0';
        out(nb); out(" bytes -> ");
    }
    {
        char nb[24]; int i = 0; long v = gzlen;
        char tmp[24]; int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t > 0) nb[i++] = tmp[--t];
        nb[i] = '\0';
        out(nb); out(" gz bytes\n");
    }

    out("GZIP SELFTEST: PASS\n");
    return 0;
}

/* =========================================================================
 *  Entry point. crt0.asm provides _start, parses argc/argv off the stack and
 *  calls main(argc, argv); the return becomes the process exit code.
 * ========================================================================= */
int main(int argc, char **argv)
{
    if (argc > 1) {
        return gzip_main(argc, argv);
    }
    (void)selftest();
    return 0;
}

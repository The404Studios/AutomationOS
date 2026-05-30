/*
 * sh_git.c -- native, git-like version control for the terminal shell.
 *
 * Self-contained freestanding implementation: no libc, no malloc, no stdio.
 * Talks to the kernel only through raw syscalls; provides its own string
 * helpers, SHA-1, and static buffers.
 *
 * Repository layout (under <cwd>/.git):
 *   HEAD              -> "ref: refs/heads/master\n"
 *   refs/heads/master -> "<commit40hex>\n" (absent until first commit)
 *   index             -> plain text, lines: "<id> <relpath>\n"
 *   objects/<id>      -> flat object store (raw blob content / tree body /
 *                        commit body), <id> is the 40-hex SHA-1 of the object.
 *
 * Subcommands: init, add, status, commit -m, log, (usage on unknown/empty).
 *
 * Public entry point declared in sh_git.h:
 *   int git_run(const char* argline, const char* cwd, void (*out)(const char*));
 */

#include "sh_git.h"

/* ------------------------------------------------------------------ */
/* Syscall wrapper (provided verbatim by the kernel ABI spec).        */
/* ------------------------------------------------------------------ */
static inline long sc(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
    asm volatile("syscall" : "=a"(r)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return r;
}

/* Syscall numbers (real kernel values). */
#define SYS_READ     2
#define SYS_WRITE    3
#define SYS_OPEN     4
#define SYS_CLOSE    5
#define SYS_OPENDIR  30
#define SYS_READDIR  31
#define SYS_CLOSEDIR 32
#define SYS_STAT     33
#define SYS_UNLINK   34
#define SYS_MKDIR    67

/* Directory entry layout: mirrors `struct dirent` in kernel/include/vfs.h.
 * sys_readdir() copies sizeof(struct dirent) bytes into our buffer and returns
 * 0 when an entry was filled, <0 at end-of-directory or on error (matches the
 * terminal app cmd_ls() convention and kernel sys_readdir()). */
#define DT_DIR     4
#define DT_REG     8
#define NAME_MAX_  256
struct k_dirent {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[NAME_MAX_];
};

/* open() flags. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

/* ------------------------------------------------------------------ */
/* Buffer limits (each documented inline).                            */
/* ------------------------------------------------------------------ */
#define MAX_CONTENT 65536      /* 64 KB: largest blob/working-file content    */
#define MAX_INDEX   16384      /* 16 KB: largest index file we read/write     */
#define MAX_OBJBODY 98304      /* 96 KB: tree/commit body assembly buffer     */
#define MAX_PATH    4096       /* path handed to syscalls (kernel copies 4096)*/
#define OID_LEN     41         /* 40 hex chars + NUL                          */
#define MAX_MSG     4096       /* commit message buffer                       */

/* ------------------------------------------------------------------ */
/* Minimal string helpers.                                            */
/* ------------------------------------------------------------------ */
static unsigned long g_slen(const char* s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static int g_streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Copy at most cap-1 bytes of src into dst, NUL-terminate. Returns length copied. */
static unsigned long g_scopy(char* dst, unsigned long cap, const char* src) {
    unsigned long i = 0;
    if (cap == 0) return 0;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return i;
}

/* Append src to dst (which has total capacity cap incl. NUL). Returns new length. */
static unsigned long g_scat(char* dst, unsigned long cap, const char* src) {
    unsigned long len = g_slen(dst);
    unsigned long i = 0;
    while (src[i] && len + 1 < cap) {
        dst[len++] = src[i++];
    }
    dst[len] = 0;
    return len;
}

/* Raw memory copy. */
static void g_memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* dp = (unsigned char*)d;
    const unsigned char* sp = (const unsigned char*)s;
    for (unsigned long i = 0; i < n; i++) dp[i] = sp[i];
}

/* Render unsigned long as decimal into buf; returns number of chars written. */
static unsigned long g_utoa(unsigned long v, char* buf) {
    char tmp[24];
    unsigned long n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (unsigned long i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* Output helpers.                                                    */
/* ------------------------------------------------------------------ */
static void (*g_out)(const char*) = 0;
static void emit(const char* s) { if (g_out) g_out(s); }

/* ------------------------------------------------------------------ */
/* SHA-1 (correct, self-contained).                                   */
/* Verified against SHA-1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d. */
/* ------------------------------------------------------------------ */
typedef unsigned int u32;

static u32 sha1_rotl(u32 x, int c) {
    return (x << c) | (x >> (32 - c));
}

/* Compute SHA-1 of data[0..len) into out20 (20 raw bytes). */
static void sha1(const unsigned char* data, unsigned long len, unsigned char out20[20]) {
    u32 h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu,
        h3 = 0x10325476u, h4 = 0xC3D2E1F0u;

    /* Total length in bits (fits in 64; our buffers are well under 2^32 bytes). */
    unsigned long long bitlen = (unsigned long long)len * 8ULL;

    /* Process full 64-byte blocks first, then a synthesized tail block(s)
     * built from the remaining bytes + padding + length, without allocating
     * a copy of the whole message. */
    unsigned long full = len / 64;          /* number of complete 64B blocks  */
    unsigned long rem = len - full * 64;    /* leftover bytes 0..63            */

    /* Tail: leftover bytes, 0x80, zero pad, 8-byte big-endian length.
     * Worst case the tail spans two 64B blocks (when rem >= 56). */
    unsigned char tail[128];
    unsigned long tlen = 0;
    for (unsigned long i = 0; i < rem; i++) tail[tlen++] = data[full * 64 + i];
    tail[tlen++] = 0x80;
    /* Pad with zeros until length-mod-64 == 56. */
    while ((tlen % 64) != 56) tail[tlen++] = 0x00;
    /* Append 64-bit big-endian bit length. */
    for (int i = 7; i >= 0; i--) tail[tlen++] = (unsigned char)((bitlen >> (i * 8)) & 0xFF);

    unsigned long total_blocks = full + (tlen / 64);
    u32 w[80];

    for (unsigned long b = 0; b < total_blocks; b++) {
        const unsigned char* blk;
        if (b < full) blk = data + b * 64;
        else          blk = tail + (b - full) * 64;

        for (int t = 0; t < 16; t++) {
            w[t] = ((u32)blk[t * 4] << 24) | ((u32)blk[t * 4 + 1] << 16) |
                   ((u32)blk[t * 4 + 2] << 8) | ((u32)blk[t * 4 + 3]);
        }
        for (int t = 16; t < 80; t++) {
            w[t] = sha1_rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
        }

        u32 a = h0, bb = h1, c = h2, d = h3, e = h4;
        for (int t = 0; t < 80; t++) {
            u32 f, k;
            if (t < 20)      { f = (bb & c) | ((~bb) & d);          k = 0x5A827999u; }
            else if (t < 40) { f = bb ^ c ^ d;                      k = 0x6ED9EBA1u; }
            else if (t < 60) { f = (bb & c) | (bb & d) | (c & d);   k = 0x8F1BBCDCu; }
            else             { f = bb ^ c ^ d;                      k = 0xCA62C1D6u; }
            u32 tmp = sha1_rotl(a, 5) + f + e + k + w[t];
            e = d; d = c; c = sha1_rotl(bb, 30); bb = a; a = tmp;
        }
        h0 += a; h1 += bb; h2 += c; h3 += d; h4 += e;
    }

    u32 hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++) {
        out20[i * 4]     = (unsigned char)((hs[i] >> 24) & 0xFF);
        out20[i * 4 + 1] = (unsigned char)((hs[i] >> 16) & 0xFF);
        out20[i * 4 + 2] = (unsigned char)((hs[i] >> 8) & 0xFF);
        out20[i * 4 + 3] = (unsigned char)(hs[i] & 0xFF);
    }
}

static const char HEX[] = "0123456789abcdef";

/* Render 20 raw SHA-1 bytes as 40 lowercase hex chars + NUL into out (>=41). */
static void sha1_hex(const unsigned char in20[20], char out[OID_LEN]) {
    for (int i = 0; i < 20; i++) {
        out[i * 2]     = HEX[(in20[i] >> 4) & 0xF];
        out[i * 2 + 1] = HEX[in20[i] & 0xF];
    }
    out[40] = 0;
}

/* ------------------------------------------------------------------ */
/* Path handling.                                                     */
/* ------------------------------------------------------------------ */
static const char* g_cwd = 0;   /* set per call */

/* Join cwd + name into out (cap MAX_PATH). Absolute names pass through.
 * Avoids a double slash when cwd == "/". Returns 1 on success, 0 if truncated. */
static int join_path(char* out, const char* name) {
    out[0] = 0;
    if (name[0] == '/') {
        g_scopy(out, MAX_PATH, name);
        return g_slen(out) == g_slen(name);
    }
    unsigned long need;
    g_scopy(out, MAX_PATH, g_cwd);
    /* ensure exactly one slash between cwd and name */
    if (!(g_cwd[0] == '/' && g_cwd[1] == 0)) {
        g_scat(out, MAX_PATH, "/");
    } else if (out[0] == 0) {
        g_scat(out, MAX_PATH, "/");
    }
    g_scat(out, MAX_PATH, name);
    need = g_slen(g_cwd) + 1 + g_slen(name);
    (void)need;
    return 1;
}

/* Build a path under .git: "<cwd>/.git/<sub>" into out. */
static void gitpath(char* out, const char* sub) {
    out[0] = 0;
    if (g_cwd[0] == '/' && g_cwd[1] == 0) {
        g_scopy(out, MAX_PATH, "/.git");
    } else {
        g_scopy(out, MAX_PATH, g_cwd);
        g_scat(out, MAX_PATH, "/.git");
    }
    if (sub && sub[0]) {
        g_scat(out, MAX_PATH, "/");
        g_scat(out, MAX_PATH, sub);
    }
}

/* ------------------------------------------------------------------ */
/* Low-level file I/O.                                                */
/* ------------------------------------------------------------------ */

/* Read entire file at path into buf (cap bytes). Returns count >=0, or
 * -1 if open failed, or -2 if file larger than cap (overflow guard). */
static long read_file(const char* path, char* buf, unsigned long cap) {
    static char p[MAX_PATH] __attribute__((aligned(16)));
    g_scopy(p, MAX_PATH, path);
    int fd = (int)sc(SYS_OPEN, (long)p, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return -1;
    unsigned long total = 0;
    for (;;) {
        if (total >= cap) {
            /* would overflow: drain one extra byte to detect oversize */
            char overflow;
            long extra = sc(SYS_READ, fd, (long)&overflow, 1, 0, 0, 0);
            sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
            if (extra > 0) return -2;
            return (long)total;
        }
        long n = sc(SYS_READ, fd, (long)(buf + total), (long)(cap - total), 0, 0, 0);
        if (n < 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0); return -1; }
        if (n == 0) break;
        total += (unsigned long)n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return (long)total;
}

/* Write len bytes from buf to path (create/truncate). Returns 0 ok, -1 error. */
static int write_file(const char* path, const char* buf, unsigned long len) {
    static char p[MAX_PATH] __attribute__((aligned(16)));
    g_scopy(p, MAX_PATH, path);
    int fd = (int)sc(SYS_OPEN, (long)p, O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0, 0);
    if (fd < 0) return -1;
    unsigned long total = 0;
    while (total < len) {
        long n = sc(SYS_WRITE, fd, (long)(buf + total), (long)(len - total), 0, 0, 0);
        if (n <= 0) { sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0); return -1; }
        total += (unsigned long)n;
    }
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return 0;
}

/* Does a path exist? (open RDONLY succeeds). */
static int path_exists(const char* path) {
    static char p[MAX_PATH] __attribute__((aligned(16)));
    g_scopy(p, MAX_PATH, path);
    int fd = (int)sc(SYS_OPEN, (long)p, O_RDONLY, 0, 0, 0, 0);
    if (fd < 0) return 0;
    sc(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return 1;
}

/* Is there a repo at <cwd>/.git ? Check for HEAD (a regular file). */
static int repo_exists(void) {
    static char p[MAX_PATH] __attribute__((aligned(16)));
    gitpath(p, "HEAD");
    return path_exists(p);
}

/* ------------------------------------------------------------------ */
/* Object store.                                                      */
/* ------------------------------------------------------------------ */

/* Compute the git-style blob id for content[0..len): SHA-1 of
 * "blob <len>\0<content>", as 40 hex into oid. Uses a static framing buffer. */
static void blob_oid(const char* content, unsigned long len, char oid[OID_LEN]) {
    /* header "blob " + up to 20 digits + NUL fits easily in 32 bytes;
     * total framed buffer holds header + content. */
    static unsigned char framed[32 + MAX_CONTENT] __attribute__((aligned(16)));
    unsigned long h = 0;
    const char* pre = "blob ";
    for (unsigned long i = 0; pre[i]; i++) framed[h++] = (unsigned char)pre[i];
    char num[24];
    unsigned long nd = g_utoa(len, num);
    for (unsigned long i = 0; i < nd; i++) framed[h++] = (unsigned char)num[i];
    framed[h++] = 0x00;
    g_memcpy(framed + h, content, len);
    h += len;
    unsigned char dig[20];
    sha1(framed, h, dig);
    sha1_hex(dig, oid);
}

/* Store an object: write raw bytes to <cwd>/.git/objects/<oid>. 0 ok, -1 err. */
static int store_object(const char* oid, const char* data, unsigned long len) {
    static char p[MAX_PATH] __attribute__((aligned(16)));
    static char sub[64];
    g_scopy(sub, sizeof(sub), "objects/");
    g_scat(sub, sizeof(sub), oid);
    gitpath(p, sub);
    return write_file(p, data, len);
}

/* Read an object body into buf (cap). Returns count, -1 missing, -2 oversize. */
static long read_object(const char* oid, char* buf, unsigned long cap) {
    static char p[MAX_PATH] __attribute__((aligned(16)));
    static char sub[64];
    g_scopy(sub, sizeof(sub), "objects/");
    g_scat(sub, sizeof(sub), oid);
    gitpath(p, sub);
    return read_file(p, buf, cap);
}

/* ------------------------------------------------------------------ */
/* Index handling. The index is plain text: "<40hex> <relpath>\n".     */
/* ------------------------------------------------------------------ */
static char g_index[MAX_INDEX] __attribute__((aligned(16)));   /* current index text */
static unsigned long g_index_len = 0;

/* Load index into g_index; absent index => empty. Returns 0 ok, -2 oversize. */
static int index_load(void) {
    static char p[MAX_PATH] __attribute__((aligned(16)));
    gitpath(p, "index");
    long n = read_file(p, g_index, MAX_INDEX - 1);
    if (n == -1) { g_index_len = 0; g_index[0] = 0; return 0; }  /* no index yet */
    if (n == -2) { g_index_len = 0; g_index[0] = 0; return -2; }
    g_index_len = (unsigned long)n;
    g_index[g_index_len] = 0;
    return 0;
}

/* Persist g_index. Returns 0 ok, -1 err. */
static int index_save(void) {
    static char p[MAX_PATH] __attribute__((aligned(16)));
    gitpath(p, "index");
    return write_file(p, g_index, g_index_len);
}

/* Remove any existing index line whose relpath == path. Rewrites g_index. */
static void index_remove_path(const char* path) {
    static char rebuilt[MAX_INDEX];
    unsigned long ri = 0;
    unsigned long i = 0;
    unsigned long plen = g_slen(path);
    while (i < g_index_len) {
        unsigned long ls = i;                  /* line start */
        while (i < g_index_len && g_index[i] != '\n') i++;
        unsigned long le = i;                  /* line end (at '\n' or EOF) */
        if (i < g_index_len) i++;              /* skip newline */
        /* line layout: 40 hex, space, relpath */
        int keep = 1;
        if (le - ls > 41) {
            const char* lp = g_index + ls + 41; /* relpath start */
            unsigned long llen = le - (ls + 41);
            if (llen == plen) {
                int same = 1;
                for (unsigned long k = 0; k < plen; k++) {
                    if (lp[k] != path[k]) { same = 0; break; }
                }
                if (same) keep = 0;
            }
        }
        if (keep) {
            for (unsigned long k = ls; k < le; k++) rebuilt[ri++] = g_index[k];
            rebuilt[ri++] = '\n';
        }
    }
    g_memcpy(g_index, rebuilt, ri);
    g_index_len = ri;
    g_index[ri] = 0;
}

/* Append one "<oid> <path>\n" line to g_index. Returns 0 ok, -1 full. */
static int index_append(const char* oid, const char* path) {
    unsigned long need = 40 + 1 + g_slen(path) + 1;
    if (g_index_len + need + 1 >= MAX_INDEX) return -1;
    for (int i = 0; i < 40; i++) g_index[g_index_len++] = oid[i];
    g_index[g_index_len++] = ' ';
    for (unsigned long i = 0; path[i]; i++) g_index[g_index_len++] = path[i];
    g_index[g_index_len++] = '\n';
    g_index[g_index_len] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Argument tokenizer.                                                */
/* ------------------------------------------------------------------ */

/* Skip leading spaces/tabs. */
static const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Copy the next whitespace-delimited token from *pp into tok (cap), advance *pp
 * past it. Returns token length (0 means none left). */
static unsigned long next_token(const char** pp, char* tok, unsigned long cap) {
    const char* s = skip_ws(*pp);
    unsigned long n = 0;        /* total token chars seen (may exceed cap) */
    unsigned long w = 0;        /* chars actually written to tok           */
    while (*s && *s != ' ' && *s != '\t') {
        if (w + 1 < cap) tok[w++] = *s;
        n++;
        s++;
    }
    if (cap > 0) tok[w] = 0;
    *pp = s;
    return n;
}

/* ------------------------------------------------------------------ */
/* Subcommands.                                                       */
/* ------------------------------------------------------------------ */

/* Forward declarations for ref/HEAD helpers defined further below but used by
 * earlier subcommands (e.g. cmd_status reports the current branch). */
static int read_head_branch(char* out, unsigned long cap);
static int read_branch_commit(const char* branch, char oid[OID_LEN]);
static int read_head_commit(char oid[OID_LEN]);

static int cmd_init(void) {
    static char p[MAX_PATH] __attribute__((aligned(16)));
    int existed = repo_exists();

    /* Recursive mkdir handles parents, but make each explicitly for clarity. */
    gitpath(p, 0);              sc(SYS_MKDIR, (long)p, 0755, 0, 0, 0, 0);
    gitpath(p, "objects");      sc(SYS_MKDIR, (long)p, 0755, 0, 0, 0, 0);
    gitpath(p, "refs");         sc(SYS_MKDIR, (long)p, 0755, 0, 0, 0, 0);
    gitpath(p, "refs/heads");   sc(SYS_MKDIR, (long)p, 0755, 0, 0, 0, 0);

    gitpath(p, "HEAD");
    if (write_file(p, "ref: refs/heads/master\n", 23) != 0) {
        emit("fatal: could not write .git/HEAD\n");
        return 1;
    }

    /* Compose the message: "<verb> Git repository in <cwd>/.git". */
    static char msg[MAX_PATH + 64];
    static char gp[MAX_PATH];
    gitpath(gp, 0);
    msg[0] = 0;
    if (existed) g_scopy(msg, sizeof(msg), "Reinitialized existing Git repository in ");
    else         g_scopy(msg, sizeof(msg), "Initialized empty Git repository in ");
    g_scat(msg, sizeof(msg), gp);
    g_scat(msg, sizeof(msg), "\n");
    emit(msg);
    return 0;
}

/* Stage one relative path `name`: read the working file, hash & store its blob,
 * replace any prior index entry, append the new one, and print "add '<name>'".
 * Returns 0 on success, non-zero on error (a message is emitted). g_index must
 * already be loaded; the caller saves it afterwards. */
static int stage_one(const char* name) {
    static char content[MAX_CONTENT] __attribute__((aligned(16)));
    static char fpath[MAX_PATH] __attribute__((aligned(16)));
    static char oid[OID_LEN];
    static char line[MAX_PATH + 32];

    join_path(fpath, name);
    long n = read_file(fpath, content, MAX_CONTENT);
    if (n == -1) {
        line[0] = 0;
        g_scopy(line, sizeof(line), "fatal: pathspec '");
        g_scat(line, sizeof(line), name);
        g_scat(line, sizeof(line), "' did not match any files\n");
        emit(line);
        return 1;
    }
    if (n == -2) {
        line[0] = 0;
        g_scopy(line, sizeof(line), "error: file too large: ");
        g_scat(line, sizeof(line), name);
        g_scat(line, sizeof(line), "\n");
        emit(line);
        return 1;
    }

    blob_oid(content, (unsigned long)n, oid);
    if (store_object(oid, content, (unsigned long)n) != 0) {
        emit("error: could not write object\n");
        return 1;
    }

    /* index: replace existing entry for this path, then append */
    index_remove_path(name);
    if (index_append(oid, name) != 0) {
        emit("error: index full\n");
        return 1;
    }

    line[0] = 0;
    g_scopy(line, sizeof(line), "add '");
    g_scat(line, sizeof(line), name);
    g_scat(line, sizeof(line), "'\n");
    emit(line);
    return 0;
}

/* Stage every regular file directly under cwd (used by `git add .` / `-A`).
 * Skips ".", "..", the ".git" directory, and any non-regular entry. Returns 0
 * on success (even if the directory was empty), non-zero on error. */
static int stage_all(void) {
    static char dpath[MAX_PATH] __attribute__((aligned(16)));
    g_scopy(dpath, MAX_PATH, g_cwd);

    int dfd = (int)sc(SYS_OPENDIR, (long)dpath, 0, 0, 0, 0, 0);
    if (dfd < 0) {
        emit("error: could not open working directory\n");
        return 1;
    }

    static struct k_dirent de;
    static char nm[NAME_MAX_];
    int rc = 0;
    int guard = 0;
    for (;;) {
        if (++guard > 100000) break;            /* runaway safety */
        long r = sc(SYS_READDIR, dfd, (long)&de, 0, 0, 0, 0);
        if (r != 0) break;                      /* 0 = entry filled; <0 = end */
        de.d_name[NAME_MAX_ - 1] = 0;
        if (de.d_name[0] == 0) continue;
        if (g_streq(de.d_name, ".") || g_streq(de.d_name, "..")) continue;
        if (g_streq(de.d_name, ".git")) continue;
        if (de.d_type == DT_DIR) continue;      /* directories not recursed */
        if (de.d_type != DT_REG) continue;      /* only regular files */
        g_scopy(nm, sizeof(nm), de.d_name);
        if (stage_one(nm) != 0) rc = 1;
    }
    sc(SYS_CLOSEDIR, dfd, 0, 0, 0, 0, 0);
    return rc;
}

static int cmd_add(const char* rest) {
    if (!repo_exists()) {
        emit("fatal: not a git repository (or any of the parent directories): .git\n");
        return 1;
    }
    if (index_load() == -2) {
        emit("fatal: index too large\n");
        return 1;
    }

    static char tok[MAX_PATH];

    const char* p = rest;
    int any = 0;
    int rc = 0;

    for (;;) {
        unsigned long tn = next_token(&p, tok, sizeof(tok));
        if (tn == 0) break;
        any = 1;

        /* `git add .` / `git add -A`: stage all regular files under cwd. */
        if (g_streq(tok, ".") || g_streq(tok, "-A")) {
            if (stage_all() != 0) rc = 1;
            continue;
        }

        if (stage_one(tok) != 0) rc = 1;
    }

    if (!any) {
        emit("Nothing specified, nothing added.\n");
        return 1;
    }
    if (index_save() != 0) {
        emit("error: could not write index\n");
        return 1;
    }
    return rc;
}

static int cmd_status(void) {
    if (!repo_exists()) {
        emit("fatal: not a git repository\n");
        return 1;
    }
    if (index_load() == -2) {
        emit("fatal: index too large\n");
        return 1;
    }

    {
        static char branch[64];
        static char bl[96];
        read_head_branch(branch, sizeof(branch));
        bl[0] = 0;
        g_scopy(bl, sizeof(bl), "On branch ");
        g_scat(bl, sizeof(bl), branch);
        g_scat(bl, sizeof(bl), "\n");
        emit(bl);
    }

    if (g_index_len == 0) {
        emit("nothing to commit, working tree clean\n");
        return 0;
    }

    emit("Changes to be committed:\n");

    static char content[MAX_CONTENT] __attribute__((aligned(16)));
    static char fpath[MAX_PATH] __attribute__((aligned(16)));
    static char relp[MAX_PATH];
    static char curoid[OID_LEN];
    static char idxoid[OID_LEN];
    static char line[MAX_PATH + 64];

    unsigned long i = 0;
    while (i < g_index_len) {
        unsigned long ls = i;
        while (i < g_index_len && g_index[i] != '\n') i++;
        unsigned long le = i;
        if (i < g_index_len) i++;
        if (le - ls <= 41) continue;            /* malformed line, skip */

        /* parse oid (40) + space + relpath */
        for (int k = 0; k < 40; k++) idxoid[k] = g_index[ls + k];
        idxoid[40] = 0;
        unsigned long rl = le - (ls + 41);
        if (rl >= MAX_PATH) rl = MAX_PATH - 1;
        for (unsigned long k = 0; k < rl; k++) relp[k] = g_index[ls + 41 + k];
        relp[rl] = 0;

        join_path(fpath, relp);
        long n = read_file(fpath, content, MAX_CONTENT);

        const char* state;
        if (n == -1) {
            state = "deleted:    ";
        } else if (n == -2) {
            state = "modified:   ";   /* oversize now; differs from staged */
        } else {
            blob_oid(content, (unsigned long)n, curoid);
            if (g_streq(curoid, idxoid)) state = "new file:   ";
            else                         state = "modified:   ";
        }

        line[0] = 0;
        g_scopy(line, sizeof(line), "        ");
        g_scat(line, sizeof(line), state);
        g_scat(line, sizeof(line), relp);
        g_scat(line, sizeof(line), "\n");
        emit(line);
    }
    return 0;
}

/* Read the current branch name from .git/HEAD into out (cap). HEAD holds
 * "ref: refs/heads/<branch>\n". On any malformation, defaults to "master".
 * Returns 1 if a "ref:" line was parsed, 0 if it fell back to the default. */
static int read_head_branch(char* out, unsigned long cap) {
    static char p[MAX_PATH] __attribute__((aligned(16)));
    static char buf[128];
    g_scopy(out, cap, "master");
    gitpath(p, "HEAD");
    long n = read_file(p, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = 0;
    const char* pre = "ref: refs/heads/";
    unsigned long pl = g_slen(pre);
    for (unsigned long k = 0; k < pl; k++) {
        if (buf[k] != pre[k]) return 0;        /* not a symbolic ref -> default */
    }
    unsigned long w = 0;
    for (unsigned long k = pl; buf[k] && w + 1 < cap; k++) {
        char ch = buf[k];
        if (ch == '\n' || ch == '\r' || ch == ' ') break;
        out[w++] = ch;
    }
    out[w] = 0;
    if (w == 0) { g_scopy(out, cap, "master"); return 0; }
    return 1;
}

/* Read the commit id stored in ref file "refs/heads/<branch>" into oid.
 * Returns 1 if a 40-hex commit exists, 0 if none/absent. */
static int read_branch_commit(const char* branch, char oid[OID_LEN]) {
    static char p[MAX_PATH] __attribute__((aligned(16)));
    static char sub[64];
    static char buf[128];
    g_scopy(sub, sizeof(sub), "refs/heads/");
    g_scat(sub, sizeof(sub), branch);
    gitpath(p, sub);
    long n = read_file(p, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    int c = 0;
    for (long i = 0; i < n && c < 40; i++) {
        char ch = buf[i];
        if (ch == '\n' || ch == '\r' || ch == ' ' || ch == 0) break;
        oid[c++] = ch;
    }
    oid[c] = 0;
    return c == 40;
}

/* Read the current commit id (resolving HEAD's branch) into oid (40 hex + NUL).
 * Returns 1 if a commit exists, 0 if none. */
static int read_head_commit(char oid[OID_LEN]) {
    static char branch[64];
    read_head_branch(branch, sizeof(branch));
    return read_branch_commit(branch, oid);
}

static int cmd_commit(const char* rest) {
    if (!repo_exists()) {
        emit("fatal: not a git repository\n");
        return 1;
    }

    /* Parse: expect "-m <message...>". Message is everything after "-m ". */
    const char* p = skip_ws(rest);
    static char tok[16];
    static char msg[MAX_MSG];
    msg[0] = 0;

    next_token(&p, tok, sizeof(tok));
    if (!g_streq(tok, "-m")) {
        emit("usage: git commit -m <message>\n");
        return 1;
    }
    p = skip_ws(p);
    if (*p == 0) {
        emit("error: empty commit message\n");
        return 1;
    }
    /* strip optional surrounding quotes */
    if (*p == '"' || *p == '\'') {
        char q = *p; p++;
        unsigned long mi = 0;
        while (*p && *p != q && mi + 1 < MAX_MSG) msg[mi++] = *p++;
        msg[mi] = 0;
    } else {
        g_scopy(msg, MAX_MSG, p);
        /* trim trailing newline/space */
        unsigned long ml = g_slen(msg);
        while (ml > 0 && (msg[ml - 1] == '\n' || msg[ml - 1] == ' ' ||
                          msg[ml - 1] == '\t' || msg[ml - 1] == '\r')) {
            msg[--ml] = 0;
        }
    }

    if (index_load() == -2) {
        emit("fatal: index too large\n");
        return 1;
    }
    if (g_index_len == 0) {
        emit("nothing to commit\n");
        return 1;
    }

    /* Tree object = the index body verbatim ("<id> <relpath>\n" lines). */
    static char tree[MAX_OBJBODY] __attribute__((aligned(16)));
    if (g_index_len >= MAX_OBJBODY) {
        emit("error: tree too large\n");
        return 1;
    }
    g_memcpy(tree, g_index, g_index_len);
    unsigned long treelen = g_index_len;

    unsigned char dig[20];
    static char treeid[OID_LEN];
    sha1((const unsigned char*)tree, treelen, dig);
    sha1_hex(dig, treeid);
    if (store_object(treeid, tree, treelen) != 0) {
        emit("error: could not write tree object\n");
        return 1;
    }

    /* Parent (if any). */
    static char parent[OID_LEN];
    int have_parent = read_head_commit(parent);

    /* Build commit body. */
    static char body[MAX_OBJBODY] __attribute__((aligned(16)));
    body[0] = 0;
    g_scat(body, MAX_OBJBODY, "tree ");
    g_scat(body, MAX_OBJBODY, treeid);
    g_scat(body, MAX_OBJBODY, "\n");
    if (have_parent) {
        g_scat(body, MAX_OBJBODY, "parent ");
        g_scat(body, MAX_OBJBODY, parent);
        g_scat(body, MAX_OBJBODY, "\n");
    }
    g_scat(body, MAX_OBJBODY, "author user <0> +0000\n");
    g_scat(body, MAX_OBJBODY, "committer user <0> +0000\n");
    g_scat(body, MAX_OBJBODY, "\n");
    g_scat(body, MAX_OBJBODY, msg);
    g_scat(body, MAX_OBJBODY, "\n");
    unsigned long bodylen = g_slen(body);

    static char commitid[OID_LEN];
    sha1((const unsigned char*)body, bodylen, dig);
    sha1_hex(dig, commitid);
    if (store_object(commitid, body, bodylen) != 0) {
        emit("error: could not write commit object\n");
        return 1;
    }

    /* Update refs/heads/<current-branch>. */
    static char branch[64];
    read_head_branch(branch, sizeof(branch));
    static char refbuf[OID_LEN + 2];
    g_scopy(refbuf, sizeof(refbuf), commitid);
    g_scat(refbuf, sizeof(refbuf), "\n");
    static char rsub[80];
    g_scopy(rsub, sizeof(rsub), "refs/heads/");
    g_scat(rsub, sizeof(rsub), branch);
    static char rp[MAX_PATH] __attribute__((aligned(16)));
    gitpath(rp, rsub);
    if (write_file(rp, refbuf, g_slen(refbuf)) != 0) {
        emit("error: could not update ref\n");
        return 1;
    }

    /* Print "[<branch> <first7>] <msg>". */
    static char line[MAX_MSG + 64];
    line[0] = 0;
    g_scat(line, sizeof(line), "[");
    g_scat(line, sizeof(line), branch);
    g_scat(line, sizeof(line), " ");
    for (int i = 0; i < 7; i++) {
        char one[2]; one[0] = commitid[i]; one[1] = 0;
        g_scat(line, sizeof(line), one);
    }
    g_scat(line, sizeof(line), "] ");
    g_scat(line, sizeof(line), msg);
    g_scat(line, sizeof(line), "\n");
    emit(line);
    return 0;
}

static int cmd_log(void) {
    if (!repo_exists()) {
        emit("fatal: not a git repository\n");
        return 1;
    }

    static char oid[OID_LEN];
    if (!read_head_commit(oid)) {
        emit("fatal: your current branch 'master' does not have any commits yet\n");
        return 1;
    }

    static char body[MAX_OBJBODY] __attribute__((aligned(16)));
    static char line[OID_LEN + 16];
    static char nextparent[OID_LEN];
    static char msgbuf[MAX_MSG];

    int guard = 0;
    for (;;) {
        if (++guard > 100000) break;   /* cycle safety */

        long n = read_object(oid, body, MAX_OBJBODY - 1);
        if (n < 0) {
            emit("error: missing commit object\n");
            return 1;
        }
        body[n] = 0;

        /* parse lines: capture parent, find blank line -> message follows */
        int have_parent = 0;
        nextparent[0] = 0;
        unsigned long i = 0;
        unsigned long blank_at = (unsigned long)n;  /* offset just after blank line */
        unsigned long bn = (unsigned long)n;
        while (i < bn) {
            unsigned long ls = i;
            while (i < bn && body[i] != '\n') i++;
            unsigned long le = i;
            if (i < bn) i++;
            if (le == ls) { blank_at = i; break; }   /* empty line => header end */

            /* check for "parent " prefix */
            const char* pp = "parent ";
            int isp = 1;
            for (int k = 0; k < 7; k++) {
                if (ls + (unsigned long)k >= le || body[ls + k] != pp[k]) { isp = 0; break; }
            }
            if (isp) {
                int c = 0;
                for (unsigned long k = ls + 7; k < le && c < 40; k++) nextparent[c++] = body[k];
                nextparent[c] = 0;
                if (c == 40) have_parent = 1;
            }
        }

        /* message = everything from blank_at to end (trim trailing newline) */
        unsigned long mi = 0;
        for (unsigned long k = blank_at; k < bn && mi + 1 < MAX_MSG; k++) msgbuf[mi++] = body[k];
        while (mi > 0 && (msgbuf[mi - 1] == '\n' || msgbuf[mi - 1] == '\r')) mi--;
        msgbuf[mi] = 0;

        /* print */
        line[0] = 0;
        g_scopy(line, sizeof(line), "commit ");
        g_scat(line, sizeof(line), oid);
        g_scat(line, sizeof(line), "\n");
        emit(line);
        emit("    ");
        emit(msgbuf);
        emit("\n\n");

        if (!have_parent) break;
        g_scopy(oid, OID_LEN, nextparent);
    }
    return 0;
}

/* Look up the staged blob id for relpath in g_index. On hit, copies the 40-hex
 * id into oid and returns 1; returns 0 if the path is not staged. g_index must
 * already be loaded. */
static int index_find(const char* relpath, char oid[OID_LEN]) {
    unsigned long plen = g_slen(relpath);
    unsigned long i = 0;
    while (i < g_index_len) {
        unsigned long ls = i;
        while (i < g_index_len && g_index[i] != '\n') i++;
        unsigned long le = i;
        if (i < g_index_len) i++;
        if (le - ls <= 41) continue;            /* malformed line */
        const char* lp = g_index + ls + 41;     /* relpath start */
        unsigned long llen = le - (ls + 41);
        if (llen != plen) continue;
        int same = 1;
        for (unsigned long k = 0; k < plen; k++) {
            if (lp[k] != relpath[k]) { same = 0; break; }
        }
        if (same) {
            for (int k = 0; k < 40; k++) oid[k] = g_index[ls + k];
            oid[40] = 0;
            return 1;
        }
    }
    return 0;
}

/* git diff: for every index entry, compare staged blob id to the working file's
 * recomputed blob id. On mismatch (or missing working file) print a header and a
 * naive line-by-line diff: '-' for staged lines, '+' for working lines. */
static int cmd_diff(void) {
    if (!repo_exists()) {
        emit("fatal: not a git repository\n");
        return 1;
    }
    if (index_load() == -2) {
        emit("fatal: index too large\n");
        return 1;
    }

    static char fpath[MAX_PATH] __attribute__((aligned(16)));
    static char relp[MAX_PATH];
    static char idxoid[OID_LEN];
    static char curoid[OID_LEN];
    static char work[MAX_CONTENT] __attribute__((aligned(16)));   /* working file  */
    static char staged[MAX_CONTENT] __attribute__((aligned(16))); /* staged blob   */
    static char hdr[MAX_PATH * 2 + 32];

    int printed = 0;
    unsigned long i = 0;
    while (i < g_index_len) {
        unsigned long ls = i;
        while (i < g_index_len && g_index[i] != '\n') i++;
        unsigned long le = i;
        if (i < g_index_len) i++;
        if (le - ls <= 41) continue;            /* malformed line, skip */

        for (int k = 0; k < 40; k++) idxoid[k] = g_index[ls + k];
        idxoid[40] = 0;
        unsigned long rl = le - (ls + 41);
        if (rl >= MAX_PATH) rl = MAX_PATH - 1;
        for (unsigned long k = 0; k < rl; k++) relp[k] = g_index[ls + 41 + k];
        relp[rl] = 0;

        join_path(fpath, relp);
        long wn = read_file(fpath, work, MAX_CONTENT);

        int differs = 0;
        if (wn < 0) {
            differs = 1;                        /* missing/oversize working file */
        } else {
            blob_oid(work, (unsigned long)wn, curoid);
            if (!g_streq(curoid, idxoid)) differs = 1;
        }
        if (!differs) continue;

        /* header */
        hdr[0] = 0;
        g_scopy(hdr, sizeof(hdr), "diff --git a/");
        g_scat(hdr, sizeof(hdr), relp);
        g_scat(hdr, sizeof(hdr), " b/");
        g_scat(hdr, sizeof(hdr), relp);
        g_scat(hdr, sizeof(hdr), "\n");
        emit(hdr);
        printed = 1;

        /* Load the staged blob body (raw content of the blob object). */
        long sn = read_object(idxoid, staged, MAX_CONTENT);
        if (sn < 0) sn = 0;                     /* treat missing object as empty */

        /* Naive line-by-line compare. Walk both buffers a line at a time; on a
         * differing pair, print the staged line with '-' and the working line
         * with '+'. Identical lines are skipped. */
        unsigned long si = 0, wi = 0;
        unsigned long sb = (unsigned long)sn;
        unsigned long wb = (wn > 0) ? (unsigned long)wn : 0;
        static char oline[512];
        for (;;) {
            if (si >= sb && wi >= wb) break;
            /* extract staged line [ss, se) */
            unsigned long ss = si, se = si;
            while (se < sb && staged[se] != '\n') se++;
            unsigned long snext = (se < sb) ? se + 1 : se;
            /* extract working line [ws, we) */
            unsigned long ws = wi, we = wi;
            while (we < wb && work[we] != '\n') we++;
            unsigned long wnext = (we < wb) ? we + 1 : we;

            int shave = (si < sb);
            int whave = (wi < wb);

            /* identical line? */
            int eq = 0;
            if (shave && whave) {
                unsigned long slen = se - ss, wlen = we - ws;
                if (slen == wlen) {
                    eq = 1;
                    for (unsigned long k = 0; k < slen; k++) {
                        if (staged[ss + k] != work[ws + k]) { eq = 0; break; }
                    }
                }
            }

            if (eq) {
                si = snext; wi = wnext;
                continue;
            }
            if (shave) {
                oline[0] = '-';
                unsigned long o = 1, k = ss;
                while (k < se && o + 2 < sizeof(oline)) oline[o++] = staged[k++];
                oline[o++] = '\n';
                oline[o] = 0;
                emit(oline);
                si = snext;
            }
            if (whave) {
                oline[0] = '+';
                unsigned long o = 1, k = ws;
                while (k < we && o + 2 < sizeof(oline)) oline[o++] = work[k++];
                oline[o++] = '\n';
                oline[o] = 0;
                emit(oline);
                wi = wnext;
            }
        }
    }

    if (!printed) emit("# no changes\n");
    return 0;
}

/* git branch [name]: list branches (no arg) or create a new branch (with name).*/
static int cmd_branch(const char* rest) {
    if (!repo_exists()) {
        emit("fatal: not a git repository\n");
        return 1;
    }

    static char tok[64];
    const char* p = rest;
    unsigned long tn = next_token(&p, tok, sizeof(tok));

    static char cur[64];
    read_head_branch(cur, sizeof(cur));

    if (tn == 0) {
        /* List refs/heads. */
        static char dpath[MAX_PATH] __attribute__((aligned(16)));
        gitpath(dpath, "refs/heads");
        int dfd = (int)sc(SYS_OPENDIR, (long)dpath, 0, 0, 0, 0, 0);
        if (dfd < 0) {
            emit("# no branches yet\n");
            return 0;
        }
        static struct k_dirent de;
        static char line[NAME_MAX_ + 8];
        int guard = 0;
        for (;;) {
            if (++guard > 100000) break;
            long r = sc(SYS_READDIR, dfd, (long)&de, 0, 0, 0, 0);
            if (r != 0) break;
            de.d_name[NAME_MAX_ - 1] = 0;
            if (de.d_name[0] == 0) continue;
            if (g_streq(de.d_name, ".") || g_streq(de.d_name, "..")) continue;
            if (de.d_type == DT_DIR) continue;  /* only ref files */
            line[0] = 0;
            if (g_streq(de.d_name, cur)) g_scopy(line, sizeof(line), "* ");
            else                         g_scopy(line, sizeof(line), "  ");
            g_scat(line, sizeof(line), de.d_name);
            g_scat(line, sizeof(line), "\n");
            emit(line);
        }
        sc(SYS_CLOSEDIR, dfd, 0, 0, 0, 0, 0);
        return 0;
    }

    /* Create a new branch pointing at the current branch's tip commit. */
    static char oid[OID_LEN];
    if (!read_branch_commit(cur, oid)) {
        emit("fatal: not a valid object name: current branch has no commits\n");
        return 1;
    }
    static char sub[80];
    g_scopy(sub, sizeof(sub), "refs/heads/");
    g_scat(sub, sizeof(sub), tok);
    static char rp[MAX_PATH] __attribute__((aligned(16)));
    gitpath(rp, sub);
    static char refbuf[OID_LEN + 2];
    g_scopy(refbuf, sizeof(refbuf), oid);
    g_scat(refbuf, sizeof(refbuf), "\n");
    if (write_file(rp, refbuf, g_slen(refbuf)) != 0) {
        emit("error: could not create branch\n");
        return 1;
    }
    return 0;
}

/* git checkout <name> | git checkout -- <file>. */
static int cmd_checkout(const char* rest) {
    if (!repo_exists()) {
        emit("fatal: not a git repository\n");
        return 1;
    }

    static char tok[MAX_PATH];
    const char* p = rest;
    unsigned long tn = next_token(&p, tok, sizeof(tok));
    if (tn == 0) {
        emit("usage: git checkout <branch> | git checkout -- <file>\n");
        return 1;
    }

    /* Restore-file form: "checkout -- <file>". */
    if (g_streq(tok, "--")) {
        static char fname[MAX_PATH];
        unsigned long fn = next_token(&p, fname, sizeof(fname));
        if (fn == 0) {
            emit("usage: git checkout -- <file>\n");
            return 1;
        }
        if (index_load() == -2) {
            emit("fatal: index too large\n");
            return 1;
        }
        static char oid[OID_LEN];
        if (!index_find(fname, oid)) {
            static char e[MAX_PATH + 48];
            e[0] = 0;
            g_scopy(e, sizeof(e), "error: pathspec '");
            g_scat(e, sizeof(e), fname);
            g_scat(e, sizeof(e), "' is not staged\n");
            emit(e);
            return 1;
        }
        static char blob[MAX_CONTENT] __attribute__((aligned(16)));
        long bn = read_object(oid, blob, MAX_CONTENT);
        if (bn < 0) {
            emit("error: missing blob object\n");
            return 1;
        }
        static char fpath[MAX_PATH] __attribute__((aligned(16)));
        join_path(fpath, fname);
        if (write_file(fpath, blob, (unsigned long)bn) != 0) {
            emit("error: could not write file\n");
            return 1;
        }
        static char line[MAX_PATH + 32];
        line[0] = 0;
        g_scopy(line, sizeof(line), "Restored '");
        g_scat(line, sizeof(line), fname);
        g_scat(line, sizeof(line), "'\n");
        emit(line);
        return 0;
    }

    /* Branch-switch form: <name> must already exist. */
    static char sub[80];
    g_scopy(sub, sizeof(sub), "refs/heads/");
    g_scat(sub, sizeof(sub), tok);
    static char rp[MAX_PATH] __attribute__((aligned(16)));
    gitpath(rp, sub);
    if (!path_exists(rp)) {
        static char e[MAX_PATH + 48];
        e[0] = 0;
        g_scopy(e, sizeof(e), "error: pathspec '");
        g_scat(e, sizeof(e), tok);
        g_scat(e, sizeof(e), "' did not match any branch\n");
        emit(e);
        return 1;
    }
    /* Point HEAD at the branch. */
    static char headbuf[96];
    headbuf[0] = 0;
    g_scopy(headbuf, sizeof(headbuf), "ref: refs/heads/");
    g_scat(headbuf, sizeof(headbuf), tok);
    g_scat(headbuf, sizeof(headbuf), "\n");
    static char hp[MAX_PATH] __attribute__((aligned(16)));
    gitpath(hp, "HEAD");
    if (write_file(hp, headbuf, g_slen(headbuf)) != 0) {
        emit("error: could not update HEAD\n");
        return 1;
    }
    static char line[96];
    line[0] = 0;
    g_scopy(line, sizeof(line), "Switched to branch '");
    g_scat(line, sizeof(line), tok);
    g_scat(line, sizeof(line), "'\n");
    emit(line);
    return 0;
}

/* git show [commit]: print "commit <id>" then the object body. Defaults to the
 * current HEAD commit. With an arg, shows whatever object <id> names. */
static int cmd_show(const char* rest) {
    if (!repo_exists()) {
        emit("fatal: not a git repository\n");
        return 1;
    }

    static char oid[OID_LEN];
    static char tok[OID_LEN + 8];
    const char* p = rest;
    unsigned long tn = next_token(&p, tok, sizeof(tok));
    if (tn == 0) {
        if (!read_head_commit(oid)) {
            emit("fatal: your current branch does not have any commits yet\n");
            return 1;
        }
    } else {
        /* take up to 40 chars of the given id */
        int c = 0;
        for (unsigned long k = 0; tok[k] && c < 40; k++) oid[c++] = tok[k];
        oid[c] = 0;
    }

    static char body[MAX_OBJBODY] __attribute__((aligned(16)));
    long n = read_object(oid, body, MAX_OBJBODY - 1);
    if (n < 0) {
        emit("fatal: bad object\n");
        return 1;
    }
    body[n] = 0;

    static char hdr[OID_LEN + 16];
    hdr[0] = 0;
    g_scopy(hdr, sizeof(hdr), "commit ");
    g_scat(hdr, sizeof(hdr), oid);
    g_scat(hdr, sizeof(hdr), "\n");
    emit(hdr);
    emit(body);
    if (n > 0 && body[n - 1] != '\n') emit("\n");
    return 0;
}

/* git log --oneline: walk commits like cmd_log but print "<7hex> <first line>".*/
static int cmd_log_oneline(void) {
    static char oid[OID_LEN];
    if (!read_head_commit(oid)) {
        emit("fatal: your current branch does not have any commits yet\n");
        return 1;
    }

    static char body[MAX_OBJBODY] __attribute__((aligned(16)));
    static char nextparent[OID_LEN];
    static char line[OID_LEN + MAX_MSG + 8];

    int guard = 0;
    for (;;) {
        if (++guard > 100000) break;

        long n = read_object(oid, body, MAX_OBJBODY - 1);
        if (n < 0) {
            emit("error: missing commit object\n");
            return 1;
        }
        body[n] = 0;
        unsigned long bn = (unsigned long)n;

        int have_parent = 0;
        nextparent[0] = 0;
        unsigned long i = 0;
        unsigned long blank_at = bn;
        while (i < bn) {
            unsigned long ls = i;
            while (i < bn && body[i] != '\n') i++;
            unsigned long le = i;
            if (i < bn) i++;
            if (le == ls) { blank_at = i; break; }
            const char* pp = "parent ";
            int isp = 1;
            for (int k = 0; k < 7; k++) {
                if (ls + (unsigned long)k >= le || body[ls + k] != pp[k]) { isp = 0; break; }
            }
            if (isp) {
                int c = 0;
                for (unsigned long k = ls + 7; k < le && c < 40; k++) nextparent[c++] = body[k];
                nextparent[c] = 0;
                if (c == 40) have_parent = 1;
            }
        }

        /* first line of message only */
        line[0] = 0;
        for (int k = 0; k < 7; k++) {
            char one[2]; one[0] = oid[k]; one[1] = 0;
            g_scat(line, sizeof(line), one);
        }
        g_scat(line, sizeof(line), " ");
        unsigned long ll = g_slen(line);
        for (unsigned long k = blank_at; k < bn && ll + 2 < sizeof(line); k++) {
            char ch = body[k];
            if (ch == '\n' || ch == '\r') break;
            line[ll++] = ch;
        }
        line[ll++] = '\n';
        line[ll] = 0;
        emit(line);

        if (!have_parent) break;
        g_scopy(oid, OID_LEN, nextparent);
    }
    return 0;
}

/* Dispatch the "log" verb: plain log, or --oneline. */
static int cmd_log_dispatch(const char* rest) {
    if (!repo_exists()) {
        emit("fatal: not a git repository\n");
        return 1;
    }
    static char tok[32];
    const char* p = rest;
    next_token(&p, tok, sizeof(tok));
    if (g_streq(tok, "--oneline")) return cmd_log_oneline();
    return cmd_log();
}

static void usage(void) {
    emit("usage: git <command> [<args>]\n");
    emit("\n");
    emit("commands:\n");
    emit("   init      Create an empty Git repository\n");
    emit("   add       Add file contents to the index (add <file>... | . | -A)\n");
    emit("   status    Show the working tree status\n");
    emit("   diff      Show changes between the index and working tree\n");
    emit("   commit    Record changes to the repository (commit -m <msg>)\n");
    emit("   branch    List or create branches (branch [<name>])\n");
    emit("   checkout  Switch branches or restore files (checkout <branch> | -- <file>)\n");
    emit("   show      Show a commit or object (show [<id>])\n");
    emit("   log       Show commit logs (log [--oneline])\n");
}

/* ------------------------------------------------------------------ */
/* Optional dormant SHA-1 self-check (not called on normal runs).     */
/* Verifies SHA-1("abc") == a9993e364706816aba3e25717850c26c9cd0d89d. */
/* ------------------------------------------------------------------ */
int git_sha1_selftest(void);   /* exported for host test harness */
int git_sha1_selftest(void) {
    unsigned char d[20];
    char hex[OID_LEN];
    sha1((const unsigned char*)"abc", 3, d);
    sha1_hex(d, hex);
    return g_streq(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Public entry point.                                                */
/* ------------------------------------------------------------------ */
int git_run(const char* argline, const char* cwd, void (*out)(const char*)) {
    g_out = out;
    g_cwd = cwd;

    const char* p = skip_ws(argline);
    static char verb[32];
    next_token(&p, verb, sizeof(verb));

    if (verb[0] == 0) { usage(); return 1; }

    if (g_streq(verb, "init"))     return cmd_init();
    if (g_streq(verb, "add"))      return cmd_add(p);
    if (g_streq(verb, "status"))   return cmd_status();
    if (g_streq(verb, "diff"))     return cmd_diff();
    if (g_streq(verb, "commit"))   return cmd_commit(p);
    if (g_streq(verb, "branch"))   return cmd_branch(p);
    if (g_streq(verb, "checkout")) return cmd_checkout(p);
    if (g_streq(verb, "show"))     return cmd_show(p);
    if (g_streq(verb, "log"))      return cmd_log_dispatch(p);

    {
        static char line[64];
        g_scopy(line, sizeof(line), "git: '");
        g_scat(line, sizeof(line), verb);
        g_scat(line, sizeof(line), "' is not a git command.\n");
        emit(line);
    }
    usage();
    return 1;
}

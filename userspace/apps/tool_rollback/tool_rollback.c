/*
 * tool_rollback -- TOOLSET-0 / Phase C3 gated agent tool (AutomationOS).
 * ====================================================================
 *
 * FREESTANDING ring-3 ELF (NO libc, NO headers, single self-contained .c).
 * crt0 provides _start and calls main(argc,argv) -- we do NOT define _start.
 * The .o MUST carry ZERO fs:0x28 references (the orchestrator gates this); the
 * build adds -fno-stack-protector to make that so.
 *
 * --------------------------------------------------------------------------
 * WHAT IT DOES (the inverse of a mutating tool run):
 *
 *   agentd's pre_snapshot writes  /var/snapshots/<basename>.<seq>  BEFORE a
 *   mutating tool touches <path>, bumping <seq> each time. tool_rollback finds
 *   the HIGHEST <seq> for <path>'s basename and copies that snapshot back over
 *   the original path -- a one-step "undo last mutation".
 *
 * CONTRACT (argv = [path]):
 *   1. GATE path_write_allowed(argv[1]) -- the restore WRITES <path>, so it must
 *      pass the WRITE gate (copied VERBATIM from tool_write.c). On deny print
 *      "TOOL_ROLLBACK: DENY policy <path>" to fd1 and return 1. The model is
 *      HOSTILE TEXT: fail closed on traversal / protected prefixes.
 *   2. Compute basename(<path>). opendir /var/snapshots, scan entries matching
 *      "<basename>.<digits>" (the WHOLE suffix after the final '.' must be all
 *      digits), track the highest numeric <seq>. If none: print
 *      "TOOL_ROLLBACK: none <path>" and return 0.
 *   3. Read /var/snapshots/<basename>.<maxseq> into a bounded (~8KB) buffer.
 *      If the snapshot is larger than the buffer: print "TOOL_ROLLBACK: too-large"
 *      and return 1 (never a silent truncated restore).
 *   4. Open <path> O_WRONLY|O_CREAT|O_TRUNC, write the bytes back, close, print
 *      "TOOL_ROLLBACK: OK <path> <- <basename>.<maxseq> (<n> bytes)", return 0.
 *
 * FD CONVENTION: fd1 is the capability channel sbin/agentd CAPTURES as the
 * RESULT the model reads -- the ONE-LINE OUTCOME goes to fd1.
 *
 * Build (flags DIRECTLY on the command line, never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/apps/tool_rollback/tool_rollback.c -o tool_rollback.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       tool_rollback.o crt0.o -o build/tool_rollback
 *   objdump -d build/tool_rollback | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified vs kernel/include/syscall.h).
 *   SYS_READ=2 SYS_WRITE=3 SYS_OPEN=4 SYS_CLOSE=5
 *   SYS_OPENDIR=30 SYS_READDIR=31 SYS_CLOSEDIR=32
 * --------------------------------------------------------------------- */
#define SYS_READ      2
#define SYS_WRITE     3
#define SYS_OPEN      4
#define SYS_CLOSE     5
#define SYS_OPENDIR  30
#define SYS_READDIR  31
#define SYS_CLOSEDIR 32

/* open() flags. */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_CREAT    0x40
#define O_TRUNC    0x200

#define FD_STDOUT  1   /* the capability/result channel agentd captures */

#define SNAP_DIR   "/var/snapshots"

/* Snapshot read cap (bounded static buffer, per spec ~8KB). */
#define SNAP_CAP   8192
/* Max length of a basename / built snapshot path we will handle. */
#define NAME_MAX_  256
#define PATHBUF    512

/* k_dirent_t mirrors the kernel `struct dirent` (kernel/include/vfs.h) byte for
 * byte; SYS_READDIR copies sizeof(struct dirent) into this. */
typedef struct {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[NAME_MAX_];
} k_dirent_t;

/* -----------------------------------------------------------------------
 * Inline syscall wrapper (3 args is all we need). Copied verbatim from kit.
 * --------------------------------------------------------------------- */
static long sc(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

/* -----------------------------------------------------------------------
 * Freestanding helpers.
 * --------------------------------------------------------------------- */
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { sc(SYS_WRITE, fd, (long)s, (long)slen(s)); }

/* Print an unsigned decimal to fd. Values are bounded (<= SNAP_CAP, or a small
 * snapshot seq), so the const /10 here stays in 32-bit range. */
static void out_u(int fd, unsigned v)
{
    char tmp[16];
    int  i = 0;
    if (v == 0) { out(fd, "0"); return; }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    char rev[16];
    int  j = 0;
    while (i-- > 0) rev[j++] = tmp[i];
    rev[j] = '\0';
    out(fd, rev);
}

/* =======================================================================
 *  THE PATH GATE -- copied VERBATIM from tool_write.c (== codeagent /
 *  aibroker policy). The agent is HOSTILE TEXT: writes are allowed ONLY to
 *  scratch/project dirs; traversal + protected system paths are denied.
 *  The restore step WRITES <path>, so it must pass this WRITE gate.
 * ======================================================================= */
static int has_sub(const char *p, const char *s)
{
    for (int i = 0; p[i]; i++) {
        int j = 0;
        while (s[j] && p[i + j] == s[j]) j++;
        if (!s[j]) return 1;
    }
    return 0;
}
static int starts(const char *p, const char *pre)
{
    int j = 0;
    while (pre[j]) { if (p[j] != pre[j]) return 0; j++; }
    return 1;
}
static int path_write_allowed(const char *p)
{
    if (!p || !p[0]) return 0;
    if (has_sub(p, ".."))     return 0;                 /* no traversal          */
    if (starts(p, "/boot") || starts(p, "/sbin") ||
        starts(p, "/bin")  || starts(p, "/etc")  ||
        has_sub(p, "kernel")) return 0;                 /* protected system paths */
    if (starts(p, "/tmp") || starts(p, "/home") ||
        starts(p, "/usr/src")) return 1;                /* allowlist             */
    return 0;                                           /* deny by default       */
}

/* =======================================================================
 *  Small string helpers (no libc).
 * ======================================================================= */

/* Copy src into dst (capacity cap incl. NUL). Returns length, or -1 on overflow. */
static int str_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i]) {
        if (i >= cap - 1) return -1;
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

/* Append src onto dst (which already holds `len` chars), capacity cap incl. NUL.
 * Returns the new length, or -1 on overflow. */
static int str_append(char *dst, int len, const char *src, int cap)
{
    int i = 0;
    while (src[i]) {
        if (len >= cap - 1) return -1;
        dst[len++] = src[i++];
    }
    dst[len] = '\0';
    return len;
}

static int str_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* basename: pointer to the last path component (after the final '/'). */
static const char *basename_of(const char *p)
{
    const char *base = p;
    for (int i = 0; p[i]; i++)
        if (p[i] == '/') base = &p[i + 1];
    return base;
}

/* =======================================================================
 *  Snapshot-name matcher.
 *
 *  An entry matches iff it is exactly  "<basename>.<digits>"  where:
 *    - the literal basename is a prefix,
 *    - the next char is '.',
 *    - the remainder is one or more characters, ALL ASCII digits.
 *  On match, *seq receives the parsed unsigned value and we return 1.
 *  We parse the seq ourselves (no libc), with simple overflow saturation.
 * ======================================================================= */
static int snap_match(const char *entry, const char *base, unsigned *seq)
{
    int i = 0;
    /* basename prefix */
    while (base[i]) {
        if (entry[i] != base[i]) return 0;
        i++;
    }
    /* the dot */
    if (entry[i] != '.') return 0;
    i++;
    /* at least one digit, then all digits to end-of-string */
    if (entry[i] < '0' || entry[i] > '9') return 0;
    unsigned v = 0;
    while (entry[i]) {
        char c = entry[i];
        if (c < '0' || c > '9') return 0;           /* a non-digit -> not a seq */
        unsigned d = (unsigned)(c - '0');
        if (v > (0xFFFFFFFFu - d) / 10u) v = 0xFFFFFFFFu;   /* saturate, don't wrap */
        else v = v * 10u + d;
        i++;
    }
    *seq = v;
    return 1;
}

/* =======================================================================
 *  Entry point.
 *
 *  crt0 provides _start, parses argc/argv off the kernel-prepared stack,
 *  calls main(argc, argv), and feeds the return value to SYS_EXIT. We always
 *  validate argc/argv before any deref.
 * ======================================================================= */
int main(int argc, char **argv)
{
    const char *path = (argc > 1 && argv && argv[1]) ? argv[1] : "";

    /* 1. GATE: the model is hostile text -- deny first, fail closed. The
     *    restore WRITES <path>, so it must clear the WRITE policy. */
    if (!path_write_allowed(path)) {
        out(FD_STDOUT, "TOOL_ROLLBACK: DENY policy ");
        out(FD_STDOUT, path[0] ? path : "(none)");
        out(FD_STDOUT, "\n");
        return 1;
    }

    /* 2. basename(<path>), then scan /var/snapshots for the highest seq. */
    const char *base = basename_of(path);
    if (!base[0]) {                                  /* path ended in '/' -> no file */
        out(FD_STDOUT, "TOOL_ROLLBACK: none ");
        out(FD_STDOUT, path);
        out(FD_STDOUT, "\n");
        return 0;
    }

    long dfd = sc(SYS_OPENDIR, (long)SNAP_DIR, 0, 0);
    if (dfd < 0) {                                   /* no snapshot dir == nothing to roll back */
        out(FD_STDOUT, "TOOL_ROLLBACK: none ");
        out(FD_STDOUT, path);
        out(FD_STDOUT, "\n");
        return 0;
    }

    unsigned best_seq = 0;
    int      found    = 0;
    k_dirent_t de;
    for (;;) {
        long r = sc(SYS_READDIR, dfd, (long)&de, 0);
        if (r != 0) break;                           /* 0 = got entry; nonzero = end/error */
        de.d_name[NAME_MAX_ - 1] = '\0';
        const char *nm = de.d_name;
        if (!nm[0]) continue;
        if (str_eq(nm, ".") || str_eq(nm, "..")) continue;
        unsigned seq;
        if (!snap_match(nm, base, &seq)) continue;
        if (!found || seq > best_seq) { best_seq = seq; found = 1; }
    }
    sc(SYS_CLOSEDIR, dfd, 0, 0);

    if (!found) {
        out(FD_STDOUT, "TOOL_ROLLBACK: none ");
        out(FD_STDOUT, path);
        out(FD_STDOUT, "\n");
        return 0;
    }

    /* Build "/var/snapshots/<basename>.<best_seq>" without sprintf. */
    char snap_path[PATHBUF];
    int  spl = str_copy(snap_path, SNAP_DIR, PATHBUF);
    if (spl < 0) { out(FD_STDOUT, "TOOL_ROLLBACK: too-large\n"); return 1; }
    spl = str_append(snap_path, spl, "/", PATHBUF);
    if (spl < 0) { out(FD_STDOUT, "TOOL_ROLLBACK: too-large\n"); return 1; }
    spl = str_append(snap_path, spl, base, PATHBUF);
    if (spl < 0) { out(FD_STDOUT, "TOOL_ROLLBACK: too-large\n"); return 1; }
    spl = str_append(snap_path, spl, ".", PATHBUF);
    if (spl < 0) { out(FD_STDOUT, "TOOL_ROLLBACK: too-large\n"); return 1; }
    {
        /* append the decimal seq */
        char seqs[16];
        int  i = 0;
        unsigned v = best_seq;
        if (v == 0) { seqs[i++] = '0'; }
        else { char t[16]; int k = 0;
               while (v > 0 && k < (int)sizeof(t)) { t[k++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
               while (k-- > 0) seqs[i++] = t[k]; }
        seqs[i] = '\0';
        spl = str_append(snap_path, spl, seqs, PATHBUF);
        if (spl < 0) { out(FD_STDOUT, "TOOL_ROLLBACK: too-large\n"); return 1; }
    }

    /* 3. Read the snapshot into a bounded buffer. Reject (do NOT truncate) if
     *    its bytes exceed the buffer: a partial restore would silently corrupt
     *    the file. We detect overflow by reading one extra byte beyond the cap. */
    long sfd = sc(SYS_OPEN, (long)snap_path, O_RDONLY, 0);
    if (sfd < 0) {
        /* The dir listing said it existed; treat a vanished snapshot as "none". */
        out(FD_STDOUT, "TOOL_ROLLBACK: none ");
        out(FD_STDOUT, path);
        out(FD_STDOUT, "\n");
        return 0;
    }
    static char buf[SNAP_CAP];
    int  total = 0;
    long n;
    while (total < SNAP_CAP && (n = sc(SYS_READ, sfd, (long)(buf + total), SNAP_CAP - total)) > 0)
        total += (int)n;
    if (n < 0) {                                     /* a read error mid-stream */
        sc(SYS_CLOSE, sfd, 0, 0);
        out(FD_STDOUT, "TOOL_ROLLBACK: too-large\n");
        return 1;
    }
    if (total >= SNAP_CAP) {
        /* Buffer is full -- probe for one more byte; if present, the snapshot is
         * larger than we can safely restore. */
        char extra;
        long more = sc(SYS_READ, sfd, (long)&extra, 1);
        sc(SYS_CLOSE, sfd, 0, 0);
        if (more > 0) {
            out(FD_STDOUT, "TOOL_ROLLBACK: too-large\n");
            return 1;
        }
    } else {
        sc(SYS_CLOSE, sfd, 0, 0);
    }

    /* 4. Restore: open <path> O_WRONLY|O_CREAT|O_TRUNC, write the bytes back. */
    long fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0) {
        out(FD_STDOUT, "TOOL_ROLLBACK: ERR open ");
        out(FD_STDOUT, path);
        out(FD_STDOUT, "\n");
        return 1;
    }
    int written = 0;
    while (written < total) {
        long w = sc(SYS_WRITE, fd, (long)(buf + written), total - written);
        if (w <= 0) {                                /* write failure / short stall */
            sc(SYS_CLOSE, fd, 0, 0);
            out(FD_STDOUT, "TOOL_ROLLBACK: ERR write ");
            out(FD_STDOUT, path);
            out(FD_STDOUT, "\n");
            return 1;
        }
        written += (int)w;
    }
    sc(SYS_CLOSE, fd, 0, 0);

    /* Success: one-line outcome to the capability channel.
     *   TOOL_ROLLBACK: OK <path> <- <basename>.<maxseq> (<n> bytes) */
    out(FD_STDOUT, "TOOL_ROLLBACK: OK ");
    out(FD_STDOUT, path);
    out(FD_STDOUT, " <- ");
    out(FD_STDOUT, base);
    out(FD_STDOUT, ".");
    out_u(FD_STDOUT, best_seq);
    out(FD_STDOUT, " (");
    out_u(FD_STDOUT, (unsigned)written);
    out(FD_STDOUT, " bytes)\n");
    return 0;
}

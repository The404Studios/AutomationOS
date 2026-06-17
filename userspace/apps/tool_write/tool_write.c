/*
 * tool_write -- TOOLSET-0 write_file gated agent tool (AutomationOS).
 * ===================================================================
 *
 * FREESTANDING ring-3 ELF (NO libc, NO headers, single self-contained .c).
 * crt0 provides _start and calls main(argc,argv) -- we do NOT define _start.
 * The .o MUST carry ZERO fs:0x28 references (the orchestrator gates this); the
 * build adds -fno-stack-protector to make that so.
 *
 * --------------------------------------------------------------------------
 * CONTRACT (argv = [path, base64content]):
 *   1. GATE path_write_allowed(argv[1]). On deny: print "DENY policy <path>"
 *      to fd1 and return 2. (The model is HOSTILE TEXT -- the gate is copied
 *      VERBATIM from the ABI kit / codeagent / aibroker.)
 *   2. base64-DECODE argv[2] (standard alphabet A-Za-z0-9+/ with '=' padding,
 *      ignoring newlines/whitespace) into a bounded static buffer (cap 8192).
 *      On any invalid char: print "ERR b64" to fd1 and return 3.
 *   3. Open argv[1] O_WRONLY|O_CREAT|O_TRUNC. On open failure: print
 *      "ERR open <path>" to fd1 and return 2.
 *   4. Write the decoded bytes, close, print "WROTE <path> <nbytes>" to fd1,
 *      return 0.
 *
 * FD CONVENTION: fd1 is the capability channel that sbin/agentd CAPTURES as the
 * RESULT the model reads -- so the ONE-LINE OUTCOME goes to fd1. (This tool has
 * no diagnostics worth a separate fd2 line; the outcome line is the result.)
 *
 * Build (flags DIRECTLY on the command line, never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/tool_write/tool_write.c -o tool_write.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       tool_write.o crt0.o -o build/tool_write
 *   objdump -d build/tool_write | grep fs:0x28   # MUST be empty
 */

/* -----------------------------------------------------------------------
 * Syscall numbers (verified vs kernel/include/syscall.h, per the ABI kit).
 * --------------------------------------------------------------------- */
#define SYS_WRITE  3
#define SYS_OPEN   4
#define SYS_CLOSE  5

/* open() flags. */
#define O_WRONLY   1
#define O_CREAT    0x40
#define O_TRUNC    0x200

#define FD_STDOUT  1   /* the capability/result channel agentd captures */

/* Decoded-content cap (bounded static buffer, per spec). */
#define DECODE_CAP 8192

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
 * Freestanding helpers (kit-standard).
 * --------------------------------------------------------------------- */
static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(int fd, const char *s) { sc(SYS_WRITE, fd, (long)s, (long)slen(s)); }

/* Print an unsigned decimal to fd. Small numbers only (nbytes <= DECODE_CAP),
 * built into a tiny char[] then emitted -- no 64-bit division (the const /10
 * here is fine on a 32-bit-range value). */
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
 *  THE PATH GATE -- copied VERBATIM from the ABI kit (== codeagent /
 *  aibroker policy). The agent is HOSTILE TEXT: writes are allowed ONLY to
 *  scratch/project dirs; traversal + protected system paths are denied.
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
 *  Base64 decode (standard alphabet A-Za-z0-9+/, '=' padding).
 *
 *  Decodes `src` into `dst` (capacity `cap`). Newlines and ASCII whitespace
 *  are ignored. Any other non-alphabet, non-pad character is INVALID. We
 *  process 4 valid symbols -> 3 bytes, honoring up to two trailing '=' pads.
 *  Returns the number of decoded bytes on success, or -1 on a malformed
 *  input (bad char / pad in the wrong place / overflow of `cap`).
 * ======================================================================= */

/* Map one base64 char to its 6-bit value; -1 = not an alphabet char. */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';        /*  0..25 */
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;   /* 26..51 */
    if (c >= '0' && c <= '9') return c - '0' + 52;   /* 52..61 */
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static int b64_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static long b64_decode(const char *src, char *dst, int cap)
{
    int  quad[4];   /* the 4 six-bit symbols of the current group           */
    int  qn   = 0;  /* how many symbols collected into `quad` so far (0..4) */
    int  pad  = 0;  /* '=' padding seen in this group (0..2)                */
    int  outn = 0;  /* decoded bytes emitted                                */
    int  done = 0;  /* set once a padded (final) group has been processed   */

    for (int i = 0; src[i]; i++) {
        char c = src[i];
        if (b64_is_space(c)) continue;          /* ignore whitespace/newlines */

        /* Nothing legal may follow a completed, padded final group. */
        if (done) return -1;

        if (c == '=') {
            /* Pad only valid as the 3rd/4th symbol of a group. */
            if (qn < 2) return -1;
            pad++;
            quad[qn++] = 0;                     /* placeholder, contributes nothing */
        } else {
            int v = b64_val(c);
            if (v < 0) return -1;               /* invalid character */
            if (pad) return -1;                 /* data after padding is malformed */
            quad[qn++] = v;
        }

        if (qn == 4) {
            /* Emit 3 - pad bytes from the 4 six-bit symbols. */
            int b0 = (quad[0] << 2) | (quad[1] >> 4);
            int b1 = ((quad[1] & 0xF) << 4) | (quad[2] >> 2);
            int b2 = ((quad[2] & 0x3) << 6) | quad[3];

            if (outn >= cap) return -1;
            dst[outn++] = (char)b0;
            if (pad < 2) { if (outn >= cap) return -1; dst[outn++] = (char)b1; }
            if (pad < 1) { if (outn >= cap) return -1; dst[outn++] = (char)b2; }

            if (pad) done = 1;                  /* a padded group is necessarily last */
            qn = 0;
            pad = 0;
        }
    }

    /* A leftover, unterminated group is malformed (only 0 is acceptable). */
    if (qn != 0) return -1;
    return outn;
}

/* =======================================================================
 *  Entry point.
 *
 *  crt0 provides _start, parses argc/argv off the kernel-prepared stack,
 *  calls main(argc, argv), and feeds the return value to SYS_EXIT. We always
 *  validate argc/argv before any deref (never touch a missing/empty argv).
 * ======================================================================= */
int main(int argc, char **argv)
{
    /* Need both path (argv[1]) and base64 content (argv[2]). A missing/empty
     * path is treated as a policy denial -- the gate fails closed on it. */
    const char *path = (argc > 1 && argv && argv[1]) ? argv[1] : "";
    const char *b64  = (argc > 2 && argv && argv[2]) ? argv[2] : "";

    /* 1. GATE: the model is hostile text -- deny first, fail closed. */
    if (!path_write_allowed(path)) {
        out(FD_STDOUT, "DENY policy ");
        out(FD_STDOUT, path[0] ? path : "(none)");
        out(FD_STDOUT, "\n");
        return 2;
    }

    /* 2. base64-decode into a bounded static buffer (cap 8192). */
    static char decoded[DECODE_CAP];
    long n = b64_decode(b64, decoded, DECODE_CAP);
    if (n < 0) {
        out(FD_STDOUT, "ERR b64\n");
        return 3;
    }

    /* 3. open O_WRONLY|O_CREAT|O_TRUNC. */
    long fd = sc(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0) {
        out(FD_STDOUT, "ERR open ");
        out(FD_STDOUT, path);
        out(FD_STDOUT, "\n");
        return 2;
    }

    /* 4. write the exact decoded bytes (one bounded call), then close. */
    long w = sc(SYS_WRITE, fd, (long)decoded, n);
    sc(SYS_CLOSE, fd, 0, 0);
    if (w < 0) {                                /* write itself failed */
        out(FD_STDOUT, "ERR open ");            /* report against the path, as spec'd for failures */
        out(FD_STDOUT, path);
        out(FD_STDOUT, "\n");
        return 2;
    }

    /* Success: one-line outcome to the capability channel. */
    out(FD_STDOUT, "WROTE ");
    out(FD_STDOUT, path);
    out(FD_STDOUT, " ");
    out_u(FD_STDOUT, (unsigned)w);
    out(FD_STDOUT, "\n");
    return 0;
}

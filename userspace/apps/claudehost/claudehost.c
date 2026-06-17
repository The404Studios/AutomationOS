/* claudehost -- CLAUDE-API-0 (broker backend): send a prompt to the host
 * Claude broker over the proven slirp seam and print Claude's reply IN the OS.
 * ==========================================================================
 *   sbin/claudehost  --TCP 10.0.2.2:8432-->  host scripts/claude_broker.py
 *                                          --HTTPS--> api.anthropic.com
 *
 * The API key lives ONLY on the host broker -- it never enters this image.
 * This is the broker backend of the LLM seam; an in-OS https_post backend
 * (direct to api.anthropic.com) is the follow-on, behind the same idea.
 *
 * Freestanding ring-3 (no libc): every I/O is an inline syscall, every buffer
 * is a fixed static array. Modeled on modelbridge.c's model_exchange + nc.c's
 * relay. If the broker/net is absent, SKIP cleanly (bounded, exit 0) so the
 * default boot stays clean.
 *
 * Usage:  claudehost ["<prompt>"]      (no arg -> a built-in demo prompt)
 *
 * Build (apidemo-style not needed -- only socket syscalls, like modelbridge):
 *   gcc -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic
 *       -fno-pie -mno-red-zone -mstackrealign -O2 -c claudehost.c
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld
 *       crt0.o claudehost.o -o claudehost.elf
 */

#define SYS_WRITE     3
#define SYS_YIELD    15
#define SYS_SOCKET   51
#define SYS_CONNECT  52
#define SYS_SEND     53
#define SYS_RECV     54
#define SYS_CLOSE_SK 55
#define SYS_SOCK_POLL 58
#define SOCK_STREAM   1
#define EAGAIN_NEG  (-11)

#define BROKER_IP    0x0A000202u   /* 10.0.2.2 = the QEMU slirp host */
#define BROKER_PORT  8432
#define REPLY_CAP    8192
#define RECV_MAX     4000000       /* bounded; Claude can take tens of seconds */

/* raw 6-arg inline syscall (rdi/rsi/rdx/r10/r8), identical ABI to nc.c */
static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}
static unsigned slen(const char* s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(SYS_WRITE, 1, (long)s, (long)slen(s), 0, 0); }
static void out_num(long v) {
    char b[24]; int i = 0; char t[24]; int j = 0;
    if (v == 0) b[i++] = '0';
    while (v > 0) { t[j++] = (char)('0' + (v % 10)); v /= 10; }
    while (j > 0) b[i++] = t[--j];
    b[i] = 0; out(b);
}

static char g_reply[REPLY_CAP];

static const char* DEFAULT_PROMPT =
    "In one short, warm sentence, greet the developer of AutomationOS -- a "
    "from-scratch hobby OS that just made its first real HTTPS request to the "
    "live internet -- and name one thing such an OS should be proud of.";

static long send_all(long fd, const char* b, long len) {
    long off = 0; int guard = 0;
    while (off < len) {
        long n = sc(SYS_SEND, fd, (long)(b + off), len - off, 0, 0);
        if (n > 0) { off += n; guard = 0; continue; }
        if (n == EAGAIN_NEG) { sc(SYS_YIELD, 0, 0, 0, 0, 0); if (++guard > 200000) break; continue; }
        return n;
    }
    return off;
}

int main(int argc, char** argv) {
    const char* prompt = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : DEFAULT_PROMPT;

    out("CLAUDEHOST: connecting to host Claude broker 10.0.2.2:8432 ...\n");
    long fd = sc(SYS_SOCKET, SOCK_STREAM, 0, 0, 0, 0);
    if (fd < 0) { out("CLAUDEHOST: SKIP (no socket)\n"); return 0; }

    long cr = sc(SYS_CONNECT, fd, (long)BROKER_IP, BROKER_PORT, 0, 0);
    if (cr < 0) {
        out("CLAUDEHOST: SKIP (broker unreachable -- run `python3 scripts/claude_broker.py` "
            "on the host and boot with -netdev user -device e1000)\n");
        sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
        return 0;
    }

    /* send the prompt as one newline-terminated line */
    send_all(fd, prompt, (long)slen(prompt));
    send_all(fd, "\n", 1);

    /* drain the full reply until the broker closes the connection */
    long total = 0;
    for (long it = 0; it < RECV_MAX && total < REPLY_CAP - 1; it++) {
        sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0);
        long rn = sc(SYS_RECV, fd, (long)(g_reply + total), REPLY_CAP - 1 - total, 0, 0);
        if (rn > 0) { total += rn; continue; }
        if (rn == 0) break;                                  /* broker closed = done */
        if (rn == EAGAIN_NEG) { sc(SYS_YIELD, 0, 0, 0, 0, 0); continue; }
        break;                                               /* hard error */
    }
    sc(SYS_CLOSE_SK, fd, 0, 0, 0, 0);
    g_reply[total] = 0;

    if (total <= 0) { out("CLAUDEHOST: SKIP (empty reply from broker)\n"); return 0; }

    out("CLAUDEHOST: ================ Claude says ================\n");
    out(g_reply);
    out("\nCLAUDEHOST: ================ end (");
    out_num(total);
    out(" bytes) ================\n");
    out("CLAUDEHOST: PASS reply_received=1\n");
    return 0;
}

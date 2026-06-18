/* tool_key -- SYNTHINPUT-0 gated keyboard injection. argv=[mode, arg].
 *
 * The agent rail (sbin/agentd) uses this ONE gated tool to type keystrokes into
 * the focused window, exactly as a human hand would. The compositor owns the
 * global keyboard focus and there is no wl message for "some other process
 * pressed a key", so the well-known SysV SHM page (synthinput.h) is the seam:
 * this tool ATTACHES that page LOOKUP-ONLY (shmget(KEY,SIZE,0) -- NO IPC_CREAT,
 * the compositor created + owns it) and ENQUEUES synthetic SI_EV_KEY events the
 * compositor's input pump drains.
 *
 * Usage:
 *   tool_key type <text>   -- for each printable-ASCII char in <text>, enqueue a
 *                             press (value=1) then a release (value=0). '\n' in
 *                             <text> maps to keycode 10 (Enter); all other
 *                             control chars (< 0x20, == 0x7F) are skipped.
 *   tool_key key  <code>   -- enqueue a single press+release for the decimal
 *                             integer keycode <code>.
 *
 * KEYCODE ENCODING ASSUMPTION (document for the compositor researcher):
 *   For `type`, the event `code` is the literal ASCII value of the character
 *   (e.g. 'A' -> 65, 'a' -> 97, ' ' -> 32, '\n' -> 10). This is NOT a raw PS/2
 *   scancode or an evdev keycode -- it is the printable character itself. The
 *   compositor side (send_key_to_focus) must therefore treat an SI_EV_KEY code
 *   coming off this ring as an ASCII character and feed it straight into its
 *   key-handling path (the same byte it would deliver to the focused app's
 *   textbox), rather than running it through a scancode->ASCII table. The
 *   `key <code>` form passes the integer through verbatim for callers that
 *   genuinely want a specific keycode.
 *
 * THE GATE (the model is HOSTILE TEXT, so validate + guard BEFORE injecting):
 *   - require a valid mode + arg, else "TOOL_KEY: ERR usage" (exit 2);
 *   - the SHM page must be present, magic==SYNTHINPUT_MAGIC AND active!=0,
 *     i.e. an agent currently HOLDS the input rail. If the page is missing, the
 *     magic mismatches, or active==0, print "TOOL_KEY: SKIP no_inject" and
 *     exit 0 (a revoked/absent rail is a clean no-op, not an error).
 *
 * Enqueue is a classic single-producer ring write: fill q[head] then advance
 * head=(head+1)%QMAX through a VOLATILE pointer (the compositor must not see a
 * half-written slot; we never cache head). A full ring (head+1 == tail) drops
 * the newest event rather than corrupting an unread one.
 *
 * FD CONVENTION: fd1 is the capability channel the agent CAPTURES as the RESULT
 * the model reads, so the ONE-LINE outcome is written to fd1. On success:
 * "TOOL_KEY: OK typed=<n>" where <n> is the number of CHARACTERS / keycodes
 * injected (each = one press + one release event pair).
 *
 * Freestanding ring 3 (NO libc, NO headers beyond the SHM contract); crt0
 * provides _start and calls main(argc,argv). Built with -fno-stack-protector
 * => the .o must have ZERO fs:0x28 references (the orchestrator gates this). */

#include "../../include/synthinput.h"

#define SYS_WRITE   3
#define SYS_SHMGET 18   /* sc(18, key, size, 0) -> id  / <0 err (0 flag = lookup-only) */
#define SYS_SHMAT  19   /* sc(19, id, 0, 0)     -> addr / <=0 err                       */

#define FD_OUT      1   /* capability channel: the agent reads this as RESULT */

/* Verbatim 3-arg syscall wrapper (see ABI kit). */
static long sc(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void out(const char *s) { sc(SYS_WRITE, FD_OUT, (long)s, (long)slen(s)); }

/* Append an unsigned decimal to fd1 (small counts only; const /10). */
static void out_u(unsigned v)
{
    char b[16]; int i = 0;
    if (!v) { out("0"); return; }
    while (v) { b[i++] = (char)('0' + v % 10); v /= 10; }
    char r[16]; int j = 0; while (i) r[j++] = b[--i]; r[j] = 0; out(r);
}

/* Strict decimal parse. Returns 0 and sets *ok=1 on a clean digit string; any
 * non-digit byte (or empty input) leaves *ok=0 and the value undefined. */
static int parse_int(const char *s, int *ok)
{
    *ok = 0;
    if (!s || !s[0]) return 0;
    int v = 0;
    for (unsigned i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;   /* *ok stays 0 -> reject */
        v = v * 10 + (s[i] - '0');
    }
    *ok = 1;
    return v;
}

/* ASCII compare for the two-byte mode word ("type" / "key"). */
static int streq(const char *a, const char *b)
{
    unsigned i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];   /* both terminate together */
}

/* Attach the compositor-owned synthinput page LOOKUP-ONLY. Returns the mapped
 * (volatile) pointer, or 0 if it is absent (compositor not granting input). */
static volatile synthinput_shm_t *attach_synthinput(void)
{
    long id = sc(SYS_SHMGET, (long)SYNTHINPUT_SHM_KEY, (long)SYNTHINPUT_SHM_SIZE, 0);
    if (id < 0) return (volatile synthinput_shm_t *)0;
    long p = sc(SYS_SHMAT, id, 0, 0);
    if (p <= 0) return (volatile synthinput_shm_t *)0;
    return (volatile synthinput_shm_t *)p;
}

/* Single-producer ring enqueue of one SI_EV_KEY event (code, value). The
 * compositor is the sole consumer (advances tail); we are the sole producer
 * (advance head LAST, through the volatile pointer). A full ring drops the
 * newest event rather than overwriting an unread one. Returns 1 if enqueued. */
static int enqueue_key(volatile synthinput_shm_t *shm, unsigned short code, int value)
{
    unsigned head = shm->head;
    unsigned tail = shm->tail;
    unsigned next = (head + 1u) % SYNTHINPUT_QMAX;
    if (next == tail) return 0;             /* ring full: drop newest */
    shm->q[head].type  = (unsigned short)SI_EV_KEY;
    shm->q[head].code  = code;
    shm->q[head].value = value;
    shm->head = next;                       /* publish the slot LAST */
    return 1;
}

/* Enqueue a press(value=1) immediately followed by a release(value=0) for one
 * keycode. Returns 1 only if BOTH halves made it into the ring (a key whose
 * press fit but whose release was dropped would stick "down" at the compositor,
 * so we count it as not-injected). */
static int tap_key(volatile synthinput_shm_t *shm, unsigned short code)
{
    if (!enqueue_key(shm, code, 1)) return 0;
    if (!enqueue_key(shm, code, 0)) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    /* validate argc/argv before any deref (never touch a missing/empty argv) */
    if (argc < 3 || !argv[1] || !argv[1][0] || !argv[2] || !argv[2][0]) {
        out("TOOL_KEY: ERR usage\n");
        return 2;
    }

    const char *mode = argv[1];
    const char *arg  = argv[2];
    int do_type = streq(mode, "type");
    int do_key  = streq(mode, "key");
    if (!do_type && !do_key) { out("TOOL_KEY: ERR usage\n"); return 2; }

    /* For `key <code>`, validate the integer BEFORE touching the rail so a bad
     * code is a usage error, not a silent skip. */
    int code_ok = 0, code_val = 0;
    if (do_key) {
        code_val = parse_int(arg, &code_ok);
        if (!code_ok || code_val < 0 || code_val > 0xFFFF) {
            out("TOOL_KEY: ERR usage\n");
            return 2;
        }
    }

    /* THE GATE: the page must exist, be initialised, AND have an agent actively
     * holding the input rail. Any of {absent, magic mismatch, active==0} => a
     * clean no-op (the rail was revoked or never granted). */
    volatile synthinput_shm_t *shm = attach_synthinput();
    if (!shm || shm->magic != SYNTHINPUT_MAGIC || shm->active == 0) {
        out("TOOL_KEY: SKIP no_inject\n");
        return 0;
    }

    unsigned typed = 0;

    if (do_key) {
        if (tap_key(shm, (unsigned short)code_val)) typed++;
    } else {
        /* `type <text>`: one press+release per printable ASCII char.
         * '\n' -> keycode 10 (Enter); other control bytes (< 0x20, 0x7F) are
         * skipped. Bytes are read unsigned so high-bit chars are bounded. */
        for (unsigned i = 0; arg[i]; i++) {
            unsigned char ch = (unsigned char)arg[i];
            unsigned short code;
            if (ch == '\n') {
                code = 10;                        /* Enter */
            } else if (ch >= 0x20 && ch < 0x7F) {
                code = (unsigned short)ch;         /* ASCII value AS keycode */
            } else {
                continue;                          /* skip control / non-printable */
            }
            if (!tap_key(shm, code)) break;        /* ring full: stop, report partial */
            typed++;
        }
    }

    out("TOOL_KEY: OK typed=");
    out_u(typed);
    out("\n");
    return 0;
}

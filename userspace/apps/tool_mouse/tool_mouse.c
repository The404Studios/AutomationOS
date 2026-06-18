/*
 * tool_mouse -- gated synthetic-pointer agent tool (AutomationOS, SYNTHINPUT-0).
 * =============================================================================
 *
 * FREESTANDING ring-3 ELF (NO libc, NO headers beyond the SHM contract). crt0
 * provides _start and calls main(argc, argv) -- we do NOT define _start. The .o
 * MUST carry ZERO fs:0x28 references (the orchestrator gates this); the build
 * adds -fno-stack-protector to make that so. Compiled by the host toolchain
 * (build_all.sh), so normal C is fine -- string literals / arrays are OK here.
 *
 * --------------------------------------------------------------------------
 * WHAT IT DOES
 *
 * The agent uses this tool to MOVE the mouse and CLICK, exactly as a human hand
 * would. The compositor owns the global cursor / button state and drains a
 * well-known single-producer/single-consumer SHM ring (see synthinput.h); this
 * tool is the sole producer: it ATTACHES the page LOOKUP-ONLY and ENQUEUES
 * synthetic pointer events the compositor's input pump will replay.
 *
 * argv (the agent passes these; the gate may treat argv[1] as the "path", but
 *       here the tokens are just this tool's own coords -- it parses them):
 *
 *   tool_mouse move <dx> <dy>   enqueue REL/X(value=dx) then REL/Y(value=dy)
 *   tool_mouse click <button>   l|left / r|right / m|middle -> BTN_*; enqueue
 *                               press(value=1) then release(value=0)
 *   tool_mouse wheel <delta>    enqueue REL/WHEEL(value=delta)
 *
 * GUARD: attach the page; require magic == SYNTHINPUT_MAGIC AND active != 0
 * (the compositor sets `active` only while injection is authorised -- it is the
 * hard-stop / kill switch). On magic mismatch or !active, print
 *   "TOOL_MOUSE: SKIP no_inject"
 * to fd1 and exit 0 (no events enqueued). On success print
 *   "TOOL_MOUSE: OK <what>"
 *
 * FD CONVENTION: fd1 is the capability channel the orchestrator CAPTURES as the
 * result line the model reads -- so the ONE-LINE OUTCOME goes to fd1. Diagnostic
 * noise (if any) goes to fd2; it is never mixed into fd1.
 *
 * ENQUEUE (single-producer ring): write q[head % QMAX], then publish the new
 * head = (head + 1) % QMAX. We never advance head INTO tail -- a full ring drops
 * the newest event rather than corrupting an unread one (matches synthinput.h).
 * head/tail are accessed through a volatile pointer so neither side caches them.
 *
 * Build (flags DIRECTLY on the command line, never via a shell variable, or
 * -fno-stack-protector is dropped and the program faults at CR2=0x28):
 *
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/apps/tool_mouse/tool_mouse.c -o tool_mouse.o
 *   ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
 *       tool_mouse.o crt0.o -o build/tool_mouse
 *   objdump -d build/tool_mouse | grep fs:0x28   # MUST be empty
 */

#include "../../include/synthinput.h"

/* -----------------------------------------------------------------------
 * Syscall numbers (verified vs kernel/include/syscall.h).
 * --------------------------------------------------------------------- */
#define SYS_WRITE   3
#define SYS_SHMGET 18
#define SYS_SHMAT  19

#define FD_STDOUT   1   /* capability/result channel the orchestrator captures */

/* -----------------------------------------------------------------------
 * Inline syscall wrapper (3 args is all we need). Kit-standard.
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
static void out(const char *s) { sc(SYS_WRITE, FD_STDOUT, (long)s, (long)slen(s)); }

/* Print a signed decimal to fd1 (small magnitudes; built into a tiny buffer). */
static void out_i(int v)
{
    char tmp[16];
    int  i = 0;
    unsigned mag;
    int neg = 0;

    if (v < 0) { neg = 1; mag = (unsigned)(-(long)v); }
    else         mag = (unsigned)v;

    if (mag == 0) { out("0"); return; }
    while (mag > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (int)(mag % 10u));
        mag /= 10u;
    }
    char rev[18];
    int  j = 0;
    if (neg) rev[j++] = '-';
    while (i-- > 0) rev[j++] = tmp[i];
    rev[j] = '\0';
    out(rev);
}

/* Parse a small signed integer from text (leading '+'/'-' allowed; stops at the
 * first non-digit). No libc. */
static int parse_int(const char *s)
{
    if (!s) return 0;
    int i = 0, neg = 0;
    if (s[i] == '+') i++;
    else if (s[i] == '-') { neg = 1; i++; }
    int v = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
    }
    return neg ? -v : v;
}

/* strcmp-style equality for short verb tokens. */
static int streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

/* -----------------------------------------------------------------------
 * The attached ring (set by attach(); 0 until then).
 * --------------------------------------------------------------------- */
static volatile synthinput_shm_t *g_si = (volatile synthinput_shm_t *)0;

/* Attach the compositor-owned page LOOKUP-ONLY (shmget with key + size + 0
 * flags -- NO IPC_CREAT) then shmat. Returns the mapping or 0 on failure. */
static volatile synthinput_shm_t *attach(void)
{
    long id = sc(SYS_SHMGET, (long)SYNTHINPUT_SHM_KEY, (long)SYNTHINPUT_SHM_SIZE, 0);
    if (id < 0) return (volatile synthinput_shm_t *)0;
    long p = sc(SYS_SHMAT, id, 0, 0);
    if (p <= 0) return (volatile synthinput_shm_t *)0;
    return (volatile synthinput_shm_t *)p;
}

/* Enqueue one event into the single-producer ring. Never advance head INTO
 * tail: a full ring drops the newest event (does not clobber an unread one). */
static void enqueue(unsigned short type, unsigned short code, int value)
{
    volatile synthinput_shm_t *si = g_si;
    unsigned head = si->head;                 /* our cursor; read once          */
    unsigned tail = si->tail;                 /* consumer cursor; volatile read */
    unsigned next = (head + 1u) % SYNTHINPUT_QMAX;

    if (next == (tail % SYNTHINPUT_QMAX)) return;   /* ring full -> drop newest */

    si->q[head % SYNTHINPUT_QMAX].type  = type;
    si->q[head % SYNTHINPUT_QMAX].code  = code;
    si->q[head % SYNTHINPUT_QMAX].value = value;
    si->head = next;                          /* publish LAST */
}

/* =======================================================================
 *  Entry point.
 *
 *  crt0 provides _start, parses argc/argv off the kernel-prepared stack and
 *  calls main(argc, argv). We validate argc/argv before any deref.
 * ======================================================================= */
int main(int argc, char **argv)
{
    const char *verb = (argc > 1 && argv && argv[1]) ? argv[1] : "";

    if (!verb[0]) { out("TOOL_MOUSE: ERR usage\n"); return 2; }

    /* Attach + guard FIRST: the agent is hostile text and the compositor's
     * `active` flag is the authorisation / kill switch. If the page is not
     * present, not initialised, or injection is not authorised, skip cleanly. */
    g_si = attach();
    if (!g_si || g_si->magic != SYNTHINPUT_MAGIC || g_si->active == 0) {
        out("TOOL_MOUSE: SKIP no_inject\n");
        return 0;
    }

    if (streq(verb, "move")) {
        if (argc < 4 || !argv[2] || !argv[3]) { out("TOOL_MOUSE: ERR usage\n"); return 2; }
        int dx = parse_int(argv[2]);
        int dy = parse_int(argv[3]);
        enqueue(SI_EV_REL, SI_REL_X, dx);
        enqueue(SI_EV_REL, SI_REL_Y, dy);
        out("TOOL_MOUSE: OK move ");
        out_i(dx); out(" "); out_i(dy); out("\n");
        return 0;
    }

    if (streq(verb, "click")) {
        if (argc < 3 || !argv[2] || !argv[2][0]) { out("TOOL_MOUSE: ERR usage\n"); return 2; }
        const char *b = argv[2];
        unsigned short btn;
        const char *name;
        if (streq(b, "l") || streq(b, "left"))        { btn = SI_BTN_LEFT;   name = "left";   }
        else if (streq(b, "r") || streq(b, "right"))  { btn = SI_BTN_RIGHT;  name = "right";  }
        else if (streq(b, "m") || streq(b, "middle")) { btn = SI_BTN_MIDDLE; name = "middle"; }
        else { out("TOOL_MOUSE: ERR button\n"); return 2; }

        enqueue(SI_EV_KEY, btn, 1);   /* press   */
        enqueue(SI_EV_KEY, btn, 0);   /* release */
        out("TOOL_MOUSE: OK click ");
        out(name);
        out("\n");
        return 0;
    }

    if (streq(verb, "wheel")) {
        if (argc < 3 || !argv[2] || !argv[2][0]) { out("TOOL_MOUSE: ERR usage\n"); return 2; }
        int delta = parse_int(argv[2]);
        enqueue(SI_EV_REL, SI_REL_WHEEL, delta);
        out("TOOL_MOUSE: OK wheel ");
        out_i(delta);
        out("\n");
        return 0;
    }

    out("TOOL_MOUSE: ERR usage\n");
    return 2;
}

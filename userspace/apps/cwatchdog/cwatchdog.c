/* cwatchdog — SELFHEAL desktop recovery supervisor  (sbin/cwatchdog)
 *
 * The *trigger* half of the desktop self-heal story; the kernel already provides
 * the *visible* half (SYS_RECOVERY_OVERLAY = the fluid-circle "Recovering
 * desktop..." animation).
 *
 * HOW IT WORKS
 *   The compositor stamps a monotonic frame_counter into a shared page once per
 *   frame-loop iteration (see userspace/include/selfheal.h + compositor_m8.c).
 *   The loop turns even on idle frames (it sleeps ~16ms then loops), so the
 *   counter advances ~60/s on a healthy desktop.  If it stops advancing for
 *   STALL_MS the desktop is frozen — either a blocking freeze (a syscall never
 *   returns) or a tight-loop freeze (for(;;); only recoverable under the PREEMPT
 *   kernel, which can preempt a ring-3 spinner).
 *
 *   On a stall we: (1) fire SYS_RECOVERY_OVERLAY so the user sees the animation;
 *   (2) SIGTERM then SIGKILL the compositor (pid read from the heartbeat);
 *   (3) init's existing reaper respawns sbin/compositor (userspace/init/main.c);
 *   (4) wait for the heartbeat to RESUME — which PROVES recovery → "PASS respawned".
 *
 *   A circuit breaker bounds us to BREAKER_MAX recoveries per BREAKER_WINDOW;
 *   exceeding it prints "CWATCHDOG: FAIL recovery storm" and stops (so a
 *   permanently-sick compositor can't become an infinite kill/respawn loop).
 *
 * OWNERSHIP: cwatchdog is a NON-owner, LOOKUP-ONLY attacher — shmget(KEY, 0, 0),
 * never IPC_CREAT — so it can never become the segment owner (init owns it; see
 * selfheal.h for why that is load-bearing).  It only ever READS the page.
 *
 * Spawned by init ONLY under -DSELFHEAL, so the default desktop never runs it.
 */

typedef unsigned long size_t;

#include "../../include/selfheal.h"

/* ---- syscall numbers (kernel/include/syscall.h) ---- */
#define SYS_WRITE             3
#define SYS_SLEEP             9
#define SYS_SHMGET           18
#define SYS_SHMAT            19
#define SYS_KILL             26
#define SYS_GET_TICKS_MS     40
#define SYS_RECOVERY_OVERLAY 88

#define SIGKILL    9
#define SIGTERM   15

/* ---- tuning ---- */
#define POLL_MS             250   /* heartbeat sample cadence                       */
#define STALL_MS           2500   /* counter unchanged this long => FROZEN          */
#define RESUME_TIMEOUT_MS  5000   /* after kill, wait this long for the resume      */
#define ATTACH_RETRIES       40   /* wait up to ~10s for init to create the segment */
#define BREAKER_WINDOW    60000   /* circuit-breaker sliding window (ms)            */
#define BREAKER_MAX           3   /* > this many recoveries in the window => storm  */
#define OVERLAY_MS         2000   /* recovery animation duration                    */

static inline long syscall(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
}

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void print(const char* s) { syscall(SYS_WRITE, 1, (long)s, slen(s)); }
static void print_num(long v) {
    char b[24]; int i = 0;
    if (v < 0) { print("-"); v = -v; }
    do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
    while (i > 0) { char c = b[--i]; syscall(SYS_WRITE, 1, (long)&c, 1); }
}
static long now_ms(void) { return syscall(SYS_GET_TICKS_MS, 0, 0, 0); }
static void msleep(long ms) { syscall(SYS_SLEEP, ms, 0, 0); }

void _start(void) {
    print("CWATCHDOG: watching (poll="); print_num(POLL_MS);
    print("ms stall="); print_num(STALL_MS); print("ms)\n");

    /* Attach the heartbeat page LOOKUP-ONLY (init created+owns it).  Retry: init
     * creates it before spawning us, but tolerate any startup skew. */
    /* Lookup-only: real size + NO IPC_CREAT. size==0 is rejected by this kernel
     * (shm.c:275); with a valid size and the key present it returns the existing
     * segment without creating, so we never become the owner. */
    long id = -1;
    for (int t = 0; t < ATTACH_RETRIES && id < 0; t++) {
        id = syscall(SYS_SHMGET, (long)SELFHEAL_SHM_KEY, (long)SELFHEAL_SHM_SIZE, 0);
        if (id < 0) msleep(POLL_MS);
    }
    if (id < 0) { print("CWATCHDOG: FAIL no heartbeat segment\n"); for (;;) msleep(1000); }
    long addr = syscall(SYS_SHMAT, id, 0, 0);
    if (addr <= 0) { print("CWATCHDOG: FAIL shmat r="); print_num(addr); print("\n");
                     for (;;) msleep(1000); }
    volatile sh_heartbeat_t* hb = (volatile sh_heartbeat_t*)addr;

    /* circuit-breaker: timestamps of recent recoveries (sliding 60s window) */
    long rec_times[BREAKER_MAX + 1];
    int  rec_n = 0;

    unsigned long long last_fc = 0;
    long last_change = now_ms();
    int  seeded = 0;

    for (;;) {
        msleep(POLL_MS);
        long t = now_ms();

        if (hb->magic != SELFHEAL_MAGIC) { continue; }   /* not published yet */
        unsigned long long fc = hb->frame_counter;

        if (!seeded) { last_fc = fc; last_change = t; seeded = 1; continue; }
        if (fc != last_fc) { last_fc = fc; last_change = t; continue; }   /* healthy */
        if (t - last_change < STALL_MS) continue;                        /* stalled, not long enough */

        /* ---------- FREEZE DETECTED ---------- */
        print("CWATCHDOG: heartbeat stalled frame="); print_num((long)fc);
        print(" for "); print_num(t - last_change); print("ms\n");

        /* circuit breaker: keep only timestamps inside the window, then check the rate */
        int kept = 0;
        for (int i = 0; i < rec_n; i++)
            if (t - rec_times[i] <= BREAKER_WINDOW) rec_times[kept++] = rec_times[i];
        rec_n = kept;
        if (rec_n >= BREAKER_MAX) {
            print("CWATCHDOG: FAIL recovery storm ("); print_num(BREAKER_MAX);
            print("+ recoveries in 60s) -- giving up\n");
            for (;;) msleep(1000);
        }
        rec_times[rec_n++] = t;

        /* 1. visible recovery animation (kernel draws it from its own FB mapping;
         *    blocks us ~OVERLAY_MS, which is fine — the desktop is already frozen) */
        syscall(SYS_RECOVERY_OVERLAY, 0, OVERLAY_MS, 0);
        print("CWATCHDOG: recovery overlay fired\n");

        /* 2. kill the frozen compositor; init respawns sbin/compositor */
        int cpid = (int)hb->compositor_pid;
        if (cpid > 0) {
            syscall(SYS_KILL, cpid, SIGTERM, 0);
            msleep(300);
            if (hb->frame_counter == last_fc)            /* still stuck? force it */
                syscall(SYS_KILL, cpid, SIGKILL, 0);
            print("CWATCHDOG: killed compositor pid="); print_num(cpid); print("\n");
        } else {
            print("CWATCHDOG: WARN no compositor_pid in heartbeat\n");
        }

        /* 3. prove recovery: the respawned compositor advances the beat (or takes
         *    a new pid).  Either signals a fresh, live instance. */
        long deadline = now_ms() + RESUME_TIMEOUT_MS;
        int resumed = 0;
        while (now_ms() < deadline) {
            msleep(POLL_MS);
            if (hb->magic == SELFHEAL_MAGIC &&
                ((int)hb->compositor_pid != cpid || hb->frame_counter != last_fc)) {
                resumed = 1; break;
            }
        }
        if (resumed) print("CWATCHDOG: PASS respawned (heartbeat resumed)\n");
        else         print("CWATCHDOG: FAIL no resume within timeout\n");

        /* re-seed so we keep guarding the freshly respawned compositor */
        seeded = 0;
    }
}

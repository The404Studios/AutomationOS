/*
 * service.c -- AutomationOS background service template
 *
 * WHAT THIS IS
 * ------------
 * A headless daemon: no window, no framebuffer.  It wakes up every
 * HEARTBEAT_MS milliseconds, does periodic work, yields the CPU so
 * other processes can run, then loops.  After MAX_BEATS heartbeats it
 * exits cleanly.  Clone this and replace the TODO comments to write
 * your own system service.
 *
 * HOW TO COMPILE (on-device IDE, Build key)
 * ------------------------------------------
 * The IDE's on-device cc compiles a single .c file to a static ELF64.
 * Supported builtins: sys_write(fd, buf, len) and sys_exit(code).
 * For SYS_YIELD and SYS_GET_TICKS_MS we inline the raw syscall
 * instruction in small helper functions (gcc path) or use the
 * equivalent busy-delay (cc path -- see #ifdef below).
 *
 * SYSCALLS USED
 * -------------
 *   SYS_EXIT         (0)  -- terminate the process
 *   SYS_WRITE        (3)  -- write bytes to fd 1 (stdout / serial log)
 *   SYS_OPEN         (4)  -- open a VFS file for appending the log
 *   SYS_CLOSE        (5)  -- close the log file descriptor
 *   SYS_GET_TICKS_MS (40) -- monotonic millisecond clock since boot
 *   SYS_YIELD        (15) -- cooperatively yield the CPU timeslice
 *
 * CALLING CONVENTION (AOS / SysV x86-64)
 * ----------------------------------------
 *   syscall number in rax; args in rdi rsi rdx r10 r8 r9; result in rax.
 */

/* ------------------------------------------------------------------ */
/* AOS syscall numbers (match kernel/include/syscall.h)               */
/* ------------------------------------------------------------------ */
#define SYS_EXIT         0
#define SYS_WRITE        3
#define SYS_OPEN         4
#define SYS_CLOSE        5
#define SYS_YIELD        15
#define SYS_GET_TICKS_MS 40

/* ------------------------------------------------------------------ */
/* Service tuning -- change these for your own daemon                 */
/* ------------------------------------------------------------------ */
#define SERVICE_NAME   "mysvc"   /* appears in every log line         */
#define HEARTBEAT_MS   500       /* wake up every 500 ms              */
#define MAX_BEATS      20        /* exit cleanly after this many beats */
#define LOG_PATH       "/var/log/mysvc.log"  /* VFS log file          */

/* ------------------------------------------------------------------ */
/* Tiny string helpers (no libc -- freestanding)                      */
/* ------------------------------------------------------------------ */

/* Return length of NUL-terminated string. */
static int slen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Write a decimal integer into buf (must be >=20 bytes).  Returns chars. */
static int itoa10(unsigned long v, char *buf) {
    char tmp[20];
    int  i = 0, j = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v) { tmp[i++] = '0' + (int)(v % 10); v /= 10; }
    while (i-- > 0) buf[j++] = tmp[i];  /* reverse */
    buf[j] = 0;
    return j;
}

/* Copy src into dst, return pointer past the last byte written. */
static char *scopy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    return dst;
}

/* ------------------------------------------------------------------ */
/* Raw syscall wrapper (gcc / clang path)                              */
/* The on-device cc uses sys_write / sys_exit builtins directly and   */
/* does not need this -- but it is harmless to include.               */
/* ------------------------------------------------------------------ */
static long aos_syscall3(long nr, long a1, long a2, long a3) {
    long ret;
    __asm__ __volatile__(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static long aos_syscall1(long nr, long a1) {
    return aos_syscall3(nr, a1, 0, 0);
}

static long aos_syscall0(long nr) {
    return aos_syscall3(nr, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Convenience wrappers around individual syscalls                     */
/* ------------------------------------------------------------------ */

/* Write `len` bytes from `buf` to file descriptor `fd`. */
static void svc_write(int fd, const char *buf, int len) {
    /* sys_write is also a cc builtin: sys_write(fd, buf, len) */
    aos_syscall3(SYS_WRITE, fd, (long)buf, len);
}

/* Return monotonic milliseconds since boot. */
static unsigned long svc_ticks_ms(void) {
    long v = aos_syscall0(SYS_GET_TICKS_MS);
    return (unsigned long)(v < 0 ? 0 : v);
}

/* Yield remaining CPU timeslice (cooperative multitasking). */
static void svc_yield(void) {
    aos_syscall0(SYS_YIELD);
}

/* Open a VFS path for appending (O_WRONLY|O_CREAT|O_APPEND = 0x0441). */
static int svc_open_log(const char *path) {
    /* TODO: open your device or data file here instead of a log.
     * Flags: O_WRONLY=1, O_CREAT=0x40, O_APPEND=0x400  -> 0x441        */
    return (int)aos_syscall3(SYS_OPEN, (long)path, 0x441, 0600);
}

/* Close a file descriptor. */
static void svc_close(int fd) {
    aos_syscall1(SYS_CLOSE, fd);
}

/* Terminate the process with the given exit code. */
static void svc_exit(int code) {
    /* sys_exit is also a cc builtin: sys_exit(code) */
    aos_syscall1(SYS_EXIT, code);
    while (1) {}   /* satisfy noreturn path for gcc */
}

/* ------------------------------------------------------------------ */
/* Busy-wait until `deadline_ms` (burns CPU -- only used as fallback  */
/* when the ticks syscall is unavailable in early-boot environments).  */
/* Normally prefer svc_yield() + checking svc_ticks_ms().             */
/* ------------------------------------------------------------------ */
static void svc_busy_wait(unsigned long deadline_ms) {
    while (svc_ticks_ms() < deadline_ms) {
        svc_yield();   /* be cooperative even during the wait */
    }
}

/* ------------------------------------------------------------------ */
/* Logging helpers                                                     */
/* ------------------------------------------------------------------ */

/* Build and emit one log line to fd (stdout=1 or a file).
 * Format: "[SERVICE_NAME] tick N at T ms\n"                          */
static void log_heartbeat(int fd, int beat, unsigned long t_ms) {
    char buf[128];
    char num[20];
    char *p = buf;

    p = scopy(p, "[");
    p = scopy(p, SERVICE_NAME);
    p = scopy(p, "] tick ");
    itoa10((unsigned long)beat, num);
    p = scopy(p, num);
    p = scopy(p, " at ");
    itoa10(t_ms, num);
    p = scopy(p, num);
    p = scopy(p, " ms\n");

    svc_write(fd, buf, (int)(p - buf));
}

/* ------------------------------------------------------------------ */
/* Startup / shutdown banners                                          */
/* ------------------------------------------------------------------ */

static void log_banner(int fd, const char *msg) {
    char buf[80];
    char *p = buf;
    p = scopy(p, "[");
    p = scopy(p, SERVICE_NAME);
    p = scopy(p, "] ");
    p = scopy(p, msg);
    p = scopy(p, "\n");
    svc_write(fd, buf, (int)(p - buf));
}

/* ------------------------------------------------------------------ */
/* Service initialisation                                              */
/* Called once before the main loop.  Open devices, files, etc. here. */
/* ------------------------------------------------------------------ */
static int service_init(void) {
    /* TODO: open your device / shared-memory segment / IPC channel here.
     * Return 0 on success, non-zero to abort startup.                  */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-heartbeat work                                                  */
/* This is the core of your service.  Keep it short and non-blocking. */
/* If you need to do slow I/O, split it across multiple beats or use  */
/* a non-blocking open + SYS_EPOLL_WAIT (SYS #73-#75).               */
/* ------------------------------------------------------------------ */
static void service_tick(int beat, unsigned long t_ms) {
    (void)beat; (void)t_ms;
    /* TODO: do your periodic work here.
     * Examples:
     *   - poll a hardware register via SYS_IOCTL (36)
     *   - read a message queue via SYS_MSGRCV   (24)
     *   - write a sensor sample to a VFS file
     *   - post a desktop notification via SYS_NOTIFY (65)           */
}

/* ------------------------------------------------------------------ */
/* Service shutdown                                                    */
/* Close file descriptors, flush buffers, release resources.          */
/* ------------------------------------------------------------------ */
static void service_shutdown(void) {
    /* TODO: close your device / file / IPC handles here. */
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                    */
/* ------------------------------------------------------------------ */
int main(void) {
    /* --- 1. Initialise the service --- */
    if (service_init() != 0) {
        /* Write to stderr (fd 2) so the shell can see the error. */
        const char *emsg = "[" SERVICE_NAME "] init failed, aborting\n";
        svc_write(2, emsg, slen(emsg));
        svc_exit(1);
    }

    /* --- 2. Open a persistent log file via VFS ---
     * If the open fails (negative fd) we fall back to stdout (fd 1).
     * TODO: change LOG_PATH to wherever your service should write.   */
    int log_fd = svc_open_log(LOG_PATH);
    if (log_fd < 0) {
        log_fd = 1;   /* fall back to stdout */
    }

    log_banner(log_fd, "starting");

    /* --- 3. Main service loop --- */
    int  beat       = 0;
    unsigned long next_wake = svc_ticks_ms() + HEARTBEAT_MS;

    while (beat < MAX_BEATS) {

        /* Yield the CPU until our next scheduled wake time.
         * svc_yield() returns immediately; the scheduler will
         * run other processes while we busy-wait cooperatively.  */
        svc_busy_wait(next_wake);
        next_wake += HEARTBEAT_MS;
        beat++;

        /* Do the periodic work for this tick. */
        service_tick(beat, svc_ticks_ms());

        /* Write a heartbeat line so the system knows we're alive.
         * Remove or replace this with your own logging as needed.  */
        log_heartbeat(log_fd, beat, svc_ticks_ms());
    }

    /* --- 4. Shutdown --- */
    log_banner(log_fd, "stopping");
    service_shutdown();

    if (log_fd != 1) {
        svc_close(log_fd);
    }

    /* Exit code 0 = clean shutdown. init / servicemanager can restart
     * us if we exit non-zero (policy depends on your service manifest). */
    svc_exit(0);
    return 0;   /* unreachable; satisfies the compiler */
}

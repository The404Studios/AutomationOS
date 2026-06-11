/*
 * bklstorm.c -- SMP-H1 BKL-LITE: the 60-second two-CPU syscall storm.
 * =====================================================================
 *
 * TWO instances run concurrently (kernel.c spawns both under SMP_BKL):
 *   - one pinned to CPU1 (PINNED_RT, the cpu1hello placement pattern)
 *   - one unpinned (NORMAL -> home CPU0 via the funnel)
 * Each hammers the MARKED syscall groups (FS reads/stat/readdir, IPC shm
 * churn with pattern verification, clipboard) for 60 wall-clock seconds,
 * yielding every 64 iterations (cooperative kernel -- the desktop must stay
 * alive). The shm pattern verify is the corruption detector: a cross-CPU
 * heap/slab/shm race under the un-serialized old world shows up as a
 * pattern mismatch or a kernel invariant/panic; under the BKL it must not.
 *
 * Output: ONE line at completion (counted by scripts/bkl_smoke.sh):
 *   BKLSTORM pid=<p> done iters=<n> errors=<e> secs=<s>
 * Two such lines with errors=0 secs>=60 = syscall_storm/duration/corruption
 * gates. Which CPU each pid ran on is proven kernel-side (the funnel's
 * '[SCHED] submit:' narration + the [BKL] engaged counters).
 *
 * NO libc, NO stdio (the cpu1hello house pattern; crt0 -> SYS_EXIT).
 */

#define SYS_READ          2
#define SYS_WRITE         3
#define SYS_OPEN          4
#define SYS_CLOSE         5
#define SYS_GETPID        8
#define SYS_YIELD        15
#define SYS_SHMGET       18
#define SYS_SHMAT        19
#define SYS_SHMDT        20
#define SYS_OPENDIR      30
#define SYS_READDIR      31
#define SYS_CLOSEDIR     32
#define SYS_STAT         33
#define SYS_GET_TICKS_MS 40
#define SYS_CLIP_SET     63
#define SYS_CLIP_GET     64

#define STORM_MS         60000UL    /* the 60 s acceptance duration */
#define SHM_BYTES        4096

static inline long sc(long n, long a1, long a2, long a3)
{
    long r;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return r;
}

static void print(const char *m)
{
    unsigned long len = 0;
    while (m[len]) len++;
    sc(SYS_WRITE, 1, (long)m, (long)len);
}

static char *u2s(char *p, unsigned long v)
{
    char tmp[24];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = '0' + (v % 10); v /= 10; }
    while (i) *p++ = tmp[--i];
    return p;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    long pid   = sc(SYS_GETPID, 0, 0, 0);
    long start = sc(SYS_GET_TICKS_MS, 0, 0, 0);

    unsigned long iters  = 0;
    unsigned long errors = 0;

    /* per-instance shm key + fill byte derived from the pid so the two
     * storms never share a segment but DO churn the same kernel allocator */
    long shmkey = 0x424B4C00L + pid;
    unsigned char fill = (unsigned char)(0xA0 ^ (pid & 0xFF));

    char fbuf[64];
    char clip[32];

    for (;;) {
        long now = sc(SYS_GET_TICKS_MS, 0, 0, 0);
        if (now - start >= (long)STORM_MS) break;

        /* ---- FS group: open/read/close + stat on a known initrd file ---- */
        long fd = sc(SYS_OPEN, (long)"/etc/toolset0.txt", 0, 0);
        if (fd >= 0) {
            long n = sc(SYS_READ, fd, (long)fbuf, sizeof(fbuf));
            if (n <= 0) errors++;
            sc(SYS_CLOSE, fd, 0, 0);
        }
        /* stat return is environment-dependent; exercise, don't grade */
        sc(SYS_STAT, (long)"/etc/toolset0.txt", (long)fbuf, 0);

        /* ---- FS group: a short readdir walk ---- */
        long d = sc(SYS_OPENDIR, (long)"/etc", 0, 0);
        if (d >= 0) {
            for (int k = 0; k < 4; k++) {
                if (sc(SYS_READDIR, d, (long)fbuf, sizeof(fbuf)) != 0) break;
            }
            sc(SYS_CLOSEDIR, d, 0, 0);
        }

        /* ---- IPC group: shm churn + the CORRUPTION DETECTOR ---- */
        long shmid = sc(SYS_SHMGET, shmkey, SHM_BYTES, 1 /*create*/);
        if (shmid >= 0) {
            unsigned char *p = (unsigned char *)sc(SYS_SHMAT, shmid, 0, 0);
            if ((long)p > 0) {
                for (int k = 0; k < SHM_BYTES; k += 64) p[k] = fill;
                for (int k = 0; k < SHM_BYTES; k += 64) {
                    if (p[k] != fill) { errors++; break; }
                }
                sc(SYS_SHMDT, (long)p, 0, 0);
            }
        }

        /* ---- IPC group: clipboard ping (two storms deliberately race
         * this shared resource; return codes only -- content belongs to
         * whoever wrote last) ---- */
        clip[0] = 'B'; clip[1] = 'K'; clip[2] = 'L';
        clip[3] = (char)('0' + (pid % 10)); clip[4] = 0;
        sc(SYS_CLIP_SET, (long)clip, 5, 0);
        sc(SYS_CLIP_GET, (long)clip, sizeof(clip), 0);

        iters++;
        if ((iters & 63) == 0) {
            sc(SYS_YIELD, 0, 0, 0);   /* cooperative fairness */
        }
    }

    long secs = (sc(SYS_GET_TICKS_MS, 0, 0, 0) - start) / 1000;

    char line[96];
    char *q = line;
    const char *s = "BKLSTORM pid=";          while (*s) *q++ = *s++;
    q = u2s(q, (unsigned long)pid);
    s = " done iters=";                       while (*s) *q++ = *s++;
    q = u2s(q, iters);
    s = " errors=";                           while (*s) *q++ = *s++;
    q = u2s(q, errors);
    s = " secs=";                             while (*s) *q++ = *s++;
    q = u2s(q, (unsigned long)secs);
    *q++ = '\n'; *q = 0;
    print(line);

    return (errors == 0) ? 0 : 1;
}

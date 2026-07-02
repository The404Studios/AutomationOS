/*
 * renderworker.c -- SMP-RENDER-0: the CPU1 scan worker.
 * ======================================================
 *
 * A plain BATCH-class ring-3 process (dsplit allowlist -> the seam routes it
 * to CPU1). It attaches the compositor's job page + scene buffers and, for
 * every `gen` bump, runs the phase-1 back-vs-prev dirty scan over its band
 * [y0,y1) and publishes the partial bbox. It READS back/prev and writes ONLY
 * its result fields in the job page -- never the framebuffer, never the
 * scene. If the compositor never shows up it exits cleanly; if IT dies, the
 * compositor's join timeout falls back to solo scans (correctness never
 * depends on this process).
 *
 * NO libc, NO stdio (the batchdemo/cpu1hello house pattern; crt0 -> SYS_EXIT).
 * Serial evidence: "[RENDERWORKER] ready" once attached.
 */

#include "../../compositor/smprender.h"

#define SYS_WRITE   3
#define SYS_YIELD  15
#define SYS_SHMGET 18
#define SYS_SHMAT  19

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

/* Bounded attach helper: the compositor may not have created the page yet
 * (spawn order), so retry with yields; give up cleanly after the cap. */
static volatile smpr_job_t *attach_job_page(void)
{
    for (long tries = 0; tries < 200000; tries++) {
        long id = sc(SYS_SHMGET, (long)SMPR_JOB_KEY, (long)SMPR_JOB_BYTES, 0);
        if (id >= 0) {
            long p = sc(SYS_SHMAT, id, 0, 0);
            if (p > 0) {
                volatile smpr_job_t *job = (volatile smpr_job_t *)p;
                if (job->magic == SMPR_JOB_MAGIC) return job;
            }
        }
        sc(SYS_YIELD, 0, 0, 0);
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    volatile smpr_job_t *job = attach_job_page();
    if (!job) { print("[RENDERWORKER] no job page -- exiting\n"); return 1; }

    long bp = sc(SYS_SHMAT, (long)job->back_shm_id, 0, 0);
    long pp = sc(SYS_SHMAT, (long)job->prev_shm_id, 0, 0);
    if (bp <= 0 || pp <= 0) { print("[RENDERWORKER] shmat failed -- exiting\n"); return 1; }
    const smpr_u32 *back = (const smpr_u32 *)bp;
    const smpr_u32 *prev = (const smpr_u32 *)pp;

    job->worker_ready = SMPR_JOB_MAGIC;
    print("[RENDERWORKER] ready\n");

    unsigned long idle = 0;
    for (;;) {
        smpr_u32 g = job->gen;
        if (g != job->done_gen) {
            smpr_u32 mnx, mny, mxx, mxy;
            int any = smpr_scan_band(back, prev, job->w, job->stride,
                                     job->y0, job->y1,
                                     &mnx, &mny, &mxx, &mxy);
            job->r_minx = mnx; job->r_miny = mny;
            job->r_maxx = mxx; job->r_maxy = mxy;
            job->r_any  = (smpr_u32)any;
            job->frames = job->frames + 1;
            job->done_gen = g;            /* publish LAST (x86 TSO) */
            idle = 0;
        } else if (((++idle) & 2047ul) == 0) {
            /* CPU1 is otherwise idle (BATCH world), but stay a cooperative
             * citizen: yield periodically so kernel housekeeping runs. */
            sc(SYS_YIELD, 0, 0, 0);
        }
    }
    return 0;   /* unreached */
}

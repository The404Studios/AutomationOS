/*
 * smprender.h -- SMP-RENDER-0: compositor <-> renderworker protocol.
 * ===================================================================
 *
 * Multicore rendering, first brick. The compositor delegates the TOP band of
 * present_diff()'s phase-1 back-vs-prev scan to `sbin/renderworker`, a BATCH
 * process the DESKTOP-SPLIT seam routes to CPU1. Sharing surface is minimal
 * and read-mostly:
 *
 *   - this JOB PAGE (one SysV SHM segment, key SMPR_JOB_KEY): geometry, the
 *     scene-buffer shm ids, the per-frame band job, and the worker's partial
 *     dirty-bbox result;
 *   - the back + prev scene buffers (SysV SHM under COMP_SMP_RENDER instead
 *     of anonymous mmap) -- the worker READS both, writes NEITHER;
 *   - the framebuffer is NEVER touched by the worker (phase 2 stays wholly
 *     on the compositor/CPU0).
 *
 * Handshake (x86-TSO + volatile program-order stores; single writer per
 * field group):
 *   compositor: fill y0/y1 -> bump `gen` LAST -> scan its own band -> spin
 *               on done_gen==gen (iteration-capped; on timeout scan the
 *               worker's band itself and stop delegating -- correctness
 *               never depends on the worker).
 *   worker:     spin on gen!=done_gen (yielding) -> scan [y0,y1) -> write
 *               r_* -> publish done_gen LAST.
 *
 * One shared scan routine (smpr_scan_band) is the single source of truth so
 * the split scan is byte-equivalent to the solo scan by construction; the
 * boot selftest still PROVES it (solo vs split bbox identity on synthetic
 * patterns spanning both bands).
 */
#ifndef SMPRENDER_H
#define SMPRENDER_H

#define SMPR_JOB_KEY    0x524E4430u   /* "RND0" -- the job page             */
#define SMPR_BACK_KEY   0x524E4231u   /* "RNB1" -- back scene buffer        */
#define SMPR_PREV_KEY   0x524E4232u   /* "RNB2" -- prev scene buffer        */
#define SMPR_JOB_MAGIC  0x534D5052u   /* "SMPR" -- job page is live         */
#define SMPR_JOB_BYTES  4096

typedef unsigned int smpr_u32;

typedef struct {
    volatile smpr_u32 magic;        /* compositor: page initialized (LAST)  */
    volatile smpr_u32 worker_ready; /* worker: == SMPR_JOB_MAGIC when live  */
    volatile int      back_shm_id;  /* scene buffers (worker attaches RO-use) */
    volatile int      prev_shm_id;
    volatile smpr_u32 w, h, stride; /* pixel geometry (stride in PIXELS)    */
    /* per-frame job: scan band [y0, y1) */
    volatile smpr_u32 y0, y1;
    volatile smpr_u32 gen;          /* compositor bumps LAST                */
    /* worker's partial result for job `done_gen` */
    volatile smpr_u32 r_minx, r_miny, r_maxx, r_maxy, r_any;
    volatile smpr_u32 done_gen;     /* worker publishes LAST                */
    volatile smpr_u32 frames;       /* worker: total jobs completed (stat)  */
} smpr_job_t;

/* Phase-1 dirty-bbox scan of back vs prev over rows [y0,y1) -- THE scan both
 * sides run. Returns 1 if any pixel differs; bbox outputs only valid then. */
static inline int smpr_scan_band(const smpr_u32 *back, const smpr_u32 *prev,
                                 smpr_u32 w, smpr_u32 stride,
                                 smpr_u32 y0, smpr_u32 y1,
                                 smpr_u32 *minx, smpr_u32 *miny,
                                 smpr_u32 *maxx, smpr_u32 *maxy)
{
    smpr_u32 mnx = 0xFFFFFFFFu, mny = 0xFFFFFFFFu, mxx = 0, mxy = 0;
    int any = 0;
    for (smpr_u32 y = y0; y < y1; y++) {
        smpr_u32 off = y * stride;
        for (smpr_u32 x = 0; x < w; x++) {
            if (back[off + x] != prev[off + x]) {
                if (x < mnx) mnx = x;
                if (x > mxx) mxx = x;
                if (y < mny) mny = y;
                if (y > mxy) mxy = y;
                any = 1;
            }
        }
    }
    *minx = mnx; *miny = mny; *maxx = mxx; *maxy = mxy;
    return any;
}

#endif /* SMPRENDER_H */

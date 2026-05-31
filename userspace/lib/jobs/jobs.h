/*
 * userspace/lib/jobs/jobs.h -- a tiny userspace job queue with worker threads.
 * ===========================================================================
 *
 * A bounded MPMC ring buffer of {fn,arg} jobs plus N worker threads. Work is
 * SUBMITTED to the queue; workers PULL jobs and run them on memory shared with
 * the submitter (threads share the address space -- see userspace/libc/thread.h);
 * the submitter DRAINS (blocks until every submitted job has completed). This is
 * the "compute coordination layer": a correct, reusable primitive for handing a
 * pile of independent work to a pool of consumers.
 *
 * **NOT A SPEEDUP.** AutomationOS is SINGLE-CORE. N worker threads time-share the
 * ONE cpu; they do not run simultaneously. Dispatching a matmul through this
 * queue is therefore NOT faster than computing it inline on the calling thread --
 * it is in fact slightly SLOWER (queue + futex + context-switch overhead). The
 * value here is the COORDINATION MACHINERY, proven correct: submit -> pull ->
 * compute-on-shared-memory -> collect, with the threaded result bit-identical to
 * a single-threaded reference. Real N-way parallel SPEEDUP only arrives with SMP
 * (multiple cpus running workers at the same time) -- a separate, later project.
 *
 * SYNCHRONIZATION (every wait BLOCKS via futex; NO spin loops -- the default
 * kernel is cooperative, so a spin-wait would never yield and would deadlock):
 *   - a short futex mutex guards the ring's head/tail (the long part -- running
 *     the job -- is OUTSIDE the lock);
 *   - workers block on a monotonic `submitted` counter when the queue is empty
 *     (submit bumps it + wakes a worker); waiting on the exact observed value
 *     means a concurrent submit makes FUTEX_WAIT return EAGAIN instantly -> no
 *     lost wakeup;
 *   - drain blocks on a monotonic `completed` counter (each finished job bumps it
 *     + wakes the drainer), same no-lost-wakeup property.
 *
 * USAGE:
 *   jobsys_init(2);                       // spawn 2 workers
 *   for (...) jobsys_submit(my_fn, arg);  // hand work to the pool
 *   jobsys_drain();                       // block until all jobs done
 *   jobsys_shutdown();                    // stop + join every worker (no orphans)
 */

#ifndef LIB_JOBS_H
#define LIB_JOBS_H

/* A unit of work: run fn(arg) on a worker thread. */
typedef void (*job_fn_t)(void* arg);
typedef struct {
    job_fn_t fn;
    void*    arg;
} job_t;

/* Spawn n_workers worker threads (clamped to [1, JOBSYS_MAX_WORKERS]). Returns
 * the number of workers actually started (>0) or a negative value on failure.
 * Must be called once before any submit/drain. */
int  jobsys_init(int n_workers);

/* Enqueue fn(arg). Blocks only if the ring is momentarily full (waits on a slot
 * via futex -- not a hard error, just back-pressure). Wakes one worker. */
void jobsys_submit(job_fn_t fn, void* arg);

/* Block (futex) until every submitted job has completed (completed == submitted). */
void jobsys_drain(void);

/* Signal stop, wake all workers, and JOIN every worker thread so none is left
 * orphaned (and the shared address space is not leaked). After this returns the
 * job system is dormant; call jobsys_init() again to restart. */
void jobsys_shutdown(void);

#endif /* LIB_JOBS_H */

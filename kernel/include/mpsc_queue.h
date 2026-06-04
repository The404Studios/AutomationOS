#ifndef MPSC_QUEUE_H
#define MPSC_QUEUE_H
/* ===========================================================================
 * mpsc_queue.h — Lock-free multi-producer, single-consumer queue (SMP brick 9)
 *
 * PURPOSE
 *   Replace the single global cpu1_job slot (ap_boot.c) with a BOUNDED lock-free
 *   ring that allows MANY producers (BSP + future APs, plus future usermode
 *   syscalls) to dispatch kernel jobs to ONE consumer (CPU1 worker loop) with
 *   NO contention on the hot path and BOUNDED wait-freedom.
 *
 * ALGORITHM — Vyukov bounded MPMC ring, specialized to MPSC
 *   - 256 cells, each with its OWN 64-bit sequence number that is BOTH the ABA
 *     version tag AND the publish/consume gate. No raw (head < tail) compare —
 *     that is the classic index-only ring bug this design avoids.
 *   - Two 64-bit monotonic positions: enq_pos (producer CAS cursor), deq_pos
 *     (consumer-only cursor). Indexing is (pos & MPSC_QUEUE_MASK).
 *   - A cell seq field determines whether it is ready for enqueue (seq == pos),
 *     ready for dequeue (seq == pos + 1), or neither (full or stale).
 *
 * CONCURRENCY PROPERTIES
 *   - SYSTEM WAIT-FREE for producers: a producer only retries while OTHER
 *     producers make progress. It never blocks. If the ring is full, it returns
 *     -EAGAIN immediately (the job is NOT dropped or clobbered).
 *   - LOCK-FREE for the consumer: exactly one consumer (CPU1), no CAS needed on
 *     dequeue path (just atomic loads/stores). Single-consumer dequeue is ALWAYS
 *     lock-free, never blocks.
 *   - ZERO locks, NO spin_lock calls. The only atomic ops are CAS on enq_pos and
 *     acquire/release loads/stores of cell.seq.
 *
 * CELL SIZE — 128 bytes (two cache lines)
 *   Why: ownership_t is 40 bytes on this toolchain (it embeds a 16-byte spinlock_t),
 *   NOT the 24 bytes the ownership.h comment originally claimed. Splitting the
 *   ownership descriptor off-cell would reintroduce false sharing (producers
 *   racing on a shared descriptor array), so each cell EMBEDS its arg_own by value.
 *
 *   Layout:
 *     seq8 + fn8 + arg8 + owner_pid4 + _pad04 = 32 bytes
 *     + arg_own40 = 72 bytes
 *     + _pad1[56] = 128 bytes (two 64-byte cache lines)
 *
 *   The 256-cell ring is 32 KiB total (.bss). Each cell is NATURALLY 128-byte
 *   aligned (modulo alignment of the ring array itself), so the padding arithmetic
 *   is compile-time verified by a sizeof assertion in the header.
 *
 * FALSE-SHARING AVOIDANCE
 *   - enq_pos, deq_pos, and ring each start on SEPARATE cache lines:
 *       - enq_pos lives at offset 0 (producers CAS here)
 *       - deq_pos lives at offset 64 (consumer bumps here)
 *       - ring starts at offset 128 (no contention with the cursors)
 *   - Because each cell is 128 bytes, cells NEVER share a line with each other.
 *   - The producer pack (many CPUs) and the lone consumer NEVER share a line.
 *
 * FAILURE MODES
 *   - mpsc_enqueue returns -EAGAIN (not 0) when the ring is FULL. The job is NOT
 *     submitted; the caller must decide whether to retry, drop, or run inline.
 *     The cell that would have held the job is NOT clobbered (the seq check fails
 *     BEFORE writing).
 *   - The consumer NEVER blocks. mpsc_dequeue returns 0 (EMPTY) or 1 (job dequeued).
 *
 * SAFETY
 *   - The arg ownership state is TRANSFERRED from the producer CPU to CPU1 on
 *     enqueue success. The producer MUST NOT touch arg after enqueue returns 0.
 *   - On process exit, mpsc_orphan_pid walks the ring and transitions matching
 *     owner_pid cells from TRANSFERRED -> ORPHANED (mirrors cpu1_orphan_jobs).
 *   - Each cell arg_own is own_init'd to OWNED at mpsc_init time.
 *
 * INTEGRATION POINTS
 *   - This header defines the data structures and API declarations. The
 *     IMPLEMENTATION lives in kernel/core/smp/mpsc_queue.c (SMP brick 9).
 *   - Nothing in the boot path includes this header yet — it is the vetted
 *     successor to cpu1_job, but switching ap_boot.c over is the NEXT brick.
 *   - The current cpu1_job slot (ap_boot.c) remains the active dispatch until
 *     brick 9 lands AND passes smoke tests.
 * ===========================================================================
 */

#include "types.h"
#include "ownership.h"

/* ---------------------------------------------------------------------------
 * CONFIGURATION — Ring size + alignment
 * ------------------------------------------------------------------------- */
#define MPSC_QUEUE_SIZE      256u                   /* MUST be power-of-two         */
#define MPSC_QUEUE_MASK      (MPSC_QUEUE_SIZE - 1)  /* Index mask: & 0xFF           */
#define MPSC_CELL_BYTES      128u                   /* Two cache lines (64*2)       */
#define MPSC_QUEUE_CACHELINE 64u                    /* x86-64 cache line size       */

/* ---------------------------------------------------------------------------
 * JOB FUNCTION TYPE — same signature as cpu1_job (ap_boot.c)
 * ------------------------------------------------------------------------- */
typedef void (*cpu_job_fn)(void *);

/* ---------------------------------------------------------------------------
 * CELL STRUCTURE — 128 bytes, naturally aligned to two cache lines
 *
 * Fields:
 *   seq         — 64-bit sequence number: the ABA tag AND the publish/consume gate.
 *                 Determines whether the cell is ready for enqueue (seq == pos),
 *                 ready for dequeue (seq == pos+1), or neither (full/stale).
 *   fn          — Kernel function pointer (never user code). Same type as cpu1_job.
 *   arg         — Opaque pointer argument passed to fn. The OWNERSHIP of *arg is
 *                 tracked in arg_own below.
 *   owner_pid   — PID of the process that enqueued this job (for orphan cleanup
 *                 on process exit). 0 = kernel job (no owner process).
 *   _pad0       — Pad to 32 bytes before arg_own.
 *   arg_own     — Ownership descriptor for *arg (40 bytes on this toolchain).
 *                 TRANSFERRED from producer -> CPU1 on enqueue success.
 *   _pad1       — Pad the entire cell to MPSC_CELL_BYTES (128).
 *
 * LAYOUT INVARIANT (compile-time checked):
 *   sizeof(mpsc_cell_t) == MPSC_CELL_BYTES (128).
 *   The assertion lives at the end of this header so it fires on every compile
 *   that includes mpsc_queue.h. If ownership_t grows (e.g. new bookkeeping
 *   fields), the build FAILS loudly and _pad1 must be adjusted.
 * ------------------------------------------------------------------------- */
typedef struct mpsc_cell {
    volatile uint64_t  seq;                         /* ABA tag + publish/consume gate */
    cpu_job_fn         fn;                          /* Kernel function pointer        */
    void              *arg;                         /* Opaque arg (ownership tracked) */
    uint32_t           owner_pid;                   /* Submitting process PID (0=kern)*/
    uint32_t           _pad0;                       /* Align to 32 before arg_own     */
    ownership_t        arg_own;                     /* Ownership state for *arg (40B) */
    uint8_t            _pad1[MPSC_CELL_BYTES - 32 - sizeof(ownership_t)]; /* = 56 */
} mpsc_cell_t;

/* Compile-time layout check (fires on every include of this header). */
typedef char mpsc_cell_layout_check[
    (sizeof(mpsc_cell_t) == MPSC_CELL_BYTES) ? 1 : -1
];

/* ---------------------------------------------------------------------------
 * QUEUE STRUCTURE — producer cursor + consumer cursor + ring (false-share-free)
 *
 * Fields:
 *   enq_pos — Monotonic 64-bit producer cursor. Producers CAS here. Aligned to
 *             its OWN cache line (offset 0) so producer contention never collides
 *             with the consumer.
 *   deq_pos — Monotonic 64-bit consumer cursor (CPU1 only, no CAS). Aligned to
 *             its OWN cache line (offset 64) so the consumer never shares a line
 *             with producers.
 *   ring    — 256 cells, each 128 bytes. Starts at offset 128 (after the two
 *             cursor lines). Total 32 KiB, lives in .bss.
 *
 * ALIGNMENT:
 *   - The entire struct is aligned to 64 bytes (via __attribute__((aligned(64)))).
 *   - enq_pos is at offset 0 (first cache line).
 *   - deq_pos is at offset 64 (second cache line).
 *   - ring starts at offset 128 (third+ cache lines).
 *   - Each cell is 128 bytes, so cells NEVER share lines with each other.
 *
 * FALSE-SHARING PROOF:
 *   - Producers only CAS enq_pos (line 0) and write into ring[enq_pos & MASK]
 *     (lines 2+). They NEVER touch deq_pos (line 1).
 *   - The consumer only bumps deq_pos (line 1) and reads from ring[deq_pos & MASK]
 *     (lines 2+). It NEVER writes enq_pos (line 0).
 *   - Each cell is 128 bytes (two lines), so no two cells share a line.
 *   - Ergo: producer/consumer cursors never collide, and cell accesses never
 *     false-share across cells.
 * ------------------------------------------------------------------------- */
typedef struct mpsc_queue {
    volatile uint64_t  enq_pos __attribute__((aligned(MPSC_QUEUE_CACHELINE))); /* offset 0  */
    volatile uint64_t  deq_pos __attribute__((aligned(MPSC_QUEUE_CACHELINE))); /* offset 64 */
    mpsc_cell_t        ring[MPSC_QUEUE_SIZE] __attribute__((aligned(MPSC_QUEUE_CACHELINE))); /* offset 128 */
} mpsc_queue_t;

/* ---------------------------------------------------------------------------
 * API DECLARATIONS (implementations in kernel/core/smp/mpsc_queue.c)
 *
 * SAFETY CONTRACT:
 *   - All functions are safe to call from ANY CPU (except dequeue, which is
 *     CPU1-only — the API does NOT enforce this statically; the CALLER must
 *     ensure single-consumer discipline).
 *   - Enqueue is SYSTEM WAIT-FREE: a producer only retries while OTHER producers
 *     make progress. If the ring is full, it returns -EAGAIN immediately (the
 *     job is NOT dropped).
 *   - Dequeue is LOCK-FREE (single consumer, no CAS contention).
 *   - The ownership of *arg is TRANSFERRED from the producer to CPU1 on enqueue
 *     success. The producer MUST NOT touch *arg after enqueue returns 0.
 *   - On enqueue failure (-EAGAIN), arg ownership is NOT transferred; the caller
 *     still owns *arg and may retry, drop, or run inline.
 * ------------------------------------------------------------------------- */

/* Initialize the queue: seed each cell seq = i (so the first enqueue at index i
 * sees seq == i, which is the "ready for enqueue" condition), zero the cursors,
 * and own_init each cell arg_own to OWNED. Call once at boot (or whenever a
 * new mpsc_queue_t is allocated). */
void mpsc_init(mpsc_queue_t *q);

/* Enqueue a job: fn(arg). On SUCCESS, returns 0 AND transfers ownership of *arg
 * from the calling CPU to CPU1 (arg_own state: OWNED -> TRANSFERRED). On FAILURE
 * (ring FULL), returns -EAGAIN and does NOT transfer ownership (the caller still
 * owns *arg). owner_pid is the PID of the submitting process (0 for kernel jobs).
 *
 * WAIT-FREE: a producer only retries while OTHER producers make progress. The
 * retry loop is BOUNDED by the ring size (256 cells). If the ring is full, the
 * function returns -EAGAIN immediately (the cell seq check fails BEFORE writing).
 *
 * SAFE FROM ANY CPU (except CPU1 if it tries to enqueue to itself; that works
 * but is silly — CPU1 should just run fn(arg) inline). */
int mpsc_enqueue(mpsc_queue_t *q, cpu_job_fn fn, void *arg, uint32_t owner_pid);

/* Dequeue a job: if a job is READY (cell seq == deq_pos + 1), CAS deq_pos,
 * read the job out, RELEASE-store cell.seq = deq_pos + MASK + 1 (re-arm the
 * cell for its NEXT lap 256 cells later), and return 1. If EMPTY (cell seq !=
 * deq_pos + 1), return 0. On success, *out_fn / *out_arg / *out_pid are
 * populated with the job. The ownership of *arg is ALREADY TRANSFERRED by the
 * enqueuer; the consumer now owns it (arg_own.owner_cpu == cpu_id()).
 *
 * LOCK-FREE (single consumer, no CAS contention). SAFE ONLY FROM CPU1 — the
 * API does not enforce this statically; the CALLER (ap_boot.c worker loop or
 * its successor) is responsible for ensuring EXACTLY ONE consumer. */
int mpsc_dequeue(mpsc_queue_t *q, cpu_job_fn *out_fn, void **out_arg, uint32_t *out_pid);

/* Advisory O(1) snapshot helpers (ACQUIRE-loaded positions). Safe for heuristics
 * ("should I run inline instead?", "how deep is the queue?") but NEVER build
 * your own enqueue/dequeue loop on these — they are SNAPSHOTS, not guarantees.
 * The ring may be full/empty a nanosecond later.
 *
 * mpsc_empty — Returns 1 if the queue APPEARS empty (enq_pos == deq_pos), else 0.
 * mpsc_full  — Returns 1 if the queue APPEARS full (depth >= SIZE), else 0.
 * mpsc_depth — Returns the APPARENT number of jobs in the ring (enq_pos - deq_pos).
 *              Clamped to [0, SIZE]. */
int      mpsc_empty(const mpsc_queue_t *q);
int      mpsc_full(const mpsc_queue_t *q);
uint64_t mpsc_depth(const mpsc_queue_t *q);

/* Orphan cleanup: walk the ENTIRE ring (256 cells) and transition any cell whose
 * arg_own.owner_cpu == pid AND state == TRANSFERRED to ORPHANED. Mirrors the
 * cpu1_orphan_jobs() logic in ap_boot.c. Called on process exit.
 *
 * WHY THE FULL SCAN: a process may have enqueued jobs that are still pending in
 * the ring (not yet dequeued by CPU1). Those jobs arg pointers may reference
 * memory the exiting process owned. We mark them ORPHANED so CPU1 knows not to
 * access the arg after the job runs (or to skip the job entirely if the arg_own
 * state shows it is unsafe).
 *
 * SAFE FROM ANY CPU (the scan takes a full-ring traversal, but it is a rare
 * operation — only on process exit). */
void mpsc_orphan_pid(mpsc_queue_t *q, uint32_t pid);

#endif /* MPSC_QUEUE_H */

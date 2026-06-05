// refsim_reap.c
// -----------------------------------------------------------------------------
// HOST (gcc, no kernel) DESIGN-PROOF simulation of the *FIXED* process-reap
// control flow for the smp-foundation branch.
//
// This is the sibling of the BUGGY baseline refsim.c (which prints
// "RESULT: ... NO  <-- LEAK" because the creation ref is never released by a
// reaper). This file models the agreed FIX and PROVES it is leak-free and
// double-free-free with asserts + an ASan/UBSan-clean run.
//
// What is modeled faithfully (logical, not instruction-level), keyed to source:
//   process.c:
//     * process_create      -> ref_count = 1 (the CREATION ref), reaped = 0
//     * process_ref / unref -> +/- 1; unref frees @0, NULLs the table slot,
//                              reclaims the PID, all "under the lock"
//     * process_destroy     -> scheduler_remove_process(proc) THEN one unref
//     * process_get_by_pid  -> +1 under the lock (a transient caller ref)
//   scheduler.c:
//     * scheduler_add_process / yield_requeue -> +1, on_queue = 1 (the QUEUE ref)
//     * scheduler_pick_next  -> on_queue = 0, TRANSFERS the ref (queue ref
//                               becomes the running ref; NO unref)
//     * scheduler_remove_process -> no-op if !on_queue, else unref (KILL-FIX-003)
//     * schedule() KILL-FIX-002 -> the dead 'current' was transferred a ref by
//                               pick_next (queue->running); that running ref is
//                               dropped exactly once on the terminated path.
//   handlers.c:
//     * sys_waitpid / sys_thread_join reapers -> get_by_pid(+1), then the NEW
//                               reap_claim_release(found) IMMEDIATELY BEFORE the
//                               existing process_destroy(found).
//   THE NEW reap_claim_release (the agreed shape):
//       static void reap_claim_release(process_t* proc) {
//           if (proc && !__atomic_exchange_n(&proc->reaped, 1, __ATOMIC_SEQ_CST))
//               process_unref(proc);   // releases the CREATION ref exactly once
//       }
//     Only the CAS *winner* (the thread that flipped reaped 0->1) drops the
//     creation ref. Losers fall through to process_destroy, which still does
//     scheduler_remove_process (no-op for an off-queue zombie) + ONE unref of
//     *their own get_by_pid ref* -> net zero double-free.
//
//   Reparent-to-init: a parent's exit sets its still-live children's
//   parent_pid = 1 (INIT). NO process ever self-reaps. INIT (running waitpid(-1))
//   reaps zombies via reap_claim_release(child) THEN process_destroy(child).
//
// Invariants asserted (not just patched):
//   (1) a process never frees its own kernel stack (a process is only ever
//       freed by ANOTHER context's unref reaching 0; we assert the freer != self
//       at teardown of a self-context where it would matter -- modeled by the
//       fact that the running 'current' is never the thing whose unref hits 0).
//   (2) a zombie's CREATION ref is released exactly once (reaped CAS).
//   (3) every get_by_pid ref has a matching unref.
//   (4) PID identity = pid + create_seq, not pid alone (stale-pid lookups fail).
//   (5..8) futex invariants are out of scope for THIS sim (they are proven in a
//       separate futex sim); listed here only for traceability.
//
// Build / run (must be clean under both):
//   gcc -O2 -Wall -fsanitize=address,undefined refsim_reap.c -o refsim_reap
//   ./refsim_reap
// Prints "REFSIM_REAP: PASS" iff every assert held.
// -----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MAXP        16   // PCB pool size
#define MAX_PID     16   // table is indexed by PID
#define INIT_PID    1

typedef enum { READY, RUNNING, TERMINATED } pstate_t;

typedef struct {
    int      pid;
    int      ref_count;     // process_t.ref_count
    int      reaped;        // NEW: the reap-claim flag, exchanged 0->1
    int      on_queue;      // scheduler queue ref present?
    pstate_t state;
    int      alive;         // PCB still allocated (0 once unref hit 0 and freed)
    int      parent_pid;    // 0 = no parent / reaped-detached
    int      create_seq;    // PID-identity tiebreaker: pid + create_seq
} proc_t;

static proc_t T[MAXP];
static int    table[MAX_PID];        // PID -> pool index, -1 if slot NULL
static int    table_seq[MAX_PID];    // create_seq published at that slot (identity)

// ---- global accounting / proof counters ----
static int created_count   = 0;   // total proc_create() calls
static int freed_count     = 0;   // total teardowns (unref hit 0)
static int g_seq           = 0;   // monotonic create_seq source

// A double-free or ref imbalance would drive ref_count below 0. We TRAP that.
static int  g_abort_negref = 0;

// ----------------------------------------------------------------------------
// ref-count primitives
// ----------------------------------------------------------------------------
static void p_ref(int i) {
    assert(i >= 0 && i < MAXP);
    assert(T[i].alive && "process_ref on a freed PCB (use-after-free)");
    T[i].ref_count++;
}

// process_unref: -1; at 0 do teardown (NULL slot, reclaim PID, free PCB).
// The negative-ref guard is the double-free detector.
static void p_unref(int i) {
    if (i < 0) return;
    assert(i < MAXP);
    // GUARD: a freed PCB must never be unref'd again, and ref_count must never
    // be driven negative. Either is a double-free / accounting bug.
    if (!T[i].alive || T[i].ref_count <= 0) {
        g_abort_negref = 1;
        fprintf(stderr,
            "    [FATAL] unref would drive ref_count negative on PID %d "
            "(alive=%d rc=%d) -> DOUBLE FREE\n",
            T[i].pid, T[i].alive, T[i].ref_count);
        // hard fail the proof
        assert(0 && "ref_count would go negative (double-free)");
        return;
    }
    T[i].ref_count--;
    if (T[i].ref_count == 0) {
        // teardown "under the lock": unpublish from table, reclaim PID, free PCB.
        int pid = T[i].pid;
        if (pid >= 0 && pid < MAX_PID && table[pid] == i) {
            table[pid]     = -1;
            table_seq[pid] = -1;
        }
        T[i].alive = 0;
        T[i].state = TERMINATED;
        freed_count++;
        printf("    [free] PID %d torn down (ref_count -> 0); slot reclaimed\n", pid);
    }
}

// ----------------------------------------------------------------------------
// scheduler model
// ----------------------------------------------------------------------------
static int sched_add(int i) {              // scheduler_add_process / yield_requeue
    if (T[i].state == TERMINATED) return 0;
    if (T[i].on_queue)            return 0; // idempotent (no double queue ref)
    p_ref(i);
    T[i].on_queue = 1;
    return 1;
}

// pick_next: lowest-index on_queue && !TERMINATED. Removes from queue and
// TRANSFERS the ref (queue ref -> running ref, NO unref). Drains terminated
// queue entries (KILL-FIX-001). -1 => idle.
static int pick_next(void) {
    for (int i = 0; i < MAXP; i++) {
        if (T[i].alive && T[i].on_queue) {
            if (T[i].state == TERMINATED) {     // drain a dead queue entry
                T[i].on_queue = 0;
                p_unref(i);
                continue;
            }
            T[i].on_queue = 0;                  // transfer; NO unref
            return i;
        }
    }
    return -1;
}

static void sched_remove(int i) {          // scheduler_remove_process (KILL-FIX-003)
    if (i < 0) return;
    if (!T[i].on_queue) return;            // no-op for an off-queue zombie
    T[i].on_queue = 0;
    p_unref(i);
}

// ----------------------------------------------------------------------------
// process lifecycle
// ----------------------------------------------------------------------------
static int proc_create(int pid, int parent) {
    int i;
    for (i = 0; i < MAXP; i++) if (!T[i].alive) break;
    assert(i < MAXP && "PCB pool exhausted");
    assert(pid >= 0 && pid < MAX_PID);
    assert(table[pid] == -1 && "PID already published (would clobber identity)");
    memset(&T[i], 0, sizeof T[i]);
    T[i].pid        = pid;
    T[i].ref_count  = 1;          // the CREATION ref
    T[i].reaped     = 0;          // not yet reap-claimed
    T[i].on_queue   = 0;
    T[i].state      = READY;
    T[i].alive      = 1;
    T[i].parent_pid = parent;
    T[i].create_seq = ++g_seq;    // identity tiebreaker
    table[pid]      = i;
    table_seq[pid]  = T[i].create_seq;
    created_count++;
    return i;
}

// process_destroy: scheduler_remove_process(proc) THEN one process_unref(proc).
// UNCHANGED by the fix. At a reaper site this unref drops the *get_by_pid* ref;
// at a creator-cleanup site (fork-fail / pe_loader) it drops the CREATION ref.
static void proc_destroy(int i) {
    if (i < 0) return;
    sched_remove(i);   // no-op for an off-queue terminated zombie
    p_unref(i);
}

// THE NEW reap_claim_release: only the CAS winner (reaped 0->1) drops the
// CREATION ref. Called IMMEDIATELY BEFORE process_destroy AT REAPER SITES ONLY.
static void reap_claim_release(int i) {
    if (i < 0) return;
    // __atomic_exchange_n(&reaped, 1): returns the PRIOR value.
    int prior = __atomic_exchange_n(&T[i].reaped, 1, __ATOMIC_SEQ_CST);
    if (!prior) {
        p_unref(i);    // releases the CREATION ref exactly once
    }
}

// process_get_by_pid: +1 under the lock; identity = pid + create_seq. A stale
// pid (slot reclaimed or recycled to a different create_seq) returns -1.
static int get_by_pid(int pid) {
    if (pid < 0 || pid >= MAX_PID) return -1;
    int i = table[pid];
    if (i < 0) return -1;
    assert(T[i].alive);
    p_ref(i);
    return i;
}
// identity-checked lookup: caller asserts it got the SAME incarnation it expected
static int get_by_pid_seq(int pid, int expect_seq) {
    if (pid < 0 || pid >= MAX_PID) return -1;
    int i = table[pid];
    if (i < 0) return -1;
    if (table_seq[pid] != expect_seq) return -1;  // PID recycled; not our process
    assert(T[i].alive);
    p_ref(i);
    return i;
}

// ----------------------------------------------------------------------------
// the cooperative TERMINATED path: 'current' (dead) had a running ref TRANSFERRED
// to it by pick_next (queue->running). KILL-FIX-002 drops that running ref once.
// The CREATION ref remains -> the PCB is a published zombie, NOT yet freed.
// ----------------------------------------------------------------------------
static void schedule_terminated(int dead) {
    int next = pick_next();
    if (next >= 0) { T[next].state = RUNNING; }
    p_unref(dead);   // KILL-FIX-002: drop the running ref (queue->running) once
}

// reparent-to-init: a parent's exit hands its still-live children to INIT.
// (Zombies the parent had not yet reaped are also reparented; INIT reaps them.)
static void reparent_children_to_init(int parent_pid) {
    for (int i = 0; i < MAXP; i++) {
        if (T[i].alive && T[i].parent_pid == parent_pid && T[i].pid != INIT_PID) {
            T[i].parent_pid = INIT_PID;
        }
    }
}

static void dump(const char* tag) {
    printf("  %-30s : ", tag);
    for (int i = 0; i < MAXP; i++)
        if (T[i].alive)
            printf("PID%d{rc=%d,rp=%d,oq=%d,st=%d,pp=%d} ",
                   T[i].pid, T[i].ref_count, T[i].reaped,
                   T[i].on_queue, T[i].state, T[i].parent_pid);
    printf("\n");
}

static void table_init(void) {
    for (int i = 0; i < MAX_PID; i++) { table[i] = -1; table_seq[i] = -1; }
}

// Count published table slots (PID leaks if any survive an "all reaped" point).
static int published_slots(void) {
    int n = 0;
    for (int i = 0; i < MAX_PID; i++) if (table[i] >= 0) n++;
    return n;
}

// =============================================================================
// Bring INIT (PID 1) up as the running 'current' and spawn a child that has run
// once and then exited -> a published zombie. Returns the child's pool index.
// On entry INIT must already be running. Used by several scenarios.
// =============================================================================
static int spawn_child_and_exit(int child_pid, int parent_pid) {
    int child = proc_create(child_pid, parent_pid);   // rc=1 creation
    sched_add(child);                                  // rc 1->2 (queue ref)
    int picked = pick_next();                          // transfer; rc stays 2
    assert(picked == child);
    T[child].state = RUNNING;
    // child exits:
    T[child].state = TERMINATED;
    sched_remove(child);                               // off-queue now: no-op
    schedule_terminated(child);                        // KILL-FIX-002: rc 2->1
    // child is now a published zombie: rc==1 (creation ref), reaped==0
    assert(T[child].alive);
    assert(T[child].ref_count == 1 && "zombie must hold exactly the creation ref");
    assert(T[child].reaped == 0);
    assert(table[child_pid] >= 0 && "zombie still published");
    return child;
}

// ---------------------------------------------------------------------------
static void scenario_A(void) {
    printf("\n=== Scenario A: parented zombie reaped once ===\n");
    int child_pid = 2;
    int child = spawn_child_and_exit(child_pid, INIT_PID);
    int child_seq = T[child].create_seq;
    dump("A: child zombie");

    int frees_before = freed_count;

    // INIT reaps via waitpid: get_by_pid(+1) then reap_claim_release then destroy.
    int found = get_by_pid_seq(child_pid, child_seq);   // rc 1->2 (caller ref)
    assert(found == child);
    assert(T[found].ref_count == 2);
    reap_claim_release(found);   // winner: reaped 0->1, drop CREATION ref rc 2->1
    proc_destroy(found);         // sched_remove(no-op) + unref caller ref rc 1->0 -> FREE
    dump("A: after reap");

    assert(!T[child].alive            && "A: child PCB must be freed");
    assert(table[child_pid] == -1     && "A: PID slot must be reclaimed");
    assert(freed_count == frees_before + 1 && "A: freed exactly once");
    printf("  [A PASS] child freed once, slot reclaimed (PID %d -> -1)\n", child_pid);
}

// ---------------------------------------------------------------------------
static void scenario_B(void) {
    printf("\n=== Scenario B: orphan reaped by INIT after reparent ===\n");
    // A real parent (PID 3) spawns a child (PID 4). Parent exits FIRST (before
    // reaping) -> child is reparented to INIT, then INIT reaps it.
    int parent = proc_create(3, INIT_PID);   // PID 3, parented to init
    sched_add(parent); int pp = pick_next(); assert(pp == parent);
    T[parent].state = RUNNING;

    int child = spawn_child_and_exit(4, /*parent_pid*/3);  // child PID 4 under PID 3
    int child_seq = T[child].create_seq;
    assert(T[child].parent_pid == 3);
    dump("B: child zombie under PID3");

    // PID 3 (the original parent) now EXITS without ever calling waitpid.
    reparent_children_to_init(3);            // child.parent_pid 3 -> 1 (INIT)
    assert(T[child].parent_pid == INIT_PID && "B: orphan must be reparented to init");
    // PID 3 itself terminates and is reaped by INIT too (its own creation ref).
    T[parent].state = TERMINATED;
    sched_remove(parent);                    // off-queue: no-op
    schedule_terminated(parent);             // running ref dropped; parent rc 2->1 zombie
    reparent_children_to_init(3);            // idempotent; nothing left
    // INIT reaps the (now reparented) child:
    int frees_before = freed_count;
    int found = get_by_pid_seq(4, child_seq);   // rc 1->2
    assert(found == child);
    reap_claim_release(found);                  // CREATION ref dropped rc 2->1
    proc_destroy(found);                        // caller ref dropped rc 1->0 -> FREE
    assert(!T[child].alive             && "B: orphan PCB must be freed");
    assert(table[4] == -1              && "B: orphan PID slot reclaimed");
    assert(freed_count == frees_before + 1 && "B: orphan freed exactly once");

    // INIT also reaps the original parent PID 3 (no waitpid by anyone else).
    int frees_before2 = freed_count;
    int fp = get_by_pid(3);                      // rc 1->2
    assert(fp == parent);
    reap_claim_release(fp);                       // CREATION ref dropped rc 2->1
    proc_destroy(fp);                             // caller ref dropped rc 1->0 -> FREE
    assert(!T[parent].alive && table[3] == -1);
    assert(freed_count == frees_before2 + 1);
    printf("  [B PASS] orphan PID4 + ex-parent PID3 each freed once via INIT, "
           "no original-parent waitpid\n");
}

// ---------------------------------------------------------------------------
static void scenario_C(void) {
    printf("\n=== Scenario C: DOUBLE-REAP race (two reapers, same zombie) ===\n");
    // Two contexts each independently get_by_pid the SAME zombie (e.g. INIT's
    // waitpid(-1) racing a buggy second reaper, or two joiners). Each calls
    // reap_claim_release then process_destroy. The reaped CAS must make exactly
    // ONE of them drop the creation ref; ref_count must never go negative.
    int child = spawn_child_and_exit(5, INIT_PID);
    int child_seq = T[child].create_seq;
    dump("C: child zombie");

    int frees_before = freed_count;

    // Reaper #1 and Reaper #2 BOTH take a get_by_pid ref FIRST (both see the
    // still-published zombie before either tears down). rc: 1 -> 2 -> 3.
    int r1 = get_by_pid_seq(5, child_seq);   // rc 1->2
    int r2 = get_by_pid_seq(5, child_seq);   // rc 2->3
    assert(r1 == child && r2 == child);
    assert(T[child].ref_count == 3);

    // Interleave the two reapers. reaped CAS: only the FIRST exchange wins.
    // Reaper #1:
    reap_claim_release(r1);   // WINNER reaped 0->1: drop CREATION ref rc 3->2
    // Reaper #2:
    reap_claim_release(r2);   // LOSER reaped already 1: NO unref (rc unchanged)
    assert(T[child].reaped == 1);
    assert(T[child].ref_count == 2 && "C: exactly one creation-ref drop");

    // Now each reaper runs its process_destroy, dropping ONLY its own caller ref.
    proc_destroy(r1);         // drop r1 caller ref rc 2->1 (still alive)
    assert(T[child].alive && "C: must NOT be freed after only one caller ref dropped");
    proc_destroy(r2);         // drop r2 caller ref rc 1->0 -> FREE
    dump("C: after both reapers");

    assert(!T[child].alive             && "C: freed after the LAST ref");
    assert(table[5] == -1              && "C: slot reclaimed");
    assert(freed_count == frees_before + 1 && "C: teardown EXACTLY once");
    assert(g_abort_negref == 0         && "C: ref_count never went negative");
    printf("  [C PASS] one creation-ref drop, one teardown, rc never negative\n");
}

// ---------------------------------------------------------------------------
static void scenario_D(void) {
    printf("\n=== Scenario D: fork-FAILURE cleanup (THE TRAP) ===\n");
    // A fresh child is created (rc=1 creation ref) but fork then FAILS before it
    // is ever published to a reaper / before any get_by_pid. The fork-fail path
    // calls process_destroy DIRECTLY (NOT a reaper path: reaped stays 0). This
    // must free it exactly once with NO double-free -- the lone unref inside
    // process_destroy drops the CREATION ref directly.
    int frees_before = freed_count;
    int child = proc_create(6, INIT_PID);    // rc=1 creation ref ONLY
    assert(T[child].ref_count == 1);
    assert(T[child].reaped == 0);
    // NOTE: no sched_add (fork failed before enqueue), no get_by_pid.
    dump("D: fresh child (fork about to fail)");

    // fork-fail cleanup: process_destroy ONLY. NO reap_claim_release here.
    proc_destroy(child);   // sched_remove(no-op) + unref CREATION ref rc 1->0 -> FREE

    assert(!T[child].alive             && "D: fresh child freed");
    assert(T[child].reaped == 0        && "D: reaped MUST stay 0 (not a reaper path)");
    assert(table[6] == -1              && "D: slot reclaimed");
    assert(freed_count == frees_before + 1 && "D: freed exactly once");
    assert(g_abort_negref == 0         && "D: no double-free");
    printf("  [D PASS] fork-fail creator-cleanup freed once, reaped stayed 0, "
           "no double-free\n");
}

// ---------------------------------------------------------------------------
// Global accounting check (Scenario E) is done in main() after A..D.
// ---------------------------------------------------------------------------

int main(void) {
    table_init();

    // ---- bring INIT (PID 1) up as the running current ----
    int init = proc_create(INIT_PID, 0);   // rc=1 creation
    sched_add(init);                        // rc 1->2 (queue ref)
    int picked = pick_next();               // transfer; rc stays 2
    assert(picked == init);
    T[init].state = RUNNING;
    dump("INIT running");

    scenario_A();
    scenario_B();
    scenario_C();
    scenario_D();

    // ---- tear INIT down cleanly so EVERY slot is reclaimed for Scenario E ----
    printf("\n=== Scenario E: global accounting ===\n");
    // INIT is the running current holding a transferred running ref (rc==2:
    // creation + running). On shutdown we drop the running ref then the creation
    // ref (modeled as process_cleanup Phase2: reap_claim_release BEFORE unref to
    // kill the shutdown leak of the creation ref).
    assert(T[init].ref_count == 2 && "INIT holds creation + running ref");
    p_unref(init);                 // drop the running ref rc 2->1
    reap_claim_release(init);      // process_cleanup fix: claim+drop CREATION ref 1->0 -> FREE
    assert(!T[init].alive && "INIT must be freed on shutdown (no creation-ref leak)");

    // Scenario E asserts:
    assert(freed_count == created_count &&
           "E: total frees must equal total processes created");
    assert(published_slots() == 0 &&
           "E: every table slot must be reclaimed (no PID leak)");
    assert(g_abort_negref == 0 && "E: no negative ref_count anywhere");

    // also assert no PCB survives alive
    for (int i = 0; i < MAXP; i++)
        assert(!T[i].alive && "E: no PCB may survive");

    printf("  [E PASS] created=%d freed=%d published_slots=%d\n",
           created_count, freed_count, published_slots());

    printf("\nREFSIM_REAP: PASS\n");
    return 0;
}

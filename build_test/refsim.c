// Faithful standalone simulation of the kernel's process ref-counting control
// flow, extracted verbatim (logically) from:
//   process.c: process_create(ref=1), process_ref(+1), process_unref(-1,free@0),
//              process_destroy(remove + unref)
//   scheduler.c: scheduler_add_process(ref+1, on_queue=1),
//                scheduler_yield_requeue(ref+1, on_queue=1),
//                scheduler_pick_next(on_queue=0, TRANSFER no unref),
//                scheduler_remove_process(no-op if !on_queue, else unref),
//                schedule() KILL-FIX-002 (unref dead once),
//                schedule_from_irq RESUME_CRETURN re-queue: add(+1) THEN unref(-1)
//   handlers.c: sys_exit, sys_waitpid reap (get_by_pid +1, process_destroy)
//
// We do NOT model the assembly switch; instead we model the SALIENT fact: the
// cooperative switch_to(from,next) does NOT return until `from` is later picked
// again. So we drive the cycle explicitly turn by turn.
#include <stdio.h>
#include <string.h>

typedef enum { READY, RUNNING, TERMINATED } pstate_t;
typedef struct {
    int pid;
    int ref_count;
    int on_queue;
    pstate_t state;
    int alive;        // 0 once freed (process_unref hit 0)
    int parent_pid;
} proc_t;

#define MAXP 8
static proc_t T[MAXP];
static int table[MAXP];      // pid -> index, -1 if slot NULL
static int g_current = -1;   // index of current

static int freed_count = 0;

static void p_ref(int i)   { T[i].ref_count++; }
static void p_unref(int i) {
    if (i < 0) return;
    T[i].ref_count--;
    if (T[i].ref_count == 0) {
        // teardown: NULL table slot, free PID, free PCB
        table[T[i].pid] = -1;
        T[i].alive = 0;
        freed_count++;
        printf("    [free] PID %d torn down (ref_count reached 0)\n", T[i].pid);
    }
}

static int sched_add(int i) {          // scheduler_add_process
    if (T[i].state == TERMINATED) return 0;
    if (T[i].on_queue) return 0;       // idempotency guard
    p_ref(i);
    T[i].on_queue = 1;
    return 1;
}
static int yield_requeue(int i) {      // scheduler_yield_requeue (cooperative)
    if (T[i].state == TERMINATED) return 0;
    if (T[i].on_queue) return 0;
    p_ref(i);
    T[i].on_queue = 1;
    return 1;
}
// pick_next: returns an index that is on_queue, removes it from queue, TRANSFERS
// ref (no unref). Picks lowest index that is on_queue && !TERMINATED. -1 => idle.
static int pick_next(void) {
    for (int i = 0; i < MAXP; i++) {
        if (T[i].alive && T[i].on_queue) {
            if (T[i].state == TERMINATED) { // KILL-FIX-001 drain
                T[i].on_queue = 0;
                p_unref(i);
                continue;
            }
            T[i].on_queue = 0;          // transfer, NO unref
            return i;
        }
    }
    return -1;
}
static void sched_remove(int i) {       // scheduler_remove_process
    if (i < 0) return;
    if (!T[i].on_queue) return;         // no-op (KILL-FIX-003)
    T[i].on_queue = 0;
    p_unref(i);
}

static int proc_create(int pid, int parent) {
    int i;
    for (i = 0; i < MAXP; i++) if (!T[i].alive) break;
    memset(&T[i], 0, sizeof T[i]);
    T[i].pid = pid; T[i].ref_count = 1; T[i].on_queue = 0;
    T[i].state = READY; T[i].alive = 1; T[i].parent_pid = parent;
    table[pid] = i;
    return i;
}
static void proc_destroy(int i) {       // process_destroy
    if (i < 0) return;
    sched_remove(i);                    // no-op for off-queue terminated zombie
    p_unref(i);
}

// --- COOPERATIVE schedule() on the TERMINATED path (KILL-FIX-002) ---
// current is TERMINATED; pick next, make it current, unref dead ONCE.
static void schedule_terminated(int dead) {
    int next = pick_next();             // may be -1 (idle) -> model as staying
    if (next >= 0) { T[next].state = RUNNING; g_current = next; }
    p_unref(dead);                      // KILL-FIX-002
}

// --- get_by_pid: refs and returns index ---
static int get_by_pid(int pid) {
    int i = table[pid];
    if (i < 0) return -1;
    p_ref(i);
    return i;
}

static void dump(const char* tag) {
    printf("  %-26s : ", tag);
    for (int i = 0; i < MAXP; i++)
        if (T[i].alive)
            printf("PID%d{rc=%d,oq=%d,st=%d} ", T[i].pid, T[i].ref_count, T[i].on_queue, T[i].state);
    printf("\n");
}

int main(void) {
    for (int i = 0; i < MAXP; i++) table[i] = -1;

    // ---- parent (PID 1) created and running ----
    int parent = proc_create(1, 0);
    // simulate parent being the running current (came up via scheduler_start which
    // pick_next-transferred a ref). Model: parent was added then picked.
    sched_add(parent);                  // rc 1->2
    int pp = pick_next();               // transfer, rc stays 2; parent current
    (void)pp; T[parent].state = RUNNING; g_current = parent;
    dump("parent running");

    // ---- parent spawns child (PID 2) via exec path ----
    int child = proc_create(2, 1);      // rc=1 (creation)
    sched_add(child);                   // rc 1->2 (queue ref)
    dump("child created+queued");

    // ---- a few cooperative yield round-trips of the CHILD ----
    // child gets picked to run:
    int picked = pick_next();           // picks child(idx) lowest on_queue... parent not on queue now
    // NOTE parent is NOT on queue (it's running/current). So pick returns child.
    if (picked != child) { printf("UNEXPECTED pick %d\n", picked); }
    T[child].state = RUNNING; g_current = child;
    dump("child running (1st)");

    // NO yields: isolate the create->run->exit->reap accounting (the actual claim).
    // (The yield-growth question is a SEPARATE concern; here we test the leak claim
    //  on the simplest possible lifecycle: spawn, run once, exit, reap.)

    // ---- child exits ----
    T[child].state = TERMINATED;
    sched_remove(child);                // on_queue==0 -> no-op
    schedule_terminated(child);         // KILL-FIX-002 unref once; switch to parent? parent off-queue -> idle
    // After child exit, make parent current again (it was off-queue; model resume)
    T[parent].state = RUNNING; g_current = parent;
    dump("child exited (zombie)");
    printf("  >>> child rc AFTER exit = %d (table slot %s)\n",
           T[child].ref_count, table[2] >= 0 ? "PUBLISHED" : "NULL");

    // ---- parent reaps via sys_waitpid ----
    int found = get_by_pid(2);          // rc+1
    if (found < 0) { printf("  reap: child already gone\n"); }
    else {
        T[found].parent_pid = 0;        // orphan
        proc_destroy(found);            // sched_remove(no-op) + unref ONCE
        dump("child reaped");
        printf("  >>> child rc AFTER reap = %d (table slot %s) alive=%d\n",
               T[found].ref_count, table[2] >= 0 ? "PUBLISHED" : "NULL", T[found].alive);
    }

    printf("\nRESULT: child PCB freed? %s   (total frees=%d)\n",
           T[child].alive ? "NO  <-- LEAK" : "YES", freed_count);
    printf("        PID 2 table slot = %s\n", table[2] >= 0 ? "STILL PUBLISHED (PID leaked)" : "NULL (reclaimed)");
    return 0;
}

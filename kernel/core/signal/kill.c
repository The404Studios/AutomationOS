// kernel/core/signal/kill.c - Signal sending implementation
#include "../../include/syscall.h"
#include "../../include/sched.h"
#include "../../include/kernel.h"

// Signal definitions (match userspace/libc/signal.h)
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31

#define _NSIG 32

// sys_kill - Send signal to process
// Args: pid = target process ID, sig = signal number
// Returns: 0 on success, negative error code on failure
int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;  // Unused

    // Validate signal number
    if (sig >= _NSIG) {
        kprintf("[KILL] Invalid signal number: %llu\n", sig);
        return EINVAL;
    }

    // Special case: sig=0 just checks if process exists.
    // process_get_by_pid takes a ref; process_unref drops it before return.
    // Ref balance: +1 (get_by_pid) -1 (unref) = 0. Correct.
    if (sig == 0) {
        process_t* target = process_get_by_pid((uint32_t)pid);
        if (!target) {
            return ESRCH;
        }
        process_unref(target);
        return ESUCCESS;
    }

    // Get target process.
    // process_get_by_pid increments ref_count; we own that ref for this call
    // and must call process_unref(target) on every return path below.
    process_t* target = process_get_by_pid((uint32_t)pid);
    if (!target) {
        kprintf("[KILL] Process %llu not found\n", pid);
        return ESRCH;
    }

    // Permission: a process may signal another only with the same UID; root
    // (uid 0) may signal anyone. Today everything runs as uid 0 so this always
    // allows (force-quit / task manager keep working), but it stops an
    // unprivileged process from killing init/compositor under a future MU model.
    {
        process_t* killer = process_get_current();
        if (killer && killer->uid != 0 && killer->uid != target->uid) {
            process_unref(target);
            return EPERM;
        }
    }

    // FIX: Do not deliver signals to an already-terminated process.
    //
    // If we allowed SIGSTOP/SIGCONT through to a TERMINATED process we could
    // corrupt state after teardown has begun; SIGKILL/SIGTERM are harmless but
    // pointless.  Return ESRCH (consistent with Linux behaviour) and release
    // the reference taken by process_get_by_pid.
    if (target->state == PROCESS_TERMINATED) {
        kprintf("[KILL] Signal %llu to already-terminated process %u — ignored\n",
                sig, target->pid);
        process_unref(target);
        return ESRCH;
    }

    // Handle different signal types
    switch (sig) {
        case SIGKILL:
            // SIGKILL: Immediately terminate process.
            //
            // Ref accounting for scheduler_remove_process():
            //   If the process is in the ready queue, scheduler_remove_process
            //   removes it and calls process_unref(), dropping the queue's ref.
            //   If the process is currently RUNNING (not in the queue), the
            //   function finds nothing, returns without unref — safe because the
            //   scheduler's "current" ref is held by schedule() / context_switch
            //   and will be released by KILL-FIX-002 when schedule() next fires.
            //
            // Self-SIGKILL (target == current_process): the process is RUNNING,
            // not in the queue.  scheduler_remove_process is idempotent here.
            // schedule() will see TERMINATED on the next tick and switch away via
            // KILL-FIX-002.  No double-remove, no double-unref.
            kprintf("[KILL] Sending SIGKILL to process %u (%s)\n",
                    target->pid, target->name);
            target->state = PROCESS_TERMINATED;
            target->exit_status = 128 + SIGKILL;   // conventional "killed by sig"
            process_on_terminate(target);          // wake a waitpid'ing parent
            scheduler_remove_process(target);
            break;

        case SIGTERM:
            // SIGTERM: Graceful termination (for now, same as SIGKILL).
            // Same ref/removal semantics as SIGKILL above.
            kprintf("[KILL] Sending SIGTERM to process %u (%s)\n",
                    target->pid, target->name);
            target->state = PROCESS_TERMINATED;
            target->exit_status = 128 + SIGTERM;
            process_on_terminate(target);
            scheduler_remove_process(target);
            break;

        case SIGSTOP:
            // SIGSTOP: Suspend process execution.
            //
            // We only act if the process is READY or RUNNING; a process that is
            // already BLOCKED (e.g. stopped or waiting on I/O) gets a no-op so
            // we avoid double-removal from the ready queue.
            //
            // Ref accounting for scheduler_remove_process():
            //   READY process: it is in the ready queue.  scheduler_remove_process
            //   dequeues it and calls process_unref() — dropping the queue's ref.
            //   The process is now kept alive only by the ref we hold here (from
            //   process_get_by_pid) plus whatever other refs exist (parent, etc.).
            //
            //   RUNNING process: it is NOT in the ready queue (scheduler_pick_next
            //   dequeued it before dispatch).  scheduler_remove_process finds
            //   nothing and returns without unref — correct, there is no queue ref
            //   to drop.  schedule() will not re-enqueue it because the state is
            //   now BLOCKED, so the process stays suspended until SIGCONT.
            //
            // FIX H4 (prerequisite): correctly removing from the ready queue here
            // means SIGCONT's scheduler_add_process() re-takes exactly one queue
            // ref, keeping accounting balanced (see SIGCONT case below).
            kprintf("[KILL] Sending SIGSTOP to process %u (%s)\n",
                    target->pid, target->name);
            if (target->state == PROCESS_RUNNING || target->state == PROCESS_READY) {
                target->state = PROCESS_BLOCKED;
                scheduler_remove_process(target);
            }
            break;

        case SIGCONT:
            // SIGCONT: Resume a stopped process.
            //
            // FIX H4: After setting state to READY we MUST call
            // scheduler_add_process() so the process is actually placed on the
            // ready queue.  Without this call the process has state==READY but is
            // invisible to the scheduler — it never runs again and can never be
            // killed (unrunnable, unkillable limbo).
            //
            // Ref accounting (paired with SIGSTOP fix above):
            //   SIGSTOP on a READY process: scheduler_remove_process dropped the
            //   queue's ref (ref_count -= 1).
            //   SIGCONT via scheduler_add_process: takes a new queue ref
            //   (ref_count += 1).
            //   Net change across stop+resume: zero.  Balanced.
            //
            //   SIGSTOP on a RUNNING process: no queue ref was held, so
            //   scheduler_remove_process was a no-op for refs.
            //   SIGCONT via scheduler_add_process: takes one queue ref
            //   (ref_count += 1).
            //   Net: +1, which is correct — the process is now in the queue and
            //   that queue slot owns one ref.  This mirrors what happened when the
            //   process was first enqueued by scheduler_add_process() at creation.
            //
            // We only re-enqueue if the process is actually BLOCKED; delivering
            // SIGCONT to a READY or RUNNING process is a no-op per POSIX.
            kprintf("[KILL] Sending SIGCONT to process %u (%s)\n",
                    target->pid, target->name);
            if (target->state == PROCESS_BLOCKED) {
                target->state = PROCESS_READY;
                scheduler_add_process(target);
                // Note: scheduler_add_process() calls process_ref() internally,
                // so after this call target->ref_count is incremented by 1 for
                // the queue's ownership.  Our own ref (from process_get_by_pid)
                // is still live and will be dropped by process_unref() below.
            }
            break;

        default:
            // TODO: Implement full signal delivery mechanism
            // For now, just log and ignore other signals
            kprintf("[KILL] Signal %llu to process %u not yet implemented\n",
                    sig, target->pid);
            process_unref(target);
            return ENOTSUP;
    }

    process_unref(target);
    return ESUCCESS;
}

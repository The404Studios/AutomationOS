// kernel/core/signal/kill.c - Signal sending implementation
#include "../../include/syscall.h"
#include "../../include/sched.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"      // copy_to_user / copy_from_user (SIG-FULL-0)
#include "../../include/errno.h"    // EFAULT / EINVAL / ESUCCESS

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

    // Reject PIDs that don't fit in 32 bits BEFORE the (uint32_t) casts below,
    // so a value like 0x1_00000001 can't truncate to a valid low PID (e.g. 1 =
    // init) and signal the wrong process.
    if (pid > 0xFFFFFFFFULL) {
        return ESRCH;
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
            // #9 kill-while-blocked: if the victim is BLOCKED on a wait_object,
            // force-unlink it so the object-ref it holds is dropped. Snapshot
            // wait_on BEFORE marking TERMINATED. Without this, an event-only waiter
            // (futex/waitpid/epoll, not on the timer sleep list) stays linked
            // forever, the object-ref leaks, ref_count never reaches 0, and the PCB
            // is never reaped. wait_object_abort is idempotent vs a racing signal.
            // target is kept alive by our get_by_pid ref, so the unref can't free it
            // mid-handler.
            {
                struct wait_object* wo = target->wait_on;
                if (target->state == PROCESS_BLOCKED && wo) {
                    wait_object_abort(wo, target);
                }
            }
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
            // Same blocked-waiter reclamation as SIGKILL above (#9): drop the
            // stranded wait_object ref so a BLOCKED victim becomes a reapable zombie.
            {
                struct wait_object* wo = target->wait_on;
                if (target->state == PROCESS_BLOCKED && wo) {
                    wait_object_abort(wo, target);
                }
            }
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
                target->stopped_by_signal = 1;
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
            if (target->state == PROCESS_BLOCKED && target->stopped_by_signal) {
                target->stopped_by_signal = 0;
                process_set_ready(target);
                scheduler_add_process(target);
                // Note: scheduler_add_process() calls process_ref() internally,
                // so after this call target->ref_count is incremented by 1 for
                // the queue's ownership.  Our own ref (from process_get_by_pid)
                // is still live and will be dropped by process_unref() below.
            }
            break;

        default:
            // SIG-FULL-0 (B8): a CATCHABLE signal -> set it PENDING. The effect
            // (a registered handler, or the POSIX default action via
            // signal_default_action) is applied at the target's next
            // return-to-user, in deliver_pending_signals(). SIGKILL/SIGSTOP/
            // SIGCONT above stay immediate + uncatchable.
            target->sig_pending |= (1ull << sig);
            kprintf("[KILL] Signal %llu pending for process %u (%s)\n",
                    sig, target->pid, target->name);
            // Wake a plainly-blocked target so it reaches a delivery point.
            if (target->state == PROCESS_BLOCKED && !target->stopped_by_signal) {
                struct wait_object* wo = target->wait_on;
                if (wo) wait_object_abort(wo, target);
            }
            break;
    }

    process_unref(target);
    return ESUCCESS;
}

/* =========================================================================
 * SIG-FULL-0 (B8): handler delivery, masks, sigreturn.
 *
 * syscall.asm saves the user GP registers on the kernel stack, stashes a
 * pointer to that frame in g_sig_frame BEFORE calling syscall_dispatch, and
 * calls deliver_pending_signals(frame, retval) AFTER. deliver builds a context
 * frame on the USER stack and rewrites the saved frame so `sysret` resumes in
 * the handler; SYS_RT_SIGRETURN reverses it.
 * ========================================================================= */

/* Saved GP frame in kernel-stack order (matches syscall.asm's push sequence:
 * rdi pushed last == lowest address == frame[0]). */
typedef struct {
    uint64_t rdi, rsi, rdx, r8, r9, r10;
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t rflags;     /* saved r11 */
    uint64_t rip;        /* saved rcx */
    uint64_t user_rsp;
} sig_gpframe_t;

/* Context saved on the USER stack across a handler, restored by sigreturn. */
#define SIG_UC_MAGIC 0x5347524554554BULL   /* "SGRETUK" */
typedef struct {
    uint64_t magic;
    uint64_t rax;        /* interrupted syscall's return value */
    uint64_t rdi, rsi, rdx, r8, r9, r10;
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t rflags, rip, rsp;
    uint64_t saved_mask;
} sig_ucontext_t;

/* In-flight syscall frame for the current syscall (set by syscall.asm before
 * dispatch). Single ring-3 CPU at a time on this path in the default build;
 * SMP must make this per-CPU. */
sig_gpframe_t* g_sig_frame = 0;

/* SECURITY (P0): a user-supplied handler/restorer/ucontext RIP eventually reaches
 * syscall.asm's `pop rcx; o64 sysret`. On Intel, SYSRET with a NON-CANONICAL RCX
 * raises #GP delivered in RING 0 *after* RSP is already the user value — the
 * CVE-2012-0217 escalation. So every user-controlled code/stack address that can
 * become a sysret target MUST be a canonical user VA (below the user/kernel split,
 * matching exec.c). Non-zero so a handler can't be 0 (that's SIG_DFL). */
#define SIG_USER_VA_MAX 0x0000800000000000ULL
static inline int sig_va_user(uint64_t va) { return va != 0 && va < SIG_USER_VA_MAX; }

/* SECURITY (P0): sanitize a user-supplied RFLAGS before it returns to ring 3.
 * Force IF=1 + the reserved bit; CLEAR the privileged/dangerous bits ring 3 must
 * not set — IOPL (12-13: would grant IN/OUT port I/O + CLI/STI), TF (8), NT (14),
 * DF (10), AC (18). Mirrors sys_fork / enter_usermode. */
static inline uint64_t sig_clean_rflags(uint64_t rf) {
    return (rf & ~0x3000ULL & ~0x100ULL & ~0x4000ULL & ~0x400ULL & ~0x40000ULL) | 0x202ULL;
}

/* POSIX default action for an uncaught (SIG_DFL) signal. */
static void signal_default_action(process_t* p, int sig) {
    int ignore = (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH);
    int stop   = (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU);
    if (ignore) return;
    if (stop) {
        if (p->state == PROCESS_RUNNING || p->state == PROCESS_READY) {
            p->state = PROCESS_BLOCKED;
            p->stopped_by_signal = 1;
            scheduler_remove_process(p);
            /* P1: this runs at the CURRENT process's syscall exit (p == current).
             * Without switching away, deliver returns to syscall.asm which sysrets
             * straight back into the now-"stopped" process. schedule() yields the
             * CPU; SIGCONT re-readies us and we resume here -> sysret to userspace. */
            schedule();
        }
        return;
    }
    {
        struct wait_object* wo = p->wait_on;
        if (p->state == PROCESS_BLOCKED && wo) wait_object_abort(wo, p);
    }
    p->state = PROCESS_TERMINATED;
    p->exit_status = 128 + sig;
    process_on_terminate(p);
    scheduler_remove_process(p);
    /* P1: same as sys_exit — the caller (deliver_pending_signals at this process's
     * own syscall exit) must NOT sysret back into a TERMINATED process. schedule()
     * switches to another task and never returns here (the PCB lives on as a zombie
     * until reaped). Mirrors sys_exit (handlers.c). */
    schedule();
}

/* Deliver at most one pending, unblocked signal. Called from syscall.asm after
 * the dispatched syscall, with the saved frame and that syscall's return value. */
void deliver_pending_signals(sig_gpframe_t* f, uint64_t retval) {
    process_t* p = process_get_current();
    int sig;
    uint64_t h, deliverable;
    if (!p || !f) return;

    deliverable = p->sig_pending & ~p->sig_mask;
    if (!deliverable) return;

    for (sig = 1; sig < 32; sig++)
        if (deliverable & (1ull << sig)) break;
    if (sig >= 32) return;

    p->sig_pending &= ~(1ull << sig);            /* consume */
    h = p->sig_handlers[sig];

    if (h == 0) { signal_default_action(p, sig); return; }   /* SIG_DFL */
    if (h == 1) return;                                      /* SIG_IGN */

    /* Custom handler: build a ucontext on the user stack + redirect sysret.
     * SECURITY (P0): defense-in-depth — h and the restorer were validated at
     * sigaction time, but re-check they are canonical user VAs before they can
     * become the sysret RIP; a bad one fails safe to the default action. */
    if (!f->user_rsp || !sig_va_user(h) || !sig_va_user(p->sig_restorer)) {
        signal_default_action(p, sig); return;
    }
    {
        sig_ucontext_t uc;
        uint64_t sp, uc_addr, hsp, ret_addr;

        uc.magic = SIG_UC_MAGIC;
        uc.rax = retval;
        uc.rdi = f->rdi; uc.rsi = f->rsi; uc.rdx = f->rdx;
        uc.r8 = f->r8; uc.r9 = f->r9; uc.r10 = f->r10;
        uc.r15 = f->r15; uc.r14 = f->r14; uc.r13 = f->r13; uc.r12 = f->r12;
        uc.rbp = f->rbp; uc.rbx = f->rbx;
        uc.rflags = f->rflags; uc.rip = f->rip; uc.rsp = f->user_rsp;
        uc.saved_mask = p->sig_mask;

        sp      = f->user_rsp - 128;             /* skip the red zone */
        uc_addr = (sp - sizeof(uc)) & ~0xFULL;   /* 16-aligned ucontext */
        hsp     = uc_addr - 8;                    /* handler RSP (== 8 mod 16) */
        ret_addr = p->sig_restorer;

        /* The destination stack pages may still be CoW-shared: a freshly forked
         * child that hasn't yet written this part of its stack carries it
         * read-only. copy_to_user REFUSES to write a read-only page (it would
         * either #PF the kernel or silently corrupt the shared parent frame), so
         * resolve CoW for every page we're about to touch first. cow_handle_write
         * is a no-op on an already-private/writable page and on a non-CoW page,
         * so this is safe to call unconditionally; a genuinely unmapped page is
         * still caught by copy_to_user's own accessibility check below. Without
         * this, a handler could never be delivered to a just-forked child — it
         * would always fall through to the default action. */
        {
            uint64_t pg, lo = hsp & ~0xFFFULL;
            uint64_t hi = (uc_addr + sizeof(uc) - 1) & ~0xFFFULL;
            for (pg = lo; pg <= hi; pg += 0x1000) cow_handle_write(pg);
        }

        if (copy_to_user((void*)uc_addr, &uc, sizeof(uc)) != 0 ||
            copy_to_user((void*)hsp, &ret_addr, 8) != 0) {
            signal_default_action(p, sig);        /* bad user stack -> fail safe */
            return;
        }

        f->rip      = h;                          /* enter the handler */
        f->user_rsp = hsp;
        f->rdi      = (uint64_t)sig;              /* handler(signum) */
        f->rsi      = uc_addr;                    /* 2nd arg: context pointer */
        f->rflags   = sig_clean_rflags(f->rflags);  /* P0: force IF, strip IOPL/TF/NT/DF */
        p->sig_mask |= (1ull << sig);             /* block during the handler */
    }
}

/* sigaction(sig, handler, restorer): 0=SIG_DFL, 1=SIG_IGN, else user VA. */
int64_t sys_rt_sigaction(uint64_t sig, uint64_t handler, uint64_t restorer,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    process_t* p = process_get_current();
    (void)a4; (void)a5; (void)a6;
    if (!p || sig < 1 || sig >= 32) return EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return EINVAL;   /* uncatchable */
    /* SECURITY (P0): a custom handler (>1, i.e. not SIG_DFL/SIG_IGN) and a restorer
     * both become `sysret` targets — reject anything that is not a canonical user
     * VA, so a non-canonical RIP can never reach `o64 sysret` and #GP in ring 0. */
    if (handler > 1 && !sig_va_user(handler)) return EINVAL;
    if (restorer && !sig_va_user(restorer)) return EINVAL;
    p->sig_handlers[sig] = handler;
    if (restorer) p->sig_restorer = restorer;
    return ESUCCESS;
}

/* sigprocmask(how, set*, oldset*): how 0=BLOCK 1=UNBLOCK 2=SETMASK. */
int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    process_t* p = process_get_current();
    (void)a4; (void)a5; (void)a6;
    if (!p) return ESRCH;
    if (oldset) {
        uint64_t old = p->sig_mask;
        if (copy_to_user((void*)oldset, &old, sizeof(old)) != 0) return EFAULT;
    }
    if (set) {
        uint64_t s;
        if (copy_from_user(&s, (void*)set, sizeof(s)) != 0) return EFAULT;
        s &= ~((1ull << SIGKILL) | (1ull << SIGSTOP));   /* never block these */
        if (how == 0)      p->sig_mask |= s;     /* BLOCK */
        else if (how == 1) p->sig_mask &= ~s;    /* UNBLOCK */
        else               p->sig_mask = s;      /* SETMASK */
    }
    return ESUCCESS;
}

/* sigreturn(): restore the saved user frame stashed by deliver. Returns the
 * interrupted syscall's rax (which becomes the restored rax via sysret). */
int64_t sys_rt_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    process_t* p = process_get_current();
    sig_gpframe_t* f = g_sig_frame;
    sig_ucontext_t uc;
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!p || !f) return EFAULT;
    if (copy_from_user(&uc, (void*)f->user_rsp, sizeof(uc)) != 0) return EFAULT;
    if (uc.magic != SIG_UC_MAGIC) return EFAULT;     /* corrupt -> fail safe */
    /* SECURITY (P0): the magic is a public constant — uc.rip / uc.rsp / uc.rflags
     * are fully attacker-controlled. rip+rsp become the sysret RIP/RSP, so reject
     * non-canonical-user values (a non-canonical rip #GPs in ring 0 on a
     * user-controlled stack); rflags is sanitized (no IOPL/TF/NT/DF, force IF). */
    if (!sig_va_user(uc.rip) || !sig_va_user(uc.rsp)) return EFAULT;
    f->rdi = uc.rdi; f->rsi = uc.rsi; f->rdx = uc.rdx;
    f->r8 = uc.r8; f->r9 = uc.r9; f->r10 = uc.r10;
    f->r15 = uc.r15; f->r14 = uc.r14; f->r13 = uc.r13; f->r12 = uc.r12;
    f->rbp = uc.rbp; f->rbx = uc.rbx;
    f->rflags   = sig_clean_rflags(uc.rflags);       /* P0: force IF, strip IOPL/TF/NT/DF */
    f->rip      = uc.rip;
    f->user_rsp = uc.rsp;
    p->sig_mask = uc.saved_mask;                     /* unblock the handled sig */
    return (int64_t)uc.rax;
}

/* sigpending(): observability -- return the pending-signal bitset. */
int64_t sys_sigpending(uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6) {
    process_t* p = process_get_current();
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return p ? (int64_t)p->sig_pending : 0;
}

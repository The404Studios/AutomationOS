/*
 * seccomp_syscall.c - Userspace entry points and dispatch-path enforcement
 * =======================================================================
 *
 * This file is the GLUE that makes seccomp actually enforce end-to-end:
 *
 *   1. sys_seccomp()  - the SYS_SECCOMP syscall handler. Lets a ring-3
 *                       process install a syscall policy on itself, either
 *                       strict mode or a copied-in BPF filter program.
 *
 *   2. seccomp_check() - the single hook the syscall dispatcher calls on
 *                        every syscall before running the handler. Returns 0
 *                        to allow, or a negative errno to deny.
 *
 * The BPF interpreter (filter.c) and policy evaluation (enforce.c) already
 * existed but were never wired to userspace or to the dispatcher. This file
 * connects them.
 */

#include "../../include/seccomp.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/sched.h"
#include "../../include/string.h"
#include "../../include/syscall.h"

/*
 * Negative-errno return values handed back to userspace.
 *
 * NB: kernel/include/syscall.h and kernel/include/compat/errno.h disagree on
 * the sign/value of the E* macros (syscall.h uses negative, compat uses
 * positive). To stay unambiguous regardless of include order we define our own
 * negative constants here. These match the negative convention in syscall.h
 * that userspace already observes.
 */
#define SC_EPERM   (-1)
#define SC_ESRCH   (-3)
#define SC_ENOMEM  (-12)
#define SC_EFAULT  (-14)
#define SC_EINVAL  (-22)

// ========================================
// sys_seccomp() - install policy on current process
// ========================================

int64_t sys_seccomp(uint64_t operation, uint64_t flags, uint64_t args,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;

    process_t* proc = process_get_current();
    if (!proc) {
        return SC_ESRCH;
    }

    switch (operation) {
    case SECCOMP_SET_MODE_STRICT: {
        // Strict mode needs no BPF program; enforce.c handles it directly.
        proc->seccomp_mode = SECCOMP_MODE_STRICT;
        kprintf("[SECCOMP] PID %u entered STRICT mode via SYS_SECCOMP\n",
                proc->pid);
        return 0;
    }

    case SECCOMP_SET_MODE_FILTER: {
        // 'args' is a USER pointer to struct sock_fprog.
        struct sock_fprog fprog;
        if (copy_from_user(&fprog, (const void*)args, sizeof(fprog)) != COPY_SUCCESS) {
            return SC_EFAULT;
        }

        if (fprog.len == 0 || fprog.len > SECCOMP_USER_MAX_INSNS || !fprog.filter) {
            return SC_EINVAL;
        }

        // Copy the instruction array out of userspace into a kernel buffer.
        size_t bytes = (size_t)fprog.len * sizeof(struct bpf_insn);
        struct bpf_insn* kinsns = (struct bpf_insn*)kmalloc(bytes);
        if (!kinsns) {
            return SC_ENOMEM;
        }

        if (copy_from_user(kinsns, fprog.filter, bytes) != COPY_SUCCESS) {
            kfree(kinsns);
            return SC_EFAULT;
        }

        // Build a validated BPF program (bpf_prog_create copies again into its
        // own storage, so the temporary kinsns buffer can be freed after).
        struct bpf_prog* prog = bpf_prog_create(kinsns, fprog.len);
        kfree(kinsns);
        if (!prog) {
            return SC_EINVAL;
        }

        // Translate userspace flags to internal filter flags. We always log so
        // denials are observable; STRICT (kill) is opt-in via the flags arg.
        uint32_t filter_flags = SECCOMP_FILTER_LOG;
        if (flags & SECCOMP_FILTER_STRICT) {
            filter_flags |= SECCOMP_FILTER_STRICT;
        }

        int rc = seccomp_install_filter(proc, prog, SECCOMP_MODE_FILTER,
                                        filter_flags);
        if (rc != 0) {
            bpf_prog_destroy(prog);  // drop the create() reference
            return rc;
        }

        // install_filter took its own reference; drop ours.
        bpf_prog_destroy(prog);

        kprintf("[SECCOMP] PID %u installed FILTER (%u insns) via SYS_SECCOMP\n",
                proc->pid, (uint32_t)fprog.len);
        return 0;
    }

    default:
        return SC_EINVAL;
    }
}

// ========================================
// seccomp_check() - dispatcher enforcement hook
// ========================================

int64_t seccomp_check(uint64_t syscall_nr, uint64_t arg1, uint64_t arg2,
                      uint64_t arg3, uint64_t arg4, uint64_t arg5,
                      uint64_t arg6) {
    process_t* proc = process_get_current();

    // No process context (early boot / kernel thread) -> allow.
    if (!proc) {
        return 0;
    }

    // Fast path / defensive gate.
    //
    // Only the two well-defined active modes are honoured here, and FILTER mode
    // additionally requires a non-NULL filter chain. This makes the hook robust
    // even if process_create() does not zero the PCB: an uninitialised
    // seccomp_mode that is not exactly STRICT/FILTER falls through to ALLOW, and
    // a FILTER mode without an installed filter also allows. (The integrator
    // should still zero proc->seccomp_mode/seccomp_filter in process_create --
    // see report -- but this guard prevents garbage from denying every syscall
    // system-wide if that fix lands later.)
    uint32_t mode = proc->seccomp_mode;
    if (mode == SECCOMP_MODE_DISABLED) {
        return 0;  // common case: no policy
    }
    if (mode != SECCOMP_MODE_STRICT && mode != SECCOMP_MODE_FILTER) {
        return 0;  // unrecognised (likely uninitialised) -> do not enforce
    }
    if (mode == SECCOMP_MODE_FILTER && !proc->seccomp_filter) {
        return 0;  // FILTER mode but no filter installed -> nothing to enforce
    }

    uint64_t scargs[6] = { arg1, arg2, arg3, arg4, arg5, arg6 };

    // ip is not threaded through the dispatcher; pass the saved user RIP if we
    // have it, otherwise 0. Seccomp policy here keys on the syscall number.
    uint64_t ip = proc->context.rip;

    uint32_t result = seccomp_check_syscall(proc, (uint32_t)syscall_nr,
                                            scargs, ip);
    uint32_t action = result & SECCOMP_RET_ACTION_MASK;

    if (action == SECCOMP_RET_ALLOW || action == SECCOMP_RET_LOG) {
        return 0;  // permitted (LOG already recorded by enforce.c)
    }

    // Denied. Run the violation handler (logging / kill bookkeeping) and map
    // the action to a syscall return value.
    seccomp_handle_violation(proc, result, (uint32_t)syscall_nr);

    if (action == SECCOMP_RET_ERRNO) {
        // Lower 16 bits carry the errno the filter requested.
        uint32_t errno_val = result & SECCOMP_RET_DATA_MASK;
        if (errno_val == 0) {
            return SC_EPERM;
        }
        return -(int64_t)errno_val;
    }

    // KILL / TRAP / TRACE / anything else: deny hard.
    // (Full process teardown is the integrator's job; the denial errno gives
    // the dispatcher a well-defined value to hand back to userspace.)
    return SC_EPERM;
}

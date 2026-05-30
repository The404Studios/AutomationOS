#include "../../include/seccomp.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/sched.h"
#include "../../include/string.h"
#include "../../include/syscall.h"   /* brings in canonical negative errno.h */
#include "../../include/perf.h"

// Seccomp statistics
static struct seccomp_stats global_stats = {0};

// Audit log (circular buffer)
#define AUDIT_LOG_SIZE 1024
static struct seccomp_audit_entry audit_log[AUDIT_LOG_SIZE];
static uint32_t audit_log_index = 0;

// ========================================
// Initialization
// ========================================

void seccomp_init(void) {
    kprintf("[SECCOMP] Initializing seccomp sandbox system...\n");
    memset(&global_stats, 0, sizeof(global_stats));
    memset(audit_log, 0, sizeof(audit_log));
    audit_log_index = 0;
    kprintf("[SECCOMP] Seccomp sandbox system initialized\n");
}

// ========================================
// Filter Installation
// ========================================

int seccomp_install_filter(struct process* proc, struct bpf_prog* prog, uint32_t mode, uint32_t flags) {
    if (!proc || !prog) {
        return EINVAL;
    }

    // Validate BPF program
    if (bpf_prog_validate(prog) != 0) {
        kprintf("[SECCOMP] Filter validation failed for PID %u\n", proc->pid);
        return EINVAL;
    }

    // Check if process already has a filter
    if (proc->seccomp_filter) {
        // Seccomp filters are additive (layered)
        // New filter is AND-ed with existing filters
        kprintf("[SECCOMP] Process %u already has seccomp filter, layering...\n", proc->pid);
    }

    // Allocate filter structure
    struct seccomp_filter* filter = (struct seccomp_filter*)kmalloc(sizeof(struct seccomp_filter));
    if (!filter) {
        return ENOMEM;
    }

    filter->prog = prog;
    filter->prev = proc->seccomp_filter;  // Chain filters
    filter->mode = mode;
    filter->flags = flags;

    prog->ref_count++;  // Increment reference count

    // Install filter (one-way operation - can't be removed in strict mode)
    proc->seccomp_filter = filter;
    proc->seccomp_mode = mode;

    kprintf("[SECCOMP] Installed filter on PID %u (mode=%u, flags=0x%x)\n",
            proc->pid, mode, flags);

    return 0;
}

int seccomp_remove_filter(struct process* proc) {
    if (!proc || !proc->seccomp_filter) {
        return EINVAL;
    }

    struct seccomp_filter* filter = proc->seccomp_filter;

    // Can't remove strict filters (one-way operation by design)
    if (filter->flags & SECCOMP_FILTER_STRICT) {
        kprintf("[SECCOMP] Cannot remove strict filter\n");
        return EPERM;
    }

    // Destroy BPF program
    if (filter->prog) {
        bpf_prog_destroy(filter->prog);
    }

    // Unlink the top filter from the chain
    proc->seccomp_filter = filter->prev;
    if (!proc->seccomp_filter) {
        proc->seccomp_mode = SECCOMP_MODE_DISABLED;
    }

    kfree(filter);
    return 0;
}

// ========================================
// Syscall Enforcement
// ========================================

uint32_t seccomp_check_syscall(struct process* proc, uint32_t syscall_nr,
                               uint64_t* args, uint64_t ip) {
    if (!proc) {
        return SECCOMP_RET_KILL;
    }

    // Fast path: no filter installed
    if (proc->seccomp_mode == SECCOMP_MODE_DISABLED) {
        global_stats.allowed++;
        return SECCOMP_RET_ALLOW;
    }

#ifdef PERF_SECCOMP
    uint64_t start = rdtsc();
#endif

    global_stats.total_checks++;

    // Handle strict mode (no BPF filter needed)
    if (proc->seccomp_mode == SECCOMP_MODE_STRICT) {
        // Strict mode: only allow read, write, exit, sigreturn
        switch (syscall_nr) {
            case SYS_READ:
            case SYS_WRITE:
            case SYS_EXIT:
                global_stats.allowed++;
                return SECCOMP_RET_ALLOW;
            default:
                global_stats.denied++;
                return SECCOMP_RET_KILL;
        }
    }

    // Filter mode: run BPF filters
    if (proc->seccomp_mode == SECCOMP_MODE_FILTER) {
        if (!proc->seccomp_filter) {
            // No filter but filter mode set - kill
            global_stats.denied++;
            return SECCOMP_RET_KILL;
        }

        // Prepare seccomp_data for BPF
        struct seccomp_data data = {0};
        data.nr = syscall_nr;
        data.arch = AUDIT_ARCH_X86_64;
        data.instruction_pointer = ip;
        memcpy(data.args, args, sizeof(data.args));

        // Run filters in chain (most restrictive wins)
        uint32_t result = SECCOMP_RET_ALLOW;
        struct seccomp_filter* filter = proc->seccomp_filter;

        while (filter) {
            uint32_t filter_result = bpf_prog_run(filter->prog, &data);

            // Extract action
            uint32_t action = filter_result & SECCOMP_RET_ACTION_MASK;

            // Most restrictive action wins
            if (action < (result & SECCOMP_RET_ACTION_MASK)) {
                result = filter_result;
            }

            filter = filter->prev;
        }

#ifdef PERF_SECCOMP
        uint64_t end = rdtsc();
        uint64_t cycles = end - start;
        global_stats.avg_check_cycles = (global_stats.avg_check_cycles + cycles) / 2;
#endif

        // Update statistics
        if ((result & SECCOMP_RET_ACTION_MASK) == SECCOMP_RET_ALLOW) {
            global_stats.allowed++;
        } else {
            global_stats.denied++;

            // Log if requested
            if (proc->seccomp_filter->flags & SECCOMP_FILTER_LOG) {
                seccomp_audit_log(proc, syscall_nr, result, args, ip);
            }
        }

        return result;
    }

    // Unknown mode - kill
    global_stats.denied++;
    return SECCOMP_RET_KILL;
}

void seccomp_handle_violation(struct process* proc, uint32_t action,
                              uint32_t syscall_nr) {
    if (!proc) return;

    uint32_t action_type = action & SECCOMP_RET_ACTION_MASK;
    uint32_t data = action & SECCOMP_RET_DATA_MASK;

    switch (action_type) {
        case SECCOMP_RET_KILL:
            kprintf("[SECCOMP] KILL: PID %u attempted syscall %u\n",
                    proc->pid, syscall_nr);
            global_stats.killed++;
            // TODO: Terminate process
            // process_kill(proc, SIGKILL);
            break;

        case SECCOMP_RET_TRAP:
            kprintf("[SECCOMP] TRAP: PID %u attempted syscall %u\n",
                    proc->pid, syscall_nr);
            // TODO: Send SIGSYS signal
            // signal_send(proc, SIGSYS);
            break;

        case SECCOMP_RET_ERRNO:
            kprintf("[SECCOMP] ERRNO: PID %u attempted syscall %u (returning errno %u)\n",
                    proc->pid, syscall_nr, data);
            // Syscall will return -data as errno
            break;

        case SECCOMP_RET_TRACE:
            kprintf("[SECCOMP] TRACE: PID %u attempted syscall %u (notifying tracer)\n",
                    proc->pid, syscall_nr);
            // TODO: Notify ptrace tracer
            // ptrace_notify(proc, PTRACE_EVENT_SECCOMP);
            break;

        case SECCOMP_RET_LOG:
            kprintf("[SECCOMP] LOG: PID %u syscall %u (allowed but logged)\n",
                    proc->pid, syscall_nr);
            global_stats.logged++;
            break;

        default:
            kprintf("[SECCOMP] Unknown action 0x%x for PID %u syscall %u\n",
                    action_type, proc->pid, syscall_nr);
            break;
    }
}

// ========================================
// Audit Logging
// ========================================

void seccomp_audit_log(struct process* proc, uint32_t syscall_nr,
                      uint32_t action, uint64_t* args, uint64_t ip) {
    if (!proc) return;

    // Get TSC timestamp
    uint64_t timestamp = rdtsc();

    // Write to circular buffer
    struct seccomp_audit_entry* entry = &audit_log[audit_log_index];
    entry->timestamp = timestamp;
    entry->pid = proc->pid;
    entry->syscall_nr = syscall_nr;
    entry->action = action;
    memcpy(entry->args, args, sizeof(entry->args));
    entry->ip = ip;

    audit_log_index = (audit_log_index + 1) % AUDIT_LOG_SIZE;

    kprintf("[AUDIT] PID %u: syscall %u denied (action=0x%x) at IP=0x%llx\n",
            proc->pid, syscall_nr, action, ip);
}

struct seccomp_audit_entry* seccomp_get_audit_log(uint32_t* count) {
    if (count) {
        *count = (audit_log_index < AUDIT_LOG_SIZE) ? audit_log_index : AUDIT_LOG_SIZE;
    }
    return audit_log;
}

// ========================================
// Statistics
// ========================================

void seccomp_get_stats(struct seccomp_stats* stats) {
    if (stats) {
        memcpy(stats, &global_stats, sizeof(struct seccomp_stats));
    }
}

// ========================================
// Predefined Profiles
// ========================================

// Strict profile: only read/write/exit
static struct bpf_insn profile_strict[] = {
    // Check architecture is x86-64
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

    // Load syscall number
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),

    // Allow SYS_READ (2)
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 2, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    // Allow SYS_WRITE (3)
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 3, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    // Allow SYS_EXIT (0)
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    // Deny everything else
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
};

// Browser profile: allow network, file I/O, but no exec/ptrace/module loading
static struct bpf_insn profile_browser[] = {
    // Check architecture
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

    // Load syscall number
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),

    // Deny dangerous syscalls
    // SYS_EXECVE (7)
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 7, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

    // SYS_FORK (1) - limit process creation
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1),  // EPERM

    // Allow everything else (permissive browser sandbox)
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
};

// Network service profile: no exec, no ptrace
static struct bpf_insn profile_network[] = {
    // Check architecture
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

    // Load syscall number
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),

    // Deny SYS_EXECVE
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 7, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

    // Allow everything else
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
};

// Untrusted executable profile: very restrictive
static struct bpf_insn profile_untrusted[] = {
    // Check architecture
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

    // Load syscall number
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),

    // Allow only basic I/O and computation
    // SYS_READ
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 2, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    // SYS_WRITE
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 3, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    // SYS_EXIT
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    // SYS_GETPID
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 8, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    // Deny everything else
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
};

int seccomp_load_profile(struct process* proc, uint32_t profile_id) {
    if (!proc) {
        return EINVAL;
    }

    struct bpf_insn* insns = NULL;
    uint32_t len = 0;

    switch (profile_id) {
        case SECCOMP_PROFILE_STRICT:
            insns = profile_strict;
            len = sizeof(profile_strict) / sizeof(struct bpf_insn);
            break;

        case SECCOMP_PROFILE_BROWSER:
            insns = profile_browser;
            len = sizeof(profile_browser) / sizeof(struct bpf_insn);
            break;

        case SECCOMP_PROFILE_NETWORK:
            insns = profile_network;
            len = sizeof(profile_network) / sizeof(struct bpf_insn);
            break;

        case SECCOMP_PROFILE_UNTRUSTED:
            insns = profile_untrusted;
            len = sizeof(profile_untrusted) / sizeof(struct bpf_insn);
            break;

        default:
            kprintf("[SECCOMP] Unknown profile ID: %u\n", profile_id);
            return EINVAL;
    }

    // Create BPF program
    struct bpf_prog* prog = bpf_prog_create(insns, len);
    if (!prog) {
        return ENOMEM;
    }

    // Install filter
    int result = seccomp_install_filter(proc, prog, SECCOMP_MODE_FILTER,
                                       SECCOMP_FILTER_STRICT | SECCOMP_FILTER_LOG);

    if (result != 0) {
        bpf_prog_destroy(prog);
    }

    return result;
}

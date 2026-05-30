#ifndef SECCOMP_H
#define SECCOMP_H

#include "types.h"

struct process;
struct bpf_prog;

// ========================================
// Seccomp (Secure Computing) Syscall Filtering
// ========================================

// Seccomp modes
#define SECCOMP_MODE_DISABLED   0  // No filtering (default)
#define SECCOMP_MODE_STRICT     1  // Allow only read/write/exit/_exit/sigreturn
#define SECCOMP_MODE_FILTER     2  // BPF filter-based syscall filtering

// Seccomp actions (returned by BPF filter or on violation)
#define SECCOMP_RET_KILL        0x00000000  // Kill process immediately
#define SECCOMP_RET_TRAP        0x00030000  // Send SIGSYS to process
#define SECCOMP_RET_ERRNO       0x00050000  // Return errno (lower 16 bits)
#define SECCOMP_RET_TRACE       0x7ff00000  // Notify tracer (ptrace)
#define SECCOMP_RET_LOG         0x7ffc0000  // Allow but log
#define SECCOMP_RET_ALLOW       0x7fff0000  // Allow syscall

#define SECCOMP_RET_ACTION_MASK 0xffff0000
#define SECCOMP_RET_DATA_MASK   0x0000ffff

// BPF filter instruction (classic BPF - cBPF)
// This is simpler than eBPF but sufficient for syscall filtering
struct bpf_insn {
    uint16_t code;   // Opcode
    uint8_t  jt;     // Jump if true
    uint8_t  jf;     // Jump if false
    uint32_t k;      // Generic multiuse field
};

// BPF opcodes (classic BPF subset for seccomp)
#define BPF_CLASS_MASK  0x07
#define BPF_LD          0x00  // Load
#define BPF_JMP         0x05  // Jump
#define BPF_RET         0x06  // Return

// BPF LD (load) modes
#define BPF_SIZE_MASK   0x18
#define BPF_W           0x00  // Word (32-bit)
#define BPF_H           0x08  // Half-word (16-bit)
#define BPF_B           0x10  // Byte

#define BPF_MODE_MASK   0xe0
#define BPF_ABS         0x20  // Absolute offset (from data start)
#define BPF_IMM         0x00  // Immediate value

// BPF JMP (conditional jump) operations
#define BPF_OP_MASK     0xf0
#define BPF_JEQ         0x10  // Jump if equal
#define BPF_JGT         0x20  // Jump if greater than
#define BPF_JGE         0x30  // Jump if greater or equal
#define BPF_JSET        0x40  // Jump if bits set

#define BPF_SRC_MASK    0x08
#define BPF_K           0x00  // Compare with constant
#define BPF_X           0x08  // Compare with X register

// BPF helper macros for instruction construction.
// NOTE: parameter names must NOT collide with the struct field names used as
// designators below (.code/.jt/.jf/.k). The C preprocessor textually replaces
// any token equal to a parameter name -- if a parameter were named "code" it
// would also rewrite the ".code" designator, producing a syntax error. Hence
// the leading underscores.
#define BPF_STMT(_code, _k) \
    ((struct bpf_insn) { .code = (_code), .jt = 0, .jf = 0, .k = (_k) })

#define BPF_JUMP(_code, _k, _jt, _jf) \
    ((struct bpf_insn) { .code = (_code), .jt = (_jt), .jf = (_jf), .k = (_k) })

// Seccomp data passed to BPF filter
// This is the "packet" that BPF filter examines
struct seccomp_data {
    uint32_t nr;                    // System call number
    uint32_t arch;                  // Architecture (AUDIT_ARCH_X86_64, etc.)
    uint64_t instruction_pointer;   // User-space IP
    uint64_t args[6];               // Syscall arguments
};

// Architecture constants (for seccomp_data.arch field)
#define AUDIT_ARCH_X86_64   0xc000003e  // x86-64 (64-bit)
#define AUDIT_ARCH_I386     0x40000003  // x86 (32-bit)

// Offsets into seccomp_data (for BPF_LD | BPF_ABS instructions)
#define offsetof_nr             0   // Syscall number
#define offsetof_arch           4   // Architecture
#define offsetof_ip             8   // Instruction pointer
#define offsetof_args           16  // Arguments array start

// BPF filter program
struct bpf_prog {
    uint32_t len;                   // Number of instructions
    struct bpf_insn* insns;         // BPF instructions
    uint32_t flags;                 // Program flags
    uint32_t ref_count;             // Reference count
};

// Flags for bpf_prog
#define BPF_PROG_READONLY   (1 << 0)  // Program is read-only (can't be modified)

// Seccomp filter (attached to process)
struct seccomp_filter {
    struct bpf_prog* prog;          // BPF program
    struct seccomp_filter* prev;    // Previous filter in chain (for layering)
    uint32_t mode;                  // Seccomp mode
    uint32_t flags;                 // Filter flags
};

// Flags for seccomp_filter
#define SECCOMP_FILTER_STRICT   (1 << 0)  // Kill on violation (no errno return)
#define SECCOMP_FILTER_LOG      (1 << 1)  // Log all denied syscalls

// ========================================
// Seccomp Filter Management
// ========================================

// Initialize seccomp subsystem
void seccomp_init(void);

// Create BPF program from instructions
struct bpf_prog* bpf_prog_create(const struct bpf_insn* insns, uint32_t len);

// Destroy BPF program
void bpf_prog_destroy(struct bpf_prog* prog);

// Validate BPF program (check for infinite loops, invalid jumps, etc.)
int bpf_prog_validate(const struct bpf_prog* prog);

// Execute BPF program on seccomp data
uint32_t bpf_prog_run(const struct bpf_prog* prog, const struct seccomp_data* data);

// ========================================
// Process Seccomp Filter Management
// ========================================

// Install seccomp filter on current process
int seccomp_install_filter(struct process* proc, struct bpf_prog* prog, uint32_t mode, uint32_t flags);

// Check if syscall is allowed by seccomp filter
// Returns SECCOMP_RET_* action
uint32_t seccomp_check_syscall(struct process* proc, uint32_t syscall_nr,
                               uint64_t* args, uint64_t ip);

// Handle seccomp violation
void seccomp_handle_violation(struct process* proc, uint32_t action,
                              uint32_t syscall_nr);

// Remove seccomp filter (only allowed if not SECCOMP_FILTER_STRICT)
int seccomp_remove_filter(struct process* proc);

// ========================================
// Seccomp Syscalls
// ========================================

// Operations for sys_seccomp() 'operation' argument (Linux-compatible)
#define SECCOMP_SET_MODE_STRICT 0  // Enter strict mode (read/write/exit only)
#define SECCOMP_SET_MODE_FILTER 1  // Install a BPF filter

// Userspace filter program descriptor passed to SECCOMP_SET_MODE_FILTER.
// Mirrors Linux struct sock_fprog. 'filter' is a USER pointer to an array of
// 'len' bpf_insn; the kernel copies it in via copy_from_user before use.
struct sock_fprog {
    uint16_t len;                   // Number of BPF instructions
    uint16_t _pad[3];              // Alignment padding (pointer is 8-byte aligned)
    const struct bpf_insn* filter;  // USER pointer to instruction array
};

// Maximum number of BPF instructions accepted from userspace (DoS guard)
#define SECCOMP_USER_MAX_INSNS 1024

// sys_seccomp() - Enable seccomp mode or install filter.
// Called via syscall dispatch; operates on the CURRENT process.
//   operation: SECCOMP_SET_MODE_STRICT or SECCOMP_SET_MODE_FILTER
//   flags:     SECCOMP_FILTER_* flags (e.g. STRICT|LOG)
//   args:      For FILTER mode, USER pointer to struct sock_fprog
// Returns 0 on success, negative errno on failure.
int64_t sys_seccomp(uint64_t operation, uint64_t flags, uint64_t args,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);

// ========================================
// Syscall-dispatch enforcement hook
// ========================================
//
// seccomp_check() is the ONE call the syscall dispatcher must make on every
// syscall, BEFORE invoking the handler. It evaluates the current process's
// seccomp policy for `syscall_nr` and returns:
//     0            -> ALLOW: dispatcher proceeds to call the handler
//     negative     -> DENY:  dispatcher must NOT call the handler and should
//                            return this value to userspace as the syscall
//                            result (a negative errno, e.g. -EPERM).
//
// For KILL/TRAP actions the process is flagged for termination and a denial
// errno is still returned so the dispatcher has a well-defined value to hand
// back if the process is not torn down immediately.
//
// This wrapper exists so the dispatcher needs no knowledge of seccomp_data,
// BPF, or the SECCOMP_RET_* action encoding.
int64_t seccomp_check(uint64_t syscall_nr, uint64_t arg1, uint64_t arg2,
                      uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

// sys_prctl() subset - PR_SET_SECCOMP
// (Alternative interface to seccomp, for Linux compatibility)
int64_t sys_prctl_set_seccomp(struct process* proc, uint64_t mode, uint64_t arg);

// ========================================
// Seccomp Profile Presets
// ========================================

// Load predefined seccomp profile
// profile_id: SECCOMP_PROFILE_*
int seccomp_load_profile(struct process* proc, uint32_t profile_id);

// Predefined profile IDs
#define SECCOMP_PROFILE_STRICT      1  // read/write/exit only
#define SECCOMP_PROFILE_BROWSER     2  // Web browser sandbox
#define SECCOMP_PROFILE_NETWORK     3  // Network service (no exec/ptrace)
#define SECCOMP_PROFILE_UNTRUSTED   4  // Untrusted executable (very restrictive)
#define SECCOMP_PROFILE_COMPUTE     5  // Pure computation (no I/O except stdio)

// ========================================
// Audit & Logging
// ========================================

// Seccomp audit log entry
struct seccomp_audit_entry {
    uint64_t timestamp;             // TSC timestamp
    uint32_t pid;                   // Process ID
    uint32_t syscall_nr;            // Syscall number
    uint32_t action;                // Action taken (SECCOMP_RET_*)
    uint64_t args[6];               // Syscall arguments
    uint64_t ip;                    // Instruction pointer
};

// Log seccomp violation
void seccomp_audit_log(struct process* proc, uint32_t syscall_nr,
                      uint32_t action, uint64_t* args, uint64_t ip);

// Get audit log (for introspection)
struct seccomp_audit_entry* seccomp_get_audit_log(uint32_t* count);

// ========================================
// Statistics
// ========================================

struct seccomp_stats {
    uint64_t total_checks;          // Total syscall checks
    uint64_t allowed;               // Allowed syscalls
    uint64_t denied;                // Denied syscalls
    uint64_t killed;                // Processes killed
    uint64_t logged;                // Logged violations
    uint64_t avg_check_cycles;      // Average check time (cycles)
};

// Get seccomp statistics
void seccomp_get_stats(struct seccomp_stats* stats);

#endif

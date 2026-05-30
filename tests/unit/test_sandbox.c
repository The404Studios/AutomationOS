#include "../../kernel/include/seccomp.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/sched.h"
#include "../../kernel/include/mem.h"

// Test framework macros
#define ASSERT(cond) do { \
    if (!(cond)) { \
        kprintf("[TEST FAIL] %s:%d: Assertion failed: %s\n", __FILE__, __LINE__, #cond); \
        return -1; \
    } \
} while(0)

#define TEST(name) \
    int test_##name(void); \
    int test_##name(void)

// ========================================
// BPF Program Validation Tests
// ========================================

TEST(bpf_create_simple) {
    // Create simple BPF program: load syscall number and allow
    struct bpf_insn insns[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };

    struct bpf_prog* prog = bpf_prog_create(insns, 2);
    ASSERT(prog != NULL);
    ASSERT(prog->len == 2);
    ASSERT(prog->insns != NULL);

    bpf_prog_destroy(prog);
    kprintf("[TEST] BPF create simple: PASS\n");
    return 0;
}

TEST(bpf_validate_valid) {
    // Valid program
    struct bpf_insn insns[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 2, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
    };

    struct bpf_prog* prog = bpf_prog_create(insns, 4);
    ASSERT(prog != NULL);

    int result = bpf_prog_validate(prog);
    ASSERT(result == 0);

    bpf_prog_destroy(prog);
    kprintf("[TEST] BPF validate valid: PASS\n");
    return 0;
}

TEST(bpf_validate_invalid_jump) {
    // Invalid program: jump out of bounds
    struct bpf_insn insns[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 2, 100, 1),  // Jump target out of bounds
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
    };

    struct bpf_prog* prog = bpf_prog_create(insns, 4);
    ASSERT(prog != NULL);

    int result = bpf_prog_validate(prog);
    ASSERT(result != 0);  // Should fail validation

    bpf_prog_destroy(prog);
    kprintf("[TEST] BPF validate invalid jump: PASS\n");
    return 0;
}

TEST(bpf_validate_no_ret) {
    // Invalid program: no RET at end
    struct bpf_insn insns[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch),  // Ends with LD, not RET
    };

    struct bpf_prog* prog = bpf_prog_create(insns, 2);
    ASSERT(prog != NULL);

    int result = bpf_prog_validate(prog);
    ASSERT(result != 0);  // Should fail validation

    bpf_prog_destroy(prog);
    kprintf("[TEST] BPF validate no RET: PASS\n");
    return 0;
}

// ========================================
// BPF Execution Tests
// ========================================

TEST(bpf_run_allow_all) {
    // Program that allows all syscalls
    struct bpf_insn insns[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };

    struct bpf_prog* prog = bpf_prog_create(insns, 4);
    ASSERT(prog != NULL);

    // Test data
    struct seccomp_data data = {
        .nr = 2,  // sys_read
        .arch = AUDIT_ARCH_X86_64,
        .instruction_pointer = 0x400000,
        .args = {0, 0, 0, 0, 0, 0}
    };

    uint32_t result = bpf_prog_run(prog, &data);
    ASSERT(result == SECCOMP_RET_ALLOW);

    bpf_prog_destroy(prog);
    kprintf("[TEST] BPF run allow all: PASS\n");
    return 0;
}

TEST(bpf_run_allow_specific) {
    // Program that allows only sys_read (2) and sys_write (3)
    struct bpf_insn insns[] = {
        // Check architecture
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

        // Load syscall number
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),

        // Check if sys_read (2)
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 2, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

        // Check if sys_write (3)
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 3, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

        // Deny everything else
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
    };

    struct bpf_prog* prog = bpf_prog_create(insns, 9);
    ASSERT(prog != NULL);
    ASSERT(bpf_prog_validate(prog) == 0);

    // Test allowed syscall (sys_read)
    struct seccomp_data data1 = {
        .nr = 2,
        .arch = AUDIT_ARCH_X86_64,
        .instruction_pointer = 0x400000,
        .args = {0, 0, 0, 0, 0, 0}
    };
    uint32_t result1 = bpf_prog_run(prog, &data1);
    ASSERT(result1 == SECCOMP_RET_ALLOW);

    // Test denied syscall (sys_fork)
    struct seccomp_data data2 = {
        .nr = 1,
        .arch = AUDIT_ARCH_X86_64,
        .instruction_pointer = 0x400000,
        .args = {0, 0, 0, 0, 0, 0}
    };
    uint32_t result2 = bpf_prog_run(prog, &data2);
    ASSERT(result2 == SECCOMP_RET_KILL);

    bpf_prog_destroy(prog);
    kprintf("[TEST] BPF run allow specific: PASS\n");
    return 0;
}

TEST(bpf_run_errno_return) {
    // Program that returns EPERM (errno 1) for denied syscalls
    struct bpf_insn insns[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 2, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

        // Return EPERM for everything else
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1),
    };

    struct bpf_prog* prog = bpf_prog_create(insns, 7);
    ASSERT(prog != NULL);

    struct seccomp_data data = {
        .nr = 1,  // sys_fork (denied)
        .arch = AUDIT_ARCH_X86_64,
        .instruction_pointer = 0x400000,
        .args = {0, 0, 0, 0, 0, 0}
    };

    uint32_t result = bpf_prog_run(prog, &data);
    ASSERT((result & SECCOMP_RET_ACTION_MASK) == SECCOMP_RET_ERRNO);
    ASSERT((result & SECCOMP_RET_DATA_MASK) == 1);  // EPERM

    bpf_prog_destroy(prog);
    kprintf("[TEST] BPF run errno return: PASS\n");
    return 0;
}

// ========================================
// Seccomp Filter Installation Tests
// ========================================

TEST(seccomp_install_filter) {
    // Create mock process
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    ASSERT(proc != NULL);
    memset(proc, 0, sizeof(process_t));
    proc->pid = 1;
    proc->seccomp_mode = SECCOMP_MODE_DISABLED;
    proc->seccomp_filter = NULL;

    // Create simple filter
    struct bpf_insn insns[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };

    struct bpf_prog* prog = bpf_prog_create(insns, 2);
    ASSERT(prog != NULL);

    // Install filter
    int result = seccomp_install_filter(proc, prog, SECCOMP_MODE_FILTER, 0);
    ASSERT(result == 0);
    ASSERT(proc->seccomp_mode == SECCOMP_MODE_FILTER);
    ASSERT(proc->seccomp_filter != NULL);

    // Cleanup
    kfree(proc);
    kprintf("[TEST] Seccomp install filter: PASS\n");
    return 0;
}

TEST(seccomp_check_disabled) {
    // Create mock process with no filter
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    ASSERT(proc != NULL);
    memset(proc, 0, sizeof(process_t));
    proc->seccomp_mode = SECCOMP_MODE_DISABLED;

    uint64_t args[6] = {0, 0, 0, 0, 0, 0};
    uint32_t result = seccomp_check_syscall(proc, 2, args, 0x400000);

    ASSERT(result == SECCOMP_RET_ALLOW);

    kfree(proc);
    kprintf("[TEST] Seccomp check disabled: PASS\n");
    return 0;
}

TEST(seccomp_strict_mode) {
    // Create mock process in strict mode
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    ASSERT(proc != NULL);
    memset(proc, 0, sizeof(process_t));
    proc->seccomp_mode = SECCOMP_MODE_STRICT;

    uint64_t args[6] = {0, 0, 0, 0, 0, 0};

    // Allowed: read, write, exit
    ASSERT(seccomp_check_syscall(proc, 2, args, 0x400000) == SECCOMP_RET_ALLOW);  // read
    ASSERT(seccomp_check_syscall(proc, 3, args, 0x400000) == SECCOMP_RET_ALLOW);  // write
    ASSERT(seccomp_check_syscall(proc, 0, args, 0x400000) == SECCOMP_RET_ALLOW);  // exit

    // Denied: everything else
    ASSERT(seccomp_check_syscall(proc, 1, args, 0x400000) == SECCOMP_RET_KILL);  // fork
    ASSERT(seccomp_check_syscall(proc, 7, args, 0x400000) == SECCOMP_RET_KILL);  // execve

    kfree(proc);
    kprintf("[TEST] Seccomp strict mode: PASS\n");
    return 0;
}

// ========================================
// Profile Tests
// ========================================

TEST(seccomp_load_profile_strict) {
    // Create mock process
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    ASSERT(proc != NULL);
    memset(proc, 0, sizeof(process_t));
    proc->pid = 1;
    proc->seccomp_mode = SECCOMP_MODE_DISABLED;

    // Load strict profile
    int result = seccomp_load_profile(proc, SECCOMP_PROFILE_STRICT);
    ASSERT(result == 0);
    ASSERT(proc->seccomp_mode == SECCOMP_MODE_FILTER);
    ASSERT(proc->seccomp_filter != NULL);

    // Test filter
    uint64_t args[6] = {0, 0, 0, 0, 0, 0};
    ASSERT(seccomp_check_syscall(proc, 2, args, 0x400000) == SECCOMP_RET_ALLOW);  // read
    ASSERT(seccomp_check_syscall(proc, 1, args, 0x400000) == SECCOMP_RET_KILL);   // fork

    kfree(proc);
    kprintf("[TEST] Seccomp load profile strict: PASS\n");
    return 0;
}

TEST(seccomp_load_profile_browser) {
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    ASSERT(proc != NULL);
    memset(proc, 0, sizeof(process_t));
    proc->pid = 1;

    int result = seccomp_load_profile(proc, SECCOMP_PROFILE_BROWSER);
    ASSERT(result == 0);

    uint64_t args[6] = {0, 0, 0, 0, 0, 0};
    // Browser profile denies exec
    uint32_t action = seccomp_check_syscall(proc, 7, args, 0x400000);  // execve
    ASSERT((action & SECCOMP_RET_ACTION_MASK) == SECCOMP_RET_KILL);

    // Browser profile denies fork but returns errno
    action = seccomp_check_syscall(proc, 1, args, 0x400000);  // fork
    ASSERT((action & SECCOMP_RET_ACTION_MASK) == SECCOMP_RET_ERRNO);

    kfree(proc);
    kprintf("[TEST] Seccomp load profile browser: PASS\n");
    return 0;
}

TEST(seccomp_load_profile_untrusted) {
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    ASSERT(proc != NULL);
    memset(proc, 0, sizeof(process_t));
    proc->pid = 1;

    int result = seccomp_load_profile(proc, SECCOMP_PROFILE_UNTRUSTED);
    ASSERT(result == 0);

    uint64_t args[6] = {0, 0, 0, 0, 0, 0};
    // Untrusted profile only allows read/write/exit/getpid
    ASSERT(seccomp_check_syscall(proc, 2, args, 0x400000) == SECCOMP_RET_ALLOW);  // read
    ASSERT(seccomp_check_syscall(proc, 8, args, 0x400000) == SECCOMP_RET_ALLOW);  // getpid
    ASSERT(seccomp_check_syscall(proc, 1, args, 0x400000) == SECCOMP_RET_KILL);   // fork

    kfree(proc);
    kprintf("[TEST] Seccomp load profile untrusted: PASS\n");
    return 0;
}

// ========================================
// Performance Tests
// ========================================

TEST(seccomp_performance) {
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    ASSERT(proc != NULL);
    memset(proc, 0, sizeof(process_t));
    proc->pid = 1;

    seccomp_load_profile(proc, SECCOMP_PROFILE_BROWSER);

    uint64_t args[6] = {0, 0, 0, 0, 0, 0};

    // Benchmark 10,000 checks
    uint64_t start = rdtsc();
    for (int i = 0; i < 10000; i++) {
        seccomp_check_syscall(proc, 2, args, 0x400000);
    }
    uint64_t end = rdtsc();

    uint64_t total_cycles = end - start;
    uint64_t avg_cycles = total_cycles / 10000;

    kprintf("[PERF] Seccomp check: %llu cycles average\n", avg_cycles);
    ASSERT(avg_cycles < 100);  // Must be under 100 cycles per check

    kfree(proc);
    kprintf("[TEST] Seccomp performance: PASS (avg %llu cycles)\n", avg_cycles);
    return 0;
}

// ========================================
// Test Runner
// ========================================

void run_sandbox_tests(void) {
    kprintf("[TEST] =====================================\n");
    kprintf("[TEST] Running Sandbox Tests\n");
    kprintf("[TEST] =====================================\n\n");

    int passed = 0;
    int failed = 0;

    #define RUN_TEST(name) do { \
        kprintf("[TEST] Running: %s\n", #name); \
        if (test_##name() == 0) { \
            passed++; \
        } else { \
            failed++; \
            kprintf("[TEST] FAILED: %s\n", #name); \
        } \
    } while(0)

    // BPF tests
    RUN_TEST(bpf_create_simple);
    RUN_TEST(bpf_validate_valid);
    RUN_TEST(bpf_validate_invalid_jump);
    RUN_TEST(bpf_validate_no_ret);
    RUN_TEST(bpf_run_allow_all);
    RUN_TEST(bpf_run_allow_specific);
    RUN_TEST(bpf_run_errno_return);

    // Seccomp tests
    RUN_TEST(seccomp_install_filter);
    RUN_TEST(seccomp_check_disabled);
    RUN_TEST(seccomp_strict_mode);

    // Profile tests
    RUN_TEST(seccomp_load_profile_strict);
    RUN_TEST(seccomp_load_profile_browser);
    RUN_TEST(seccomp_load_profile_untrusted);

    // Performance tests
    RUN_TEST(seccomp_performance);

    kprintf("\n[TEST] =====================================\n");
    kprintf("[TEST] Results: %d passed, %d failed\n", passed, failed);
    kprintf("[TEST] =====================================\n");
}

#include "../../include/seccomp.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/string.h"
#include "../../include/perf.h"

// BPF VM registers
struct bpf_vm {
    uint32_t A;              // Accumulator register
    uint32_t X;              // Index register
    uint32_t pc;             // Program counter
    const struct seccomp_data* data;  // Input data
};

// Maximum BPF program length (prevent DoS)
#define MAX_BPF_INSNS 4096

// Maximum jump distance (prevent infinite loops)
#define MAX_BPF_JUMP 256

// ========================================
// BPF Program Management
// ========================================

struct bpf_prog* bpf_prog_create(const struct bpf_insn* insns, uint32_t len) {
    if (!insns || len == 0 || len > MAX_BPF_INSNS) {
        kprintf("[SECCOMP] Invalid BPF program: len=%u\n", len);
        return NULL;
    }

    struct bpf_prog* prog = (struct bpf_prog*)kmalloc(sizeof(struct bpf_prog));
    if (!prog) {
        kprintf("[SECCOMP] Failed to allocate BPF program\n");
        return NULL;
    }

    // Allocate and copy instructions
    prog->insns = (struct bpf_insn*)kmalloc(sizeof(struct bpf_insn) * len);
    if (!prog->insns) {
        kfree(prog);
        return NULL;
    }

    memcpy(prog->insns, insns, sizeof(struct bpf_insn) * len);
    prog->len = len;
    prog->flags = 0;
    prog->ref_count = 1;

    return prog;
}

void bpf_prog_destroy(struct bpf_prog* prog) {
    if (!prog) return;

    if (--prog->ref_count == 0) {
        if (prog->insns) {
            kfree(prog->insns);
        }
        kfree(prog);
    }
}

// ========================================
// BPF Program Validation
// ========================================

int bpf_prog_validate(const struct bpf_prog* prog) {
    if (!prog || !prog->insns || prog->len == 0) {
        return -1;
    }

    // Check each instruction
    for (uint32_t i = 0; i < prog->len; i++) {
        const struct bpf_insn* insn = &prog->insns[i];
        uint16_t code = insn->code;
        uint8_t cls = code & BPF_CLASS_MASK;

        // Validate instruction class
        switch (cls) {
            case BPF_LD: {
                // Load instruction
                uint8_t mode = code & BPF_MODE_MASK;
                if (mode == BPF_ABS) {
                    // Check offset is within seccomp_data bounds
                    uint32_t offset = insn->k;
                    uint8_t size = code & BPF_SIZE_MASK;
                    uint32_t access_size = (size == BPF_W) ? 4 : (size == BPF_H) ? 2 : 1;

                    if (offset + access_size > sizeof(struct seccomp_data)) {
                        kprintf("[SECCOMP] BPF validation failed: out-of-bounds load at insn %u\n", i);
                        return -1;
                    }
                } else if (mode != BPF_IMM) {
                    kprintf("[SECCOMP] BPF validation failed: unsupported load mode at insn %u\n", i);
                    return -1;
                }
                break;
            }

            case BPF_JMP: {
                // Jump instruction
                if ((code & BPF_OP_MASK) == 0) {
                    // Unconditional jump (deprecated, not used in seccomp)
                    kprintf("[SECCOMP] BPF validation failed: unconditional jump at insn %u\n", i);
                    return -1;
                }

                // Check jump targets are in bounds
                uint32_t jt_target = i + insn->jt + 1;
                uint32_t jf_target = i + insn->jf + 1;

                if (jt_target >= prog->len || jf_target >= prog->len) {
                    kprintf("[SECCOMP] BPF validation failed: jump out of bounds at insn %u\n", i);
                    return -1;
                }

                // Check jump distance (prevent crazy jumps)
                if (insn->jt > MAX_BPF_JUMP || insn->jf > MAX_BPF_JUMP) {
                    kprintf("[SECCOMP] BPF validation failed: jump too far at insn %u\n", i);
                    return -1;
                }
                break;
            }

            case BPF_RET:
                // Return instruction - always valid
                break;

            default:
                kprintf("[SECCOMP] BPF validation failed: invalid instruction class at insn %u\n", i);
                return -1;
        }
    }

    // Program must end with RET instruction
    if ((prog->insns[prog->len - 1].code & BPF_CLASS_MASK) != BPF_RET) {
        kprintf("[SECCOMP] BPF validation failed: program does not end with RET\n");
        return -1;
    }

    return 0;
}

// ========================================
// BPF Interpreter (VM)
// ========================================

static inline uint32_t bpf_load(const struct seccomp_data* data, uint32_t offset, uint8_t size) {
    const uint8_t* ptr = (const uint8_t*)data + offset;

    switch (size) {
        case BPF_W:  // 32-bit word
            return *(const uint32_t*)ptr;
        case BPF_H:  // 16-bit half-word
            return *(const uint16_t*)ptr;
        case BPF_B:  // 8-bit byte
            return *(const uint8_t*)ptr;
        default:
            return 0;
    }
}

uint32_t bpf_prog_run(const struct bpf_prog* prog, const struct seccomp_data* data) {
    if (!prog || !data) {
        return SECCOMP_RET_KILL;
    }

#ifdef PERF_SECCOMP
    uint64_t start = rdtsc();
#endif

    struct bpf_vm vm = {0};
    vm.data = data;

    // Execute BPF program
    uint32_t max_iterations = prog->len * 10;  // Prevent infinite loops
    uint32_t iterations = 0;

    while (vm.pc < prog->len && iterations < max_iterations) {
        const struct bpf_insn* insn = &prog->insns[vm.pc];
        uint16_t code = insn->code;
        uint8_t cls = code & BPF_CLASS_MASK;

        iterations++;

        switch (cls) {
            case BPF_LD: {
                uint8_t mode = code & BPF_MODE_MASK;
                if (mode == BPF_ABS) {
                    // Load from absolute offset in seccomp_data
                    uint8_t size = code & BPF_SIZE_MASK;
                    vm.A = bpf_load(data, insn->k, size);
                } else if (mode == BPF_IMM) {
                    // Load immediate value
                    vm.A = insn->k;
                }
                vm.pc++;
                break;
            }

            case BPF_JMP: {
                uint8_t op = code & BPF_OP_MASK;
                uint8_t src = code & BPF_SRC_MASK;
                uint32_t k = (src == BPF_K) ? insn->k : vm.X;
                bool condition = false;

                switch (op) {
                    case BPF_JEQ:
                        condition = (vm.A == k);
                        break;
                    case BPF_JGT:
                        condition = (vm.A > k);
                        break;
                    case BPF_JGE:
                        condition = (vm.A >= k);
                        break;
                    case BPF_JSET:
                        condition = ((vm.A & k) != 0);
                        break;
                    default:
                        // Unknown jump op - treat as false
                        condition = false;
                        break;
                }

                if (condition) {
                    vm.pc += insn->jt + 1;
                } else {
                    vm.pc += insn->jf + 1;
                }
                break;
            }

            case BPF_RET: {
                // Return action
                uint32_t ret = insn->k;
#ifdef PERF_SECCOMP
                uint64_t end = rdtsc();
                uint64_t cycles = end - start;
                if (cycles > 100) {
                    kprintf("[PERF] Seccomp BPF: %llu cycles, %u iterations\n", cycles, iterations);
                }
#endif
                return ret;
            }

            default:
                // Invalid instruction - kill process
                kprintf("[SECCOMP] BPF execution error: invalid instruction at pc=%u\n", vm.pc);
                return SECCOMP_RET_KILL;
        }
    }

    // Exceeded max iterations or fell off end - kill
    kprintf("[SECCOMP] BPF execution error: exceeded max iterations or no RET\n");
    return SECCOMP_RET_KILL;
}

// ========================================
// Helper Functions for Building Filters
// ========================================

// Check syscall number against value
// Returns ALLOW or DENY based on match
struct bpf_insn* bpf_syscall_allow(uint32_t syscall_nr, uint32_t* len) {
    // Allocate 4 instructions
    struct bpf_insn* insns = (struct bpf_insn*)kmalloc(sizeof(struct bpf_insn) * 4);
    if (!insns) return NULL;

    *len = 4;

    // Load syscall number
    insns[0] = BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr);

    // Compare with target syscall
    insns[1] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, syscall_nr, 0, 1);

    // If equal: allow
    insns[2] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    // If not equal: continue to next check
    insns[3] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL);

    return insns;
}

// Check architecture (prevent 32-bit syscalls on 64-bit system)
void bpf_check_arch_x86_64(struct bpf_insn* insns, uint32_t kill_label) {
    // Load architecture field
    insns[0] = BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch);

    // Check if x86-64
    insns[1] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);

    // If not x86-64: kill
    insns[2] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL);
}

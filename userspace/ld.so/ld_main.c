/**
 * Dynamic Linker Entry Point
 * ===========================
 *
 * This is the entry point for ld.so when invoked by the kernel
 * via the PT_INTERP segment of a dynamically linked executable.
 *
 * The kernel loads ld.so and the main executable, then jumps to
 * ld.so's entry point with the auxiliary vector on the stack.
 */

#include "linker.h"

// Stack layout at entry (set up by kernel):
//
// High addresses
// +------------------+
// | envp strings     |
// +------------------+
// | argv strings     |
// +------------------+
// | auxv (AT_* pairs)|  <- Auxiliary vector
// +------------------+
// | NULL             |
// | envp[n-1]        |
// | ...              |
// | envp[0]          |
// +------------------+
// | NULL             |
// | argv[n-1]        |
// | ...              |
// | argv[0]          |
// +------------------+
// | argc             |  <- Stack pointer at entry
// +------------------+
// Low addresses

// Global linker context
static linker_context_t linker_ctx;

/**
 * Parse auxiliary vector from stack
 *
 * Extracts information passed by kernel (program headers, entry point, etc.)
 *
 * @param auxv Pointer to auxiliary vector
 * @param phdr_out Output: pointer to program headers
 * @param phent_out Output: size of program header entry
 * @param phnum_out Output: number of program headers
 * @param entry_out Output: program entry point
 * @param base_out Output: interpreter base address
 */
static void parse_auxv(elf64_auxv_t* auxv,
                       elf64_phdr_t** phdr_out,
                       uint64_t* phent_out,
                       uint64_t* phnum_out,
                       uint64_t* entry_out,
                       uint64_t* base_out) {
    while (auxv->a_type != AT_NULL) {
        switch (auxv->a_type) {
            case AT_PHDR:
                *phdr_out = (elf64_phdr_t*)auxv->a_un.a_val;
                break;
            case AT_PHENT:
                *phent_out = auxv->a_un.a_val;
                break;
            case AT_PHNUM:
                *phnum_out = auxv->a_un.a_val;
                break;
            case AT_ENTRY:
                *entry_out = auxv->a_un.a_val;
                break;
            case AT_BASE:
                *base_out = auxv->a_un.a_val;
                break;
        }
        auxv++;
    }
}

/**
 * Dynamic linker entry point
 *
 * Called by kernel with stack set up according to ELF ABI.
 * Loads dependencies, performs relocations, and transfers control
 * to the application's entry point.
 *
 * Stack at entry:
 *   argc
 *   argv[0]...argv[argc-1]
 *   NULL
 *   envp[0]...envp[n-1]
 *   NULL
 *   auxv[0]...auxv[n-1]
 *   AT_NULL
 */
void _start(void) {
    // Get stack pointer
    uint64_t* sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));

    // Parse stack
    uint64_t argc = *sp++;
    char** argv = (char**)sp;
    sp += argc + 1;  // Skip argv and NULL

    char** envp = (char**)sp;
    while (*sp++) ;  // Skip envp and NULL

    elf64_auxv_t* auxv = (elf64_auxv_t*)sp;

    // Parse auxiliary vector
    elf64_phdr_t* phdr = NULL;
    uint64_t phent = 0;
    uint64_t phnum = 0;
    uint64_t entry = 0;
    uint64_t base = 0;

    parse_auxv(auxv, &phdr, &phent, &phnum, &entry, &base);

    // Initialize linker
    linker_init(&linker_ctx);
    linker_ctx.debug = 1;  // Enable debug output

    // Create shared object for main executable
    // (This is simplified - would need to properly map the executable)
    shared_object_t* main_obj = linker_load_object(&linker_ctx, argv[0], RTLD_NOW);
    if (!main_obj) {
        // Fatal error - can't continue
        // TODO: Print error message
        __asm__ volatile("mov $1, %rax; mov $1, %rdi; syscall");  // exit(1)
    }

    linker_ctx.main_object = main_obj;

    // Load dependencies
    int ret = linker_load_dependencies(&linker_ctx, main_obj, RTLD_NOW);
    if (ret < 0) {
        // Fatal error
        __asm__ volatile("mov $1, %rax; mov $2, %rdi; syscall");  // exit(2)
    }

    // Relocate all objects
    ret = linker_relocate_all(&linker_ctx, RTLD_NOW);
    if (ret < 0) {
        // Fatal error
        __asm__ volatile("mov $1, %rax; mov $3, %rdi; syscall");  // exit(3)
    }

    // Run initializers
    ret = linker_run_initializers(&linker_ctx);
    if (ret < 0) {
        // Fatal error
        __asm__ volatile("mov $1, %rax; mov $4, %rdi; syscall");  // exit(4)
    }

    // Set up stack for main program
    // The stack is already set up correctly, we just need to jump to entry

    // Transfer control to main program
    // Jump to entry point with original stack
    __asm__ volatile(
        "mov %0, %%rax\n"
        "jmp *%%rax\n"
        : : "r"(entry)
    );

    // Should never reach here
    __asm__ volatile("mov $1, %rax; mov $255, %rdi; syscall");  // exit(255)
}

/**
 * Simplified exit function
 */
void _exit(int status) {
    // Run finalizers
    linker_run_finalizers(&linker_ctx);

    // Call exit syscall
    __asm__ volatile(
        "mov $1, %%rax\n"      // syscall number for exit
        "mov %0, %%rdi\n"       // exit code
        "syscall\n"
        : : "r"((uint64_t)status)
    );

    // Should never return
    while (1) ;
}

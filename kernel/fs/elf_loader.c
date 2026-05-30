/**
 * ELF64 Loader for AutomationOS
 * ==============================
 *
 * Loads ELF64 executables from initrd and prepares them for user mode execution.
 *
 * CRITICAL BLOCKER #3: This is required for loading /init from initrd.
 */

#include "../include/kernel.h"
#include "../include/elf.h"
#include "../include/mem.h"
#include "../include/initrd.h"
#include "../include/string.h"

// User space constants
#define USER_STACK_SIZE (8 * 1024 * 1024)  // 8MB user stack
#define USER_STACK_TOP  0x00007FFFFFFFE000ULL  // Just below kernel space

/**
 * Validate ELF64 header
 *
 * Checks magic number, class, architecture, and type.
 */
int elf_validate_header(const elf64_ehdr_t* ehdr) {
    if (!ehdr) {
        return 0;
    }

    // Check ELF magic number
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        kprintf("[ELF] Invalid magic number\n");
        return 0;
    }

    // Check ELF class (must be 64-bit)
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        kprintf("[ELF] Not a 64-bit ELF (class=%d)\n", ehdr->e_ident[EI_CLASS]);
        return 0;
    }

    // Check data encoding (must be little-endian)
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        kprintf("[ELF] Not little-endian (data=%d)\n", ehdr->e_ident[EI_DATA]);
        return 0;
    }

    // Check version
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
        kprintf("[ELF] Invalid ELF version (version=%d)\n", ehdr->e_ident[EI_VERSION]);
        return 0;
    }

    // Check machine type (must be x86_64)
    if (ehdr->e_machine != EM_X86_64) {
        kprintf("[ELF] Wrong architecture (machine=%d, expected %d)\n",
                ehdr->e_machine, EM_X86_64);
        return 0;
    }

    // Check file type (must be executable)
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        kprintf("[ELF] Not an executable (type=%d)\n", ehdr->e_type);
        return 0;
    }

    // Check entry point (must be in user space)
    if (ehdr->e_entry >= KERNEL_SPACE_START) {
        kprintf("[ELF] Entry point 0x%016lx is in kernel space\n", ehdr->e_entry);
        return 0;
    }

    return 1;
}

/**
 * Calculate page permissions from ELF program header flags
 */
static uint32_t elf_pf_to_page_flags(uint32_t p_flags) {
    uint32_t flags = PAGE_PRESENT | PAGE_USER;

    // Writable segments get PAGE_WRITE
    if (p_flags & PF_W) {
        flags |= PAGE_WRITE;
    }

    // Note: x86_64 doesn't have separate execute permission in page tables
    // Execute permission is controlled by NX bit (not set = executable)

    return flags;
}

/**
 * Load a PT_LOAD segment into user memory
 *
 * Allocates pages, maps them to user virtual addresses, and copies segment data.
 * Handles BSS (p_memsz > p_filesz) by zero-filling the remainder.
 */
static int elf_load_segment(const elf64_phdr_t* phdr, const void* elf_data) {
    // Calculate aligned boundaries
    uint64_t vaddr_start = ALIGN_DOWN(phdr->p_vaddr, PAGE_SIZE);
    uint64_t vaddr_end = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, PAGE_SIZE);
    uint64_t num_pages = (vaddr_end - vaddr_start) / PAGE_SIZE;

    kprintf("[ELF]   Segment: vaddr=0x%016lx size=0x%lx pages=%lu flags=%s%s%s\n",
            phdr->p_vaddr, phdr->p_memsz, num_pages,
            (phdr->p_flags & PF_R) ? "R" : "-",
            (phdr->p_flags & PF_W) ? "W" : "-",
            (phdr->p_flags & PF_X) ? "X" : "-");

    // Validate segment is in user space
    if (vaddr_start >= USER_SPACE_END) {
        kprintf("[ELF]   ERROR: Segment vaddr 0x%016lx is outside user space\n",
                vaddr_start);
        return ELF_ERR_PERM;
    }

    // Calculate page flags
    uint32_t page_flags = elf_pf_to_page_flags(phdr->p_flags);

    // --- Phase 1: allocate + map the whole segment page range up front ---
    // Page allocation is kept separate from data loading, and done in BATCHES:
    // one contiguous physical run for the whole segment + one range-map (which
    // walks the upper page-table levels once per 2MB instead of per page). We do
    // NOT pre-zero pages here -- that would write every page twice (once to zero,
    // once via the file copy). We zero only the non-file bytes in Phase 2.
    extern void* pmm_alloc_pages(size_t count);
    extern int   vmm_map_range(uint64_t vaddr, uint64_t paddr, uint64_t count, uint64_t flags);

    void* phys_base = pmm_alloc_pages(num_pages);
    if (phys_base) {
        // Fast path: one contiguous run, one batched range map.
        vmm_map_range(vaddr_start, (uint64_t)phys_base, num_pages, page_flags);
    } else {
        // Fallback: physical memory too fragmented for a contiguous run --
        // allocate + map page by page (still correct, just slower).
        for (uint64_t i = 0; i < num_pages; i++) {
            uint64_t vaddr = vaddr_start + (i * PAGE_SIZE);
            void* phys_page = pmm_alloc_page();
            if (!phys_page) {
                kprintf("[ELF]   ERROR: Out of physical memory\n");
                return ELF_ERR_NOMEM;
            }
            vmm_map_page((void*)vaddr, phys_page, page_flags);
        }
    }

    // --- Phase 2: load file data, then zero only the non-file-backed bytes ---
    uint64_t file_end = phdr->p_vaddr + phdr->p_filesz;

    // Leading gap: [page-aligned start .. p_vaddr) when p_vaddr isn't aligned.
    if (phdr->p_vaddr > vaddr_start) {
        memset((void*)vaddr_start, 0, phdr->p_vaddr - vaddr_start);
    }

    // File-backed bytes: a single bulk copy (memcpy is word-at-a-time).
    if (phdr->p_filesz > 0) {
        const void* src = (const uint8_t*)elf_data + phdr->p_offset;
        memcpy((void*)phdr->p_vaddr, src, phdr->p_filesz);
    }

    // BSS + trailing page padding: [file_end .. aligned segment end). This
    // covers p_memsz > p_filesz (BSS) AND the slack up to the page boundary,
    // so no page byte is left uninitialized.
    if (vaddr_end > file_end) {
        kprintf("[ELF]   BSS+pad: zero-filling 0x%lx bytes at 0x%016lx\n",
                vaddr_end - file_end, file_end);
        memset((void*)file_end, 0, vaddr_end - file_end);
    }

    return ELF_SUCCESS;
}

/**
 * Setup user mode stack
 *
 * Allocates stack pages and places argc, argv, and envp on the stack.
 * Returns initial stack pointer (RSP value).
 */
static int elf_setup_stack(int argc, char** argv, uint64_t* stack_out) {
    // Calculate stack boundaries
    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
    uint64_t num_pages = USER_STACK_SIZE / PAGE_SIZE;

    kprintf("[ELF] Setting up user stack: 0x%016lx - 0x%016lx (%lu pages)\n",
            stack_bottom, USER_STACK_TOP, num_pages);

    // Allocate and map stack pages
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t vaddr = stack_bottom + (i * PAGE_SIZE);
        void* phys_page = pmm_alloc_page();

        if (!phys_page) {
            kprintf("[ELF] ERROR: Out of memory for stack\n");
            return ELF_ERR_NOMEM;
        }

        // Map with user permissions and write enabled
        vmm_map_page((void*)vaddr, phys_page, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

        // Zero the page
        memset(phys_page, 0, PAGE_SIZE);
    }

    // Setup stack contents
    // Stack layout (grows down):
    //   [envp strings]
    //   [argv strings]
    //   [NULL]              <- envp terminator
    //   [envp[n-1]]
    //   ...
    //   [envp[0]]
    //   [NULL]              <- argv terminator
    //   [argv[n-1]]
    //   ...
    //   [argv[0]]
    //   [argc]              <- RSP points here
    //
    // For now, we'll do a simple layout with just argc and NULL-terminated argv

    uint64_t sp = USER_STACK_TOP;

    // Align stack to 16 bytes (required by x86_64 ABI)
    sp &= ~0xFULL;

    // Reserve space for argc (8 bytes)
    sp -= 8;
    *(uint64_t*)sp = argc;

    // Reserve space for argv array (NULL-terminated)
    // For simplicity, we'll leave argv as NULL for now
    // TODO: Properly copy argv strings and pointers

    // Final alignment check
    if (sp & 0xF) {
        sp &= ~0xFULL;
    }

    *stack_out = sp;

    kprintf("[ELF] Stack initialized, RSP=0x%016lx\n", sp);

    return ELF_SUCCESS;
}

/**
 * Load ELF64 executable from initrd
 *
 * Main entry point for ELF loader. Performs complete loading:
 * 1. Retrieve ELF file from initrd
 * 2. Validate ELF header
 * 3. Load PT_LOAD segments into user memory
 * 4. Setup user mode stack
 * 5. Return entry point and stack pointer
 *
 * @param path Path to ELF file in initrd
 * @param argc Argument count
 * @param argv Argument vector (currently unused, TODO)
 * @param entry_out Output: entry point virtual address
 * @param stack_out Output: initial stack pointer
 * @return ELF_SUCCESS on success, negative error code on failure
 */
int elf_load(const char* path, int argc, char** argv,
             uint64_t* entry_out, uint64_t* stack_out) {
    if (!path || !entry_out || !stack_out) {
        return ELF_ERR_INVALID;
    }

    kprintf("[ELF] Loading ELF: %s\n", path);

    // Get file from initrd
    uint64_t file_size = 0;
    void* elf_data = initrd_get_file(path, &file_size);

    if (!elf_data) {
        kprintf("[ELF] File not found: %s\n", path);
        return ELF_ERR_NOT_FOUND;
    }

    kprintf("[ELF] Found file: %s (size=%lu bytes)\n", path, file_size);

    // Validate ELF header size
    if (file_size < sizeof(elf64_ehdr_t)) {
        kprintf("[ELF] File too small to be valid ELF\n");
        return ELF_ERR_INVALID;
    }

    // Validate ELF header
    const elf64_ehdr_t* ehdr = (const elf64_ehdr_t*)elf_data;

    if (!elf_validate_header(ehdr)) {
        return ELF_ERR_INVALID;
    }

    kprintf("[ELF] Valid ELF64 executable\n");
    kprintf("[ELF]   Entry point: 0x%016lx\n", ehdr->e_entry);
    kprintf("[ELF]   Program headers: %d entries at offset 0x%lx\n",
            ehdr->e_phnum, ehdr->e_phoff);

    // e_phnum * e_phentsize is otherwise computed in 32-bit int and can wrap;
    // promote all three terms to 64-bit. Also require the canonical entry size
    // since phdr[i] below indexes with a fixed elf64_phdr_t stride.
    if (ehdr->e_phentsize != sizeof(elf64_phdr_t) || ehdr->e_phnum > 1024) {
        kprintf("[ELF] ERROR: bad e_phentsize=%u / e_phnum=%u\n",
                ehdr->e_phentsize, ehdr->e_phnum);
        return ELF_ERR_INVALID;
    }
    // Check program header table is within file bounds
    uint64_t phdr_end = (uint64_t)ehdr->e_phoff
                      + (uint64_t)ehdr->e_phnum * (uint64_t)ehdr->e_phentsize;
    if (phdr_end > file_size) {
        kprintf("[ELF] Program header table extends beyond file\n");
        return ELF_ERR_INVALID;
    }

    // Load PT_LOAD segments
    const elf64_phdr_t* phdr = (const elf64_phdr_t*)((uint8_t*)elf_data + ehdr->e_phoff);

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            int ret = elf_load_segment(&phdr[i], elf_data);
            if (ret != ELF_SUCCESS) {
                return ret;
            }
        } else {
            kprintf("[ELF]   Skipping segment type 0x%x\n", phdr[i].p_type);
        }
    }

    // Setup user stack
    int ret = elf_setup_stack(argc, argv, stack_out);
    if (ret != ELF_SUCCESS) {
        return ret;
    }

    // Return entry point
    *entry_out = ehdr->e_entry;

    kprintf("[ELF] Load complete: entry=0x%016lx stack=0x%016lx\n",
            *entry_out, *stack_out);

    return ELF_SUCCESS;
}

/**
 * Print ELF file information (debugging)
 */
void elf_print_info(const char* path) {
    uint64_t file_size = 0;
    void* elf_data = initrd_get_file(path, &file_size);

    if (!elf_data) {
        kprintf("[ELF] File not found: %s\n", path);
        return;
    }

    const elf64_ehdr_t* ehdr = (const elf64_ehdr_t*)elf_data;

    kprintf("[ELF] File: %s\n", path);
    kprintf("  Size: %lu bytes\n", file_size);

    if (!elf_validate_header(ehdr)) {
        kprintf("  Invalid ELF header\n");
        return;
    }

    kprintf("  Type: 0x%x\n", ehdr->e_type);
    kprintf("  Machine: 0x%x\n", ehdr->e_machine);
    kprintf("  Entry: 0x%016lx\n", ehdr->e_entry);
    kprintf("  Program headers: %d\n", ehdr->e_phnum);
    kprintf("  Section headers: %d\n", ehdr->e_shnum);

    // Print program headers
    const elf64_phdr_t* phdr = (const elf64_phdr_t*)((uint8_t*)elf_data + ehdr->e_phoff);

    kprintf("  Program Headers:\n");
    for (int i = 0; i < ehdr->e_phnum; i++) {
        kprintf("    [%d] type=0x%x vaddr=0x%016lx memsz=0x%lx flags=0x%x\n",
                i, phdr[i].p_type, phdr[i].p_vaddr,
                phdr[i].p_memsz, phdr[i].p_flags);
    }
}

/**
 * Exec - Execute User Mode Programs
 * ==================================
 *
 * High-level interface for loading and executing ELF programs.
 * Bridges ELF loader with process management.
 */

#include "../include/kernel.h"
#include "../include/elf.h"
#include "../include/sched.h"
#include "../include/mem.h"
#include "../include/string.h"
#include "../include/vma.h"
#include "../include/x86_64.h"

/* Verbose per-spawn ELF-loader logging is gated: it emits ~20 serial lines per
 * process, which dominates boot time under QEMU (serial I/O is slow). Define
 * EXEC_QUIET (set in the kernel build) to silence it. */
#ifdef EXEC_QUIET
#define EXEC_LOG(...) ((void)0)
#else
#define EXEC_LOG(...) kprintf(__VA_ARGS__)
#endif

// GDT segment selectors (from gdt.c)
// Entry 3 = user code (0x18), with RPL=3 → 0x1B
// Entry 4 = user data (0x20), with RPL=3 → 0x23
#define USER_CODE_SELECTOR 0x1B
#define USER_DATA_SELECTOR 0x23

/**
 * Jump to user mode
 *
 * Assembly stub to perform ring transition from ring 0 to ring 3.
 * Sets up segment selectors and uses IRET to change privilege level.
 *
 * Parameters:
 *   - RDI: entry point (RIP)
 *   - RSI: stack pointer (RSP)
 */
static void jump_to_usermode(uint64_t entry, uint64_t stack) {
    asm volatile(
        // Setup user data segments
        "mov $0x23, %%ax\n"     // User data selector (0x20 | RPL=3)
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"

        // Setup IRET frame on kernel stack
        // Stack frame (pushed in reverse order):
        //   SS      <- top (ESP+24)
        //   RSP     <- ESP+16
        //   RFLAGS  <- ESP+8
        //   CS      <- ESP+0
        //   RIP

        "push $0x23\n"          // SS (user data selector)
        "push %1\n"             // RSP (user stack)
        "pushf\n"               // RFLAGS (current flags)
        "pop %%rax\n"
        "or $0x200, %%rax\n"    // Set IF (interrupts enabled)
        "push %%rax\n"
        "push $0x1B\n"          // CS (user code selector)
        "push %0\n"             // RIP (entry point)

        // Execute IRET to jump to user mode
        "iretq\n"
        :
        : "r"(entry), "r"(stack)
        : "rax", "memory"
    );
}

/**
 * Execute ELF program in user mode
 *
 * Loads ELF from initrd and jumps to user mode.
 * This function does NOT return.
 *
 * @param path Path to ELF executable in initrd
 * @param argc Argument count
 * @param argv Argument vector
 */
void exec_usermode(const char* path, int argc, char** argv) {
    EXEC_LOG("[EXEC] Executing: %s\n", path);

    uint64_t entry = 0;
    uint64_t stack = 0;

    // Load ELF into memory
    int ret = elf_load(path, argc, argv, &entry, &stack);

    if (ret != ELF_SUCCESS) {
        EXEC_LOG("[EXEC] Failed to load ELF: error %d\n", ret);
        kernel_panic("exec_usermode: ELF load failed");
    }

    EXEC_LOG("[EXEC] Jumping to user mode: entry=0x%016lx stack=0x%016lx\n",
            entry, stack);

    // Jump to user mode (does not return)
    jump_to_usermode(entry, stack);

    // Should never reach here
    kernel_panic("exec_usermode: returned from user mode");
}

/**
 * Create a new process and load ELF into it
 *
 * Higher-level interface that creates a process control block,
 * loads the ELF, and adds it to the scheduler.
 *
 * @param path Path to ELF executable
 * @param name Process name
 * @param argc Argument count
 * @param argv Argument vector
 * @return Process control block, or NULL on failure
 */
process_t* exec_create_process(const char* path, const char* name,
                                int argc, char** argv) {
    EXEC_LOG("[EXEC] Creating process: %s (%s)\n", name, path);

    uint64_t entry = 0;
    uint64_t stack = 0;

    // Load ELF into memory
    int ret = elf_load(path, argc, argv, &entry, &stack);

    if (ret != ELF_SUCCESS) {
        EXEC_LOG("[EXEC] Failed to load ELF: error %d\n", ret);
        return NULL;
    }

    // Create process control block
    // Note: We pass entry point, but we'll override the context below
    process_t* proc = process_create(name, (void*)entry);

    if (!proc) {
        EXEC_LOG("[EXEC] Failed to create process\n");
        // Note: elf_load already managed its own allocations; if it succeeded
        // and we fail here, the caller is responsible for cleanup. However,
        // elf_load allocates pages into the current address space (kernel)
        // which won't leak - kernel pages persist.
        return NULL;
    }

    // Setup user mode context
    proc->context.rip = entry;
    proc->context.rsp = stack;
    proc->context.rflags = 0x202;  // IF (interrupts enabled)

    // Set user mode segment selectors
    // Note: These will be pushed by the scheduler's context switch
    // For now, we'll handle this in the assembly context switch code

    // Mark as ready to run
    process_set_ready(proc);

    EXEC_LOG("[EXEC] Process created: PID=%d entry=0x%016lx stack=0x%016lx\n",
            proc->pid, entry, stack);

    return proc;
}

/**
 * Launch /init from initrd
 *
 * Convenience function to load and execute the init process.
 * This is typically called during kernel initialization.
 *
 * @return 0 on success, negative on error
 */
int exec_launch_init(void) {
    EXEC_LOG("[EXEC] Launching /init...\n");

    // Create init process
    char* argv[] = { "init", NULL };
    process_t* init_proc = exec_create_process("init", "init", 1, argv);

    if (!init_proc) {
        EXEC_LOG("[EXEC] Failed to create init process\n");
        return -1;
    }

    // Add to scheduler
    scheduler_add_process(init_proc);

    EXEC_LOG("[EXEC] Init process added to scheduler (PID %d)\n", init_proc->pid);

    return 0;
}

/**
 * Internal: free all allocated pages and clean up process on ELF load failure.
 * Called only when elf_load_and_exec fails after process creation.
 */
static void elf_cleanup_failed_load(process_t* proc) {
    if (!proc) return;

    EXEC_LOG("[EXEC] Cleaning up failed ELF load for PID %d\n", proc->pid);

    // Restore kernel PML4 target
    paging_reset_target();

    // process_unref decrements ref_count to 0, triggering:
    //   - paging_destroy_address_space() which walks page tables and frees
    //     ALL mapped user pages + page table structures
    //   - Kernel stack free
    //   - Process struct free
    process_unref(proc);
}

/**
 * Load and execute ELF from memory buffer
 *
 * Takes raw ELF binary data (e.g., extracted from initrd TAR),
 * creates a process, loads the ELF into it, and adds it to the scheduler.
 * This is the bridge function that kernel.c calls during init.
 *
 * @param elf_data Pointer to raw ELF binary in memory
 * @param elf_size Size of ELF binary in bytes
 * @param name Process name (for display purposes)
 * @return PID on success, negative error code on failure
 */
/* Spawn arguments for the next elf_load_and_exec: a space-separated string set
 * by sys_spawn (copied from the caller's user space before its CR3 switch) and
 * consumed when building the argv frame. Empty => argv = [name] only. The kernel
 * is cooperative + single-threaded through spawn, so a plain global is race-free.
 * BSS-zeroed, so the very first (init) load with no spawn sees "" -> argv=[name]. */
char g_exec_spawn_args[256];

/* AGENT-RPC-0 P6d: an explicit argv VECTOR for the next exec, set by
 * SYS_SPAWN_EX_ARGV. When g_exec_spawn_argv_len > 0, g_exec_spawn_argv holds the
 * extra entries argv[1..] as NUL-separated bytes (each entry kept intact -- split
 * ONLY on NUL, never on whitespace), and the legacy whitespace-split of
 * g_exec_spawn_args is bypassed. argv[0] is always the explicit `name` (path).
 * Consumed (length zeroed) when the argv frame is built. */
char g_exec_spawn_argv[256];
int  g_exec_spawn_argv_len;

int elf_load_and_exec(void* elf_data, size_t elf_size, const char* name) {
    if (!elf_data || elf_size == 0 || !name) {
        EXEC_LOG("[EXEC] Invalid parameters to elf_load_and_exec\n");
        return ELF_ERR_INVALID;
    }

    EXEC_LOG("[EXEC] Loading ELF from memory: %s (%lu bytes)\n", name, (unsigned long)elf_size);

    // Validate ELF header size
    if (elf_size < sizeof(elf64_ehdr_t)) {
        EXEC_LOG("[EXEC] ERROR: Buffer too small (%lu bytes) to be valid ELF (need %lu)\n",
                (unsigned long)elf_size, (unsigned long)sizeof(elf64_ehdr_t));
        return ELF_ERR_INVALID;
    }

    // Validate ELF header
    const elf64_ehdr_t* ehdr = (const elf64_ehdr_t*)elf_data;
    EXEC_LOG("[EXEC] Validating ELF header...\n");
    EXEC_LOG("[EXEC]   Magic: 0x%02x 0x%02x 0x%02x 0x%02x\n",
            ehdr->e_ident[EI_MAG0], ehdr->e_ident[EI_MAG1],
            ehdr->e_ident[EI_MAG2], ehdr->e_ident[EI_MAG3]);
    EXEC_LOG("[EXEC]   Class: %d (64-bit=%d)\n", ehdr->e_ident[EI_CLASS], ELFCLASS64);
    EXEC_LOG("[EXEC]   Machine: %d (x86_64=%d)\n", ehdr->e_machine, EM_X86_64);
    EXEC_LOG("[EXEC]   Type: %d (exec=%d, dyn=%d)\n", ehdr->e_type, ET_EXEC, ET_DYN);

    if (!elf_validate_header(ehdr)) {
        EXEC_LOG("[EXEC] ERROR: Invalid ELF header\n");
        return ELF_ERR_INVALID;
    }

    EXEC_LOG("[EXEC] Valid ELF64 executable\n");
    EXEC_LOG("[EXEC]   Entry point: 0x%016lx\n", ehdr->e_entry);
    EXEC_LOG("[EXEC]   Program headers: %d entries at offset 0x%lx\n",
            ehdr->e_phnum, ehdr->e_phoff);

    // Check program header table is within buffer bounds (overflow-safe: a
    // crafted ELF could make e_phnum*e_phentsize wrap a 32-bit product).
    if (ehdr->e_phnum > 1024) {
        EXEC_LOG("[EXEC] ERROR: implausible e_phnum=%u\n", ehdr->e_phnum);
        return ELF_ERR_INVALID;
    }
    // The program-header array is indexed as elf64_phdr_t[i] (fixed stride), so a
    // crafted e_phentsize that differs from sizeof(elf64_phdr_t) would make every
    // phdr[i] read overlap the wrong bytes. Require the canonical entry size.
    if (ehdr->e_phentsize != sizeof(elf64_phdr_t)) {
        EXEC_LOG("[EXEC] ERROR: e_phentsize=%u != %lu\n",
                ehdr->e_phentsize, (unsigned long)sizeof(elf64_phdr_t));
        return ELF_ERR_INVALID;
    }
    uint64_t phdr_end = (uint64_t)ehdr->e_phoff
                      + (uint64_t)ehdr->e_phnum * (uint64_t)ehdr->e_phentsize;
    if (phdr_end > elf_size) {
        EXEC_LOG("[EXEC] Program header table extends beyond buffer\n");
        return ELF_ERR_INVALID;
    }

    // Create process FIRST so we get a per-process address space
    EXEC_LOG("[EXEC] Creating process control block...\n");
    process_t* proc = process_create(name, (void*)ehdr->e_entry);
    if (!proc) {
        EXEC_LOG("[EXEC] ERROR: Failed to create process\n");
        return ELF_ERR_NOMEM;
    }
    EXEC_LOG("[EXEC] Process PID=%d created, CR3=0x%016lx\n", proc->pid, proc->context.cr3);

    // Target the PROCESS's PML4 for all user page mappings
    paging_set_target(proc->context.cr3);

    // Load PT_LOAD segments into process address space, with overlap detection.
    // A crafted ELF with overlapping PT_LOAD segments could overwrite previously
    // loaded code/data or bypass W^X by replacing a read-only code page.
    const elf64_phdr_t* phdr = (const elf64_phdr_t*)((uint8_t*)elf_data + ehdr->e_phoff);

#define EXEC_MAX_LOAD_SEGMENTS 16
    struct { uint64_t start; uint64_t end; } exec_loaded[EXEC_MAX_LOAD_SEGMENTS];
    int exec_n_loaded = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        EXEC_LOG("[EXEC]   Segment %d: type=0x%x offset=0x%lx filesz=0x%lx memsz=0x%lx\n",
                i, phdr[i].p_type, phdr[i].p_offset, phdr[i].p_filesz, phdr[i].p_memsz);

        if (phdr[i].p_type == PT_LOAD) {
            // Overflow guard: p_vaddr + p_memsz must not wrap (a crafted ELF could
            // otherwise make vaddr_end tiny/huge and num_pages explode, exhausting
            // the PMM). Check before computing the aligned range.
            if (phdr[i].p_memsz > 0 &&
                phdr[i].p_vaddr + phdr[i].p_memsz < phdr[i].p_vaddr) {
                EXEC_LOG("[EXEC] ERROR: seg %d vaddr+memsz overflow\n", i);
                elf_cleanup_failed_load(proc);
                return ELF_ERR_INVALID;
            }
            // Calculate aligned boundaries (with ALIGN_UP overflow guard)
            uint64_t vaddr_start = ALIGN_DOWN(phdr[i].p_vaddr, PAGE_SIZE);
            uint64_t raw_end = phdr[i].p_vaddr + phdr[i].p_memsz;
            uint64_t vaddr_end = ALIGN_UP(raw_end, PAGE_SIZE);
            if (vaddr_end < raw_end) {
                EXEC_LOG("[EXEC] ERROR: seg %d ALIGN_UP overflow\n", i);
                elf_cleanup_failed_load(proc);
                return ELF_ERR_INVALID;
            }
            uint64_t num_pages = (vaddr_end - vaddr_start) / PAGE_SIZE;

            // Segment overlap detection: reject overlapping PT_LOAD segments
            for (int j = 0; j < exec_n_loaded; j++) {
                if (vaddr_start < exec_loaded[j].end && vaddr_end > exec_loaded[j].start) {
                    EXEC_LOG("[EXEC] ERROR: seg %d [0x%lx,0x%lx) overlaps seg [0x%lx,0x%lx)\n",
                            i, vaddr_start, vaddr_end,
                            exec_loaded[j].start, exec_loaded[j].end);
                    elf_cleanup_failed_load(proc);
                    return ELF_ERR_INVALID;
                }
            }

            EXEC_LOG("[EXEC]   Loading PT_LOAD segment %d:\n", i);
            EXEC_LOG("[EXEC]     Virtual address: 0x%016lx (aligned: 0x%016lx - 0x%016lx)\n",
                    phdr[i].p_vaddr, vaddr_start, vaddr_end);
            EXEC_LOG("[EXEC]     Size: filesz=0x%lx memsz=0x%lx pages=%lu\n",
                    phdr[i].p_filesz, phdr[i].p_memsz, num_pages);
            EXEC_LOG("[EXEC]     Flags: %s%s%s\n",
                    (phdr[i].p_flags & PF_R) ? "R" : "-",
                    (phdr[i].p_flags & PF_W) ? "W" : "-",
                    (phdr[i].p_flags & PF_X) ? "X" : "-");

            // Validate segment is in user space. BOTH the start AND the page-aligned
            // END must be below the user/kernel split (0x0000800000000000): a crafted
            // PT_LOAD that STARTS in user space but EXTENDS past it (large p_memsz)
            // would otherwise pass a start-only check, and the mapping loop below would
            // install PTEs for KERNEL virtual addresses, corrupting the process's
            // kernel-half page tables. The overflow guard above already prevents
            // vaddr_end from wrapping, so this strict comparison is sound. Mirrors the
            // companion loader elf_loader.c, which already checks both ends.
            if (vaddr_start >= 0x0000800000000000ULL ||
                vaddr_end   >  0x0000800000000000ULL) {
                EXEC_LOG("[EXEC]   ERROR: Segment [0x%016lx,0x%016lx) leaves user space\n",
                        vaddr_start, vaddr_end);
                elf_cleanup_failed_load(proc);
                return ELF_ERR_PERM;
            }

            // Reject malformed segments BEFORE any allocation/copy (a hostile or
            // truncated ELF must never overflow kernel memory).
            if (phdr[i].p_filesz > phdr[i].p_memsz) {
                EXEC_LOG("[EXEC] ERROR: seg %d filesz>memsz\n", i);
                elf_cleanup_failed_load(proc);
                return ELF_ERR_INVALID;
            }
            // p_offset + p_filesz must stay within the ELF buffer (overflow-safe).
            if (phdr[i].p_offset > elf_size ||
                phdr[i].p_filesz > elf_size - phdr[i].p_offset) {
                EXEC_LOG("[EXEC] ERROR: seg %d file range beyond buffer\n", i);
                elf_cleanup_failed_load(proc);
                return ELF_ERR_INVALID;
            }

            // Calculate page flags (uint64_t to accommodate PAGE_NX in bit 63).
            //
            // W^X ENFORCEMENT (relies on userspace.ld emitting two PT_LOADs):
            //   * PF_X segment (.text/.rodata) -> present + user, READ-ONLY,
            //     EXECUTABLE: no PAGE_WRITE, no PAGE_NX. Code can run but never
            //     be written, so it can never be both writable and executable.
            //   * non-PF_X segment (.data/.bss) -> present + user + PAGE_NX,
            //     plus PAGE_WRITE iff PF_W. Data is writable but never executes.
            // PAGE_NX is inert until EFER.NXE is enabled in paging_init().
            uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
            if ((phdr[i].p_flags & PF_X) && (phdr[i].p_flags & PF_W)) {
                // SINGLE R|W|X PT_LOAD: the on-device toolchain (the IDE's native
                // compiler) emits ONE segment holding code AND mutable .data + string
                // literals, so we must honor BOTH X and W -- otherwise every global /
                // string write faults and the freshly-built program "won't run". Map
                // it RWX. Gated on the W&&X signature so it does NOT relax W^X for the
                // normal two-PT_LOAD apps below (whose .text is X-not-W and .data is
                // W-not-X). Acceptable here: these are the user's own locally-built
                // programs, and the alternative is they cannot run at all.
                page_flags |= PAGE_WRITE;   // executable AND writable
            } else if (phdr[i].p_flags & PF_X) {
                // Pure code segment: RX. Never writable (W^X), no NX bit.
            } else {
                // Non-executable segment: mark NX, add write only if PF_W.
                page_flags |= PAGE_NX;
                if (phdr[i].p_flags & PF_W) {
                    page_flags |= PAGE_WRITE;
                }
            }

            // Allocate + map the whole segment range, then populate. NO size cap
            // (the old 64-page/256KB cap hard-rejected large apps like imageviewer
            // and notes). The load runs with the KERNEL CR3 active -- paging_set_target
            // only redirects the software page-table target, not the live CR3 -- so
            // we populate via the IDENTITY-MAPPED physical addresses, never the user
            // virtual addresses. We also zero ONLY the bytes the file doesn't cover
            // (no more zero-every-page-then-copy-over-it double write).
            extern void* pmm_alloc_pages(size_t count);
            extern int   vmm_map_range(uint64_t vaddr, uint64_t paddr, uint64_t count, uint64_t flags);
            extern uint64_t paging_get_pte(uint64_t virt);  // used by the overlap guard below

            uint64_t seg_off  = phdr[i].p_vaddr - vaddr_start;  // file-data start within the region
            uint64_t filesz   = phdr[i].p_filesz;
            uint64_t file_end = seg_off + filesz;               // region offset where file data ends
            uint64_t total    = num_pages * PAGE_SIZE;
            const uint8_t* src = (const uint8_t*)elf_data + phdr[i].p_offset;

            EXEC_LOG("[EXEC]     Allocating %lu pages...\n", num_pages);
            (void)pmm_alloc_pages; (void)vmm_map_range;  // contiguous fast path retired

            // PHASE 1 -- allocate + map every page of the segment into the process's
            // address space (paging_set_target already points the page-table writer at
            // proc->cr3). Contents are populated in phase 2.
            // NOTE: an overlap "skip if already mapped" guard cannot be used
            // here — a fresh process inherits the kernel identity map (2MB huge
            // pages) at low VAs, so paging_get_pte() reports PRESENT for every
            // load address before we map it. vmm_map_page() correctly splits the
            // huge page and installs the private frame, overwriting the identity
            // mapping, so we must call it unconditionally. (The rare two-PT_LOAD-
            // share-a-4K-page leak is accepted; toolchain output is page-aligned.)
            for (uint64_t j = 0; j < num_pages; j++) {
                uint64_t vaddr = vaddr_start + (j * PAGE_SIZE);
                void* phys = pmm_alloc_page();
                if (!phys) {
                    EXEC_LOG("[EXEC]   ERROR: Out of physical memory\n");
                    elf_cleanup_failed_load(proc);
                    return ELF_ERR_NOMEM;
                }
                vmm_map_page((void*)vaddr, phys, page_flags);
            }

            // PHASE 2 -- populate each destination page by copying into its PHYSICAL
            // frame through the kernel identity map, with the KERNEL master CR3 loaded.
            //
            // The copy SOURCE (`src`) is the initrd, which can physically straddle the
            // userspace load window [0x200000, 0x400000). Two address spaces shadow
            // that window with PRIVATE pages and therefore must NOT be active during
            // the copy:
            //   * the PROCESS being loaded: exec splits the 2MB identity huge page at
            //     0x200000 and remaps it to this process's private code frames, so a
            //     source byte at VA >= 0x200000 would read the half-written destination
            //     (self-aliasing) -- this is the bug that broke the compositor, whose
            //     ELF body lives at initrd VA 0x1ff200..0x205e00.
            //   * the CALLER's process (e.g. init, which invokes sys_spawn): its own
            //     code/data already shadow [0x200000, ...) with init's private frames,
            //     so reading the initrd there returns init's memory, not the file.
            //
            // The KERNEL master PML4 never splits that window -- it identity-maps all
            // RAM with 2MB huge pages -- so under kernel CR3 both the initrd source and
            // every freshly-allocated destination frame are valid, alias-free kernel
            // pointers. We resolve the destination frames from the process page tables
            // FIRST (paging_get_pte walks active_pml4 == proc CR3, a pure RAM read that
            // does not depend on the live CR3), then switch to kernel CR3 only for the
            // memcpy/memset window. The scheduler is cooperative, so nothing runs in
            // between.
            {
                extern uint64_t paging_get_pte(uint64_t virt);
                extern uint64_t paging_kernel_cr3(void);

                uint64_t caller_cr3 = read_cr3();
                write_cr3(paging_kernel_cr3());
                for (uint64_t j = 0; j < num_pages; j++) {
                    uint64_t vaddr = vaddr_start + (j * PAGE_SIZE);
                    // Resolve the physical frame backing this user page. active_pml4 ==
                    // proc CR3 (paging_set_target(proc->cr3) is in effect); this only
                    // reads page-table memory and is unaffected by the live CR3 switch.
                    uint64_t pte = paging_get_pte(vaddr);
                    // SAFETY (kernel self-protection): if phase 1's vmm_map_page()
                    // did NOT install a private 4KB frame for this page — it ran out
                    // of memory backing a very large segment, so the page is STILL
                    // the inherited 2MB identity huge page (PS bit 7) or not present
                    // (pte==0) — then `dphys` below would be the IDENTITY physical of
                    // vaddr, i.e. KERNEL RAM. A big app's BSS VA range (photos: 35MB,
                    // VA 0x208000..0x2405728) overlaps the low physical RAM where the
                    // kernel heap/slabs live, so a memset/memcpy through such a dphys
                    // scribbles the kernel (seen: slab headers zeroed -> SLABDIAG ->
                    // kmalloc #GP). Never populate via a non-private frame; abort the
                    // load cleanly so the kernel stays intact.
                    if ((pte & 0x1) == 0 || (pte & (1ULL << 7))) {
                        write_cr3(caller_cr3);
                        kprintf("[EXEC] PROTECT: seg %d page %lu/%lu vaddr=%p not backed by a "
                                "private frame (pte=0x%lx); segment too large for free RAM — "
                                "failing load (kernel protected)\n",
                                i, j, num_pages, (void*)vaddr, pte);
                        elf_cleanup_failed_load(proc);
                        return ELF_ERR_NOMEM;
                    }
                    // Mask off BOTH the low flag bits AND the high flag bits.
                    // ~0xFFF alone leaves bit 63 (PAGE_NX) set for NX data pages,
                    // producing a non-canonical pointer that #GPs on the store
                    // below; use the architectural phys-addr mask instead.
                    uint8_t* dphys = (uint8_t*)(pte & 0x000FFFFFFFFFF000ULL);  // identity-mapped

                    // [SLABPROBE] ground-truth: does this BSS page's identity physical
                    // point at a LIVE slab page (so the memset below corrupts it)?
                    if (*(volatile uint64_t*)dphys == 0x51AB0BACE51AB0BULL) {
                        kprintf("[SLABPROBE] exec '%s' seg %d pg %lu ZEROING LIVE SLAB "
                                "dphys=%p va=%p pte=0x%lx\n", proc->name, i, j,
                                (void*)dphys, (void*)vaddr, (unsigned long)pte);
                    }

                    uint64_t page_lo = j * PAGE_SIZE;              // region offset of page start
                    uint64_t page_hi = page_lo + PAGE_SIZE;        // region offset of page end

                    // 1) Leading gap before file data (seg_off > 0): zero it.
                    // 2) File-backed bytes [seg_off, file_end): copy from initrd.
                    // 3) Trailing BSS/page pad [file_end, total): zero it.
                    for (uint64_t off = page_lo; off < page_hi; ) {
                        if (off < seg_off) {
                            uint64_t end = seg_off < page_hi ? seg_off : page_hi;
                            memset(dphys + (off - page_lo), 0, end - off);
                            off = end;
                        } else if (off < file_end) {
                            uint64_t end = file_end < page_hi ? file_end : page_hi;
                            memcpy(dphys + (off - page_lo), src + (off - seg_off), end - off);
                            off = end;
                        } else {
                            memset(dphys + (off - page_lo), 0, page_hi - off);
                            off = page_hi;
                        }
                    }
                }
                write_cr3(caller_cr3);
            }

            // Record this segment as a file-backed VMA (source of truth for the
            // page-fault handler; backs future lazy/CoW policies). Eager prefault
            // above already mapped the pages. NOTE: file_off/file_sz assume a
            // page-aligned p_vaddr (seg_off==0), which holds for userspace.ld's
            // fixed 0x200000 base.
            vma_t seg_vma = {
                .vaddr    = vaddr_start,
                .length   = total,
                .perm     = VMA_R
                          | ((phdr[i].p_flags & PF_W) ? VMA_W : 0)
                          | ((phdr[i].p_flags & PF_X) ? VMA_X : 0),
                .flags    = 0,
                .backing  = VMA_FILE,
                .file_ptr = elf_data,
                .file_off = phdr[i].p_offset,
                .file_sz  = filesz,
                .next     = NULL,
            };
            vma_add(proc, &seg_vma);

            // Record this segment for overlap detection
            if (exec_n_loaded < EXEC_MAX_LOAD_SEGMENTS) {
                exec_loaded[exec_n_loaded].start = vaddr_start;
                exec_loaded[exec_n_loaded].end = vaddr_end;
                exec_n_loaded++;
            }

            EXEC_LOG("[EXEC]   Segment %d loaded (filesz=0x%lx, %lu pages)\n",
                    i, filesz, num_pages);
        }
    }

    // Setup user stack (64KB - enough for early userspace, conserves memory)
    #define USER_STACK_SIZE (64 * 1024)
    #define USER_STACK_TOP  0x00007FFFFFFFE000ULL
    uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
    uint64_t num_stack_pages = USER_STACK_SIZE / PAGE_SIZE;

    EXEC_LOG("[EXEC] Setting up user stack: 0x%016lx - 0x%016lx (%lu pages)\n",
            stack_bottom, USER_STACK_TOP, num_stack_pages);

    // LAZY ZERO-FILL (anonymous): the user stack is a pure anonymous region, so
    // we do NOT eagerly back every page. We pre-fault ONLY the single top page
    // (the one the initial RSP lives in) as a conservative safety margin, and
    // leave the remaining pages not-present. The first write to a not-present
    // stack page faults from ring 3, lands inside the VMA_ANON record installed
    // below, and handle_page_fault() (kernel/core/mem/vma_region.c) allocates a
    // zeroed PMM frame and maps it on demand — demand-zero, recoverable.
    //
    // Why pre-fault the top page eagerly: it is the page initial RSP points into
    // (see stack_ptr below), guaranteeing the very first instruction's stack
    // access never needs the fault path. All file-backed PT_LOAD segments above
    // remain EAGER (unchanged). To disable lazy stacks, set LAZY_ANON_STACK 0.
    // Identity-mapped physical address of the topmost stack page, captured so we
    // can lay out the argv frame on it (phys==virt in the kernel identity map).
    void* stack_top_phys = NULL;
    #define LAZY_ANON_STACK 1
#if LAZY_ANON_STACK
    {
        // Pre-fault only the topmost stack page [USER_STACK_TOP-PAGE_SIZE, TOP).
        uint64_t top_page = USER_STACK_TOP - PAGE_SIZE;
        void* phys_page = pmm_alloc_page();
        if (!phys_page) {
            EXEC_LOG("[EXEC] ERROR: Out of memory for stack top page\n");
            elf_cleanup_failed_load(proc);
            return ELF_ERR_NOMEM;
        }
        // Zero the freshly-allocated frame via the DIRECT MAP. The active CR3 here is
        // the CALLER's (a PROCESS CR3 whenever a running process spawns this exec).
        // A process CR3's low identity map can be split/partial/mutated and need NOT
        // cover a HIGH physical frame, so a raw memset(phys_page) -- phys==virt --
        // #PFs (THE churn crash). PHYS_TO_DIRECT() resolves through PML4[256], the
        // shared higher-half alias present + immutable in EVERY CR3 -- always mapped,
        // no CR3 switch.
        memset(PHYS_TO_DIRECT(phys_page), 0, PAGE_SIZE);
        // Stack is writable data: W + NX (never executable).
        vmm_map_page((void*)top_page, phys_page, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);
        stack_top_phys = phys_page;
        EXEC_LOG("[EXEC]   Stack top page eager: vaddr=0x%016lx -> phys=%p (rest lazy)\n",
                top_page, phys_page);
    }
#else
    for (uint64_t i = 0; i < num_stack_pages; i++) {
        uint64_t vaddr = stack_bottom + (i * PAGE_SIZE);
        void* phys_page = pmm_alloc_page();

        if (!phys_page) {
            EXEC_LOG("[EXEC] ERROR: Out of memory for stack at page %lu/%lu\n", i, num_stack_pages);
            elf_cleanup_failed_load(proc);
            return ELF_ERR_NOMEM;
        }

        // Stack is writable data: W + NX (never executable).
        vmm_map_page((void*)vaddr, phys_page, PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX);
        // Zero via the DIRECT MAP (always mapped on any CR3; see LAZY branch note).
        memset(PHYS_TO_DIRECT(phys_page), 0, PAGE_SIZE);

        if (i == 0 || i == num_stack_pages - 1) {
            EXEC_LOG("[EXEC]   Stack page %lu: vaddr=0x%016lx -> phys=%p\n", i, vaddr, phys_page);
        }
    }
#endif

    // Install a guard page immediately below the stack. This page is never
    // faulted in -- any access to it (stack overflow) is detected by the
    // page-fault handler via VMA_FLAG_GUARD and kills the process cleanly
    // instead of silently corrupting adjacent mappings.
    if (stack_bottom >= PAGE_SIZE) {
        vma_t guard_vma = {
            .vaddr    = stack_bottom - PAGE_SIZE,
            .length   = PAGE_SIZE,
            .perm     = 0,              // no permissions -- any access faults
            .flags    = VMA_FLAG_GUARD,
            .backing  = VMA_ANON,
            .file_ptr = 0,
            .file_off = 0,
            .file_sz  = 0,
            .next     = NULL,
        };
        vma_add(proc, &guard_vma);
    }

    // Record the stack as an anonymous, grow-down VMA. With LAZY_ANON_STACK the
    // not-present pages below the top are faulted in on first write via this
    // record (backing == VMA_ANON => demand-zero in handle_page_fault).
    vma_t stack_vma = {
        .vaddr    = stack_bottom,
        .length   = USER_STACK_SIZE,
        .perm     = VMA_R | VMA_W,
        .flags    = VMA_FLAG_GROWSDOWN,
        .backing  = VMA_ANON,
        .file_ptr = 0,
        .file_off = 0,
        .file_sz  = 0,
        .next     = NULL,
    };
    vma_add(proc, &stack_vma);

    // Build a minimal SysV-style argv frame on the (identity-mapped) top stack
    // page so userspace can read argc/argv via a crt0. Phase 1: argv[0] = the
    // program name (the path passed to exec); RSP points at argc, 16-aligned.
    // This is backward-safe: existing _start(void) apps ignore the frame and
    // only need RSP 16-aligned + inside the mapped top page (it is). If we have
    // no physical handle to the top page, fall back to the old empty frame.
    uint64_t stack_ptr;
    if (stack_top_phys) {
        // The argv frame is written THROUGH the stack-top frame (U2P below). Same
        // wrong-CR3 hazard as the stack memset above: under the caller's process CR3
        // a high frame need not be identity-mapped. Address it via the DIRECT MAP
        // (PML4[256], shared + present in every CR3) instead of the raw phys -- no
        // CR3 switch needed.
        enum { EXEC_MAX_ARGS = 16 };
        uint64_t top   = USER_STACK_TOP;                       // exclusive end
        uint64_t pbase = (uint64_t)(uintptr_t)PHYS_TO_DIRECT(stack_top_phys); // direct-map alias of [top-4096, top)
        uint64_t pfloor = top - PAGE_SIZE;                     // low bound of the page
        #define U2P(uva) ((char*)(uintptr_t)(pbase + ((uva) - pfloor)))

        uint64_t argv_uva[EXEC_MAX_ARGS];
        int argc = 0;
        uint64_t cur = top;   // descending write cursor (user vaddr)

        /* argv[0] = the program name (the spawn path). */
        {
            uint64_t l = 0; while (name[l]) l++;
            uint64_t u = (cur - (l + 1)) & ~0x7ULL;
            if (u > pfloor + 128) {          /* keep headroom for the ptr array */
                for (uint64_t k = 0; k <= l; k++) U2P(u)[k] = name[k];
                argv_uva[argc++] = u; cur = u;
            }
        }
        /* argv[1..]: P6d -- if an explicit argv VECTOR was staged, split ONLY on
         * NUL (each entry intact: multi-word args + shell metacharacters survive
         * as literal bytes). Otherwise, the legacy whitespace-split of the
         * command-line string. The two are mutually exclusive (SYS_SPAWN_EX_ARGV
         * stages the vector AND leaves g_exec_spawn_args empty). */
        if (g_exec_spawn_argv_len > 0) {
            const char* a = g_exec_spawn_argv;
            int n = g_exec_spawn_argv_len, i = 0;
            while (i < n && argc < EXEC_MAX_ARGS) {
                int s = i;
                while (i < n && a[i] != '\0') i++;          /* one entry [s, i)        */
                uint64_t l = (uint64_t)(i - s);
                uint64_t u = (cur - (l + 1)) & ~0x7ULL;
                if (u <= pfloor + (uint64_t)(argc + 4) * 8) break;
                for (uint64_t k = 0; k < l; k++) U2P(u)[k] = a[s + k];
                U2P(u)[l] = '\0';
                argv_uva[argc++] = u; cur = u;
                i++;                                        /* skip the NUL separator  */
            }
        } else {
            const char* a = g_exec_spawn_args;
            int i = 0;
            while (a[i] && argc < EXEC_MAX_ARGS) {
                while (a[i] == ' ' || a[i] == '\t') i++;
                if (!a[i]) break;
                int s = i;
                while (a[i] && a[i] != ' ' && a[i] != '\t') i++;
                uint64_t l = (uint64_t)(i - s);
                uint64_t u = (cur - (l + 1)) & ~0x7ULL;
                /* leave room for the pointer array ((argc+3) qwords) below */
                if (u <= pfloor + (uint64_t)(argc + 4) * 8) break;
                for (uint64_t k = 0; k < l; k++) U2P(u)[k] = a[s + k];
                U2P(u)[l] = '\0';
                argv_uva[argc++] = u; cur = u;
            }
        }
        /* [argc][argv0..argvN-1][NULL][envp NULL], argc 16-aligned == initial RSP. */
        uint64_t frame_uva = (cur - (uint64_t)(argc + 3) * 8) & ~0xFULL;
        uint64_t* fr = (uint64_t*)U2P(frame_uva);
        fr[0] = (uint64_t)argc;
        for (int j = 0; j < argc; j++) fr[1 + j] = argv_uva[j];
        fr[1 + argc] = 0;      // argv terminator
        fr[2 + argc] = 0;      // envp terminator
        stack_ptr = frame_uva;
        #undef U2P
        g_exec_spawn_args[0] = '\0';   // consume: don't leak args to the next spawn
        g_exec_spawn_argv_len = 0;     // consume the P6d vector too
        EXEC_LOG("[EXEC] Stack setup complete, RSP=0x%016lx argc=%d argv0=\"%s\"\n",
                stack_ptr, argc, name);
    } else {
        stack_ptr = (USER_STACK_TOP & ~0xFULL) - 16;   // fallback: empty frame
        EXEC_LOG("[EXEC] Stack setup complete, initial RSP=0x%016lx (no argv frame)\n",
                stack_ptr);
    }

    // Done mapping - restore kernel PML4 as target
    paging_reset_target();
    EXEC_LOG("[EXEC] Restored kernel PML4 target\n");

    // Setup context for first run via context_switch_asm
    // context_switch_asm does `ret` which stays in ring 0. For the first run
    // of a new process, we set RIP to a trampoline that calls enter_usermode
    // to properly transition to ring 3 via IRETQ.
    extern void process_enter_usermode_trampoline(void);
    EXEC_LOG("[EXEC] Setting up CPU context...\n");

    // Store user entry/stack for scheduler_start (first process) and trampoline
    proc->user_entry = ehdr->e_entry;
    proc->user_rsp = stack_ptr;

    // Trampoline reads RDI=entry, RSI=stack, RDX=cr3 from saved context
    proc->context.rdi = ehdr->e_entry;
    proc->context.rsi = stack_ptr;
    proc->context.rdx = proc->context.cr3;

    // Set kernel-side execution state for context_switch_asm
    proc->context.rip = (uint64_t)process_enter_usermode_trampoline;
    uint64_t kstack_top = (uint64_t)proc->kernel_stack + KERNEL_STACK_SIZE;
    proc->context.rsp = kstack_top - 8;  // kernel stack, room for ret addr
    proc->context.rflags = 0x202;
    // CR3 stays as process CR3 (context_switch_asm loads it)

    EXEC_LOG("[EXEC]   Trampoline RIP=0x%016lx\n", proc->context.rip);
    EXEC_LOG("[EXEC]   User entry=0x%016lx stack=0x%016lx\n", ehdr->e_entry, stack_ptr);
    EXEC_LOG("[EXEC]   Kernel RSP=0x%016lx CR3=0x%016lx\n", proc->context.rsp, proc->context.cr3);

    // CHANNEL-0 P2: install any parent-supplied stdio channels into the child
    // (slave end, narrowed rights). No-op for a plain SYS_SPAWN (g_exec_stdio is
    // empty). Done here -- child fully built, not yet scheduled.
    {
        extern void channel_install_spawn_stdio(struct process* child);
        channel_install_spawn_stdio(proc);
    }

    // Mark as ready to run
    process_set_ready(proc);
    EXEC_LOG("[EXEC] Process state set to READY\n");

    // Add to scheduler
    EXEC_LOG("[EXEC] Adding process to scheduler...\n");
    scheduler_add_process(proc);

    EXEC_LOG("[EXEC] SUCCESS: Process PID=%d loaded and scheduled\n", proc->pid);
    EXEC_LOG("[EXEC]   Entry point: 0x%016lx\n", ehdr->e_entry);
    EXEC_LOG("[EXEC]   Stack top: 0x%016lx\n", stack_ptr);
    EXEC_LOG("[EXEC]   Process ready to execute\n");

    return proc->pid;
}

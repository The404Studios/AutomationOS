ENTRY(_start)

SECTIONS
{
    /* Multiboot header must be at low physical address */
    . = 1M;

    .multiboot ALIGN(8) :
    {
        *(.multiboot)
    }

    __kernel_phys_start = .;

    .boot ALIGN(4K) :
    {
        *(.boot)
    }

    .text ALIGN(4K) :
    {
        *(.text)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4K) :
    {
        *(.data)
    }

    /* Page tables - NOT cleared during BSS init */
    .pagetables ALIGN(4K) :
    {
        *(.pagetables)
    }

    .bss ALIGN(4K) :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        /*
         * Large, non-control-path static buffers (network fragment-reassembly
         * table, PTY backing store) are emitted LAST so the kernel's critical
         * control state (current_process, scheduler runqueues, pmm_bitmap,
         * heap/slab metadata, syscall/vfs/fs/ipc tables, ...) stays packed in
         * the first ~1.7MB of .bss — entirely BELOW the 0x200000 user-ELF load
         * base. This is load-bearing: each process address space deep-copies the
         * low-half page tables and userspace binaries are linked at 0x200000, so
         * a large user image (e.g. the on-device C compiler) maps user pages over
         * any kernel global that sits at VA >= 0x200000, shadowing it under that
         * process's CR3. Keeping all control state below 0x200000 makes it
         * immune; only these two large deferred buffers may be shadowed while a
         * big process is current (acceptable: they are not touched on the
         * scheduling / page-fault / syscall-dispatch hot path that runs under a
         * user CR3). See kernel/core/sched + paging_create_address_space().
         */
        . = ALIGN(4K);
        __bss_deferred_start = .;
        *(.bss.deferred)
        __bss_deferred_end = .;
        __bss_end = .;
    }

    __kernel_end = .;
}

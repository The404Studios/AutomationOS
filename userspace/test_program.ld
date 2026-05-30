/**
 * Linker Script for Userspace Test Programs
 * ==========================================
 *
 * Creates a simple ELF64 executable suitable for loading by the kernel.
 * Places code at a user virtual address and sets up a minimal program structure.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    /* User space starts at low addresses (0x400000 is typical for x86_64) */
    . = 0x400000;

    /* Read-only code segment */
    .text ALIGN(4K) : {
        *(.text)
        *(.text.*)
    }

    /* Read-only data segment */
    .rodata ALIGN(4K) : {
        *(.rodata)
        *(.rodata.*)
    }

    /* Read-write data segment */
    .data ALIGN(4K) : {
        *(.data)
        *(.data.*)
    }

    /* BSS (zero-initialized data) */
    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /* Discard unneeded sections */
    /DISCARD/ : {
        *(.note*)
        *(.comment)
        *(.eh_frame)
    }
}

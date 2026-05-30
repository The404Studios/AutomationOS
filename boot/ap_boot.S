/*
 * AP (Application Processor) Boot Code
 * ====================================
 *
 * This code runs when an AP CPU starts up via INIT-SIPI-SIPI sequence.
 * It starts in 16-bit real mode at address 0x8000 and transitions
 * through 32-bit protected mode to 64-bit long mode.
 */

.section .text
.code16

.global ap_boot_start
.global ap_boot_end
.global ap_boot_stack
.global ap_boot_cr3
.global ap_boot_gdtr

ap_boot_start:
    cli                         # Disable interrupts

    # Load GDT
    lgdt (ap_gdt_ptr - ap_boot_start + 0x8000)

    # Enable protected mode (set CR0.PE)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0

    # Jump to 32-bit protected mode
    ljmp $0x08, $(ap_boot_32 - ap_boot_start + 0x8000)

.code32
ap_boot_32:
    # Set up segment registers
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Enable PAE (Physical Address Extension)
    movl %cr4, %eax
    orl $0x20, %eax             # CR4.PAE
    movl %eax, %cr4

    # Load CR3 (page tables)
    movl (ap_boot_cr3 - ap_boot_start + 0x8000), %eax
    movl %eax, %cr3

    # Enable long mode (set EFER.LME)
    movl $0xC0000080, %ecx      # EFER MSR
    rdmsr
    orl $0x100, %eax            # EFER.LME
    wrmsr

    # Enable paging (set CR0.PG)
    movl %cr0, %eax
    orl $0x80000000, %eax       # CR0.PG
    movl %eax, %cr0

    # Jump to 64-bit long mode
    ljmp $0x08, $(ap_boot_64 - ap_boot_start + 0x8000)

.code64
ap_boot_64:
    # Set up segment registers for 64-bit
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Load stack pointer
    movq (ap_boot_stack - ap_boot_start + 0x8000), %rsp

    # Call C function (ap_main)
    movq $ap_main, %rax
    call *%rax

    # Should never return, but just in case
ap_halt:
    hlt
    jmp ap_halt

# GDT for AP boot
.align 16
ap_gdt:
    .quad 0x0000000000000000    # Null descriptor
    .quad 0x00AF9A000000FFFF    # Code segment (64-bit)
    .quad 0x00CF92000000FFFF    # Data segment

ap_gdt_ptr:
    .word ap_gdt_ptr - ap_gdt - 1
    .long ap_gdt - ap_boot_start + 0x8000

# Data section for AP boot
.align 8
ap_boot_stack:
    .quad 0x0000000000000000

ap_boot_cr3:
    .quad 0x0000000000000000

ap_boot_gdtr:
    .quad 0x0000000000000000

ap_boot_end:

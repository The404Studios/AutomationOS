; ============================================================================
; AP (Application Processor) Boot Trampoline
; ============================================================================
;
; This is the 16-bit real-mode entry point an Application Processor lands on
; after the BSP sends it INIT-SIPI-SIPI. The SIPI vector encodes the physical
; page the AP starts executing at (CS:IP = vector<<12 : 0). The integrator /
; smp.c copies this blob to a fixed page in low memory (< 1 MB, default 0x8000)
; so the SIPI vector is 0x08.
;
; The AP must do everything from scratch in real mode:
;   1. (real mode)  load a flat GDT, enable protected mode
;   2. (prot mode)  enable PAE, load the kernel CR3 (handed in by the BSP),
;                   set EFER.LME, enable paging  -> long mode compatibility
;   3. (long mode)  far-jump to 64-bit code, load the kernel runtime GDT/IDT,
;                   set this AP's stack, and jump to the C entry smp_ap_main.
;
; All "shared" parameters (kernel CR3, per-AP stack top, 64-bit entry pointer,
; and the kernel runtime GDTR/IDTR images) are written by smp.c into a small
; parameter block that lives at a FIXED offset from the trampoline base, so the
; running AP can read them with simple absolute low-memory addresses.
;
; This entire file only matters when the kernel is built with -DSMP_ENABLE and
; assembled into the image. With SMP disabled it is never assembled/linked, so
; the single-CPU boot path is completely unaffected.
;
; Assemble:  nasm -f elf64 ap_trampoline.asm -o ap_trampoline.o
; ============================================================================

[BITS 16]

; ----------------------------------------------------------------------------
; Layout constants. The trampoline is RELOCATED to AP_TRAMPOLINE_BASE at run
; time (default 0x8000). All real-mode absolute references are computed as
; (label - ap_trampoline_start) + AP_TRAMPOLINE_BASE so the blob is position
; independent w.r.t. assembly-time addressing.
; ----------------------------------------------------------------------------
AP_TRAMPOLINE_BASE      equ 0x8000

; Offset of the parameter block from the start of the trampoline. smp.c writes
; the handoff values here AFTER copying the code. Keep in sync with smp.c
; (AP_PARAM_OFFSET).
AP_PARAM_OFFSET         equ 0x0F00

; Selectors in the trampoline's own bootstrap GDT.
TRAMP_CS32              equ 0x08
TRAMP_DS32              equ 0x10
TRAMP_CS64              equ 0x18
TRAMP_DS64              equ 0x20

section .ap_trampoline progbits alloc exec

global ap_trampoline_start
global ap_trampoline_end

; ----------------------------------------------------------------------------
; Real-mode entry. CS = AP_TRAMPOLINE_BASE>>4, IP = 0.
; ----------------------------------------------------------------------------
ap_trampoline_start:
    cli
    cld

    ; Normalize the segment registers. The SIPI delivered CS = vector<<8 but we
    ; want a known data segment for our absolute addressing math.
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load the bootstrap GDT (32/64-bit flat descriptors). lgdt needs a linear
    ; address to the gdt pointer structure; we computed it as a relocated
    ; absolute below.
    o32 lgdt [ap_gdt32_ptr - ap_trampoline_start + AP_TRAMPOLINE_BASE]

    ; Enter protected mode: set CR0.PE
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump to flush the prefetch queue and load a 32-bit code selector.
    jmp dword TRAMP_CS32:(ap_pmode_entry - ap_trampoline_start + AP_TRAMPOLINE_BASE)

; ----------------------------------------------------------------------------
; 32-bit protected mode. Set up long mode here.
; ----------------------------------------------------------------------------
[BITS 32]
ap_pmode_entry:
    mov ax, TRAMP_DS32
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Enable PAE (CR4.PAE = bit 5)
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Load the kernel's CR3 (PML4) that the BSP handed us in the param block.
    mov eax, [AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET + 0]   ; ap_param_cr3 (low 32)
    mov cr3, eax

    ; Enable long mode in EFER (MSR 0xC0000080, LME = bit 8)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable paging (CR0.PG = bit 31). PE is already set.
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; We are now in long mode (compatibility sub-mode). Far jump into 64-bit.
    jmp TRAMP_CS64:(ap_lmode_entry - ap_trampoline_start + AP_TRAMPOLINE_BASE)

; ----------------------------------------------------------------------------
; 64-bit long mode. Switch to the kernel runtime GDT/IDT and per-CPU stack,
; then enter C. From here on we use proper 64-bit kernel state.
; ----------------------------------------------------------------------------
[BITS 64]
ap_lmode_entry:
    ; Reload data segments with the trampoline 64-bit data selector for now.
    mov ax, TRAMP_DS64
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Switch to this AP's dedicated stack (top, grows down). 64-bit value.
    ; [abs ...] forces explicit absolute (non-RIP-relative) addressing to the
    ; fixed low-memory param block.
    mov rsp, [abs AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET + 8]   ; ap_param_stack_top
    mov rbp, rsp

    ; Load the kernel's real runtime GDT image (captured by smp.c via sgdt).
    ; The 10-byte GDTR image lives in the param block.
    lgdt [abs AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET + 32]      ; ap_param_gdtr

    ; Reload CS via a far return to the kernel code selector 0x08.
    ; Build [RIP][CS] on the stack and retfq.
    lea rax, [rel ap_after_gdt]
    push 0x08
    push rax
    retfq

ap_after_gdt:
    ; Reload data segments with the kernel data selector 0x10.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Load the kernel's real runtime IDT image (captured by smp.c via sidt).
    lidt [abs AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET + 48]      ; ap_param_idtr

    ; Re-establish the stack (segment reload above could have been clobbered on
    ; some emulators; be defensive).
    mov rsp, [abs AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET + 8]
    mov rbp, rsp

    ; Call the 64-bit C entry point. Pointer handed in the param block so this
    ; file has no link-time dependency on the C symbol (keeps the blob
    ; relocatable and position independent).
    mov rax, [abs AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET + 16]  ; ap_param_entry
    mov rdi, [abs AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET + 24]  ; ap_param_arg (cpu id)
    call rax

    ; smp_ap_main should not return; park defensively if it does.
.hang:
    cli
    hlt
    jmp .hang

; ----------------------------------------------------------------------------
; Bootstrap GDT used only during the trampoline's mode transitions. Flat 4 GiB
; descriptors. Once in long mode we immediately switch to the kernel's runtime
; GDT, so this only needs valid 32/64-bit code+data entries.
; ----------------------------------------------------------------------------
align 16
ap_gdt32:
    dq 0x0000000000000000      ; 0x00 null
    dq 0x00CF9A000000FFFF      ; 0x08 32-bit code (base 0, limit 4G, G=1, D=1)
    dq 0x00CF92000000FFFF      ; 0x10 32-bit data
    dq 0x00AF9A000000FFFF      ; 0x18 64-bit code (L=1)
    dq 0x00AF92000000FFFF      ; 0x20 64-bit data
ap_gdt32_end:

ap_gdt32_ptr:
    dw ap_gdt32_end - ap_gdt32 - 1
    dd ap_gdt32 - ap_trampoline_start + AP_TRAMPOLINE_BASE   ; relocated linear base

; ----------------------------------------------------------------------------
; Parameter block. Forced to AP_PARAM_OFFSET via padding so smp.c can write to
; a fixed low-memory address. Field layout (offsets from AP_PARAM_OFFSET):
;   +0  : cr3          (8 bytes; only low 32 used to load CR3 in prot mode)
;   +8  : stack_top    (8 bytes)
;   +16 : entry        (8 bytes; &smp_ap_main)
;   +24 : arg          (8 bytes; logical cpu id passed in RDI)
;   +32 : gdtr image   (16 bytes; 10 used: limit(2)+base(8))
;   +48 : idtr image   (16 bytes; 10 used)
; ----------------------------------------------------------------------------
times AP_PARAM_OFFSET - ($ - ap_trampoline_start) db 0
ap_param_block:
    dq 0    ; +0  cr3
    dq 0    ; +8  stack_top
    dq 0    ; +16 entry
    dq 0    ; +24 arg
    times 16 db 0   ; +32 gdtr (10 used)
    times 16 db 0   ; +48 idtr (10 used)

ap_trampoline_end:

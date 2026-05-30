; usermode.asm - User mode transition for x86_64
; SYSRET-compatible GDT layout: user data=0x1B, user code=0x23

[BITS 64]
global enter_usermode

section .text

; void enter_usermode(uint64_t entry, uint64_t stack, uint64_t cr3);
; RDI = entry point, RSI = user stack, RDX = process CR3
enter_usermode:
    mov rcx, rdi        ; RCX = entry point
    mov r11, rsi        ; R11 = user stack
    mov r10, rdx        ; R10 = process CR3

    ; User data selector = 0x1B (GDT entry 3 | RPL=3)
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Build IRETQ frame before switching CR3
    push 0x1B           ; SS = user data (0x1B)
    push r11            ; RSP = user stack
    pushfq
    pop rax
    and rax, ~0x100      ; Clear TF (Trap Flag, bit 8) - prevents INT 1 single-step
    or rax, 0x200        ; IF=1 (interrupts enabled in userspace)
    and rax, ~0x3000     ; IOPL=0 (no I/O privilege for user mode)
    push rax             ; RFLAGS
    push 0x23           ; CS = user code (0x23)
    push rcx            ; RIP = entry point

    ; Switch to process address space
    mov cr3, r10

    iretq

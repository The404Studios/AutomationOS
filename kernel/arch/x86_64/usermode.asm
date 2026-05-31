; usermode.asm - User mode transition for x86_64
; SYSRET-compatible GDT layout: user data=0x1B, user code=0x23

[BITS 64]
global enter_usermode
global enter_usermode_thread

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

; void enter_usermode_thread(uint64_t entry, uint64_t stack, uint64_t cr3,
;                            uint64_t arg);
;   RDI = entry, RSI = user stack, RDX = CR3, RCX = arg
; Identical to enter_usermode EXCEPT it delivers `arg` in RDI to the user entry,
; so a thread starts executing entry(arg) per the SysV ABI (first integer arg in
; RDI). We must therefore NOT clobber the final RDI with the entry as
; enter_usermode does — we stash entry in R8 and load RDI=arg LAST, right before
; iretq. All scratch is done in caller-saved regs; iretq pops a clean ring-3
; frame so the other GP regs' values are irrelevant to the thread.
enter_usermode_thread:
    mov r8,  rdi        ; R8  = entry point (RIP)
    mov r11, rsi        ; R11 = user stack (RSP)
    mov r10, rdx        ; R10 = process CR3
    mov r9,  rcx        ; R9  = arg (-> RDI just before iretq)

    ; User data selector = 0x1B (GDT entry 3 | RPL=3)
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Build IRETQ frame before switching CR3 (same layout as enter_usermode).
    push 0x1B           ; SS = user data (0x1B)
    push r11            ; RSP = user stack
    pushfq
    pop rax
    and rax, ~0x100      ; Clear TF
    or  rax, 0x200       ; IF = 1
    and rax, ~0x3000     ; IOPL = 0
    push rax             ; RFLAGS
    push 0x23           ; CS = user code (0x23)
    push r8             ; RIP = entry point

    ; Switch to the (shared) process address space.
    mov cr3, r10

    ; SysV: first argument in RDI. Load it LAST so nothing clobbers it.
    mov rdi, r9

    iretq

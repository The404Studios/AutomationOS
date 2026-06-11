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

    ; Build IRETQ frame before switching CR3.
    ; Sanitise RFLAGS for ring 3: IF=1, TF=0, DF=0, IOPL=0.
    ; DF (direction flag, bit 10) MUST be 0: the SysV ABI requires it at
    ; function entry and GCC-compiled userspace assumes it.  A stale DF=1
    ; from the kernel / BIOS would make the very first REP MOVS run
    ; backwards -> memory corruption -> crash (silent on QEMU, fatal on
    ; real hardware such as the T410).
    push 0x1B           ; SS = user data (0x1B)
    push r11            ; RSP = user stack
    pushfq
    pop rax
    and rax, ~0x100      ; Clear TF (Trap Flag, bit 8) - prevents INT 1 single-step
    and rax, ~0x400      ; Clear DF (Direction Flag, bit 10) - SysV ABI: DF=0
    or rax, 0x200        ; IF=1 (interrupts enabled in userspace)
    and rax, ~0x3000     ; IOPL=0 (no I/O privilege for user mode)
    push rax             ; RFLAGS
    push 0x23           ; CS = user code (0x23)
    push rcx            ; RIP = entry point

    ; Clear GP registers to avoid leaking kernel pointers into ring 3.
    ; RCX (entry) and R10 (CR3) were already consumed; zero the rest so
    ; userspace starts with a deterministic, pointer-free register file.
    ; RSI, RDI, RDX were clobbered by the argument shuffle above; zero
    ; them plus every other non-stack, non-IRETQ-frame register.
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    ; r10 still holds CR3 -- zeroed after the mov cr3 below
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d

    ; Switch to process address space
    mov cr3, r10
    xor r10d, r10d       ; clear last kernel pointer

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
    ; Sanitise RFLAGS: IF=1, TF=0, DF=0, IOPL=0 (see enter_usermode comments).
    push 0x1B           ; SS = user data (0x1B)
    push r11            ; RSP = user stack
    pushfq
    pop rax
    and rax, ~0x100      ; Clear TF
    and rax, ~0x400      ; Clear DF (SysV ABI: DF=0 at entry)
    or  rax, 0x200       ; IF = 1
    and rax, ~0x3000     ; IOPL = 0
    push rax             ; RFLAGS
    push 0x23           ; CS = user code (0x23)
    push r8             ; RIP = entry point

    ; Clear GP registers (kernel pointer hygiene + deterministic entry state).
    ; RDI is set to the thread arg LAST; everything else is zeroed.
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor ebp, ebp
    xor r8d, r8d
    ; r9 holds arg (-> RDI below), r10 holds CR3 — zeroed after use
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d

    ; Switch to the (shared) process address space.
    mov cr3, r10
    xor r10d, r10d       ; clear CR3 value

    ; SysV: first argument in RDI. Load it LAST so nothing clobbers it.
    mov rdi, r9
    xor r9d, r9d         ; clear stale arg copy

    iretq

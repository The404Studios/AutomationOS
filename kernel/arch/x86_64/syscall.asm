; syscall.asm - System call entry point for x86_64
; SYSCALL does NOT switch RSP. We must manually load the kernel stack.

[BITS 64]
global syscall_entry
extern syscall_dispatch
extern tss_get

section .text

syscall_entry:
    ; SYSCALL state:
    ;   RCX = user RIP, R11 = user RFLAGS
    ;   RSP = USER stack (not kernel!)
    ;   RAX = syscall number, RDI-R9 = args

    ; Save user RSP, switch to kernel stack from TSS.RSP0
    ; TSS.RSP0 is at offset 4 in the TSS structure (after reserved0)
    mov [rel user_rsp_save], rsp    ; Save user RSP

    ; Load kernel RSP from TSS (tss.rsp0)
    ; tss_get() returns pointer to TSS, rsp0 is at offset 4
    mov rsp, [rel kernel_rsp_save]  ; Load saved kernel RSP

    ; Now on kernel stack - save ALL user registers that SYSCALL doesn't save.
    ; SYSCALL saves RCX→RIP and R11→RFLAGS. Everything else is volatile.
    ; Userspace inline asm expects rdi, rsi, rdx, r8, r9, r10 to be preserved.
    push qword [rel user_rsp_save]  ; Save user RSP on kernel stack
    push rcx                        ; User RIP
    push r11                        ; User RFLAGS
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    push r10
    push r9
    push r8
    push rdx
    push rsi
    push rdi

    ; Marshal syscall args to System V ABI.
    ; syscall_dispatch(n, a1, a2, a3, a4, a5, a6) takes its 7th argument
    ; on the stack. Original user r9 was saved 32 bytes above rsp.
    push qword [rsp + 32]  ; arg6
    mov r9, r8      ; arg5 (was user's arg4)
    mov r8, r10     ; arg4 (was user's arg3)
    mov rcx, rdx    ; arg3
    mov rdx, rsi    ; arg2
    mov rsi, rdi    ; arg1
    mov rdi, rax    ; syscall_num

    call syscall_dispatch
    add rsp, 8

    ; RAX = return value

    ; Restore user registers (reverse push order)
    pop rdi
    pop rsi
    pop rdx
    pop r8
    pop r9
    pop r10
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11         ; User RFLAGS
    or r11, 0x200   ; Ensure IF=1 so interrupts work in userspace
    pop rcx         ; User RIP
    pop rsp         ; Restore user RSP directly

    o64 sysret

section .data
align 8
user_rsp_save:  dq 0
; kernel_rsp_save is set by the kernel before entering usermode
global kernel_rsp_save
kernel_rsp_save: dq 0

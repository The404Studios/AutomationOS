; syscall.asm - System call entry point for x86_64
; Handles SYSCALL instruction and dispatches to kernel

[BITS 64]
global syscall_entry
extern syscall_dispatch

section .text

syscall_entry:
    ; When SYSCALL is executed:
    ; RAX = syscall number
    ; RDI = arg1
    ; RSI = arg2
    ; RDX = arg3
    ; R10 = arg4 (RCX is clobbered by SYSCALL, so use R10)
    ; R8  = arg5
    ; R9  = arg6
    ; RCX = return RIP (saved by SYSCALL)
    ; R11 = RFLAGS (saved by SYSCALL)

    ; Save user registers on kernel stack
    push rcx        ; Return RIP
    push r11        ; RFLAGS
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Arguments for syscall_dispatch:
    ; RDI = syscall_num (from RAX)
    ; RSI = arg1 (already in RDI)
    ; RDX = arg2 (already in RSI)
    ; RCX = arg3 (already in RDX)
    ; R8  = arg4 (from R10)
    ; R9  = arg5 (already in R8)
    ; stack = arg6 (from R9)

    ; Setup arguments in System V ABI order
    mov r9, r8      ; arg5 = R8
    mov r8, r10     ; arg4 = R10
    mov rcx, rdx    ; arg3 = RDX
    mov rdx, rsi    ; arg2 = RSI
    mov rsi, rdi    ; arg1 = RDI
    mov rdi, rax    ; syscall_num = RAX

    ; Call C dispatcher
    call syscall_dispatch

    ; RAX now contains return value

    ; Restore user registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11         ; RFLAGS
    pop rcx         ; Return RIP

    ; Return to userspace with SYSRET
    ; (For now, use a simple return - SYSRET requires MSR setup)
    ; TODO: Implement SYSRET when userspace is ready
    jmp rcx         ; Jump back to user code

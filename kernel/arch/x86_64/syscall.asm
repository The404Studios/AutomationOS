; syscall.asm - System call entry point for x86_64
; SYSCALL does NOT switch RSP. We must manually load the kernel stack.

[BITS 64]
global syscall_entry
extern syscall_dispatch
extern tss_get
extern g_sig_frame              ; SIG-FULL-0: saved-frame ptr for sys_rt_sigreturn
extern deliver_pending_signals  ; SIG-FULL-0: post-dispatch signal delivery

section .text

%ifdef SMP_SCHED
; =============================================================================
; SMP scheduler Brick C: per-CPU kernel/user RSP WITHOUT swapgs.
; -----------------------------------------------------------------------------
; A single global kernel_rsp_save is clobbered the instant CPU1 also runs ring-3
; and SYSCALLs. SYSCALL has no free register and no stack at entry, so we cannot
; cheaply detect the CPU inline -- and a swapgs-only-in-syscall scheme is broken
; (the iretq paths that first-dispatch a process to ring 3 don't swapgs, so a new
; process can inherit kernel-GS and its first swapgs goes the wrong way -> #DF).
;
; Instead we exploit that LSTAR (the SYSCALL entry MSR) is PER-CPU: CPU0's LSTAR
; points at syscall_entry (index 0), CPU1's LSTAR at syscall_entry_cpu1 (index 1).
; Each entry references ITS OWN slot in kernel_rsp_save_arr/user_rsp_save_arr, so
; there is zero cross-CPU clobber, zero swapgs, and zero ring-transition changes.
; The CPU "knows" which entry to use because its own LSTAR was set to it.
;
; %1 = byte offset into the *_arr tables (0 for CPU0, 8 for CPU1).
%macro SYSCALL_BODY 1
    ; SYSCALL state: RCX=user RIP, R11=user RFLAGS, RSP=USER stack,
    ;                RAX=syscall number, RDI-R9=args.
    mov [rel user_rsp_save_arr + %1], rsp     ; save user RSP (this CPU's scratch)
    mov rsp, [rel kernel_rsp_save_arr + %1]   ; load THIS CPU's kernel RSP

    push qword [rel user_rsp_save_arr + %1]   ; save user RSP on the kernel stack
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

    push qword [rsp + 32]  ; arg6
    mov r9, r8      ; arg5 (was user's arg4)
    mov r8, r10     ; arg4 (was user's arg3)
    mov rcx, rdx    ; arg3
    mov rdx, rsi    ; arg2
    mov rsi, rdi    ; arg1
    mov rdi, rax    ; syscall_num

    ; SIG-FULL-0: stash the saved-frame base (&rdi) for sys_rt_sigreturn.
    lea rax, [rsp + 8]              ; arg6 at [rsp]; the GP frame starts at [rsp+8]
    mov [rel g_sig_frame], rax

    call syscall_dispatch
    add rsp, 8

    ; SIG-FULL-0: deliver one pending signal (may rewrite the saved frame).
    ; The frame pointer is the LOCAL rsp (points at saved rdi after `add rsp,8`),
    ; NOT the global g_sig_frame. g_sig_frame is clobbered by every other process's
    ; syscall, so on the return of a BLOCKING syscall (waitpid/read/futex) it names
    ; a stale frame -- using rsp delivers on the correct stack regardless of blocking.
    mov rsi, rax                   ; arg2 = syscall return value
    mov rdi, rsp                   ; arg1 = this syscall's GP frame (local, always valid)
    push rax                       ; preserve retval across the C call
    call deliver_pending_signals
    pop rax

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
%endmacro

global syscall_entry
syscall_entry:                      ; CPU0 LSTAR target (index 0)
    SYSCALL_BODY 0

global syscall_entry_cpu1
syscall_entry_cpu1:                 ; CPU1 LSTAR target (index 8 bytes = slot 1)
    SYSCALL_BODY 8

section .data
align 8
; Per-CPU tables: [0]=CPU0, [1]=CPU1. Written by gdt.c::tss_set_kernel_stack()
; (indexed by cpu_id()) in lockstep with TSS.RSP0. user_rsp_save_arr is just the
; entry-time scratch for the user RSP across the stack switch.
global kernel_rsp_save_arr
global user_rsp_save_arr
kernel_rsp_save_arr: dq 0, 0
user_rsp_save_arr:   dq 0, 0

; Legacy single symbol kept ONLY so the existing C writers
; (`extern uint64_t kernel_rsp_save; kernel_rsp_save = kstack_top;` in
; scheduler.c/context.c/usermode.c) still link. Nothing reads it under SMP_SCHED
; (the entries use kernel_rsp_save_arr); the dead stores are harmless.
global kernel_rsp_save
kernel_rsp_save: dq 0

%else  ; ---------------------------------------------------------------- default

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

    ; SIG-FULL-0: stash the saved-frame base (&rdi) for sys_rt_sigreturn.
    lea rax, [rsp + 8]              ; arg6 at [rsp]; the GP frame starts at [rsp+8]
    mov [rel g_sig_frame], rax

    call syscall_dispatch
    add rsp, 8

    ; RAX = return value

    ; SIG-FULL-0: deliver one pending signal (may rewrite the saved frame).
    ; The frame pointer is the LOCAL rsp (points at saved rdi after `add rsp,8`),
    ; NOT the global g_sig_frame. g_sig_frame is clobbered by every other process's
    ; syscall, so on the return of a BLOCKING syscall (waitpid/read/futex) it names
    ; a stale frame -- using rsp delivers on the correct stack regardless of blocking.
    mov rsi, rax                   ; arg2 = syscall return value
    mov rdi, rsp                   ; arg1 = this syscall's GP frame (local, always valid)
    push rax                       ; preserve retval across the C call
    call deliver_pending_signals
    pop rax

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

%endif

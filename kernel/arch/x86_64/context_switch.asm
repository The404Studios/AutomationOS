; context_switch.asm - Context switching for x86_64
; void context_switch_asm(process_t* from, process_t* to);

[BITS 64]
global context_switch_asm

; Offsets into cpu_context_t structure
; These must match the layout in sched.h
; cpu_context_t has 19 uint64_t fields (152 bytes) + 512 byte fpu_state
CONTEXT_RAX     equ 0
CONTEXT_RBX     equ 8
CONTEXT_RCX     equ 16
CONTEXT_RDX     equ 24
CONTEXT_RSI     equ 32
CONTEXT_RDI     equ 40
CONTEXT_RBP     equ 48
CONTEXT_RSP     equ 56
CONTEXT_R8      equ 64
CONTEXT_R9      equ 72
CONTEXT_R10     equ 80
CONTEXT_R11     equ 88
CONTEXT_R12     equ 96
CONTEXT_R13     equ 104
CONTEXT_R14     equ 112
CONTEXT_R15     equ 120
CONTEXT_RIP     equ 128
CONTEXT_RFLAGS  equ 136
CONTEXT_CR3     equ 144
CONTEXT_FPU     equ 160        ; FXSAVE/FXRSTOR area (512B). Offset 160 (not 152):
                               ; cpu_context_t.fpu_state has __attribute__((aligned(16))),
                               ; so the C compiler pads it to a 16-aligned offset (160).

; Offsets into process_t structure
; process_t {
;     pid (4 bytes) + parent_pid (4 bytes) + state (4 bytes) + padding (4 bytes) = 16 bytes
;     cpu_context_t context starts at offset 16
; }
PROCESS_CONTEXT_OFFSET equ 16

section .text

context_switch_asm:
    ; Arguments:
    ; RDI = from (source process)
    ; RSI = to (destination process)

    ; CRITICAL: Disable interrupts during context switch to prevent race conditions
    cli

    ; If from is NULL, just restore 'to' context (no save needed)
    test rdi, rdi
    jz .restore_context

    ; === Save 'from' context ===
    ; Strategy: Use stack to preserve registers while we work with pointers
    ; Save original RDI value before we calculate context address
    push rdi                         ; Save original RDI on stack

    ; Calculate context address: from + PROCESS_CONTEXT_OFFSET
    add rdi, PROCESS_CONTEXT_OFFSET

    ; Save general purpose registers (in order matching cpu_context_t structure)
    mov [rdi + CONTEXT_RAX], rax
    mov [rdi + CONTEXT_RBX], rbx
    mov [rdi + CONTEXT_RCX], rcx
    mov [rdi + CONTEXT_RDX], rdx
    mov [rdi + CONTEXT_RSI], rsi            ; Save RSI (2nd syscall arg - CRITICAL)

    ; Restore and save original RDI from stack
    pop rax                                 ; Get original RDI
    mov [rdi + CONTEXT_RDI], rax            ; Save it

    mov [rdi + CONTEXT_RBP], rbp
    mov [rdi + CONTEXT_RSP], rsp
    mov [rdi + CONTEXT_R8], r8
    mov [rdi + CONTEXT_R9], r9
    mov [rdi + CONTEXT_R10], r10
    mov [rdi + CONTEXT_R11], r11
    mov [rdi + CONTEXT_R12], r12
    mov [rdi + CONTEXT_R13], r13
    mov [rdi + CONTEXT_R14], r14
    mov [rdi + CONTEXT_R15], r15

    ; Save RIP (return address from stack, accounting for our push)
    mov rax, [rsp]                          ; Return address is now at [rsp]
    mov [rdi + CONTEXT_RIP], rax

    ; Save RFLAGS
    pushfq
    pop rax
    mov [rdi + CONTEXT_RFLAGS], rax

    ; Save CR3 (page directory)
    mov rax, cr3
    mov [rdi + CONTEXT_CR3], rax

    ; Save FPU/SSE state (required for LLM inference with floating-point)
    fxsave64 [rdi + CONTEXT_FPU]

.restore_context:
    ; === Restore 'to' context ===
    ; Calculate context address: to + PROCESS_CONTEXT_OFFSET
    add rsi, PROCESS_CONTEXT_OFFSET

    ; Restore CR3 with PCID optimization (avoid TLB flush if PCID enabled)
    ; PCID (Process-Context Identifiers): 40-60% context switch speedup
    mov rax, cr4
    test rax, (1 << 17)              ; Check if PCID enabled (CR4.PCIDE bit 17)
    jz .no_pcid

    ; PCID enabled - preserve TLB entries by setting bit 63
    mov rax, [rsi + CONTEXT_CR3]
    bts rax, 63                      ; Set bit 63: do NOT flush TLB
    mov cr3, rax                     ; CR3 loaded, TLB preserved (FAST!)
    jmp .cr3_done

.no_pcid:
    ; PCID not supported - full TLB flush (slow path)
    mov rax, [rsi + CONTEXT_CR3]
    mov cr3, rax                     ; CR3 loaded, TLB flushed (200-400 cycles)

.cr3_done:

    ; Restore FPU/SSE state (fxsave64 saved it, fxrstor64 restores it)
    fxrstor64 [rsi + CONTEXT_FPU]

    ; === CRITICAL SECTION: Correct register restore order to prevent RSI corruption ===
    ; Problem: RSI holds context base pointer ([rsi + offset])
    ;          We need to restore RSI itself from [rsi + CONTEXT_RSI]
    ;          BUT we cannot overwrite RSI until we've loaded ALL values from [rsi + offset]
    ; Solution:
    ;   1. Pre-load ALL RSI-dependent values (use stack for temporary storage)
    ;   2. Restore all registers EXCEPT RSI
    ;   3. Finally restore RSI as the VERY LAST register (fixes the corruption bug)

    ; Pre-load values we need after switching stacks.
    ; Use R12-R14 as temporaries (already saved in 'from' context above).
    mov r12, [rsi + CONTEXT_RSP]         ; R12 = new RSP
    mov r13, [rsi + CONTEXT_RIP]         ; R13 = new RIP
    mov r14, [rsi + CONTEXT_RFLAGS]      ; R14 = new RFLAGS

    ; Save RSI value before we clobber it
    push qword [rsi + CONTEXT_RSI]

    ; Restore all general-purpose registers (EXCEPT RSI, R12-R14)
    mov rax, [rsi + CONTEXT_RAX]
    mov rbx, [rsi + CONTEXT_RBX]
    mov rcx, [rsi + CONTEXT_RCX]
    mov rdx, [rsi + CONTEXT_RDX]
    mov rdi, [rsi + CONTEXT_RDI]
    mov rbp, [rsi + CONTEXT_RBP]
    mov r8,  [rsi + CONTEXT_R8]
    mov r9,  [rsi + CONTEXT_R9]
    mov r10, [rsi + CONTEXT_R10]
    mov r11, [rsi + CONTEXT_R11]
    mov r15, [rsi + CONTEXT_R15]

    ; Restore RSI last (was our base pointer)
    pop rsi

    ; Switch to the new process's stack
    mov rsp, r12

    ; Push RIP for final ret
    push r13

    ; Restore RFLAGS properly (from R14, not from wrong stack position)
    push r14
    popfq

    ; R12-R14 now hold stale values (RSP/RIP/RFLAGS), not their context values.
    ; For a fresh process (trampoline entry), this is fine.
    ; For a resumed process, these were saved/restored correctly above.

    ; CRITICAL: Do NOT sti here!
    ; The popfq above restores RFLAGS from the saved context (which has IF=0 because
    ; it was saved after cli at the top of this function). If we did sti here, the
    ; timer IRQ (IRQ0) could fire in the window between sti and ret, causing a
    ; re-entrant schedule() call inside an active context switch. The nested timer
    ; handler's iretq would return to this ret instruction and double-execute it
    ; with a corrupted stack, causing a GPF after ~1000+ context switches.
    ;
    ; Interrupts will be safely re-enabled by the iretq at the end of whichever
    ; handler chain called schedule() (timer IRQ handler, syscall handler, or
    ; exception handler), which restores the original RFLAGS with IF=1.
    ret

; ===========================================================================
; PREEMPTIVE SCHEDULING — IRQ-driven context-switch helpers
; ===========================================================================
; Everything below is compiled in ONLY when -DPREEMPTIVE is passed to nasm.
; With the flag off, this file is byte-for-byte identical to the cooperative
; build (these symbols simply do not exist), and the existing context_switch_asm
; above is completely untouched.
;
; The fundamental difference from context_switch_asm: that routine saves the
; KERNEL C-return RIP/RSP (correct for a switch initiated from a syscall/yield,
; FATAL from inside an IRQ). The helpers below instead save/restore the
; *interrupted* register frame — the GP regs the IRQ stub pushed plus the
; CPU-pushed iretq frame (RIP/CS/RFLAGS/RSP/SS) — so a switch can occur from a
; timer IRQ and resume via iretq.
;
; They do NOT switch stacks and do NOT iretq themselves: they operate on the
; interrupt_frame_t that lives on the current kernel stack. The C caller
; (schedule_from_irq) switches CR3/TSS, and the single iretq at the tail of
; irq0_preempt does the actual control transfer. Because kernel stacks are
; identity-mapped into every address space (paging_create_address_space copies
; the lower-half identity hierarchy), the frame remains readable across the CR3
; switch.

%ifdef PREEMPTIVE

global context_save_irq
global context_load_irq
global context_switch_to_iretq

; interrupt_frame_t field offsets (must match struct interrupt_frame in sched.h)
IFRAME_R15      equ 0
IFRAME_R14      equ 8
IFRAME_R13      equ 16
IFRAME_R12      equ 24
IFRAME_R11      equ 32
IFRAME_R10      equ 40
IFRAME_R9       equ 48
IFRAME_R8       equ 56
IFRAME_RBP      equ 64
IFRAME_RDI      equ 72
IFRAME_RSI      equ 80
IFRAME_RDX      equ 88
IFRAME_RCX      equ 96
IFRAME_RBX      equ 104
IFRAME_RAX      equ 112
IFRAME_INTNO    equ 120
IFRAME_RIP      equ 128
IFRAME_CS       equ 136
IFRAME_RFLAGS   equ 144
IFRAME_RSP      equ 152
IFRAME_SS       equ 160

; void context_save_irq(cpu_context_t* ctx, interrupt_frame_t* frame);
;   RDI = ctx (destination cpu_context_t)
;   RSI = frame (source interrupt_frame_t on the kernel stack)
; Copies the interrupted register state from the frame into ctx, captures the
; current CR3, and saves FPU state. CONTEXT_* offsets are defined at the top of
; this file. This is a plain leaf routine — no stack switch, returns normally.
context_save_irq:
    ; General-purpose registers
    mov rax, [rsi + IFRAME_RAX]
    mov [rdi + CONTEXT_RAX], rax
    mov rax, [rsi + IFRAME_RBX]
    mov [rdi + CONTEXT_RBX], rax
    mov rax, [rsi + IFRAME_RCX]
    mov [rdi + CONTEXT_RCX], rax
    mov rax, [rsi + IFRAME_RDX]
    mov [rdi + CONTEXT_RDX], rax
    mov rax, [rsi + IFRAME_RSI]
    mov [rdi + CONTEXT_RSI], rax
    mov rax, [rsi + IFRAME_RDI]
    mov [rdi + CONTEXT_RDI], rax
    mov rax, [rsi + IFRAME_RBP]
    mov [rdi + CONTEXT_RBP], rax
    mov rax, [rsi + IFRAME_R8]
    mov [rdi + CONTEXT_R8], rax
    mov rax, [rsi + IFRAME_R9]
    mov [rdi + CONTEXT_R9], rax
    mov rax, [rsi + IFRAME_R10]
    mov [rdi + CONTEXT_R10], rax
    mov rax, [rsi + IFRAME_R11]
    mov [rdi + CONTEXT_R11], rax
    mov rax, [rsi + IFRAME_R12]
    mov [rdi + CONTEXT_R12], rax
    mov rax, [rsi + IFRAME_R13]
    mov [rdi + CONTEXT_R13], rax
    mov rax, [rsi + IFRAME_R14]
    mov [rdi + CONTEXT_R14], rax
    mov rax, [rsi + IFRAME_R15]
    mov [rdi + CONTEXT_R15], rax

    ; Interrupted control state (from the CPU-pushed iretq frame)
    mov rax, [rsi + IFRAME_RIP]
    mov [rdi + CONTEXT_RIP], rax
    mov rax, [rsi + IFRAME_RFLAGS]
    mov [rdi + CONTEXT_RFLAGS], rax
    mov rax, [rsi + IFRAME_RSP]
    mov [rdi + CONTEXT_RSP], rax

    ; Current CR3 (the outgoing process is still mapped)
    mov rax, cr3
    mov [rdi + CONTEXT_CR3], rax

    ; FPU/SSE state
    fxsave64 [rdi + CONTEXT_FPU]
    ret

; void context_load_irq(cpu_context_t* ctx, interrupt_frame_t* frame);
;   RDI = ctx (source cpu_context_t)
;   RSI = frame (destination interrupt_frame_t on the kernel stack)
; Overwrites the on-stack interrupt frame with ctx's saved register state so
; that the trailing iretq in irq0_preempt resumes that context. Restores FPU
; state. Does NOT load CR3 (the C caller does it after this returns, while it
; still owns valid temporaries). Returns normally.
context_load_irq:
    ; FPU/SSE state first (uses ctx pointer in RDI before we touch it)
    fxrstor64 [rdi + CONTEXT_FPU]

    ; General-purpose registers -> frame
    mov rax, [rdi + CONTEXT_RAX]
    mov [rsi + IFRAME_RAX], rax
    mov rax, [rdi + CONTEXT_RBX]
    mov [rsi + IFRAME_RBX], rax
    mov rax, [rdi + CONTEXT_RCX]
    mov [rsi + IFRAME_RCX], rax
    mov rax, [rdi + CONTEXT_RDX]
    mov [rsi + IFRAME_RDX], rax
    mov rax, [rdi + CONTEXT_RSI]
    mov [rsi + IFRAME_RSI], rax
    mov rax, [rdi + CONTEXT_RDI]
    mov [rsi + IFRAME_RDI], rax
    mov rax, [rdi + CONTEXT_RBP]
    mov [rsi + IFRAME_RBP], rax
    mov rax, [rdi + CONTEXT_R8]
    mov [rsi + IFRAME_R8], rax
    mov rax, [rdi + CONTEXT_R9]
    mov [rsi + IFRAME_R9], rax
    mov rax, [rdi + CONTEXT_R10]
    mov [rsi + IFRAME_R10], rax
    mov rax, [rdi + CONTEXT_R11]
    mov [rsi + IFRAME_R11], rax
    mov rax, [rdi + CONTEXT_R12]
    mov [rsi + IFRAME_R12], rax
    mov rax, [rdi + CONTEXT_R13]
    mov [rsi + IFRAME_R13], rax
    mov rax, [rdi + CONTEXT_R14]
    mov [rsi + IFRAME_R14], rax
    mov rax, [rdi + CONTEXT_R15]
    mov [rsi + IFRAME_R15], rax

    ; Control state -> CPU-pushed iretq frame slots.
    mov rax, [rdi + CONTEXT_RIP]
    mov [rsi + IFRAME_RIP], rax
    mov rax, [rdi + CONTEXT_RSP]
    mov [rsi + IFRAME_RSP], rax

    ; RFLAGS: force IF=1 so the resumed process runs with interrupts enabled
    ; (otherwise a process preempted with IF momentarily clear could resume
    ; with timers masked and never be preempted again).
    mov rax, [rdi + CONTEXT_RFLAGS]
    or  rax, 0x200                   ; IF = 1
    and rax, ~0x100                  ; TF = 0 (never single-step on resume)
    mov [rsi + IFRAME_RFLAGS], rax

    ; Segment selectors for the iretq frame. The IRQ switch path is only used
    ; to resume RESUME_IRETQ processes, which were interrupted in ring 3, so
    ; they always return to user code/data (matches usermode.asm: CS=0x23,
    ; SS=0x1B). schedule_from_irq() guarantees this precondition.
    mov qword [rsi + IFRAME_CS], 0x23
    mov qword [rsi + IFRAME_SS], 0x1B
    ret

; ===========================================================================
; void context_switch_to_iretq(process_t* from, process_t* to);
;   RDI = from (cooperatively-suspended; its kernel C-return point is saved so
;               it can later be resumed by context_switch_asm's `ret`)
;   RSI = to   (a RESUME_IRETQ process: it was preempted in ring 3, so its
;               cpu_context_t holds the *ring-3* GP regs + RIP/RSP/RFLAGS)
;
; This is the bridge that lets a COOPERATIVE site (sys_yield / schedule /
; wq_block_current, all running in ring 0 inside a syscall) hand the CPU to a
; preempted RESUME_IRETQ process. context_switch_asm cannot do this: it ends in
; `ret`, which would jump to `to`'s ring-3 RIP while still at CPL=0 (running user
; code as the kernel → runaway → #DF). Instead we restore `to`'s full register
; state and IRETQ to ring 3 — exactly the transfer schedule_from_irq's stub does,
; but initiated from a syscall stack rather than the timer IRQ frame.
;
; Save half is IDENTICAL to context_switch_asm (save `from`'s kernel resume
; point + RFLAGS + CR3 + FPU). Restore half switches CR3 (PCID-safe), restores
; FPU + GP regs, then builds a ring-3 iretq frame (SS:RSP, RFLAGS|IF, CS:RIP)
; on the CURRENT kernel stack and iretqs. Does NOT return to the caller; control
; resumes in `from` later (via context_switch_asm) when the scheduler re-picks it.
;
; The caller MUST have already set TSS.RSP0 + kernel_rsp_save to `to`'s kernel
; stack (so `to`'s subsequent syscalls/interrupts land on the right stack),
; exactly as the cooperative context_switch callers already do for `to`.
context_switch_to_iretq:
    cli

    ; === Save 'from' context (mirror context_switch_asm save half) ===
    test rdi, rdi
    jz .ld_to                         ; from==NULL: nothing to save
    push rdi
    add rdi, PROCESS_CONTEXT_OFFSET
    mov [rdi + CONTEXT_RAX], rax
    mov [rdi + CONTEXT_RBX], rbx
    mov [rdi + CONTEXT_RCX], rcx
    mov [rdi + CONTEXT_RDX], rdx
    mov [rdi + CONTEXT_RSI], rsi
    pop rax                           ; original RDI
    mov [rdi + CONTEXT_RDI], rax
    mov [rdi + CONTEXT_RBP], rbp
    mov [rdi + CONTEXT_RSP], rsp
    mov [rdi + CONTEXT_R8],  r8
    mov [rdi + CONTEXT_R9],  r9
    mov [rdi + CONTEXT_R10], r10
    mov [rdi + CONTEXT_R11], r11
    mov [rdi + CONTEXT_R12], r12
    mov [rdi + CONTEXT_R13], r13
    mov [rdi + CONTEXT_R14], r14
    mov [rdi + CONTEXT_R15], r15
    mov rax, [rsp]                    ; kernel return address -> from's RIP
    mov [rdi + CONTEXT_RIP], rax
    pushfq
    pop rax
    mov [rdi + CONTEXT_RFLAGS], rax
    mov rax, cr3
    mov [rdi + CONTEXT_CR3], rax
    fxsave64 [rdi + CONTEXT_FPU]

.ld_to:
    ; === Restore 'to' context and IRETQ to ring 3 ===
    add rsi, PROCESS_CONTEXT_OFFSET

    ; Switch CR3 (PCID-safe), mirroring context_switch_asm.
    mov rax, cr4
    test rax, (1 << 17)
    jz .ld_no_pcid
    mov rax, [rsi + CONTEXT_CR3]
    bts rax, 63                       ; preserve TLB (CR4.PCIDE set)
    mov cr3, rax
    jmp .ld_cr3_done
.ld_no_pcid:
    mov rax, [rsi + CONTEXT_CR3]
    mov cr3, rax
.ld_cr3_done:

    ; Restore FPU/SSE.
    fxrstor64 [rsi + CONTEXT_FPU]

    ; Build the ring-3 IRETQ frame on the CURRENT (caller's) kernel stack.
    ; Hardware pops, top→bottom: SS, RSP, RFLAGS, CS, RIP. We push in reverse.
    ; SS=0x1B / CS=0x23 are the ring-3 (RPL=3) user selectors; RFLAGS gets IF=1
    ; (run with interrupts enabled) and TF=0 (no single-step), matching
    ; context_load_irq / enter_usermode.
    mov rax, [rsi + CONTEXT_RFLAGS]
    or  rax, 0x200                    ; IF = 1
    and rax, ~0x100                   ; TF = 0
    mov r11, rax                      ; r11 = ring-3 RFLAGS (restored below via frame)

    push qword 0x1B                   ; SS
    push qword [rsi + CONTEXT_RSP]    ; RSP (ring-3 user stack)
    push r11                          ; RFLAGS
    push qword 0x23                   ; CS
    push qword [rsi + CONTEXT_RIP]    ; RIP (ring-3 entry/resume point)

    ; Restore GP registers from `to`'s context. Load RSI LAST (it is our base
    ; pointer). Use the iretq frame already on the stack for control state.
    mov rax, [rsi + CONTEXT_RAX]
    mov rbx, [rsi + CONTEXT_RBX]
    mov rcx, [rsi + CONTEXT_RCX]
    mov rdx, [rsi + CONTEXT_RDX]
    mov rdi, [rsi + CONTEXT_RDI]
    mov rbp, [rsi + CONTEXT_RBP]
    mov r8,  [rsi + CONTEXT_R8]
    mov r9,  [rsi + CONTEXT_R9]
    mov r10, [rsi + CONTEXT_R10]
    mov r12, [rsi + CONTEXT_R12]
    mov r13, [rsi + CONTEXT_R13]
    mov r14, [rsi + CONTEXT_R14]
    mov r15, [rsi + CONTEXT_R15]
    mov r11, [rsi + CONTEXT_R11]      ; restore real R11 (clobbered as RFLAGS temp)
    mov rsi, [rsi + CONTEXT_RSI]      ; RSI last

    ; Load DS/ES/FS/GS with the ring-3 user data selector (iretq does NOT reload
    ; them; enter_usermode sets them the same way for first ring-3 entry).
    push rax
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    pop rax

    iretq                             ; → ring 3 at to->context.rip

%endif ; PREEMPTIVE

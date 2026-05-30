[BITS 64]

; External C handlers
extern exception_handler
extern irq_handler

; IDT flush function
global idt_flush
idt_flush:
    lidt [rdi]
    ret

; Macro to create ISR stub (for exceptions WITHOUT error code)
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    cli
    push qword 0        ; Dummy error code
    push qword %1       ; Interrupt number
    jmp isr_common_stub
%endmacro

; Macro to create ISR stub (for exceptions WITH error code)
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    cli
    ; Error code already pushed by CPU
    push qword %1       ; Interrupt number
    jmp isr_common_stub
%endmacro

; Macro to create IRQ stub
%macro IRQ 2
global irq%1
irq%1:
    cli
    push qword 0        ; Dummy error code
    push qword %2       ; Interrupt number (32 + IRQ)
    jmp irq_common_stub
%endmacro

; CPU Exception ISRs (0-31)
ISR_NOERRCODE 0     ; Division By Zero
ISR_NOERRCODE 1     ; Debug
ISR_NOERRCODE 2     ; Non-Maskable Interrupt
ISR_NOERRCODE 3     ; Breakpoint
ISR_NOERRCODE 4     ; Overflow
ISR_NOERRCODE 5     ; Bound Range Exceeded
ISR_NOERRCODE 6     ; Invalid Opcode
ISR_NOERRCODE 7     ; Device Not Available
ISR_ERRCODE   8     ; Double Fault
ISR_NOERRCODE 9     ; Coprocessor Segment Overrun
ISR_ERRCODE   10    ; Invalid TSS
ISR_ERRCODE   11    ; Segment Not Present
ISR_ERRCODE   12    ; Stack-Segment Fault
ISR_ERRCODE   13    ; General Protection Fault
ISR_ERRCODE   14    ; Page Fault
ISR_NOERRCODE 15    ; Reserved
ISR_NOERRCODE 16    ; x87 Floating-Point Exception
ISR_ERRCODE   17    ; Alignment Check
ISR_NOERRCODE 18    ; Machine Check
ISR_NOERRCODE 19    ; SIMD Floating-Point Exception
ISR_NOERRCODE 20    ; Virtualization Exception
ISR_NOERRCODE 21    ; Reserved
ISR_NOERRCODE 22    ; Reserved
ISR_NOERRCODE 23    ; Reserved
ISR_NOERRCODE 24    ; Reserved
ISR_NOERRCODE 25    ; Reserved
ISR_NOERRCODE 26    ; Reserved
ISR_NOERRCODE 27    ; Reserved
ISR_NOERRCODE 28    ; Reserved
ISR_NOERRCODE 29    ; Reserved
ISR_ERRCODE   30    ; Security Exception
ISR_NOERRCODE 31    ; Reserved

; IRQ handlers (32-47)
IRQ 0, 32   ; IRQ 0 - Timer (PIT)
IRQ 1, 33   ; IRQ 1 - Keyboard
IRQ 2, 34   ; IRQ 2 - Cascade
IRQ 3, 35   ; IRQ 3 - COM2
IRQ 4, 36   ; IRQ 4 - COM1
IRQ 5, 37   ; IRQ 5 - LPT2
IRQ 6, 38   ; IRQ 6 - Floppy
IRQ 7, 39   ; IRQ 7 - LPT1
IRQ 8, 40   ; IRQ 8 - RTC
IRQ 9, 41   ; IRQ 9 - Free
IRQ 10, 42  ; IRQ 10 - Free
IRQ 11, 43  ; IRQ 11 - Free
IRQ 12, 44  ; IRQ 12 - PS/2 Mouse
IRQ 13, 45  ; IRQ 13 - FPU
IRQ 14, 46  ; IRQ 14 - Primary ATA
IRQ 15, 47  ; IRQ 15 - Secondary ATA

; Common ISR stub for exceptions
isr_common_stub:
    ; Save all registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Call C exception handler
    ; Stack layout after saving registers:
    ;   [rsp + 0]    = r15
    ;   ...
    ;   [rsp + 15*8] = int_no
    ;   [rsp + 16*8] = err_code
    ;   [rsp + 17*8] = rip (pushed by CPU)
    ;   [rsp + 18*8] = cs  (pushed by CPU)
    ;   [rsp + 19*8] = rflags (pushed by CPU)
    ;   [rsp + 20*8] = rsp (pushed by CPU)
    ;   [rsp + 21*8] = ss  (pushed by CPU)
    mov rdi, [rsp + 15*8]  ; RDI = interrupt number
    mov rsi, [rsp + 16*8]  ; RSI = error code
    mov rdx, [rsp + 17*8]  ; RDX = RIP
    mov rcx, [rsp + 18*8]  ; RCX = CS
    call exception_handler

    ; Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Clean up error code and interrupt number
    add rsp, 16

    ; Return from interrupt
    iretq

; Common IRQ stub
irq_common_stub:
    ; Save all registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Call C IRQ handler
    ; RDI = interrupt number
    mov rdi, [rsp + 15*8]  ; Get interrupt number
    call irq_handler

    ; Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Clean up error code and interrupt number
    add rsp, 16

    ; Return from interrupt
    iretq

; ===========================================================================
; PREEMPTIVE SCHEDULING — dedicated IRQ0 (timer) entry stub
; ===========================================================================
; Assembled ONLY when -DPREEMPTIVE is passed to nasm. With the flag off this
; symbol does not exist and IDT[32] keeps pointing at the cooperative irq0 stub
; above (byte-for-byte unchanged), so behavior is identical to today. This stub
; is placed at the very END of the file so the cooperative irq0/irq_common_stub
; machine code is unaffected when the flag is enabled.
;
; To ENABLE preemption the integrator points IDT entry 32 at irq0_preempt
; instead of irq0 (see report).
;
; This stub builds an interrupt_frame_t on the kernel stack whose layout EXACTLY
; matches struct interrupt_frame in sched.h, then calls schedule_from_irq().
; That C handler sends the PIC EOI itself, runs the quantum/preemption logic,
; and — when a switch is due — rewrites the on-stack frame in place (via
; context_save_irq/context_load_irq) so the single iretq below resumes the
; *next* process. When no switch is due the frame is untouched and iretq simply
; resumes the interrupted process.
;
; Stack on entry (CPU-pushed iretq frame): SS, RSP, RFLAGS, CS, RIP (top→down).
; We push int_no then the 15 GP regs so that [rsp] == &frame->r15.

%ifdef PREEMPTIVE

extern schedule_from_irq

global irq0_preempt
irq0_preempt:
    cli                              ; interrupt gate already clears IF; explicit
    push qword 32                    ; int_no (vector 32 = IRQ0); frame->int_no
    ; GP regs: pushed so rax ends highest, r15 lowest (matches struct order).
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15                         ; [rsp] now points at frame->r15 (offset 0)

    mov rdi, rsp                     ; RDI = interrupt_frame_t*
    call schedule_from_irq           ; may rewrite the frame in place

    ; Restore GP regs from the (possibly rewritten) frame.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 8                       ; discard int_no

    ; Return to (the possibly new) process. iretq pops RIP/CS/RFLAGS/RSP/SS,
    ; switching ring + restoring IF in one atomic step. CR3/TSS.RSP0 were
    ; already updated by schedule_from_irq for the incoming process.
    iretq

%endif ; PREEMPTIVE

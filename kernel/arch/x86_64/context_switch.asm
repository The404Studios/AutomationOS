; context_switch.asm - Context switching for x86_64
; void context_switch_asm(process_t* from, process_t* to);

[BITS 64]
global context_switch_asm

; Offsets into cpu_context_t structure
; These must match the layout in sched.h
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

    ; If from is NULL, just restore 'to' context (no save needed)
    test rdi, rdi
    jz .restore_context

    ; === Save 'from' context ===
    ; Calculate context address: from + PROCESS_CONTEXT_OFFSET
    add rdi, PROCESS_CONTEXT_OFFSET

    ; Save general purpose registers
    mov [rdi + CONTEXT_RAX], rax
    mov [rdi + CONTEXT_RBX], rbx
    mov [rdi + CONTEXT_RCX], rcx
    mov [rdi + CONTEXT_RDX], rdx
    mov [rdi + CONTEXT_RSI], rsi
    mov [rdi + CONTEXT_RDI], rdi
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

    ; Save RIP (return address from stack)
    mov rax, [rsp]
    mov [rdi + CONTEXT_RIP], rax

    ; Save RFLAGS
    pushfq
    pop rax
    mov [rdi + CONTEXT_RFLAGS], rax

    ; Save CR3 (page directory)
    mov rax, cr3
    mov [rdi + CONTEXT_CR3], rax

.restore_context:
    ; === Restore 'to' context ===
    ; Calculate context address: to + PROCESS_CONTEXT_OFFSET
    add rsi, PROCESS_CONTEXT_OFFSET

    ; Restore CR3 (page directory)
    mov rax, [rsi + CONTEXT_CR3]
    mov cr3, rax

    ; Restore general purpose registers (except RAX, RSP, RIP, RFLAGS)
    mov rbx, [rsi + CONTEXT_RBX]
    mov rcx, [rsi + CONTEXT_RCX]
    mov rdx, [rsi + CONTEXT_RDX]
    mov rdi, [rsi + CONTEXT_RDI]
    mov rbp, [rsi + CONTEXT_RBP]
    mov r8,  [rsi + CONTEXT_R8]
    mov r9,  [rsi + CONTEXT_R9]
    mov r10, [rsi + CONTEXT_R10]
    mov r11, [rsi + CONTEXT_R11]
    mov r12, [rsi + CONTEXT_R12]
    mov r13, [rsi + CONTEXT_R13]
    mov r14, [rsi + CONTEXT_R14]
    mov r15, [rsi + CONTEXT_R15]

    ; Restore RSP (stack pointer)
    mov rsp, [rsi + CONTEXT_RSP]

    ; Restore RFLAGS
    mov rax, [rsi + CONTEXT_RFLAGS]
    push rax
    popfq

    ; Restore RAX
    mov rax, [rsi + CONTEXT_RAX]

    ; Restore RSI (was used to hold context address)
    push qword [rsi + CONTEXT_RIP]  ; Push return address
    mov rsi, [rsi + CONTEXT_RSI]    ; Restore RSI

    ; Jump to restored RIP
    ret

; Terminal process for AutomationOS
; Reads keyboard input via SYS_READ_EVENT, echoes via SYS_WRITE
; SYS_WRITE=3, SYS_READ_EVENT=14, SYS_YIELD=15

global _start

section .text
_start:
    ; Print banner
    mov rax, 3
    mov rdi, 1
    lea rsi, [rel msg_banner]
    mov rdx, msg_banner_len
    syscall

    ; Reserve 8 bytes on stack for echo buffer
    sub rsp, 8

.input_loop:
    ; Try to read a key (non-blocking)
    mov rax, 14             ; SYS_READ_EVENT
    syscall

    ; If no key (rax==0), yield and try again
    test rax, rax
    jz .no_key

    ; Got a key! Store on stack
    mov byte [rsp], al

    ; Print the character
    mov rax, 3              ; SYS_WRITE
    mov rdi, 1              ; stdout
    mov rsi, rsp            ; buffer on stack
    mov rdx, 1              ; 1 character
    syscall

    jmp .input_loop

.no_key:
    ; Yield CPU while waiting for input
    mov rax, 15             ; SYS_YIELD
    syscall
    jmp .input_loop

section .rodata
msg_banner:
    db "[TERMINAL] AutomationOS Terminal v0.1", 10
    db "[TERMINAL] Type something! Keys echo to serial.", 10
msg_banner_len equ $ - msg_banner

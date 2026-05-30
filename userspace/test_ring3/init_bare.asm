; Init process - spawns shell then yields forever
global _start
section .text
_start:
    mov rax, 3
    mov rdi, 1
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    mov rax, 16
    lea rdi, [rel shell_path]
    syscall

.loop:
    mov rax, 15
    syscall
    jmp .loop

section .rodata
msg:
    db "[INIT] AutomationOS starting...", 10
    db "[INIT] Launching shell...", 10
msg_len equ $ - msg

shell_path:
    db "sbin/shell", 0

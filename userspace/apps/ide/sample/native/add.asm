; add.asm -- hand-written x86-64 in the IDE assembler's Intel subset.
;
; Issues a SYS_EXIT syscall directly: rax = 0 (SYS_EXIT on AutomationOS),
; rdi = 7 (exit code). The process exits with code 7.
; Build with Ctrl+B, run with Ctrl+R.

section .text
global _start
_start:
    mov rax, 0        ; SYS_EXIT (== 0 on AutomationOS)
    mov rdi, 7        ; exit code
    syscall

; add.asm -- hand-written x86-64 in the IDE assembler's Intel subset.
;
; Issues a SYS_EXIT syscall directly: rax = 1 (SYS_EXIT), rdi = 7
; (exit code). The process exits with code 7. Build (B), run (R).

section .text
global _start
_start:
    mov rax, 1        ; SYS_EXIT
    mov rdi, 7        ; exit code
    syscall

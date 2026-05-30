; _start - C runtime entry point for freestanding userspace
; Calls main(), then calls exit() with the return value

global _start
extern main

section .text
_start:
    ; Align stack to 16 bytes (System V ABI requirement)
    and rsp, ~0xF

    ; Call main()
    call main

    ; main returned - call exit(rax)
    mov rdi, rax        ; exit status = main's return value
    mov rax, 0          ; SYS_EXIT
    syscall

    ; Should never reach here
.hang:
    jmp .hang

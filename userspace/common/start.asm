; Userspace program entry point stub
; This is called by the kernel's ELF loader before main()

[BITS 64]
section .text

extern main
global _start

_start:
    ; Clear frame pointer for proper stack traces
    xor rbp, rbp

    ; Align stack to 16 bytes (required by x86_64 ABI)
    and rsp, -16

    ; Call main function
    call main

    ; main() returned - exit with its return value
    mov rdi, rax      ; Exit code from main
    mov rax, 60       ; sys_exit syscall number
    syscall

    ; Should never reach here
    ud2

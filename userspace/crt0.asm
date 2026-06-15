; userspace/crt0.asm -- C runtime startup for argv-aware userspace programs.
; The kernel (kernel/fs/exec.c) lays out a SysV-style frame at the top of the
; user stack: at process entry RSP -> argc, RSP+8 -> argv[0], ... , NULL, envp.
; This crt0 reads argc/argv into the SysV arg registers, 16-aligns the stack,
; calls main(argc, argv, envp), and turns main's return value into SYS_EXIT.
;
; Link this (crt0.o) FIRST, ahead of an app object that defines
;   int main(int argc, char **argv);
; with entry point _start (the default for these apps).
[BITS 64]
global _start
extern main

section .text
_start:
    mov  rdi, [rsp]         ; argc
    lea  rsi, [rsp + 8]     ; argv (&argv[0])
    ; envp follows argv: frame = [argc][argv0..argvN-1][NULL][envp0..][NULL], so
    ; envp = &argv[argc+1] = rsi + rdi*8 + 8. EXECVE-INPLACE-0: forward it in RDX
    ; for main(argc, argv, envp). Backward-safe: 2-arg mains just ignore RDX.
    lea  rdx, [rsi + rdi*8 + 8] ; envp (SysV 3rd arg)
    and  rsp, -16           ; 16-align (argc was 16-aligned; call re-biases to %16==8)
    call main
    mov  edi, eax           ; exit status = main()'s return value
    xor  eax, eax           ; SYS_EXIT == 0
    syscall
.hang:
    hlt
    jmp .hang

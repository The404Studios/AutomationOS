[BITS 64]

global _start
extern efi_main

section .text
_start:
    ; UEFI entry point
    ; RCX = EFI_HANDLE ImageHandle
    ; RDX = EFI_SYSTEM_TABLE* SystemTable

    ; Save UEFI parameters
    mov rdi, rcx
    mov rsi, rdx

    ; Call C entry point
    call efi_main

    ; Should not return
    cli
.halt:
    hlt
    jmp .halt

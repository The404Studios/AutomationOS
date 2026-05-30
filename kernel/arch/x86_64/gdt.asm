[BITS 64]
global gdt_flush
global tss_flush

gdt_flush:
    lgdt [rdi]

    ; Reload data segments with kernel data selector (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reload CS using far return
    ; retfq expects: [rsp] = RIP, [rsp+8] = CS
    pop rdi          ; Pop return address
    push 0x08        ; Push kernel code selector (0x08)
    push rdi         ; Push return address
    retfq            ; Far return: pop RIP, pop CS

tss_flush:
    ; RDI = TSS selector (0x28)
    ltr di
    ret

[BITS 32]
[EXTERN kernel_main]

; Multiboot1 header
section .multiboot
align 4
MBALIGN  equ 1 << 0
MBMEMINFO equ 1 << 1
MBVIDEO  equ 1 << 2
MBFLAGS  equ MBALIGN | MBMEMINFO | MBVIDEO
MBMAGIC  equ 0x1BADB002
MBCHECKSUM equ -(MBMAGIC + MBFLAGS)

multiboot_header:
    dd MBMAGIC
    dd MBFLAGS
    dd MBCHECKSUM
    dd 0, 0, 0, 0, 0         ; address fields (unused)
    dd 0                      ; mode_type: 0 = linear graphics
    ; Request the T410's NATIVE panel resolution (1280x800). GRUB's gfxpayload
    ; chain in grub.cfg provides safe fallbacks (1024x768 -> auto) for VBEs that
    ; lack 1280x800, so a framebuffer is always set even when native is missing.
    dd 1280                   ; width  (was 1024; T410 native is 1280x800)
    dd 800                    ; height (was 768)
    dd 32                     ; depth

section .boot
global _start
_start:
    ; We arrive in 32-bit protected mode from multiboot
    cli
    mov esp, stack_top32

    ; Save multiboot magic and info pointer
    mov [mb_magic], eax
    mov [mb_info], ebx

    ; Setup long mode
    ; 1. Check if CPUID available
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_long_mode

    ; 2. Check for extended CPUID
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    ; 3. Check for long mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; 4. Setup initial page tables (identity map first 2MB + higher half)
    ; PML4
    mov eax, pml4_table
    mov dword [eax], pdpt_table + 0x03  ; Present + Write
    mov dword [eax + 4], 0
    ; Higher half entry (PML4[511])
    mov dword [eax + 511*8], pdpt_table_high + 0x03
    mov dword [eax + 511*8 + 4], 0

    ; PDPT (identity)
    mov eax, pdpt_table
    mov dword [eax], pd_table + 0x03
    mov dword [eax + 4], 0

    ; PDPT high (for 0xFFFFFFFF80000000 -> PML4[511], PDPT[510])
    mov eax, pdpt_table_high
    mov dword [eax + 510*8], pd_table + 0x03
    mov dword [eax + 510*8 + 4], 0

    ; PD - map first 512MB using 2MB pages (covers all RAM)
    mov eax, pd_table
    mov ebx, 0x00000083  ; Present + Write + Huge (2MB page)
    mov ecx, 512          ; Map 512 * 2MB = 1GB
.map_pd:
    mov [eax], ebx
    mov dword [eax + 4], 0
    add eax, 8
    add ebx, 0x200000   ; Next 2MB
    dec ecx
    jnz .map_pd

    ; 5. Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; 6. Load PML4 into CR3
    mov eax, pml4_table
    mov cr3, eax

    ; 7. Enable long mode (set EFER.LME)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; 8. Enable paging
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; 9. Load 64-bit GDT and jump to long mode
    lgdt [gdt64_ptr]
    jmp 0x08:long_mode_start

.no_long_mode:
    mov eax, 0xB8000
    mov dword [eax], 0x4F214F4E   ; "N!"
    hlt

[BITS 64]
long_mode_start:
    ; Set up 64-bit segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up 64-bit stack
    mov rsp, stack_top

    ; Save mb_info BEFORE clearing BSS (since mb_info is in BSS)
    mov r12d, dword [mb_info]

    ; --- Rescue the initrd before zeroing .bss -----------------------------
    ; GRUB loads our initrd (multiboot module 0) right after the kernel image.
    ; On the T410's fragmented E820 that lands INSIDE [__bss_start,__bss_end], so
    ; the `rep stosq` below would zero the compositor/init binary -> userspace
    ; spawn fails -> boot hangs just after the splash (the documented "kernel
    ; .bss grew past where GRUB drops the initrd" regression). The multiboot info
    ; + module structs sit OUTSIDE .bss (the splash, which reads framebuffer info
    ; from them, still renders), so copy module 0's DATA up to 16 MiB and rewrite
    ; its mod_start/mod_end; kernel.c then reads the safe copy transparently.
    ; No-op in QEMU (the module already lands outside .bss) and when no module is
    ; present. 16 MiB sits above __bss_end (~4 MiB) and above the source initrd.
    mov rsi, r12                  ; rsi = multiboot info struct
    mov ecx, dword [rsi + 20]     ; mods_count
    test ecx, ecx
    jz .initrd_done
    mov ebx, dword [rsi + 24]     ; mods_addr (module-struct array)
    test ebx, ebx
    jz .initrd_done
    mov r8d, dword [rbx]          ; mod_start (module 0)
    mov r9d, dword [rbx + 4]      ; mod_end
    mov rcx, r9
    sub rcx, r8                   ; rcx = initrd size in bytes
    jz .initrd_done
    mov rsi, r8                   ; src  = mod_start
    mov rdi, 0x1000000            ; dest = 16 MiB
    push rcx
    rep movsb                     ; lift the initrd up out of .bss
    pop rcx
    mov dword [rbx], 0x1000000    ; mod_start := 16 MiB
    add ecx, 0x1000000
    mov dword [rbx + 4], ecx      ; mod_end   := 16 MiB + size
.initrd_done:

    ; Clear BSS (page tables are in separate .pagetables section)
    extern __bss_start
    extern __bss_end
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    shr rcx, 3
    xor eax, eax
    rep stosq

    ; Pass saved multiboot info pointer to kernel_main
    mov edi, r12d
    call kernel_main

.halt:
    hlt
    jmp .halt

; GDT for 64-bit mode
section .rodata
align 16
gdt64:
    dq 0                    ; Null descriptor
    dq 0x00AF9A000000FFFF   ; 64-bit code segment
    dq 0x00CF92000000FFFF   ; 64-bit data segment
gdt64_ptr:
    dw $ - gdt64 - 1
    dq gdt64

; Page tables in separate section so BSS clearing won't destroy them
section .pagetables nobits alloc write
align 4096
pml4_table:   resb 4096
pdpt_table:   resb 4096
pdpt_table_high: resb 4096
pd_table:     resb 4096

section .bss nobits alloc write
align 16
stack_bottom32: resb 1024
stack_top32:

align 16
stack_bottom:  resb 16384
stack_top:

align 8
mb_magic: resq 1
mb_info:  resq 1

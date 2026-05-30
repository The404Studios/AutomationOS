; =============================================================================
; ABLoader Stage 1 -- Boot Sector (512 bytes, org 0x7C00)
; Author: fourzerofour
;
; Layout on disk (512-byte sectors):
;   Sector 0 (LBA 0)      : This file (stage1) -- MBR boot sector
;   Sectors 1-8 (LBA 1-8) : stage2 (~4 KB fits comfortably in 8 sectors)
;   Sectors 9+  (LBA 9+)  : kernel ELF appended by build script
;
; Stage 1 responsibilities:
;   1. Print "ABLoader by fourzerofour" via BIOS int 0x10
;   2. Load stage 2 (8 sectors starting at LBA 1) to 0x7E00
;   3. Jump to stage 2
; =============================================================================

[BITS 16]
[ORG 0x7C00]

; ---- Entry ----
start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00          ; Stack grows down from 0x7C00
    sti

    ; Save boot drive number (BIOS passes it in DL)
    mov [boot_drive], dl

    ; Clear screen (scroll up with blank lines)
    mov ax, 0x0003          ; INT 10h AH=00 AL=03 = set video mode 80x25 text
    int 0x10

    ; Set cursor position row=0 col=0
    mov ah, 0x02
    xor bh, bh
    xor dx, dx
    int 0x10

    ; Print banner
    mov si, msg_banner
    call print_string

    mov si, msg_loading
    call print_string

    ; ---- Load Stage 2 via INT 13h (CHS, LBA=1, 8 sectors -> 0x7E00) ----
    ; CHS for LBA 1 on a floppy/HDD with 18 spt, 2 heads:
    ;   LBA 1 => C=0, H=0, S=2
    ; We use INT 13h AH=42h (extended LBA read) if available,
    ; falling back to AH=02h (CHS) if not.

    ; Check for INT 13h extensions
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc  .use_chs
    cmp bx, 0xAA55
    jne .use_chs
    test cl, 1              ; bit 0 = enhanced disk drive functions
    jz  .use_chs

    ; Extended read (LBA)
    mov si, dap             ; DS:SI -> Disk Address Packet
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc  .disk_error
    jmp .stage2_loaded

.use_chs:
    ; CHS fallback: LBA 1 -> sector 2, head 0, cylinder 0
    mov ah, 0x02            ; Read sectors
    mov al, 8               ; 8 sectors = stage 2
    mov ch, 0               ; Cylinder 0
    mov cl, 2               ; Sector 2 (1-based)
    mov dh, 0               ; Head 0
    mov dl, [boot_drive]
    mov bx, 0x7E00          ; ES:BX destination (ES=0 set above)
    int 0x13
    jc  .disk_error
    jmp .stage2_loaded

.disk_error:
    mov si, msg_disk_err
    call print_string
.hang:
    cli
    hlt
    jmp .hang

.stage2_loaded:
    mov si, msg_ok
    call print_string

    ; Jump to stage 2
    mov dl, [boot_drive]    ; Pass boot drive in DL
    jmp 0x0000:0x7E00

; ---- Subroutine: print null-terminated string at DS:SI via INT 10h ----
print_string:
    pusha
.loop:
    lodsb
    test al, al
    jz  .done
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x0F            ; White on black
    int 0x10
    jmp .loop
.done:
    popa
    ret

; ---- Data ----
msg_banner   db 0x0D, 0x0A
             db "  ABLoader by fourzerofour", 0x0D, 0x0A
             db "  AutomationOS BIOS Bootloader", 0x0D, 0x0A
             db 0
msg_loading  db "  Loading stage 2...", 0
msg_ok       db " OK", 0x0D, 0x0A, 0
msg_disk_err db 0x0D, 0x0A, "DISK ERROR!", 0

boot_drive   db 0x80        ; Default: first HDD

; ---- Disk Address Packet (for INT 13h AH=42h) ----
align 4
dap:
    db 0x10                 ; Size of DAP = 16 bytes
    db 0                    ; Reserved
    dw 8                    ; Number of sectors to read
    dw 0x7E00               ; Destination offset
    dw 0x0000               ; Destination segment
    dq 1                    ; Starting LBA (sector 1)

; ---- Boot sector signature ----
times 510 - ($ - $$) db 0
dw 0xAA55

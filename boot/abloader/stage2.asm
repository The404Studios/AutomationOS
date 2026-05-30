; =============================================================================
; ABLoader Stage 2  (v2 — multiboot1 32-bit handoff + initrd + VBE)
; Author: fourzerofour
;
; Loaded by stage1 at physical address 0x7E00 (real mode, 16-bit).
; Stage1 passes boot drive number in DL on entry.
;
; Boot flow:
;   [16-bit real mode]
;     1. Save boot drive, set segments to 0, stack at 0x7C00
;     2. Display splash
;     3. VBE: set 1024x768x32 linear-framebuffer mode (Fix #3)
;     4. Enable A20
;     5. Load kernel ELF from disk to scratch buffer at 0x10000
;     6. Load initrd from disk to INITRD_LOAD_PHY (Fix #2)
;     7. Parse ELF64 program headers (PT_LOAD), record segments
;     8. Build multiboot1 info struct at MBINFO_BASE (Fix #2: modules + VBE)
;     9. Detect memory (INT 15h E820)
;    10. Enter 32-bit protected mode (CR0.PE), paging OFF  (Fix #1)
;   [32-bit protected mode — paging OFF, per multiboot1 spec]
;    11. Copy PT_LOAD segments from scratch to their physical addresses
;    12. Set EAX=0x2BADB002, EBX=MBINFO_BASE
;    13. Far-jump to kernel e_entry (physical addr) in 32-bit protected mode
;        The kernel's own _start then sets up long mode itself (Fix #1)
;
; Memory layout used by this loader:
;   0x7C00   Stack (grows down)
;   0x7E00   This stage2 code (8 sectors = 4096 bytes)
;   0x9800   multiboot_info_t  (MBINFO_BASE)
;   0x9900   E820 mmap entries (MMAP_BASE)
;   0x9A00   multiboot_module_t[1]  (MODSTRUCT_BASE)
;   0x10000  Kernel ELF scratch buffer (ELF_SCRATCH_PHY, up to ~512 KB)
;   INITRD_LOAD_PHY  Initrd raw image (1 MB above scratch, at 0x500000)
;
; Disk image layout (sectors, 512 B each):
;   0       stage1.bin (MBR)
;   1-8     stage2.bin (this file, exactly 4096 bytes)
;   9+      kernel.elf (KERNEL_SECTOR_COUNT sectors)
;   9+KSC   initrd.img  (INITRD_SECTOR_COUNT sectors) — appended by build.sh
;
; Multiboot1 contract delivered to kernel _start:
;   EAX = 0x2BADB002
;   EBX = 0x9800  (physical pointer to multiboot_info_t)
;   CPU = 32-bit protected mode, paging DISABLED
;
;   Flags set in multiboot_info_t:
;     bit 0  (0x001): mem_lower=640, mem_upper=detected
;     bit 3  (0x008): mods_count=1, mods_addr=0x9A00  (initrd)
;     bit 6  (0x040): mmap_addr=0x9900, mmap_length=detected
;     bit 11 (0x800): vbe_control_info/vbe_mode_info filled (if VBE found)
;     bit 12 (0x1000): framebuffer_addr/pitch/width/height/bpp/type set
; =============================================================================

[BITS 16]
[ORG 0x7E00]

; ------------- Constants -----------------------------------------------------
MBINFO_BASE          equ 0x9800   ; Physical address of multiboot_info_t
MMAP_BASE            equ 0x9900   ; Memory map entries start here
MODSTRUCT_BASE       equ 0x9A00   ; multiboot_module_t[1] (16 bytes)
VBE_CTRL_BASE        equ 0x9B00   ; VBE controller info (512 bytes)
VBE_MODE_BASE        equ 0x9D00   ; VBE mode info (256 bytes)
ELF_SCRATCH_SEG      equ 0x1000   ; Segment for ELF load buffer (phy = 0x10000)
ELF_SCRATCH_PHY      equ 0x10000  ; Physical address of ELF scratch buffer
INITRD_LOAD_PHY      equ 0x500000 ; Physical address to load initrd (5 MB)
KERNEL_LBA           equ 9        ; First LBA sector of kernel ELF in image
KERNEL_SECTOR_COUNT  equ 480
; INITRD_LBA and INITRD_SECTOR_COUNT are patched by build.sh at assemble time:
INITRD_LBA           equ 489
INITRD_SECTOR_COUNT  equ 2540
MB_FLAG_MEM          equ (1 << 0)
MB_FLAG_MODS         equ (1 << 3)
MB_FLAG_MMAP         equ (1 << 6)
MB_FLAG_VBE          equ (1 << 11)
MB_FLAG_FB           equ (1 << 12)
MAX_SEGS             equ 8        ; Max PT_LOAD segments we track

; ---- Debug helper: emit byte AL to port 0xE9 (QEMU debugcon) ----
%macro DBG 1
    push ax
    mov  al, %1
    out  0xE9, al
    pop  ax
%endmacro

; ------------- Entry ---------------------------------------------------------
stage2_entry:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  sp, 0x7C00
    sti

    mov  [boot_drive], dl
    DBG 0x41            ; 'A' = stage2 started

    ; ---- Splash screen ----
    call splash
    DBG 0x42            ; 'B' = splash done

    ; ---- Enable A20 ----
    mov  si, msg_a20
    call puts
    call enable_a20
    DBG 0x45            ; 'E' = A20 done
    mov  si, msg_ok
    call puts

    ; ---- Load kernel ELF sectors from disk ----
    mov  si, msg_load_kernel
    call puts
    call load_kernel
    DBG 0x46            ; 'F' = kernel loaded
    mov  si, msg_ok
    call puts

    ; ---- Load initrd (if present) ----
    mov  si, msg_load_initrd
    call puts
    call load_initrd
    DBG 0x47            ; 'G' = initrd loaded
    mov  si, msg_ok
    call puts

    ; ---- Parse ELF ----
    mov  si, msg_parse_elf
    call puts
    call parse_elf
    DBG 0x48            ; 'H' = ELF parsed
    mov  si, msg_ok
    call puts

    ; ---- Detect memory and build multiboot info ----
    call build_mb_info
    DBG 0x49            ; 'I' = mb_info built

    ; ---- VBE: try to set 1024x768x32 linear framebuffer mode ----
    ; Done LAST before pmode, after all INT 10h text output is finished.
    ; VBE mode switch breaks INT 10h TTY output, so this must come last.
    mov  si, msg_vbe
    call puts
    DBG 0x43            ; 'C' = entering VBE
    call try_vbe
    ; Report vbe_found status
    push ax
    mov  al, 0x44           ; 'D' = VBE done
    out  0xE9, al
    mov  al, [vbe_found]    ; 0x00=not found, 0x01=found
    add  al, 0x30           ; '0' or '1'
    out  0xE9, al
    pop  ax

    ; Update multiboot info with VBE results (now that try_vbe has run)
    call mb_update_vbe

    ; Note: don't call puts here — INT 10h TTY may not work in VBE graphics mode

    ; ---- Enter 32-bit protected mode ----
    ; msg_pmode cannot be printed after VBE mode set (INT 10h broken in graphics mode)
    DBG 0x4A            ; 'J' = entering pmode
    cli
    lgdt [gdt32_ptr]
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  0x08:pmode32_entry     ; Far jump flushes prefetch queue, loads CS

; =============================================================================
; 16-BIT SUBROUTINES
; =============================================================================

; ---- splash: clear screen, print banner, draw progress bar ----
splash:
    mov  ax, 0x0003         ; Set text mode 80x25 (also clears screen)
    int  0x10
    ; Hide cursor
    mov  ah, 0x01
    mov  cx, 0x2607
    int  0x10
    ; Position cursor row 2, col 0
    mov  ah, 0x02
    xor  bh, bh
    mov  dh, 2
    xor  dl, dl
    int  0x10
    mov  si, banner0
    call puts
    mov  si, banner1
    call puts
    mov  si, banner2
    call puts
    mov  si, banner3
    call puts
    mov  si, banner4
    call puts
    mov  si, banner5
    call puts
    ; Progress bar label, row 9
    mov  ah, 0x02
    xor  bh, bh
    mov  dh, 9
    xor  dl, dl
    int  0x10
    mov  si, msg_boot_label
    call puts
    ; Animate 6 ticks
    mov  cx, 6
.tick:
    push cx
    mov  si, msg_tick
    call puts
    call delay
    pop  cx
    loop .tick
    mov  si, msg_boot_end
    call puts
    ; Newline
    mov  ah, 0x0E
    mov  al, 0x0D
    int  0x10
    mov  al, 0x0A
    int  0x10
    ret

; ---- delay: busy-wait loop ----
delay:
    push cx
    push dx
    mov  cx, 0x02           ; Reduced from 0x0F for faster boot
.outer:
    mov  dx, 0x8000         ; Reduced from 0xFFFF
.inner:
    dec  dx
    jnz  .inner
    loop .outer
    pop  dx
    pop  cx
    ret

; ---- puts: print null-terminated string at DS:SI via BIOS TTY ----
puts:
    pusha
.l: lodsb
    test al, al
    jz   .d
    mov  ah, 0x0E
    xor  bh, bh
    mov  bl, 0x0A           ; Bright green
    int  0x10
    jmp  .l
.d: popa
    ret

; =============================================================================
; FIX #3: VBE FRAMEBUFFER SETUP
; Try to set a 1024x768x32 linear framebuffer VBE mode.
; We use:
;   INT 10h AX=4F00h -> VbeInfoBlock at ES:DI = VBE_CTRL_BASE:0
;   INT 10h AX=4F01h CX=mode -> ModeInfoBlock at ES:DI = VBE_MODE_BASE:0
;   INT 10h AX=4F02h BX=mode|0x4000 -> set mode with linear framebuffer
;
; On success, vbe_found is set to 1 and vbe_mode/vbe_fb_addr/etc. are filled.
; The build_mb_info routine reads these to fill multiboot_info_t.
; =============================================================================
try_vbe:
    ; --- Get VBE Controller Info (VBE 2.0) ---
    mov  ax, VBE_CTRL_BASE >> 4
    mov  es, ax
    xor  di, di
    ; Write "VBE2" signature to request VBE 2.0 info
    mov  dword [es:di], 0x32454256   ; "VBE2"
    mov  ax, 0x4F00
    int  0x10
    cmp  ax, 0x004F
    jne  .no_vbe
    ; Check signature "VESA"
    mov  eax, dword [es:0]
    cmp  eax, 0x41534556            ; "VESA"
    jne  .no_vbe

    ; --- Walk the mode list looking for 1024x768x32 linear ---
    ; Mode list pointer is at offset 14 in VbeInfoBlock (far ptr: seg:off)
    mov  ax, [es:14 + 2]           ; segment of mode list
    mov  [vbe_modeptr_seg], ax
    mov  ax, [es:14]               ; offset of mode list
    mov  [vbe_modeptr_off], ax

    xor  ax, ax
    mov  es, ax

    ; Iterate modes (limit to 256 iterations to prevent hang)
    mov  cx, [vbe_modeptr_seg]
    mov  dx, [vbe_modeptr_off]
    mov  [vbe_iter_limit], word 256

.mode_loop:
    ; Safety: exit if we've scanned too many modes
    cmp  word [vbe_iter_limit], 0
    je   .no_mode
    dec  word [vbe_iter_limit]

    ; Read mode number (16-bit) from far ptr cx:dx
    push es
    mov  es, cx
    mov  bx, dx
    mov  ax, [es:bx]
    pop  es
    cmp  ax, 0xFFFF                 ; End of list
    je   .no_mode

    ; Get mode info
    push cx
    push dx
    push ax                         ; save mode number

    ; ModeInfoBlock -> ES:DI = VBE_MODE_BASE:0
    mov  bx, VBE_MODE_BASE >> 4
    mov  es, bx
    xor  di, di
    mov  cx, ax                     ; mode number
    mov  ax, 0x4F01
    int  0x10
    cmp  ax, 0x004F
    jne  .next_mode

    xor  ax, ax
    mov  es, ax

    ; Check attributes: bit 0=supported, bit 1=optional (info avail),
    ;                   bit 3=color, bit 4=graphics, bit 7=linear fb
    mov  ax, [VBE_MODE_BASE]        ; ModeAttributes (offset 0)
    test ax, (1 << 7)               ; Linear framebuffer bit
    jz   .next_mode
    test ax, (1 << 4)               ; Graphics mode
    jz   .next_mode
    test ax, (1 << 0)               ; Supported
    jz   .next_mode

    ; Check width=1024 (offset 18), height=768 (offset 20), bpp=32 (offset 25)
    cmp  word  [VBE_MODE_BASE + 18], 1024
    jne  .next_mode
    cmp  word  [VBE_MODE_BASE + 20], 768
    jne  .next_mode
    cmp  byte  [VBE_MODE_BASE + 25], 32
    jne  .next_mode

    ; Found our mode! Record everything.
    pop  ax                         ; mode number
    push ax
    mov  [vbe_mode_num], ax

    ; LinearBasePtr (offset 40 in ModeInfoBlock) - 32-bit physical address
    mov  eax, dword [VBE_MODE_BASE + 40]
    mov  [vbe_fb_phys], eax

    ; BytesPerScanLine (offset 16)
    movzx eax, word [VBE_MODE_BASE + 16]
    mov  [vbe_pitch], eax

    mov  byte [vbe_found], 1

    ; Set the mode: BX = mode | 0x4000 (use linear framebuffer)
    pop  bx
    or   bx, 0x4000
    mov  ax, 0x4F02
    int  0x10
    ; Ignore error here — if mode set fails we just won't have graphics

    pop  dx
    pop  cx
    ret

.next_mode:
    pop  ax                         ; discard saved mode number
    pop  dx
    pop  cx
    ; Advance pointer by 2
    add  dx, 2
    jmp  .mode_loop

.no_mode:
    xor  ax, ax
    mov  es, ax
.no_vbe:
    xor  ax, ax
    mov  es, ax
    ret

; ---- enable_a20 ----
enable_a20:
    mov  ax, 0x2401
    int  0x15
    jnc  .done
    ; 8042 method
    call .kbwait
    mov  al, 0xAD           ; Disable keyboard
    out  0x64, al
    call .kbwait
    mov  al, 0xD0           ; Read output port
    out  0x64, al
    call .rdwait
    in   al, 0x60
    push ax
    call .kbwait
    mov  al, 0xD1           ; Write output port
    out  0x64, al
    call .kbwait
    pop  ax
    or   al, 2              ; Set A20 bit
    out  0x60, al
    call .kbwait
    mov  al, 0xAE           ; Enable keyboard
    out  0x64, al
    call .kbwait
.done:
    mov  cx, 0xFFFF
.s: loop .s
    ret
.kbwait:
    in   al, 0x64
    test al, 2
    jnz  .kbwait
    ret
.rdwait:
    in   al, 0x64
    test al, 1
    jz   .rdwait
    ret

; ---- load_kernel: load KERNEL_SECTOR_COUNT sectors from LBA KERNEL_LBA ----
; Destination: physical 0x10000 (segment 0x1000, offset 0x0000).
load_kernel:
    mov  ah, 0x41
    mov  bx, 0x55AA
    mov  dl, [boot_drive]
    int  0x13
    jc   .chs
    cmp  bx, 0xAA55
    jne  .chs
    test cl, 1
    jz   .chs

    mov  dword [lba_lo], KERNEL_LBA
    mov  dword [lba_hi], 0
    mov  word  [cur_seg], ELF_SCRATCH_SEG
    mov  word  [remaining], KERNEL_SECTOR_COUNT

.lba_loop:
    cmp  word [remaining], 0
    jle  .done

    mov  ax, [remaining]
    cmp  ax, 64
    jle  .use_ax
    mov  ax, 64
.use_ax:
    mov  [this_cnt], ax

    mov  word  [dap + 0], 0x0010
    mov  ax, [this_cnt]
    mov  word  [dap + 2], ax
    mov  word  [dap + 4], 0x0000
    mov  ax, [cur_seg]
    mov  word  [dap + 6], ax
    mov  eax, [lba_lo]
    mov  dword [dap + 8], eax
    mov  eax, [lba_hi]
    mov  dword [dap + 12], eax

    mov  ah, 0x42
    mov  dl, [boot_drive]
    mov  si, dap
    int  0x13
    jc   .err

    movzx eax, word [this_cnt]
    add  [lba_lo], eax

    mov  ax, [this_cnt]
    shl  ax, 5
    add  [cur_seg], ax

    mov  ax, [this_cnt]
    sub  [remaining], ax
    jmp  .lba_loop

.chs:
    mov  ah, 0x02
    mov  al, KERNEL_SECTOR_COUNT & 0xFF
    mov  ch, 0
    mov  cl, (KERNEL_LBA + 1) & 0x3F
    mov  dh, 0
    mov  dl, [boot_drive]
    mov  ax, ELF_SCRATCH_SEG
    mov  es, ax
    xor  bx, bx
    int  0x13
    jc   .err
    xor  ax, ax
    mov  es, ax
    jmp  .done

.err:
    mov  si, msg_disk_err
    call puts
    cli
.h: hlt
    jmp  .h

.done:
    xor  ax, ax
    mov  es, ax
    ret

; =============================================================================
; FIX #2 PART A: load_initrd
; Load INITRD_SECTOR_COUNT sectors from LBA INITRD_LBA into INITRD_LOAD_PHY.
; If INITRD_SECTOR_COUNT == 0, skip silently.
; INITRD_LOAD_PHY = 0x500000 = segment 0x50000, but real-mode segment
; addressing only covers 20 bits (1 MB).  We must use BIOS extended read
; with a 32-bit physical address packed into the DAP segment:offset fields
; that represent a LINEAR address when the BIOS interprets them as
; seg*16 + off.  With ES:BX we can only reach 1 MB in real mode.
; The initrd is loaded in 32-bit protected mode (after kernel copy_segments)
; using direct IDE LBA28 PIO.  In real mode, load_initrd just records that
; an initrd is present (sets initrd_loaded=1) without reading from disk.
; The actual disk read happens in pmode32 via ide_load_initrd.
;
; INITRD_LOAD_PHY_ACTUAL: where the initrd lands in physical memory.
; Must be above the kernel (kernel highest paddr: ~0x12A000 + ~0x9FF88 ≈ 0x1CA000)
; and above any page tables. Use 0x500000 (5 MB) — plenty of room.
; =============================================================================
INITRD_LOAD_PHY_ACTUAL  equ 0x500000 ; 5 MB — safe, above all kernel segments
INITRD_CHUNK_SECTORS    equ 64       ; Sectors per IDE read call

; load_initrd (real mode): just record that initrd is present.
; Actual loading is done in pmode32 by ide_load_initrd (direct IDE PIO).
load_initrd:
    cmp  word [initrd_remaining], 0
    je   .no_initrd
    mov  byte [initrd_loaded], 1
    push ax
    mov  al, 0x47   ; 'G' = initrd present (will load in pmode32)
    out  0xE9, al
    pop  ax
    ret
.no_initrd:
    mov  byte [initrd_loaded], 0
    push ax
    mov  al, 0x67   ; 'g' = no initrd
    out  0xE9, al
    pop  ax
    ret

; ---- parse_elf: parse ELF at 0x10000, record PT_LOAD segments ----
parse_elf:
    mov  ax, ELF_SCRATCH_SEG
    mov  ds, ax

    cmp  dword [ds:0], 0x464C457F
    jne  .bad
    cmp  byte  [ds:4], 2
    jne  .bad

    mov  eax, dword [ds:24]
    mov  [cs:kentry_lo], eax
    mov  eax, dword [ds:28]
    mov  [cs:kentry_hi], eax

    mov  eax, dword [ds:32]
    mov  [cs:phoff], eax

    movzx ecx, word [ds:56]
    mov  [cs:phnum], cx

    xor  ax, ax
    mov  ds, ax

    xor  cx, cx
    mov  cx, [phnum]
    mov  eax, [phoff]

.phdr_loop:
    test cx, cx
    jz   .done

    push ax
    push cx
    mov  bx, ELF_SCRATCH_SEG
    mov  ds, bx
    mov  si, ax

    mov  ebx, dword [ds:si + 0]
    cmp  ebx, 1
    jne  .skip

    xor  bx, bx
    mov  ds, bx
    mov  bx, [seg_count]
    cmp  bx, MAX_SEGS - 1
    jge  .skip_restore_ds

    mov  bx, ELF_SCRATCH_SEG
    mov  ds, bx
    mov  edi, dword [ds:si + 24]
    mov  edx, dword [ds:si + 8]
    mov  ebp, dword [ds:si + 32]
    mov  ecx, dword [ds:si + 40]

    xor  bx, bx
    mov  ds, bx

    mov  bx, [seg_count]
    imul bx, bx, 20
    lea  bx, [seg_table + bx]

    mov  dword [bx + 0],  edi
    mov  dword [bx + 4],  edx
    mov  dword [bx + 8],  ebp
    mov  dword [bx + 12], ecx

    inc  word [seg_count]
    jmp  .next

.skip_restore_ds:
    xor  bx, bx
    mov  ds, bx
    jmp  .next

.skip:
    xor  bx, bx
    mov  ds, bx
.next:
    pop  cx
    pop  ax
    add  ax, 56
    dec  cx
    jmp  .phdr_loop

.done:
    ret

.bad:
    xor  ax, ax
    mov  ds, ax
    mov  si, msg_bad_elf
    call puts
    cli
.h: hlt
    jmp  .h

; =============================================================================
; FIX #2 PART B: build_mb_info
; Build multiboot_info_t at MBINFO_BASE with modules + VBE/framebuffer info.
; =============================================================================
build_mb_info:
    ; Zero the info region
    mov  di, MBINFO_BASE
    mov  cx, 256
    xor  ax, ax
    rep  stosw

    ; Detect memory via E820
    call detect_memory

    ; Base flags: mem + mmap
    mov  dword [MBINFO_BASE + 0], MB_FLAG_MEM | MB_FLAG_MMAP

    ; mem_lower = 640 KB
    mov  dword [MBINFO_BASE + 4], 640
    ; mem_upper
    mov  eax, [ram_kb_upper]
    mov  dword [MBINFO_BASE + 8], eax

    ; mmap
    mov  eax, [mmap_length]
    mov  dword [MBINFO_BASE + 44], eax
    mov  dword [MBINFO_BASE + 48], MMAP_BASE

    ; ---- Modules (initrd) — Fix #2 ----
    cmp  byte [initrd_loaded], 1
    jne  .no_mods

    ; Build multiboot_module_t at MODSTRUCT_BASE:
    ;   u32 mod_start  = INITRD_LOAD_PHY_ACTUAL
    ;   u32 mod_end    = mod_start + initrd_size_bytes
    ;   u32 cmdline    = 0 (or pointer to "initrd" string — skip for now)
    ;   u32 padding    = 0
    mov  dword [MODSTRUCT_BASE + 0],  INITRD_LOAD_PHY_ACTUAL
    ; initrd_size = INITRD_SECTOR_COUNT * 512
    mov  eax, INITRD_SECTOR_COUNT
    shl  eax, 9                    ; * 512
    add  eax, INITRD_LOAD_PHY_ACTUAL
    mov  dword [MODSTRUCT_BASE + 4],  eax   ; mod_end
    mov  dword [MODSTRUCT_BASE + 8],  0     ; cmdline
    mov  dword [MODSTRUCT_BASE + 12], 0     ; padding

    ; Set flags bit 3, mods_count=1, mods_addr=MODSTRUCT_BASE
    or   dword [MBINFO_BASE + 0], MB_FLAG_MODS
    mov  dword [MBINFO_BASE + 20], 1              ; mods_count
    mov  dword [MBINFO_BASE + 24], MODSTRUCT_BASE ; mods_addr

.no_mods:

.no_vbe_in_build:
    ret

; ---- mb_update_vbe: update multiboot info with VBE/framebuffer data ----
; Call this AFTER try_vbe so vbe_found, vbe_fb_phys, etc. are populated.
mb_update_vbe:
    cmp  byte [vbe_found], 0
    je   .no_vbe

    ; framebuffer_addr (offset 88 in multiboot_info_t, 64-bit)
    mov  eax, [vbe_fb_phys]
    mov  dword [MBINFO_BASE + 88], eax
    mov  dword [MBINFO_BASE + 92], 0

    ; framebuffer_pitch (offset 96)
    mov  eax, [vbe_pitch]
    mov  dword [MBINFO_BASE + 96], eax

    ; framebuffer_width (offset 100)
    mov  dword [MBINFO_BASE + 100], 1024

    ; framebuffer_height (offset 104)
    mov  dword [MBINFO_BASE + 104], 768

    ; framebuffer_bpp (offset 108)
    mov  byte  [MBINFO_BASE + 108], 32

    ; framebuffer_type (offset 109): 1 = RGB direct color
    mov  byte  [MBINFO_BASE + 109], 1

    ; VBE mode/info pointers
    mov  dword [MBINFO_BASE + 72], VBE_CTRL_BASE
    mov  dword [MBINFO_BASE + 76], VBE_MODE_BASE
    mov  ax, [vbe_mode_num]
    mov  word  [MBINFO_BASE + 80], ax

    ; Set flags bits 11 and 12
    or   dword [MBINFO_BASE + 0], MB_FLAG_VBE | MB_FLAG_FB

.no_vbe:
    ret

; ---- detect_memory: populate mmap table at MMAP_BASE via INT 15h E820 ----
detect_memory:
    mov  dword [mmap_length], 0
    mov  dword [ram_kb_upper], 0

    xor  ebx, ebx
    mov  di, MMAP_BASE

.e820:
    mov  eax, 0x0000E820
    mov  ecx, 20
    mov  edx, 0x534D4150
    push di
    mov  di, e820_buf
    int  0x15
    pop  di
    jc   .e820_done
    cmp  eax, 0x534D4150
    jne  .e820_done

    mov  dword [di + 0], 20

    mov  eax, dword [e820_buf + 0]
    mov  dword [di + 4], eax
    mov  eax, dword [e820_buf + 4]
    mov  dword [di + 8], eax

    mov  eax, dword [e820_buf + 8]
    mov  dword [di + 12], eax
    mov  eax, dword [e820_buf + 12]
    mov  dword [di + 16], eax

    mov  eax, dword [e820_buf + 16]
    mov  dword [di + 20], eax

    cmp  eax, 1
    jne  .skip_acc
    cmp  dword [di + 4], 0x100000
    jb   .skip_acc
    mov  eax, dword [di + 12]
    shr  eax, 10
    add  [ram_kb_upper], eax
.skip_acc:

    add  di, 24
    add  dword [mmap_length], 24

    mov  ax, [mmap_length]
    cmp  ax, 16 * 24
    jge  .e820_done

    test ebx, ebx
    jnz  .e820

.e820_done:
    cmp  dword [mmap_length], 0
    jg   .have_mmap

    mov  ax, 0xE801
    int  0x15
    jc   .fallback88
    movzx eax, cx
    movzx edx, dx
    shl  edx, 6
    add  eax, edx
    mov  [ram_kb_upper], eax
    call .build_simple_mmap
    jmp  .have_mmap

.fallback88:
    mov  ah, 0x88
    int  0x15
    jc   .use_default
    movzx eax, ax
    mov  [ram_kb_upper], eax
    call .build_simple_mmap
    jmp  .have_mmap

.use_default:
    mov  dword [ram_kb_upper], 0x1F000
    call .build_simple_mmap

.have_mmap:
    ret

.build_simple_mmap:
    mov  di, MMAP_BASE
    mov  dword [di + 0],  20
    mov  dword [di + 4],  0
    mov  dword [di + 8],  0
    mov  dword [di + 12], 0xA0000
    mov  dword [di + 16], 0
    mov  dword [di + 20], 1
    mov  dword [di + 24], 20
    mov  dword [di + 28], 0x100000
    mov  dword [di + 32], 0
    mov  eax, [ram_kb_upper]
    shl  eax, 10
    mov  dword [di + 36], eax
    mov  dword [di + 40], 0
    mov  dword [di + 44], 1
    mov  dword [mmap_length], 48
    ret

; =============================================================================
; FIX #1: 32-BIT PROTECTED MODE ENTRY — NO LONG MODE SETUP
;
; The kernel's _start (kernel/arch/x86_64/boot.asm) is [BITS 32] and expects:
;   - 32-bit protected mode, flat segments, paging OFF
;   - EAX = 0x2BADB002 (multiboot1 magic)
;   - EBX = physical address of multiboot_info_t
;
; We must NOT enter long mode here.  The kernel sets up its own page tables,
; enables PAE, sets EFER.LME, enables paging, and enters long mode itself.
; =============================================================================
[BITS 32]

pmode32_entry:
    mov  ax, 0x10           ; 32-bit flat data segment selector
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, 0x7C000       ; Stack below stage2, well clear of kernel

    ; Debug: signal pmode32_entry reached via port 0xE9
    mov  al, 0x4B           ; 'K' = pmode32_entry
    out  0xE9, al

    ; ---- Copy PT_LOAD segments from ELF scratch to physical addresses ----
    call copy_segments

    mov  al, 0x4C           ; 'L' = segments copied
    out  0xE9, al

    ; ---- Load initrd from disk if present (IDE PIO, pmode32) ----
    cmp  byte [initrd_loaded], 1
    jne  .no_initrd
    call ide_load_initrd
    mov  al, 0x52           ; 'R' = initrd loaded in pmode32
    out  0xE9, al
.no_initrd:

    ; ---- Deliver multiboot1 contract to kernel _start in 32-bit pmode ----
    ; Paging is OFF (we never enabled it).
    ; Load kernel entry from the variables filled by parse_elf.
    ; e_entry for this kernel is 0x101000 (physical) — but we read it from
    ; the ELF header to be robust.
    mov  eax, dword [kentry_lo]   ; Lower 32 bits of e_entry (= physical addr)
    ; kentry_hi should be 0 for a non-higher-half kernel; if it's non-zero,
    ; the kernel is linked at >4 GB which would not work in 32-bit pmode.
    ; In our case it's 0 (entry = 0x101000).

    ; Debug: emit entry point low byte
    mov  dl, al
    mov  al, 0x4D           ; 'M' = about to jump
    out  0xE9, al
    mov  al, dl             ; emit entry point low byte
    out  0xE9, al

    ; Set multiboot magic in EBX first, then switch EAX last
    mov  ebx, MBINFO_BASE         ; EBX = multiboot_info physical pointer
    mov  ecx, 0x2BADB002          ; Magic to put in EAX after we're done with eax

    ; eax holds the entry point; we need to jump there with EAX=magic.
    ; Use a far pointer pushed on stack and retf, or a register jump.
    ; We'll use: push 0x08 (cs), push eax (target), then set EAX=magic, retf.
    push dword 0x08               ; Code segment selector
    push eax                      ; Offset (kernel entry point)
    mov  eax, ecx                 ; EAX = 0x2BADB002
    ; EBX is already = MBINFO_BASE
    retf                          ; Far return: CS=0x08, EIP=kernel entry

; ---- copy_segments: iterate seg_table and memcpy each PT_LOAD ----
copy_segments:
    movzx ecx, word [seg_count]    ; seg_count is a 'dw'; zero-extend to avoid garbage in upper 16 bits
    test ecx, ecx
    jz   .done
    lea  esi, [seg_table]

.loop:
    test ecx, ecx
    jz   .done

    mov  edi, dword [esi + 0]   ; paddr (destination)
    mov  eax, dword [esi + 4]   ; file offset
    mov  ebx, dword [esi + 8]   ; filesz
    mov  edx, dword [esi + 12]  ; memsz

    push esi
    push ecx

    mov  esi, ELF_SCRATCH_PHY
    add  esi, eax               ; esi = source

    mov  ecx, ebx
    cld
    rep  movsb

    cmp  edx, ebx
    jle  .no_bss
    mov  ecx, edx
    sub  ecx, ebx
    xor  al, al
    rep  stosb
.no_bss:

    pop  ecx
    pop  esi
    add  esi, 20
    dec  ecx
    jmp  .loop

.done:
    ret

; =============================================================================
; ide_load_initrd: load initrd from IDE disk in 32-bit protected mode
; Uses primary IDE LBA28 PIO (ports 0x1F0-0x1F7).
; Reads INITRD_SECTOR_COUNT sectors starting at INITRD_LBA into INITRD_LOAD_PHY_ACTUAL.
;
; Register usage:
;   ESI = current LBA
;   EDI = destination pointer
;   ECX = sectors remaining
; Clobbers: EAX, EBX, ECX, EDX, ESI, EDI
; =============================================================================
ide_load_initrd:
    mov  esi, dword [initrd_lba_val]        ; starting LBA
    movzx ecx, word [initrd_remaining]      ; sector count
    mov  edi, INITRD_LOAD_PHY_ACTUAL        ; dest physical address

.ide_loop:
    test ecx, ecx
    jz   .ide_done

    ; Poll BSY (bit 7 of status register 0x1F7)
    mov  edx, 0x1F7
.bsy_wait:
    in   al, dx
    test al, 0x80                   ; BSY bit
    jnz  .bsy_wait

    ; Poll DRDY (bit 6) — drive ready
    test al, 0x40
    jz   .bsy_wait

    ; Send LBA28 to drive registers
    ; 0x1F2: sector count = 1
    mov  edx, 0x1F2
    mov  al, 1
    out  dx, al

    ; 0x1F3: LBA bits 0-7
    mov  edx, 0x1F3
    mov  eax, esi
    out  dx, al

    ; 0x1F4: LBA bits 8-15
    mov  edx, 0x1F4
    shr  eax, 8
    out  dx, al

    ; 0x1F5: LBA bits 16-23
    mov  edx, 0x1F5
    shr  eax, 8
    out  dx, al

    ; 0x1F6: LBA bits 24-27 + drive select + LBA mode
    mov  edx, 0x1F6
    shr  eax, 8
    and  al, 0x0F                   ; bits 24-27
    or   al, 0xE0                   ; LBA mode (bit 6) + master (bit 4) + bits 5,7 set
    out  dx, al

    ; 0x1F7: command = 0x20 (READ SECTORS)
    mov  edx, 0x1F7
    mov  al, 0x20
    out  dx, al

    ; Wait for DRQ (bit 3) and not BSY
.drq_wait:
    in   al, dx
    test al, 0x80                   ; BSY
    jnz  .drq_wait
    test al, 0x08                   ; DRQ
    jz   .drq_wait

    ; Check ERR/DF
    test al, 0x21                   ; ERR (bit 0) or DF (bit 5)
    jnz  .ide_err

    ; Read 256 WORDs (512 bytes) from data port 0x1F0 into [EDI]
    mov  edx, 0x1F0
    mov  ebx, 256                   ; 256 words = 512 bytes
.read_loop:
    in   ax, dx
    mov  [edi], ax
    add  edi, 2
    dec  ebx
    jnz  .read_loop

    ; Advance LBA and decrement count
    inc  esi
    dec  ecx
    jmp  .ide_loop

.ide_err:
    ; IDE error — mark initrd as not loaded (multiboot will have mods_count=0)
    mov  byte [initrd_loaded], 0
    mov  al, 0x45           ; 'E' = IDE error
    out  0xE9, al
    ret

.ide_done:
    ret

; =============================================================================
; DATA (16-bit addressable, within stage2's 4096-byte window)
; =============================================================================

; ---- GDT for 32-bit protected mode ----
align 8
gdt32:
    dq 0                        ; 00: null
    dq 0x00CF9A000000FFFF        ; 08: 32-bit code (lim=4G, base=0)
    dq 0x00CF92000000FFFF        ; 10: 32-bit data (lim=4G, base=0)
gdt32_end:
gdt32_ptr:
    dw gdt32_end - gdt32 - 1
    dd gdt32

; ---- Strings ----
banner0  db 0x0D, 0x0A, 0
banner1  db "  +----------------------------------------------+", 0x0D, 0x0A, 0
banner2  db "  |  ABLoader v2.0  --  by fourzerofour          |", 0x0D, 0x0A, 0
banner3  db "  |  AutomationOS BIOS Bootloader                |", 0x0D, 0x0A, 0
banner4  db "  +----------------------------------------------+", 0x0D, 0x0A, 0
banner5  db 0x0D, 0x0A, 0

msg_vbe          db 0x0D, 0x0A, "  [VBE] Setting up framebuffer...", 0
msg_a20          db 0x0D, 0x0A, "  [A20] Enabling A20...", 0
msg_load_kernel  db 0x0D, 0x0A, "  [KRN] Loading kernel...", 0
msg_load_initrd  db 0x0D, 0x0A, "  [RD]  Loading initrd...", 0
msg_parse_elf    db 0x0D, 0x0A, "  [ELF] Parsing ELF segments...", 0
msg_pmode        db 0x0D, 0x0A, "  [CPU] Entering 32-bit protected mode...", 0x0D, 0x0A, 0
msg_ok           db " OK", 0
msg_disk_err     db 0x0D, 0x0A, "  !! DISK ERROR - halted !!", 0
msg_bad_elf      db 0x0D, 0x0A, "  !! BAD ELF - halted !!", 0
msg_boot_label   db "  Boot: [", 0
msg_tick         db "=", 0
msg_boot_end     db "]", 0

; ---- Disk Address Packet (for INT 13h AX=42h) — kernel ----
align 4
dap:
    dw 0x0010
    dw 0
    dw 0
    dw 0
    dd 0
    dd 0

; ---- Disk Address Packet (for INT 13h AX=42h) — initrd ----
; (seg/off fields filled at runtime by load_initrd)
align 4
idap:
    dw 0x0010
    dw 0
    dw 0        ; dest offset (filled at runtime)
    dw 0        ; dest segment (filled at runtime)
    dd 0        ; LBA lo (filled at runtime)
    dd 0        ; LBA hi = 0

; ---- E820 output buffer (24 bytes) ----
e820_buf:
    times 24 db 0

; ---- Persistent variables ----
boot_drive        db 0x80
ram_kb_upper      dd 0
mmap_length       dd 0
lba_lo            dd KERNEL_LBA
lba_hi            dd 0
cur_seg           dw ELF_SCRATCH_SEG
remaining         dw KERNEL_SECTOR_COUNT
this_cnt          dw 0

; ---- Initrd load variables ----
; initrd_lba_val and initrd_remaining are patched by build.sh
initrd_lba_val    dd INITRD_LBA
initrd_remaining  dw INITRD_SECTOR_COUNT
initrd_loaded     db 0

; ---- VBE variables ----
vbe_found         db 0
vbe_mode_num      dw 0
vbe_fb_phys       dd 0
vbe_pitch         dd 0
vbe_modeptr_seg   dw 0
vbe_modeptr_off   dw 0
vbe_iter_limit    dw 256

; ---- Kernel entry point (filled by parse_elf) ----
kentry_lo         dd 0
kentry_hi         dd 0

; ---- ELF phdr parse state ----
phoff             dd 0
phnum             dw 0

; ---- PT_LOAD segment table ----
; 20 bytes per entry: paddr(4), file_offset(4), filesz(4), memsz(4), _pad(4)
seg_count         dw 0
seg_table:
    times (MAX_SEGS * 20) db 0

; ---- Pad to exactly 8 sectors = 4096 bytes ----
times (8 * 512) - ($ - $$) db 0

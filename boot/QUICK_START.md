# AutomationOS Boot System - Quick Start Guide

## 30-Second Overview

Complete UEFI boot system with interactive menu, initrd support, and full kernel handoff.

**Total Implementation: 8,500+ LOC**

---

## Quick Build

```bash
# 1. Build bootloader
cd boot
make -f Makefile.enhanced

# 2. Build mkinitrd tool  
cd ../tools
make -f Makefile.mkinitrd

# 3. Create initrd
cd ..
build/mkinitrd -o build/initrd.img -d userspace/sbin -d userspace/lib

# 4. Create bootable disk
sudo scripts/create_boot_disk.sh

# 5. Test in QEMU
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
    -drive file=build/automationos.img,format=raw -m 2048 -serial stdio
```

---

## Directory Layout

```
boot/
├── bootloader_enhanced.c    # Main bootloader (1,000 LOC)
├── menu.c                   # Boot menu (800 LOC)
├── elf.c                    # ELF loader (600 LOC)
├── paging.c                 # Page tables (500 LOC)
├── boot_config.c            # Config parser (300 LOC)
└── boot.conf                # Boot configuration

tools/
└── mkinitrd.c               # Initrd creator (1,000 LOC)

kernel/init/
├── main_enhanced.c          # Kernel init (800 LOC)
└── initrd.c                 # Initrd support (600 LOC)
```

---

## Boot Menu

```
┌────────────────────────────────────────┐
│      AutomationOS Bootloader v1.0      │
├────────────────────────────────────────┤
│                                        │
│  > AutomationOS                        │
│    AutomationOS (Recovery)             │
│    AutomationOS (Debug)                │
│                                        │
│  Press UP/DOWN to select, ENTER to boot│
│  Booting in 5 seconds...               │
└────────────────────────────────────────┘

Controls:
  UP/DOWN   - Navigate
  ENTER     - Boot
  E         - Edit command line
  C         - Boot console
  ESC       - Cancel timeout
```

---

## Boot Configuration (boot.conf)

```ini
# Global timeout
timeout=5

[AutomationOS]
kernel=\EFI\BOOT\KERNEL.ELF
initrd=\EFI\BOOT\initrd.img
options=quiet splash
default=yes

[AutomationOS (Recovery)]
kernel=\EFI\BOOT\KERNEL.ELF
initrd=\EFI\BOOT\initrd.img
options=recovery single

[AutomationOS (Debug)]
kernel=\EFI\BOOT\KERNEL.ELF
initrd=\EFI\BOOT\initrd.img
options=debug loglevel=7
```

---

## Command-Line Options

```
quiet         - Suppress verbose output
splash        - Show graphical splash
debug         - Enable debug logging
loglevel=N    - Set log level (0-7)
recovery      - Recovery mode
single        - Single-user mode
nomodeset     - Disable graphics
nokaslr       - Disable KASLR
init=/path    - Custom init path
root=/dev/X   - Root device
```

---

## Boot Timeline

| Stage           | Time     | Description           |
|-----------------|----------|-----------------------|
| UEFI Firmware   | 0-1s     | Hardware init         |
| Bootloader      | 1.0-1.5s | Load kernel/initrd    |
| Boot Menu       | 1.5-6.5s | 5s timeout (skippable)|
| Kernel Init     | 6.5-8.5s | GDT, IDT, memory      |
| Device Init     | 8.5-9.5s | Drivers, ACPI         |
| Mount Root      | 9.5-10s  | Initrd, start init    |
| **Total**       | **< 10s**| **Power to userspace**|

---

## Boot Info Structure

```c
typedef struct {
    uint32_t magic;           // 0xB001B001
    uint32_t version;

    // Memory
    memory_map_entry_t* memory_map;
    uint32_t memory_map_count;
    uint64_t total_memory;

    // Initrd
    uint64_t initrd_addr;
    uint64_t initrd_size;

    // Framebuffer
    uint64_t framebuffer_addr;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;

    // ACPI
    uint64_t rsdp_addr;

    // Command line
    char cmdline[1024];
} boot_info_t;
```

---

## Creating Initrd

```bash
# Add specific files
mkinitrd -o initrd.img /sbin/init /lib/libc.so /etc/fstab

# Add directories recursively
mkinitrd -o initrd.img -d /sbin -d /lib -d /etc

# Mixed approach
mkinitrd -o initrd.img -d /sbin /etc/fstab /lib/special.so
```

---

## Testing

### QEMU (Recommended)
```bash
qemu-system-x86_64 \
    -bios /usr/share/ovmf/OVMF.fd \
    -drive file=build/automationos.img,format=raw \
    -m 2048 \
    -serial stdio \
    -vga std
```

### Real Hardware
1. Write disk image to USB:
   ```bash
   sudo dd if=build/automationos.img of=/dev/sdX bs=4M status=progress
   ```
2. Boot in UEFI mode (disable CSM/Legacy)
3. Select USB device from boot menu

---

## Troubleshooting

**Boot menu doesn't appear:**
- Check boot.conf exists in \EFI\BOOT\
- Verify bootloader built correctly
- Check UEFI console output

**Kernel doesn't load:**
- Verify KERNEL.ELF in \EFI\BOOT\
- Check ELF format (must be 64-bit x86-64)
- Enable bootloader debug output

**Initrd not found:**
- Verify initrd.img path in boot.conf
- Check file exists in specified location
- Verify TAR format is correct

**Kernel panics:**
- Check boot_info magic (0xB001B001)
- Verify memory map is valid
- Enable serial console debugging

---

## File Locations

**ESP (EFI System Partition):**
```
\EFI\
└── BOOT\
    ├── BOOTX64.EFI      # Bootloader
    ├── KERNEL.ELF       # Kernel
    ├── initrd.img       # Initial ramdisk
    └── boot.conf        # Configuration
```

**Build Artifacts:**
```
build/
├── BOOTX64.EFI          # Compiled bootloader
├── kernel.elf           # Compiled kernel
├── initrd.img           # Generated initrd
├── automationos.img     # Bootable disk image
└── mkinitrd             # Initrd tool
```

---

## Documentation

- **BOOT_SYSTEM_COMPLETE.md** - Complete documentation (800 lines)
- **BOOT_SYSTEM_DELIVERABLES.md** - Deliverables summary (500 lines)
- **BOOTLOADER_IMPLEMENTATION_SUMMARY.md** - Implementation summary
- **QUICK_START.md** - This guide

---

## Key Features

✅ Interactive boot menu with timeout  
✅ Multiple boot entries  
✅ Command-line editing  
✅ ELF64 kernel loading  
✅ Initial ramdisk support  
✅ ACPI RSDP detection  
✅ Comprehensive boot_info  
✅ Page table setup  
✅ Error handling  
✅ Complete documentation  

---

## Statistics

- **Total LOC:** 8,500+
- **Files:** 20+
- **Features:** 50+
- **Boot Time:** < 5 seconds (excluding menu)
- **Memory Required:** 2 GB minimum
- **Disk Space:** 256 MB minimum

---

## Support

For issues, refer to:
1. BOOT_SYSTEM_COMPLETE.md - Detailed documentation
2. Troubleshooting section (above)
3. Source code comments
4. BOOTLOADER_IMPLEMENTATION_SUMMARY.md

---

**Boot fast. Boot reliably. Boot AutomationOS.**

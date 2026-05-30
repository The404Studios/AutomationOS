# Initrd Quick Start Guide

## What is the Initrd?

The **initrd** (initial ramdisk) is a TAR archive containing all userspace binaries needed for boot. It's loaded by GRUB and extracted by the kernel.

## Quick Build & Test

```bash
# Build everything (kernel + userspace + initrd + ISO)
make all

# Test in QEMU
make qemu
```

## Manual Steps

### 1. Build mkinitrd Tool
```bash
gcc tools/mkinitrd.c -o build/mkinitrd
```

### 2. Build Userspace
```bash
make userspace
```

This builds:
- `build/userspace/init/init` - Init process (PID 1)
- `build/userspace/shell/shell` - Shell
- `build/userspace/libc/libc.a` - C library

### 3. Create Initrd
```bash
./scripts/mkinitrd.sh
```

Output: `build/initrd.img`

### 4. Build ISO with Initrd
```bash
python3 scripts/build-iso.py
```

Output: `build/automationos.iso`

### 5. Test in QEMU
```bash
./scripts/run-qemu.sh
```

## Verify Initrd Contents

```bash
# List files in initrd
tar -tf build/initrd.img

# Extract to directory
mkdir /tmp/initrd_check
cd /tmp/initrd_check
tar -xf /path/to/build/initrd.img
ls -laR
```

## Expected Kernel Output

When booting with initrd, you should see:

```
[BOOT] Found 1 multiboot module(s)
[BOOT] Initrd detected:
  Address: 0x01000000
  Size: 245760 bytes (240 KB)
[KERNEL] Loading initrd...
[INITRD] Initrd at 0x01000000, size 245760 bytes
[KERNEL] Mounting initrd as root filesystem...
[INITRD] Mounting initrd...
[INITRD] Extracting: sbin/init (12345 bytes)
[INITRD] Extracting: bin/sh (23456 bytes)
[INITRD] Mounted successfully, 15 files extracted
[INITRD] File listing:
  f    12345  sbin/init
  f    23456  bin/sh
  ...
[KERNEL] Loading /sbin/init...
[KERNEL] Found init: 12345 bytes
[KERNEL] Init process started (PID 1)
```

## Troubleshooting

### Problem: "No multiboot modules loaded"
**Solution**: Check GRUB config has:
```grub
menuentry "AutomationOS" {
    multiboot /boot/kernel.elf
    module /boot/initrd.img      # ← This line is required!
    boot
}
```

### Problem: "No init binary found"
**Solution**:
```bash
# Check init exists
ls -l build/userspace/init/init

# Rebuild if missing
cd userspace/init
make clean
make
```

### Problem: "Invalid TAR header"
**Solution**: Use ustar format:
```bash
tar --format=ustar -cf build/initrd.img -C build/initrd_root .
```

## Directory Structure

```
build/
├── initrd.img           # Final initrd TAR archive
├── initrd_root/         # Staging directory for initrd contents
│   ├── sbin/
│   │   └── init         # Init binary
│   ├── bin/
│   │   ├── sh           # Shell
│   │   ├── compositor   # Graphics compositor
│   │   ├── wm           # Window manager
│   │   └── terminal     # Terminal
│   ├── lib/             # Libraries
│   ├── etc/
│   │   ├── fstab        # Filesystem table
│   │   └── inittab      # Init config
│   └── dev/             # Device nodes (empty)
└── iso/
    └── boot/
        ├── kernel.elf
        └── initrd.img   # Copied to ISO
```

## Key Files

| File | Purpose |
|------|---------|
| `scripts/mkinitrd.sh` | Creates initrd from userspace binaries |
| `tools/mkinitrd.c` | TAR archive creator (C implementation) |
| `kernel/init/initrd.c` | Kernel TAR parser |
| `kernel/kernel.c` | Multiboot module detection |
| `build/iso/boot/grub/grub.cfg` | GRUB bootloader config |

## Next Steps

1. **Test Boot**: Run `make qemu` to test kernel boot with initrd
2. **Add Programs**: Add more binaries to `scripts/mkinitrd.sh`
3. **Custom Init**: Modify `userspace/init/init.c` for custom startup
4. **Desktop**: Build compositor, WM, and shell for full desktop

## See Also

- `docs/INITRD_GUIDE.md` - Full technical documentation
- `kernel/init/initrd.c` - Kernel TAR parser implementation
- `tools/mkinitrd.c` - TAR creator source code

# AutomationOS Initial Ramdisk (Initrd) Guide

## Overview

The AutomationOS initrd is a TAR-format archive loaded by the bootloader that contains the minimal userspace needed for early boot. The kernel extracts the initrd contents and mounts it as the root filesystem.

## Architecture

### Format

- **Archive Format**: POSIX ustar TAR (uncompressed)
- **Block Size**: 512 bytes (standard TAR)
- **Encoding**: POSIX.1-1988 (ustar)

### Boot Flow

```
1. GRUB loads kernel.elf and initrd.img as multiboot modules
2. Kernel parses multiboot info to find initrd location
3. Kernel calls initrd_init(addr, size)
4. Kernel calls initrd_mount() to extract TAR contents
5. Kernel creates VFS entries for all files
6. Kernel loads /sbin/init from initrd
7. Kernel executes init as PID 1
```

## Directory Structure

```
initrd/
├── sbin/
│   └── init          # Init process (PID 1)
├── bin/
│   ├── sh            # Shell
│   ├── compositor    # Graphics compositor
│   ├── wm            # Window manager
│   ├── desktop-shell # Desktop shell
│   └── terminal      # Terminal emulator
├── lib/
│   └── *.a           # Userspace libraries
├── etc/
│   ├── fstab         # Filesystem table
│   └── inittab       # Init configuration
├── dev/              # Device nodes (empty, created by kernel)
├── proc/             # Process info (empty, mounted by kernel)
├── sys/              # Sysfs (empty, mounted by kernel)
└── tmp/              # Temporary files
```

## Building the Initrd

### Method 1: Using the Build System

```bash
# Build all components and create initrd
make initrd

# Or build everything including ISO
make all
```

### Method 2: Manual Build

```bash
# Build mkinitrd tool
gcc tools/mkinitrd.c -o build/mkinitrd

# Build userspace binaries
make userspace

# Create initrd
./scripts/mkinitrd.sh
```

### Method 3: Using Custom Binaries

```bash
# Create initrd from custom directory
./build/mkinitrd -o custom.img -d /path/to/initrd_root
```

## GRUB Configuration

The initrd must be loaded as a multiboot module:

```grub
menuentry "AutomationOS" {
    multiboot /boot/kernel.elf
    module /boot/initrd.img
    set gfxpayload=1024x768x32
    boot
}
```

## Kernel Integration

### Multiboot Module Detection

The kernel parses multiboot info to detect modules:

```c
// kernel/kernel.c
typedef struct {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t padding;
} multiboot_module_t;

// First module is assumed to be initrd
if (mb->flags & (1 << 3)) {
    multiboot_module_t* mods = (multiboot_module_t*)mb->mods_addr;
    boot_info->initrd_addr = mods[0].mod_start;
    boot_info->initrd_size = mods[0].mod_end - mods[0].mod_start;
}
```

### Initrd Initialization

```c
// Initialize initrd subsystem
initrd_init(boot_info->initrd_addr, boot_info->initrd_size);

// Mount as root filesystem
initrd_mount();

// List files for debugging
initrd_list_files();

// Load init binary
void* init_data = initrd_get_file("sbin/init", &init_size);
```

### TAR Parsing

The kernel includes a full TAR parser (`kernel/init/initrd.c`):

- Validates ustar magic and version
- Parses octal size/mode fields
- Handles directories (type '5') and files (type '0')
- Creates VFS entries for all files
- Supports 512-byte block alignment

## Testing

### Test Initrd Build

```bash
./scripts/test_initrd_build.sh
```

This script:
1. Builds the mkinitrd tool
2. Builds userspace binaries
3. Creates the initrd
4. Verifies TAR format
5. Lists contents

### Test in QEMU

```bash
# Build and run
make qemu

# Expected output:
# [BOOT] Found 1 multiboot module(s)
# [BOOT] Initrd detected: 0xXXXXXXXX, Size: XXXXX bytes
# [KERNEL] Loading initrd...
# [INITRD] Initrd at 0xXXXXXXXX, size XXXXX bytes
# [INITRD] Mounting initrd...
# [INITRD] Extracting: sbin/init (XXXX bytes)
# [INITRD] Mounted successfully, N files extracted
```

### Debug Initrd Contents

```bash
# Extract initrd to verify contents
mkdir -p /tmp/initrd_test
cd /tmp/initrd_test
tar -xf /path/to/build/initrd.img
ls -laR
```

## API Reference

### kernel/init/initrd.c

#### `void initrd_init(uint64_t addr, uint64_t size)`
Initialize the initrd subsystem with the physical address and size.

#### `int initrd_mount(void)`
Mount the initrd as the root filesystem. Parses the TAR archive and creates VFS entries.

Returns: 0 on success, -1 on error

#### `void* initrd_get_file(const char* path, uint64_t* size_out)`
Get a file from the initrd by path.

Returns: Pointer to file data, or NULL if not found

#### `void initrd_list_files(void)`
List all files in the initrd (for debugging).

#### `void initrd_get_stats(uint64_t* total_files, uint64_t* total_size)`
Get initrd statistics.

## Files Modified/Created

### New Files
- `scripts/mkinitrd.sh` - Initrd build script (enhanced)
- `scripts/build_mkinitrd.sh` - mkinitrd tool builder
- `scripts/test_initrd_build.sh` - Initrd test script
- `docs/INITRD_GUIDE.md` - This guide

### Modified Files
- `kernel/kernel.c` - Added multiboot module parsing
- `build/iso/boot/grub/grub.cfg` - Added initrd module line
- `scripts/build-iso.py` - Added initrd copying

### Existing Files (Already Implemented)
- `kernel/init/initrd.c` - TAR parser and initrd subsystem
- `kernel/include/initrd.h` - Initrd API
- `tools/mkinitrd.c` - TAR archive creator

## Troubleshooting

### Initrd not detected

**Symptoms**: Kernel says "No initrd detected"

**Solutions**:
1. Check GRUB config has `module /boot/initrd.img` line
2. Verify initrd.img exists in ISO: `build/iso/boot/initrd.img`
3. Rebuild ISO: `make iso`

### Invalid TAR format

**Symptoms**: "Invalid TAR header" messages

**Solutions**:
1. Ensure using `--format=ustar` with tar
2. Or use the custom mkinitrd tool
3. Verify with: `tar -tf build/initrd.img`

### Init not found

**Symptoms**: "ERROR: /sbin/init not found in initrd"

**Solutions**:
1. Build userspace: `make userspace`
2. Check init binary exists: `ls build/userspace/init/init`
3. Rebuild initrd: `make initrd`
4. Verify contents: `tar -tf build/initrd.img | grep init`

### Init fails to execute

**Symptoms**: Init loaded but crashes

**Solutions**:
1. Check init is a valid ELF binary: `file userspace/init/init`
2. Ensure cross-compiler built it: `x86_64-elf-gcc`
3. Check dependencies are in initrd (libc, etc.)
4. Enable debug output in kernel

## Performance

- **Initrd Size**: Typically 100-500 KB uncompressed
- **Mount Time**: < 10ms for typical initrd
- **Memory Overhead**: Initrd remains in memory (loaded by bootloader)
- **File Access**: Direct memory access (very fast)

## Future Enhancements

1. **Compression**: Add gzip/bzip2 decompression support
2. **CPIO Format**: Support CPIO in addition to TAR
3. **Hot Reload**: Allow updating initrd without reboot
4. **Overlay FS**: Support layered filesystems
5. **Signature Verification**: Verify initrd integrity

## References

- TAR Format: [POSIX.1-1988 ustar](https://pubs.opengroup.org/onlinepubs/9699919799/utilities/pax.html)
- Multiboot Specification: [GNU Multiboot](https://www.gnu.org/software/grub/manual/multiboot/multiboot.html)
- Linux initrd: [Kernel.org Documentation](https://www.kernel.org/doc/html/latest/admin-guide/initrd.html)

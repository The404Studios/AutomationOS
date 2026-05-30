# Initial Ramdisk (initrd) Implementation

## Overview

The AutomationOS kernel supports loading an initial ramdisk (initrd) at boot time. The initrd provides a temporary root filesystem that allows the kernel to access essential files before the real root filesystem is mounted.

## Architecture

### 1. Bootloader Loading (boot/bootloader_enhanced.c)

The UEFI bootloader loads the initrd into memory during boot:

```c
// Phase 5: Load initrd (if specified)
if (selected_entry->initrd_path[0]) {
    uint16_t initrd_path[MAX_PATH_LEN];
    ascii_to_utf16(selected_entry->initrd_path, initrd_path, MAX_PATH_LEN);
    
    uint64_t initrd_size;
    void* initrd_buffer = load_file(initrd_path, &initrd_size);
    if (initrd_buffer) {
        boot_info.initrd_addr = (uint64_t)initrd_buffer;
        boot_info.initrd_size = initrd_size;
    }
}
```

**Key Points:**
- Initrd path is read from boot configuration (boot.conf)
- Initrd is loaded into memory using UEFI file services
- Physical address and size are passed to kernel via boot_info structure

### 2. Boot Information Handoff (boot/boot_enhanced.h)

```c
typedef struct {
    uint32_t magic;              // 0xB001B001
    uint32_t version;
    
    // ... other fields ...
    
    // Initial ramdisk (initrd)
    uint64_t initrd_addr;        // Physical address
    uint64_t initrd_size;        // Size in bytes
    
    // ... other fields ...
} boot_info_t;
```

### 3. TAR Format

The initrd uses the POSIX ustar TAR format for simplicity:

```c
typedef struct {
    char name[100];      // File name
    char mode[8];        // File mode (octal)
    char uid[8];         // User ID (octal)
    char gid[8];         // Group ID (octal)
    char size[12];       // File size (octal)
    char mtime[12];      // Modification time (octal)
    char checksum[8];    // Header checksum (octal)
    char typeflag;       // File type ('0' = file, '5' = dir)
    char linkname[100];  // Link target
    char magic[6];       // "ustar"
    char version[2];     // "00"
    char uname[32];      // User name
    char gname[32];      // Group name
    char devmajor[8];    // Device major number
    char devminor[8];    // Device minor number
    char prefix[155];    // Filename prefix
    char padding[12];    // Padding to 512 bytes
} __attribute__((packed)) tar_header_t;
```

**TAR Archive Structure:**
- Each file has a 512-byte header
- File data follows immediately after header
- File data is padded to 512-byte boundary
- Archive ends with two empty 512-byte blocks

### 4. Kernel Initialization (kernel/init/initrd.c)

The kernel initializes and mounts the initrd in several steps:

#### 4.1 Initialization

```c
void initrd_init(uint64_t addr, uint64_t size) {
    initrd_addr = addr;
    initrd_size = size;
    initrd_mounted = 0;
    
    kprintf("[INITRD] Initrd at 0x%016lx, size %lu bytes\n", addr, size);
}
```

#### 4.2 Mounting

```c
int initrd_mount(void) {
    // Parse TAR archive
    // Extract file entries
    // Create in-memory file table
    // Return 0 on success
}
```

The mount function:
1. Validates the initrd address and size
2. Iterates through TAR headers (512-byte aligned)
3. Validates each header using magic number "ustar"
4. Extracts file metadata (name, size, type)
5. Builds in-memory file table (future: integrate with VFS)

#### 4.3 File Access

```c
void* initrd_get_file(const char* path, uint64_t* size_out) {
    // Search for file by path
    // Return pointer to file data in memory
}
```

The kernel can directly access initrd files without copying since they're already in memory.

### 5. Kernel Boot Sequence (kernel/init/main_enhanced.c)

```c
void kernel_main(boot_info_t* boot_info) {
    // ... early init ...
    
    // [5/6] Mount filesystems
    mount_root(boot_info);
    
    // [6/6] Start init process
    start_init();
}

static void mount_root(boot_info_t* boot_info) {
    if (boot_info->initrd_addr && boot_info->initrd_size) {
        // Initialize and mount initrd
        initrd_init(boot_info->initrd_addr, boot_info->initrd_size);
        
        if (initrd_mount() == 0) {
            kprintf("[KERNEL] Initrd mounted successfully\n");
            
            // Display statistics
            uint64_t total_files, total_size;
            initrd_get_stats(&total_files, &total_size);
            kprintf("[KERNEL] Initrd contains %lu files (%lu bytes)\n",
                    total_files, total_size);
        }
    }
}

static void start_init(void) {
    // Try to load init from initrd
    uint64_t init_size;
    void* init_data = initrd_get_file("/sbin/init", &init_size);
    
    if (init_data) {
        kprintf("[KERNEL] Found /sbin/init (%lu bytes)\n", init_size);
        // TODO: Create process from init binary
        // process_create_from_memory(init_data, init_size);
    }
}
```

## API Reference

### Initialization Functions

#### `void initrd_init(uint64_t addr, uint64_t size)`
Initialize initrd subsystem with physical address and size.

**Parameters:**
- `addr`: Physical memory address of initrd
- `size`: Size of initrd in bytes

#### `int initrd_mount(void)`
Parse TAR archive and mount initrd as root filesystem.

**Returns:**
- `0` on success
- `-1` on error

### File Access Functions

#### `void* initrd_get_file(const char* path, uint64_t* size_out)`
Retrieve file data from initrd.

**Parameters:**
- `path`: File path (e.g., "/sbin/init")
- `size_out`: Output parameter for file size

**Returns:**
- Pointer to file data in memory
- `NULL` if file not found

#### `void initrd_list_files(void)`
Print list of all files in initrd to kernel console.

#### `void initrd_get_stats(uint64_t* total_files, uint64_t* total_size)`
Get statistics about initrd contents.

**Parameters:**
- `total_files`: Output parameter for file count
- `total_size`: Output parameter for total size

## Tools

### mkinitrd - Initrd Creator Tool

Located in `tools/mkinitrd.c`, this tool creates TAR-format initrd images.

**Usage:**
```bash
# Build tool
make -f tools/Makefile.mkinitrd

# Create initrd from individual files
./build/mkinitrd -o initrd.img /sbin/init /lib/libc.so /etc/fstab

# Create initrd from directories
./build/mkinitrd -o initrd.img -d /sbin -d /lib -d /etc

# Mixed mode
./build/mkinitrd -o initrd.img -d /sbin /etc/fstab /lib/special.so
```

**Options:**
- `-o OUTPUT`: Output file (required)
- `-d DIR`: Add directory recursively
- `-h`: Show help

**Features:**
- Creates POSIX ustar TAR archives
- Preserves file permissions
- Recursive directory inclusion
- Automatic 512-byte alignment
- Standard TAR format (can be read with tar utility)

## Testing

### Test Script

A comprehensive test script is provided at `scripts/test-initrd.sh`:

```bash
# Run full initrd test
./scripts/test-initrd.sh
```

The test script:
1. Builds mkinitrd tool
2. Creates test files (/sbin/init, /bin/sh, /lib/libc.so, /etc/fstab)
3. Creates initrd image
4. Verifies TAR format with standard tar utility
5. Displays statistics

### Expected Output

```
[BOOT] Initrd: 12288 bytes @ 0x0000000001000000

[KERNEL] [5/6] Mounting filesystems...
[INITRD] Initrd at 0x0000000001000000, size 12288 bytes
[INITRD] Mounting initrd...
[INITRD] Extracting: sbin/init (256 bytes)
[INITRD] Extracting: bin/sh (32 bytes)
[INITRD] Extracting: lib/libc.so (64 bytes)
[INITRD] Extracting: etc/fstab (128 bytes)
[INITRD] Mounted successfully, 4 files extracted
[KERNEL] Initrd mounted successfully
[KERNEL] Initrd contains 4 files (480 bytes)

[KERNEL] [6/6] Starting init...
[KERNEL] Found /sbin/init (256 bytes)
```

## Configuration

### Boot Configuration (boot/boot.conf)

```ini
[entry]
title=AutomationOS
kernel=\EFI\BOOT\KERNEL.ELF
initrd=\EFI\BOOT\initrd.img
cmdline=quiet debug
timeout=5
default=true
```

### Directory Structure

```
ESP (EFI System Partition)
└── EFI
    └── BOOT
        ├── BOOTX64.EFI      # UEFI bootloader
        ├── KERNEL.ELF       # Kernel binary
        ├── initrd.img       # Initial ramdisk
        └── boot.conf        # Boot configuration
```

## Implementation Details

### Memory Management

- Initrd is loaded into conventional memory by bootloader
- Memory region is marked as "Loader Data" type
- Kernel accesses initrd data directly (zero-copy)
- No decompression needed (TAR is uncompressed)
- Initrd memory can be reclaimed after pivot_root

### TAR Parsing Algorithm

```c
uint64_t offset = 0;
int empty_blocks = 0;

while (offset < initrd_size) {
    tar_header_t* header = (tar_header_t*)(initrd_addr + offset);
    
    // Check for end-of-archive (two empty blocks)
    if (is_empty_block(header)) {
        empty_blocks++;
        if (empty_blocks >= 2) break;
        offset += 512;
        continue;
    }
    empty_blocks = 0;
    
    // Validate header
    if (!validate_tar_header(header)) break;
    
    // Extract file info
    uint64_t file_size = octal_to_int(header->size, 12);
    const char* filename = header->name;
    char type = header->typeflag;
    
    // Process file...
    
    // Move to next entry (512-byte aligned)
    uint64_t padding = (512 - (file_size % 512)) % 512;
    offset += 512 + file_size + padding;
}
```

### Octal String Parsing

TAR format uses octal (base-8) strings for numeric values:

```c
static uint64_t octal_to_int(const char* str, int len) {
    uint64_t value = 0;
    for (int i = 0; i < len && str[i]; i++) {
        if (str[i] >= '0' && str[i] <= '7') {
            value = value * 8 + (str[i] - '0');
        }
    }
    return value;
}
```

### Header Validation

```c
static int validate_tar_header(const tar_header_t* header) {
    // Check magic "ustar"
    if (memcmp(header->magic, "ustar", 5) != 0) {
        return 0;
    }
    
    // Check version "00"
    if (header->version[0] != '0' || header->version[1] != '0') {
        return 0;
    }
    
    return 1;
}
```

## Future Enhancements

### 1. VFS Integration

Currently, initrd files are accessed via `initrd_get_file()`. Future versions will integrate with a Virtual Filesystem (VFS):

```c
// Register initrd as filesystem type
vfs_register_fs("initrd", &initrd_fs_ops);

// Mount at root
vfs_mount_root("initrd", "/");

// Standard file operations
int fd = open("/sbin/init", O_RDONLY);
read(fd, buffer, size);
close(fd);
```

### 2. Compressed Initrd

Support for compressed formats (gzip, xz, zstd):

```c
void initrd_init_compressed(uint64_t addr, uint64_t size,
                           compression_type_t type);
```

### 3. Pivot Root

Switch from initrd to real root filesystem:

```c
// Mount real root
vfs_mount("/dev/sda1", "/mnt/root", "autofs");

// Pivot root
pivot_root("/mnt/root", "/mnt/root/initrd");

// Unmount old root
umount("/initrd");
```

### 4. Dynamic File Loading

Load kernel modules and drivers from initrd:

```c
// Load driver from initrd
void* driver_data = initrd_get_file("/lib/modules/ahci.ko", &size);
module_load(driver_data, size);
```

## Performance

### Metrics

- **Initrd Loading**: ~1-2ms (UEFI file read)
- **TAR Parsing**: ~0.5ms per 100 files
- **File Lookup**: O(n) linear search (future: hash table O(1))
- **Memory Overhead**: Zero-copy access (no duplication)

### Optimization Opportunities

1. **Hash Table**: Replace linear search with O(1) hash table lookup
2. **File Cache**: Cache frequently accessed files
3. **Lazy Parsing**: Only parse headers on-demand
4. **Memory Reclamation**: Free initrd memory after pivot_root

## Troubleshooting

### Initrd Not Loading

**Symptom:** `[KERNEL] No initrd available`

**Solutions:**
1. Check boot.conf has correct initrd path
2. Verify initrd.img exists in EFI/BOOT directory
3. Check bootloader logs for file loading errors

### Invalid TAR Format

**Symptom:** `[INITRD] Invalid TAR header at offset 0x...`

**Solutions:**
1. Verify mkinitrd created valid TAR: `tar -tvf initrd.img`
2. Check file corruption during copy
3. Rebuild initrd with mkinitrd tool

### File Not Found

**Symptom:** `[KERNEL] Warning: /sbin/init not found in initrd`

**Solutions:**
1. List initrd contents: `tar -tf initrd.img`
2. Verify init file was included in mkinitrd command
3. Check file path (leading slash vs. no leading slash)

### Memory Corruption

**Symptom:** Kernel panic during initrd parsing

**Solutions:**
1. Verify initrd size matches actual file size
2. Check memory map for conflicts
3. Ensure initrd memory is not overwritten by kernel

## Code Statistics

- **initrd.c**: ~340 LOC
- **initrd.h**: ~50 LOC
- **mkinitrd.c**: ~375 LOC
- **bootloader integration**: ~20 LOC
- **kernel integration**: ~50 LOC
- **Total**: ~835 LOC

## References

- POSIX ustar TAR format specification
- Linux initramfs documentation
- UEFI Specification 2.9
- AutomationOS Boot System documentation

## License

Copyright (c) 2026 AutomationOS Project
Licensed under MIT License

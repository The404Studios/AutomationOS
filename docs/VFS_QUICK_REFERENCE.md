# VFS Quick Reference

## Include
```c
#include "include/vfs.h"
```

## Common Operations

### Open File
```c
int fd = vfs_open("/path/to/file", O_RDONLY, 0);
// Returns: fd >= 0 on success, -1 on error
```

### Read File
```c
char buf[256];
ssize_t n = vfs_read(fd, buf, sizeof(buf));
// Returns: bytes read (>= 0), -1 on error, 0 on EOF
```

### Write File
```c
const char* data = "Hello";
ssize_t n = vfs_write(fd, data, strlen(data));
// Returns: bytes written, -1 on error
```

### Close File
```c
vfs_close(fd);
// Returns: 0 on success, -1 on error
```

### Seek in File
```c
off_t pos = vfs_lseek(fd, 0, SEEK_SET);  // Beginning
off_t pos = vfs_lseek(fd, 10, SEEK_CUR); // Forward 10 bytes
off_t pos = vfs_lseek(fd, 0, SEEK_END);  // End of file
```

### Get File Info
```c
vfs_stat_t st;
vfs_stat("/path/to/file", &st);
// or
vfs_fstat(fd, &st);

// Access: st.st_size, st.st_ino, st.st_mode, etc.
```

### Create Directory
```c
vfs_mkdir("/mydir", 0755);
// or recursively:
vfs_mkdir_recursive("/path/to/mydir", 0755);
```

## Flags (for vfs_open)

| Flag | Value | Description |
|------|-------|-------------|
| O_RDONLY | 0x0000 | Read only |
| O_WRONLY | 0x0001 | Write only |
| O_RDWR | 0x0002 | Read and write |
| O_CREAT | 0x0040 | Create if doesn't exist |
| O_TRUNC | 0x0200 | Truncate to zero length |
| O_APPEND | 0x0400 | Append mode |
| O_EXCL | 0x0080 | Fail if exists (with O_CREAT) |

## Seek Modes

| Mode | Value | Description |
|------|-------|-------------|
| SEEK_SET | 0 | Absolute position |
| SEEK_CUR | 1 | Relative to current |
| SEEK_END | 2 | Relative to end |

## Creating Files Programmatically (RAMFS)

```c
// Get root directory
vfs_inode_t* root = vfs_path_lookup("/");

// Create file with data
const char* data = "File contents";
vfs_ramfs_create_file(root, "file.txt", data, strlen(data), 0644);

// Create directory
vfs_ramfs_create_dir(root, "mydir", 0755);

// Release reference
vfs_inode_put(root);
```

## Error Codes

Functions return -1 on error. Common causes:
- File not found
- Invalid file descriptor
- Out of memory
- Invalid arguments

## Standard File Descriptors

| FD | Purpose |
|----|---------|
| 0 | stdin (handled by PS/2 keyboard) |
| 1 | stdout (serial console) |
| 2 | stderr (serial console) |
| 3+ | Open files |

## Complete Example: Read Config File

```c
// Open config file
int fd = vfs_open("/etc/config.txt", O_RDONLY, 0);
if (fd < 0) {
    kprintf("Failed to open config\n");
    return;
}

// Get file size
vfs_stat_t st;
if (vfs_fstat(fd, &st) < 0) {
    vfs_close(fd);
    return;
}

// Allocate buffer
char* buf = kmalloc(st.st_size + 1);
if (!buf) {
    vfs_close(fd);
    return;
}

// Read entire file
ssize_t n = vfs_read(fd, buf, st.st_size);
if (n != (ssize_t)st.st_size) {
    kfree(buf);
    vfs_close(fd);
    return;
}

buf[n] = '\0';

// Process config...
kprintf("Config: %s\n", buf);

// Cleanup
kfree(buf);
vfs_close(fd);
```

## Complete Example: Write Log File

```c
// Open log file (create if needed)
int fd = vfs_open("/var/log/kernel.log", 
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
if (fd < 0) {
    kprintf("Failed to open log\n");
    return;
}

// Write log entry
const char* entry = "[KERNEL] System started\n";
ssize_t n = vfs_write(fd, entry, strlen(entry));
if (n < 0) {
    kprintf("Failed to write log\n");
}

// Close
vfs_close(fd);
```

## Initialization (in kernel_main)

```c
// Initialize VFS
vfs_init();

// Mount root filesystem
vfs_mount("none", "/", "ramfs");

// Now ready to use!
```

## See Also

- `docs/VFS_ARCHITECTURE.md` - Complete architecture
- `docs/VFS_INTEGRATION.md` - Integration guide
- `kernel/tests/test_vfs.c` - Test examples
- `kernel/include/vfs.h` - Full API reference

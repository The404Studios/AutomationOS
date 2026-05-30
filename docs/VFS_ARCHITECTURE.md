# VFS Architecture

## Overview

The Virtual File System (VFS) provides a unified interface for file operations across different filesystem types. It abstracts filesystem-specific details and provides a common API for:

- File operations (open, read, write, close)
- Directory operations (mkdir, rmdir)
- File metadata (stat, fstat)
- Mount/unmount operations

## Architecture

```
┌─────────────────────────────────────────────┐
│         User Space Applications             │
│  (syscalls: open, read, write, close, etc.) │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────▼──────────────────────────┐
│           System Call Layer                 │
│   (sys_open, sys_read, sys_write, etc.)     │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────▼──────────────────────────┐
│              VFS Layer                      │
│  ┌────────────────────────────────────┐    │
│  │  File Descriptor Table             │    │
│  │  - fd 0: stdin                     │    │
│  │  - fd 1: stdout                    │    │
│  │  - fd 2: stderr                    │    │
│  │  - fd 3+: open files               │    │
│  └────────────────────────────────────┘    │
│                                             │
│  ┌────────────────────────────────────┐    │
│  │  Inode Cache                       │    │
│  │  - In-memory inode objects         │    │
│  │  - Reference counting              │    │
│  └────────────────────────────────────┘    │
│                                             │
│  ┌────────────────────────────────────┐    │
│  │  Dentry Cache                      │    │
│  │  - Directory entry cache           │    │
│  │  - Path lookup optimization        │    │
│  └────────────────────────────────────┘    │
│                                             │
│  ┌────────────────────────────────────┐    │
│  │  Mount Table                       │    │
│  │  - Mounted filesystems             │    │
│  │  - Mount points                    │    │
│  └────────────────────────────────────┘    │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────▼──────────────────────────┐
│        Filesystem Implementations           │
│  ┌────────────┐  ┌────────────┐            │
│  │   RAMFS    │  │   AUTOFS   │  ...       │
│  │  (in-mem)  │  │  (native)  │            │
│  └────────────┘  └────────────┘            │
└─────────────────────────────────────────────┘
```

## Key Data Structures

### vfs_inode_t
Represents a filesystem object (file, directory, etc.)

```c
struct vfs_inode {
    uint64_t ino;           // Inode number
    uint32_t mode;          // Permissions
    uint32_t type;          // File type (FILE, DIR, etc.)
    uint64_t size;          // File size
    void* data;             // File data (for ramfs)
    vfs_inode_ops_t* ops;   // Inode operations
    uint32_t ref_count;     // Reference count
};
```

### vfs_dentry_t
Directory entry - maps name to inode

```c
struct vfs_dentry {
    char name[VFS_MAX_NAME];
    vfs_inode_t* inode;
    vfs_dentry_t* parent;
    uint32_t ref_count;
};
```

### vfs_file_t
Open file descriptor

```c
struct vfs_file {
    vfs_inode_t* inode;
    uint64_t offset;        // Current position
    uint32_t flags;         // Open flags
    vfs_file_ops_t* ops;    // File operations
};
```

### vfs_superblock_t
Mounted filesystem

```c
struct vfs_superblock {
    const char* type;       // Filesystem type
    vfs_inode_t* root;      // Root inode
    void* private_data;     // FS-specific data
};
```

## API Documentation

### Core Functions

#### vfs_init()
Initialize the VFS subsystem. Must be called before any other VFS operations.

```c
void vfs_init(void);
```

#### vfs_mount()
Mount a filesystem.

```c
int vfs_mount(const char* source, const char* target, const char* fstype);
```

Parameters:
- `source`: Source device (not used for ramfs)
- `target`: Mount point (e.g., "/")
- `fstype`: Filesystem type (currently only "ramfs")

Returns: 0 on success, -1 on error

### File Operations

#### vfs_open()
Open a file.

```c
int vfs_open(const char* path, int flags, int mode);
```

Flags:
- `O_RDONLY`: Read only
- `O_WRONLY`: Write only
- `O_RDWR`: Read/write
- `O_CREAT`: Create if doesn't exist
- `O_TRUNC`: Truncate to zero length

Returns: File descriptor (>= 0) on success, -1 on error

#### vfs_read()
Read from a file.

```c
ssize_t vfs_read(int fd, void* buf, size_t count);
```

Returns: Number of bytes read, -1 on error, 0 on EOF

#### vfs_write()
Write to a file.

```c
ssize_t vfs_write(int fd, const void* buf, size_t count);
```

Returns: Number of bytes written, -1 on error

#### vfs_close()
Close a file.

```c
int vfs_close(int fd);
```

Returns: 0 on success, -1 on error

#### vfs_lseek()
Change file position.

```c
off_t vfs_lseek(int fd, off_t offset, int whence);
```

Whence values:
- `SEEK_SET`: Absolute position
- `SEEK_CUR`: Relative to current position
- `SEEK_END`: Relative to end of file

### File Information

#### vfs_stat()
Get file status by path.

```c
int vfs_stat(const char* path, vfs_stat_t* buf);
```

#### vfs_fstat()
Get file status by descriptor.

```c
int vfs_fstat(int fd, vfs_stat_t* buf);
```

### Directory Operations

#### vfs_mkdir()
Create a directory.

```c
int vfs_mkdir(const char* path, uint32_t mode);
```

#### vfs_mkdir_recursive()
Create directory and all parent directories.

```c
int vfs_mkdir_recursive(const char* path, uint32_t mode);
```

## RAMFS Implementation

RAMFS is a simple in-memory filesystem used for:
- Initial ramdisk (initrd) mounting
- Temporary files
- Testing

### Features
- All data stored in kernel memory
- No persistence
- Simple and fast
- No block device required

### Limitations
- No advanced features (symlinks, hardlinks, etc.)
- Fixed directory entry arrays
- Memory-limited

## Integration Points

### 1. Kernel Initialization
```c
// In kernel_main()
vfs_init();
vfs_mount("none", "/", "ramfs");
```

### 2. System Calls
```c
// sys_open
int fd = vfs_open(path, flags, mode);

// sys_read
ssize_t n = vfs_read(fd, buf, count);

// sys_write
ssize_t n = vfs_write(fd, buf, count);

// sys_close
vfs_close(fd);
```

### 3. Initrd Integration
```c
// Load files from initrd into VFS
vfs_inode_t* root = vfs_path_lookup("/");
vfs_ramfs_create_file(root, "init", data, size, 0755);
```

## Future Extensions

### Additional Filesystem Types
- AutoFS (native filesystem)
- FAT32 (for compatibility)
- ISO9660 (for CD-ROM)

### Advanced Features
- Symbolic links
- Hard links
- File permissions enforcement
- Access time tracking
- Directory iteration
- File locking

### Performance Optimizations
- LRU dentry cache eviction
- Read-ahead
- Write-back caching
- Directory entry hashtables

## Testing

See `kernel/tests/test_vfs.c` for VFS test suite.

Tests cover:
1. Initialization
2. Mounting
3. File read/write
4. File stat
5. Directory operations

Run tests:
```c
test_vfs_run_all();
```

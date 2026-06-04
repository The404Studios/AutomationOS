/**
 * Virtual File System (VFS) Layer
 *
 * Provides unified interface for filesystem operations
 */

#ifndef VFS_H
#define VFS_H

#include "types.h"

// File types
#define VFS_TYPE_FILE       0x01
#define VFS_TYPE_DIR        0x02
#define VFS_TYPE_SYMLINK    0x04
#define VFS_TYPE_DEVICE     0x08

// Memory ownership flags for inode data
#define VFS_DATA_OWNED        0x01  // data was kmalloc'd, can free
#define VFS_DATA_INITRD_BACKED 0x02  // data points into initrd, don't free
#define VFS_PATH_OWNED        0x04  // path was kmalloc'd, can free

// File flags (for open)
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_EXCL      0x0080

// Seek modes
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

// Maximum values
#define VFS_MAX_FDS         1024
#define VFS_MAX_PATH        4096
#define VFS_MAX_NAME        256
#define VFS_MAX_MOUNTS      32
#define VFS_INODE_CACHE_SIZE 256
#define VFS_DENTRY_CACHE_SIZE 512

// Read-ahead configuration
#define VFS_READAHEAD_PAGES     4       // Number of pages to prefetch
#define VFS_READAHEAD_THRESHOLD 2       // Sequential reads needed to trigger
#define VFS_PAGE_SIZE           4096    // Page size for read-ahead

// dirent constants (compatible with POSIX)
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12
#define DT_WHT      14
#define NAME_MAX    256

// VFS structures forward declarations
typedef struct vfs_inode vfs_inode_t;
typedef struct vfs_dentry vfs_dentry_t;
typedef struct vfs_file vfs_file_t;
typedef struct vfs_superblock vfs_superblock_t;
typedef struct vfs_file_operations vfs_file_ops_t;
typedef struct vfs_inode_operations vfs_inode_ops_t;

/**
 * Inode - represents a file system object
 */
struct vfs_inode {
    uint64_t ino;               // Inode number
    uint32_t mode;              // Permissions and type
    uint32_t uid;               // User ID
    uint32_t gid;               // Group ID
    uint64_t size;              // File size
    uint64_t blocks;            // Number of blocks allocated
    uint64_t atime;             // Access time
    uint64_t mtime;             // Modification time
    uint64_t ctime;             // Change time
    uint32_t nlink;             // Number of hard links
    uint32_t type;              // File type (VFS_TYPE_*)

    void* private_data;         // Filesystem-specific data
    vfs_superblock_t* sb;       // Superblock
    vfs_inode_ops_t* ops;       // Inode operations

    uint32_t ref_count;         // Reference count
    uint32_t flags;             // Inode flags (VFS_DATA_OWNED, VFS_DATA_INITRD_BACKED, etc.)

    // Simple in-memory data storage for initial implementation
    void* data;                 // File data (for simple implementation)
    uint64_t data_capacity;     // Allocated capacity
};

/**
 * Directory entry - maps name to inode
 */
struct vfs_dentry {
    char name[VFS_MAX_NAME];    // Entry name
    vfs_inode_t* inode;         // Associated inode
    vfs_dentry_t* parent;       // Parent directory

    // Cache management
    vfs_dentry_t* next;         // Next in hash chain
    vfs_dentry_t* prev;         // Previous in hash chain
    uint32_t hash;              // Hash value
    uint32_t ref_count;         // Reference count
    uint64_t cache_time;        // Time cached
};

/**
 * Open file descriptor
 */
struct vfs_file {
    vfs_inode_t* inode;         // Associated inode
    vfs_dentry_t* dentry;       // Associated dentry
    uint64_t offset;            // Current file offset
    uint32_t flags;             // Open flags
    uint32_t mode;              // Open mode
    vfs_file_ops_t* ops;        // File operations
    void* private_data;         // Filesystem-specific data
    uint32_t ref_count;         // Reference count

    // Read-ahead tracking
    uint64_t ra_last_offset;    // Last read offset (for sequential detection)
    uint64_t ra_window;         // Read-ahead window size (in pages)
    uint32_t ra_sequential;     // Sequential access counter
};

/**
 * Superblock - represents a mounted filesystem
 */
struct vfs_superblock {
    uint64_t magic;             // Filesystem magic number
    uint64_t blocksize;         // Block size
    uint64_t maxbytes;          // Max file size
    const char* type;           // Filesystem type name

    vfs_inode_t* root;          // Root inode
    void* private_data;         // Filesystem-specific data

    // Superblock operations
    vfs_inode_t* (*alloc_inode)(vfs_superblock_t* sb);
    void (*destroy_inode)(vfs_inode_t* inode);
    int (*write_inode)(vfs_inode_t* inode);
    void (*sync_fs)(vfs_superblock_t* sb);
};

/**
 * File operations
 */
struct vfs_file_operations {
    ssize_t (*read)(vfs_file_t* file, void* buf, size_t count);
    ssize_t (*write)(vfs_file_t* file, const void* buf, size_t count);
    int (*open)(vfs_inode_t* inode, vfs_file_t* file);
    int (*close)(vfs_file_t* file);
    off_t (*lseek)(vfs_file_t* file, off_t offset, int whence);
};

/**
 * Inode operations
 */
struct vfs_inode_operations {
    vfs_dentry_t* (*lookup)(vfs_inode_t* dir, const char* name);
    int (*create)(vfs_inode_t* dir, vfs_dentry_t* dentry, uint32_t mode);
    int (*mkdir)(vfs_inode_t* dir, vfs_dentry_t* dentry, uint32_t mode);
    int (*unlink)(vfs_inode_t* dir, vfs_dentry_t* dentry);
    int (*rmdir)(vfs_inode_t* dir, vfs_dentry_t* dentry);
};

/**
 * File stat structure (compatible with POSIX)
 */
typedef struct {
    uint64_t st_dev;            // Device ID
    uint64_t st_ino;            // Inode number
    uint32_t st_mode;           // File mode
    uint32_t st_nlink;          // Number of hard links
    uint32_t st_uid;            // User ID
    uint32_t st_gid;            // Group ID
    uint64_t st_rdev;           // Device ID (if special file)
    uint64_t st_size;           // Total size in bytes
    uint64_t st_blksize;        // Block size for filesystem I/O
    uint64_t st_blocks;         // Number of 512B blocks allocated
    uint64_t st_atime;          // Time of last access
    uint64_t st_mtime;          // Time of last modification
    uint64_t st_ctime;          // Time of last status change
} vfs_stat_t;

/**
 * Directory entry structure (compatible with POSIX)
 */
struct dirent {
    uint64_t d_ino;             // Inode number
    int64_t d_off;              // Offset to next dirent
    uint16_t d_reclen;          // Length of this record
    uint8_t d_type;             // Type of file (DT_*)
    char d_name[NAME_MAX];      // Null-terminated filename
};

// Core VFS functions
void vfs_init(void);
int vfs_mount(const char* source, const char* target, const char* fstype);
int vfs_unmount(const char* target);

// Post-mount setup: creates standard writable dirs (/tmp, /var/tmp, /run).
// Call once after vfs_mount("/", ...) + initrd_mount().
void vfs_fs_init(void);

// File operations
int vfs_open(const char* path, int flags, int mode);
ssize_t vfs_read(int fd, void* buf, size_t count);
ssize_t vfs_write(int fd, const void* buf, size_t count);
int vfs_close(int fd);
off_t vfs_lseek(int fd, off_t offset, int whence);
int vfs_truncate(const char* path, off_t length);
int vfs_ftruncate(int fd, off_t length);
int vfs_fsync(int fd);
int vfs_sync(void);

// File information
int vfs_stat(const char* path, vfs_stat_t* buf);
int vfs_fstat(int fd, vfs_stat_t* buf);

// Directory operations
int vfs_mkdir(const char* path, uint32_t mode);
int vfs_mkdir_recursive(const char* path, uint32_t mode);
int vfs_rmdir(const char* path);

// Directory iteration operations (vfs_dir.c)
int vfs_opendir(const char* path);
int vfs_readdir(int dirfd, struct dirent* entry);
int vfs_closedir(int dirfd);
int vfs_unlink(const char* path);
int vfs_rename(const char* oldpath, const char* newpath);

// Path operations
vfs_inode_t* vfs_path_lookup(const char* path);
vfs_dentry_t* vfs_dentry_lookup(vfs_inode_t* dir, const char* name);

// Inode operations
vfs_inode_t* vfs_inode_alloc(vfs_superblock_t* sb);
void vfs_inode_free(vfs_inode_t* inode);
void vfs_inode_get(vfs_inode_t* inode);
void vfs_inode_put(vfs_inode_t* inode);

// Dentry cache operations
vfs_dentry_t* vfs_dentry_alloc(const char* name);
void vfs_dentry_free(vfs_dentry_t* dentry);
void vfs_dentry_add_child(vfs_dentry_t* parent, vfs_dentry_t* child);

// File descriptor table operations
int vfs_fd_alloc(void);
void vfs_fd_free(int fd);
vfs_file_t* vfs_fd_get(int fd);

/* Close all of a process's open fds at teardown (per-process fd table cleanup).
 * struct process is forward-declared to avoid pulling sched.h into vfs.h. */
struct process;
void vfs_close_all_fds(struct process* proc);

// Simple in-memory filesystem (for initrd)
vfs_superblock_t* vfs_create_ramfs(void);
int vfs_ramfs_create_file(vfs_inode_t* dir, const char* name,
                          const void* data, size_t size, uint32_t mode);
int vfs_ramfs_create_file_initrd(vfs_inode_t* dir, const char* name,
                                  const void* data, size_t size, uint32_t mode);
int vfs_ramfs_create_dir(vfs_inode_t* dir, const char* name, uint32_t mode);

// VFS error codes
#define VFS_ERR_INVAL   -22  // Invalid argument
#define VFS_ERR_NOMEM   -12  // Out of memory
#define VFS_ERR_NOENT   -2   // No such file or directory
#define VFS_ERR_NFILE   -23  // File table overflow
#define VFS_ERR_BADF    -9   // Bad file descriptor
#define VFS_ERR_NOSYS   -38  // Function not implemented
#define VFS_ERR_ISDIR   -21  // Is a directory
#define VFS_ERR_NOTDIR  -20  // Not a directory
#define VFS_ERR_ACCES   -13  // Permission denied

#endif // VFS_H

/**
 * @file autofs_kernel.h
 * @brief Kernel-Mode AutoFS Filesystem Interface
 *
 * AutoFS kernel filesystem with journaling, designed for the AutomationOS kernel.
 * This header defines the kernel-mode interface (no POSIX dependencies).
 */

#ifndef AUTOFS_KERNEL_H
#define AUTOFS_KERNEL_H

#include "types.h"
#include "block.h"
#include "vfs.h"

/* AutoFS constants */
#define AUTOFS_MAGIC            0x0000000000AUT0F5ULL
#define AUTOFS_VERSION          1
#define AUTOFS_BLOCK_SIZE       4096
#define AUTOFS_MAX_NAME_LEN     255
#define AUTOFS_MAX_PATH_LEN     4096
#define AUTOFS_DIRECT_BLOCKS    12

/* Feature flags */
#define AUTOFS_FEATURE_JOURNAL      (1 << 0)
#define AUTOFS_FEATURE_COW          (1 << 1)
#define AUTOFS_FEATURE_COMPRESSION  (1 << 2)
#define AUTOFS_FEATURE_ENCRYPTION   (1 << 3)
#define AUTOFS_FEATURE_SNAPSHOTS    (1 << 4)
#define AUTOFS_FEATURE_XATTR        (1 << 5)

/* Inode flags */
#define AUTOFS_INODE_COMPRESSED     (1 << 0)
#define AUTOFS_INODE_ENCRYPTED      (1 << 1)
#define AUTOFS_INODE_IMMUTABLE      (1 << 2)
#define AUTOFS_INODE_APPEND_ONLY    (1 << 3)
#define AUTOFS_INODE_NO_ATIME       (1 << 4)

/* File types */
typedef enum {
    AUTOFS_TYPE_UNKNOWN = 0,
    AUTOFS_TYPE_FILE    = 1,
    AUTOFS_TYPE_DIR     = 2,
    AUTOFS_TYPE_SYMLINK = 3,
    AUTOFS_TYPE_DEVICE  = 4,
    AUTOFS_TYPE_FIFO    = 5,
    AUTOFS_TYPE_SOCKET  = 6
} autofs_file_type_t;

/* Journal operations */
typedef enum {
    JOURNAL_OP_WRITE = 1,
    JOURNAL_OP_UNLINK = 2,
    JOURNAL_OP_MKDIR = 3,
    JOURNAL_OP_RMDIR = 4,
    JOURNAL_OP_RENAME = 5,
    JOURNAL_OP_TRUNCATE = 6,
    JOURNAL_OP_SETATTR = 7
} journal_op_t;

/**
 * @brief Superblock - First block of filesystem (4096 bytes)
 */
typedef struct autofs_superblock {
    uint64_t magic;              /* 0xAUT0F5 */
    uint64_t version;            /* Filesystem version */
    uint64_t block_size;         /* Block size (4096) */
    uint64_t block_count;        /* Total blocks */
    uint64_t free_blocks;        /* Free blocks */
    uint64_t inode_count;        /* Total inodes */
    uint64_t free_inodes;        /* Free inodes */
    uint64_t root_inode;         /* Root directory inode number */
    uint64_t journal_start;      /* Journal start block */
    uint64_t journal_size;       /* Journal size in blocks */
    uint64_t features;           /* Feature flags */

    /* Block allocation bitmap */
    uint64_t block_bitmap_start;
    uint64_t block_bitmap_blocks;

    /* Inode allocation bitmap */
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;

    /* Inode table */
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;

    /* Data blocks */
    uint64_t data_start;

    /* Snapshot info (reserved) */
    uint64_t snapshot_count;
    uint64_t snapshot_list_block;

    /* Mount info */
    uint64_t mount_time;
    uint64_t write_time;
    uint32_t mount_count;
    uint32_t max_mount_count;

    /* UUID */
    uint8_t uuid[16];

    /* Label */
    char label[64];

    /* Compression default (reserved) */
    uint32_t default_compress_algo;

    /* Reserved for future use */
    uint8_t reserved[3704];
} __attribute__((packed)) autofs_superblock_t;

_Static_assert(sizeof(autofs_superblock_t) == 4096, "Superblock must be exactly 4096 bytes");

/**
 * @brief Inode - File metadata (256 bytes)
 */
typedef struct autofs_inode {
    uint64_t ino;                /* Inode number */
    uint32_t mode;               /* File mode and permissions */
    uint32_t uid;                /* Owner user ID */
    uint32_t gid;                /* Owner group ID */

    uint64_t size;               /* File size in bytes */
    uint64_t blocks;             /* Number of 512-byte blocks allocated */

    uint64_t atime;              /* Access time */
    uint64_t mtime;              /* Modification time */
    uint64_t ctime;              /* Change time */
    uint64_t crtime;             /* Creation time */

    uint32_t links_count;        /* Hard link count */
    uint32_t flags;              /* Inode flags */

    autofs_file_type_t type;     /* File type */
    uint32_t generation;         /* File version (for NFS) */

    /* Block pointers */
    uint64_t direct[AUTOFS_DIRECT_BLOCKS];  /* Direct blocks */
    uint64_t indirect;           /* Single indirect block */
    uint64_t double_indirect;    /* Double indirect block */
    uint64_t triple_indirect;    /* Triple indirect block */

    /* Extended attributes (reserved) */
    uint64_t xattr_block;

    /* Compression info (reserved) */
    uint32_t compressed_size;
    uint32_t compress_algo;

    /* Refcount for CoW (reserved) */
    uint32_t refcount;

    /* Reserved */
    uint8_t reserved[128];
} __attribute__((packed)) autofs_inode_t;

_Static_assert(sizeof(autofs_inode_t) == 256, "Inode must be exactly 256 bytes");

/**
 * @brief Directory entry
 */
typedef struct autofs_dirent {
    uint64_t ino;                /* Inode number (0 = deleted entry) */
    uint16_t rec_len;            /* Record length */
    uint8_t name_len;            /* Name length */
    uint8_t file_type;           /* File type */
    char name[AUTOFS_MAX_NAME_LEN + 1];  /* Filename */
} __attribute__((packed)) autofs_dirent_t;

/**
 * @brief Journal transaction header
 */
typedef struct journal_transaction {
    uint64_t transaction_id;     /* Transaction ID */
    uint64_t timestamp;          /* Transaction timestamp */
    uint32_t entry_count;        /* Number of entries */
    uint32_t state;              /* Transaction state */
    uint64_t checksum;           /* Transaction checksum */
} __attribute__((packed)) journal_transaction_t;

/**
 * @brief Journal entry
 */
typedef struct journal_entry {
    uint64_t transaction_id;     /* Parent transaction ID */
    journal_op_t operation;      /* Operation type */
    uint64_t block_num;          /* Block number */
    uint8_t data[AUTOFS_BLOCK_SIZE];  /* Block data */
    uint64_t checksum;           /* Entry checksum */
} __attribute__((packed)) journal_entry_t;

/**
 * @brief In-memory filesystem structure (kernel mode)
 */
typedef struct autofs_fs {
    block_device_t* block_dev;   /* Block device */
    autofs_superblock_t* sb;     /* Superblock */

    /* Bitmaps */
    uint8_t* block_bitmap;
    uint8_t* inode_bitmap;

    /* Journal */
    void* journal_buffer;
    uint64_t current_transaction_id;

    /* Mount info */
    bool read_only;
    char device_name[32];

    /* VFS integration */
    vfs_superblock_t* vfs_sb;

    /* Statistics */
    struct {
        uint64_t reads;
        uint64_t writes;
        uint64_t cache_hits;
        uint64_t cache_misses;
    } stats;
} autofs_fs_t;

/* ========== Core Disk I/O Operations ========== */

/**
 * @brief Initialize disk I/O for filesystem
 */
int autofs_disk_init(autofs_fs_t* fs, block_device_t* bdev);

/**
 * @brief Read a filesystem block
 * @param fs Filesystem
 * @param block_num Block number (filesystem blocks, not device sectors)
 * @param buffer Buffer to read into (must be AUTOFS_BLOCK_SIZE bytes)
 * @return 0 on success, -1 on error
 */
int autofs_disk_read_block(autofs_fs_t* fs, uint64_t block_num, void* buffer);

/**
 * @brief Write a filesystem block
 */
int autofs_disk_write_block(autofs_fs_t* fs, uint64_t block_num, const void* buffer);

/**
 * @brief Sync filesystem to disk
 */
int autofs_disk_sync(autofs_fs_t* fs);

/* ========== Superblock Operations ========== */

/**
 * @brief Read and validate superblock
 */
int autofs_read_superblock(autofs_fs_t* fs);

/**
 * @brief Write superblock to disk
 */
int autofs_write_superblock(autofs_fs_t* fs);

/**
 * @brief Validate superblock magic and version
 */
int autofs_validate_superblock(autofs_superblock_t* sb);

/* ========== Inode Operations ========== */

/**
 * @brief Read inode from disk
 */
autofs_inode_t* autofs_inode_read(autofs_fs_t* fs, uint64_t ino);

/**
 * @brief Write inode to disk
 */
int autofs_inode_write(autofs_fs_t* fs, autofs_inode_t* inode);

/**
 * @brief Allocate a new inode
 * @return Inode number or 0 on error
 */
uint64_t autofs_inode_alloc(autofs_fs_t* fs);

/**
 * @brief Free an inode
 */
int autofs_inode_free(autofs_fs_t* fs, uint64_t ino);

/* ========== Block Allocation ========== */

/**
 * @brief Allocate a data block
 * @return Block number or 0 on error
 */
uint64_t autofs_block_alloc(autofs_fs_t* fs);

/**
 * @brief Free a data block
 */
int autofs_block_free(autofs_fs_t* fs, uint64_t block_num);

/**
 * @brief Load allocation bitmaps from disk
 */
int autofs_load_bitmaps(autofs_fs_t* fs);

/**
 * @brief Save allocation bitmaps to disk
 */
int autofs_save_bitmaps(autofs_fs_t* fs);

/* ========== Journal Operations ========== */

/**
 * @brief Initialize journal
 */
int autofs_journal_init(autofs_fs_t* fs);

/**
 * @brief Begin a new transaction
 */
int autofs_journal_begin(autofs_fs_t* fs);

/**
 * @brief Log a block write to journal
 */
int autofs_journal_log_block(autofs_fs_t* fs, uint64_t block_num, const void* data);

/**
 * @brief Commit current transaction
 */
int autofs_journal_commit(autofs_fs_t* fs);

/**
 * @brief Abort current transaction
 */
int autofs_journal_abort(autofs_fs_t* fs);

/**
 * @brief Recover filesystem from journal after crash
 */
int autofs_journal_recover(autofs_fs_t* fs);

/* ========== File Operations ========== */

/**
 * @brief Read from file
 */
ssize_t autofs_file_read(autofs_fs_t* fs, autofs_inode_t* inode,
                         uint64_t offset, void* buffer, size_t count);

/**
 * @brief Write to file
 */
ssize_t autofs_file_write(autofs_fs_t* fs, autofs_inode_t* inode,
                          uint64_t offset, const void* buffer, size_t count);

/**
 * @brief Truncate file to specified size
 */
int autofs_file_truncate(autofs_fs_t* fs, autofs_inode_t* inode, uint64_t new_size);

/**
 * @brief Get block number for file offset
 * @param allocate If true, allocate block if not present
 * @return Block number or 0 on error
 */
uint64_t autofs_file_get_block(autofs_fs_t* fs, autofs_inode_t* inode,
                                uint64_t block_index, bool allocate);

/* ========== Directory Operations ========== */

/**
 * @brief Lookup entry in directory
 * @return Inode number or 0 if not found
 */
uint64_t autofs_dir_lookup(autofs_fs_t* fs, autofs_inode_t* dir_inode, const char* name);

/**
 * @brief Add entry to directory
 */
int autofs_dir_add_entry(autofs_fs_t* fs, autofs_inode_t* dir_inode,
                         const char* name, uint64_t ino, uint8_t file_type);

/**
 * @brief Remove entry from directory
 */
int autofs_dir_remove_entry(autofs_fs_t* fs, autofs_inode_t* dir_inode, const char* name);

/**
 * @brief Read directory entries (for readdir)
 */
int autofs_dir_read_entries(autofs_fs_t* fs, autofs_inode_t* dir_inode,
                            autofs_dirent_t* entries, uint32_t max_entries,
                            uint64_t* offset);

/* ========== VFS Integration ========== */

/**
 * @brief Mount AutoFS filesystem (VFS interface)
 */
vfs_superblock_t* autofs_vfs_mount(block_device_t* bdev, const char* options);

/**
 * @brief Unmount AutoFS filesystem
 */
int autofs_vfs_unmount(vfs_superblock_t* vfs_sb);

/**
 * @brief VFS file read operation
 */
ssize_t autofs_vfs_read(vfs_file_t* file, void* buf, size_t count);

/**
 * @brief VFS file write operation
 */
ssize_t autofs_vfs_write(vfs_file_t* file, const void* buf, size_t count);

/**
 * @brief VFS directory lookup operation
 */
vfs_dentry_t* autofs_vfs_lookup(vfs_inode_t* dir, const char* name);

/**
 * @brief VFS create file operation
 */
int autofs_vfs_create(vfs_inode_t* dir, vfs_dentry_t* dentry, uint32_t mode);

/**
 * @brief VFS mkdir operation
 */
int autofs_vfs_mkdir(vfs_inode_t* dir, vfs_dentry_t* dentry, uint32_t mode);

/**
 * @brief VFS unlink operation
 */
int autofs_vfs_unlink(vfs_inode_t* dir, vfs_dentry_t* dentry);

/**
 * @brief VFS rmdir operation
 */
int autofs_vfs_rmdir(vfs_inode_t* dir, vfs_dentry_t* dentry);

/* ========== Utility Functions ========== */

/**
 * @brief Path lookup from root
 * @return Inode number or 0 if not found
 */
uint64_t autofs_path_lookup(autofs_fs_t* fs, const char* path);

/**
 * @brief Calculate checksum for data
 */
uint64_t autofs_checksum(const void* data, size_t len);

/**
 * @brief Print filesystem statistics
 */
void autofs_print_stats(autofs_fs_t* fs);

/* ========== Initialization ========== */

/**
 * @brief Initialize AutoFS subsystem
 * Registers filesystem type with VFS
 */
void autofs_init(void);

#endif /* AUTOFS_KERNEL_H */

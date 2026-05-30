/**
 * @file autofs.h
 * @brief AutoFS - Advanced Filesystem with Journaling, CoW, Compression & Encryption
 *
 * AutoFS combines the best features of modern filesystems:
 * - ext4-style journaling for crash consistency
 * - btrfs-style copy-on-write for snapshots
 * - ZFS-style integrity checking
 * - Transparent compression (zstd, lz4, zlib)
 * - Per-file encryption (AES-256-GCM)
 * - Extended attributes
 * - Fast performance with aggressive caching
 */

#ifndef AUTOFS_H
#define AUTOFS_H

#include "types.h"

/* time_t for filesystem timestamps (guard against double definition) */
#ifndef _KERNEL_COMPAT_TIME_H
typedef uint64_t time_t;
#endif

/* Magic number for AutoFS */
#define AUTOFS_MAGIC 0xAUT0F5

/* Version */
#define AUTOFS_VERSION 1

/* Block size (4KB) */
#define AUTOFS_BLOCK_SIZE 4096

/* Maximum filename length */
#define AUTOFS_MAX_NAME_LEN 255

/* Maximum path length */
#define AUTOFS_MAX_PATH_LEN 4096

/* Direct block pointers in inode */
#define AUTOFS_DIRECT_BLOCKS 12

/* Features flags */
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
    AUTOFS_TYPE_FILE = 1,
    AUTOFS_TYPE_DIR = 2,
    AUTOFS_TYPE_SYMLINK = 3,
    AUTOFS_TYPE_DEVICE = 4,
    AUTOFS_TYPE_FIFO = 5,
    AUTOFS_TYPE_SOCKET = 6
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

/* Compression algorithms */
typedef enum {
    COMPRESS_NONE = 0,
    COMPRESS_ZSTD = 1,
    COMPRESS_LZ4 = 2,
    COMPRESS_ZLIB = 3
} compress_algo_t;

/**
 * @brief Superblock - First block of filesystem
 *
 * Contains global filesystem metadata and configuration.
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

    /* Snapshot info */
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

    /* Compression default */
    uint32_t default_compress_algo;

    /* Reserved for future use */
    uint8_t reserved[3704];
} __attribute__((packed)) autofs_superblock_t;

/**
 * @brief Inode - File metadata
 *
 * Contains all metadata for a file/directory/symlink.
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

    /* Extended attributes */
    uint64_t xattr_block;        /* Extended attributes block */

    /* Compression info */
    uint32_t compressed_size;    /* Compressed size if compressed */
    uint32_t compress_algo;      /* Compression algorithm */

    /* Refcount for CoW */
    uint32_t refcount;           /* Reference count */

    /* Reserved */
    uint8_t reserved[128];
} __attribute__((packed)) autofs_inode_t;

/**
 * @brief Directory entry
 */
typedef struct autofs_dirent {
    uint64_t ino;                /* Inode number */
    uint16_t rec_len;            /* Record length */
    uint8_t name_len;            /* Name length */
    uint8_t file_type;           /* File type */
    char name[AUTOFS_MAX_NAME_LEN + 1];  /* Filename */
} __attribute__((packed)) autofs_dirent_t;

/**
 * @brief Journal transaction
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
 * @brief Snapshot metadata
 */
typedef struct autofs_snapshot {
    uint64_t snapshot_id;        /* Snapshot ID */
    uint64_t root_inode;         /* Snapshot root inode */
    uint64_t timestamp;          /* Creation timestamp */
    char name[256];              /* Snapshot name */
    uint64_t parent_snapshot;    /* Parent snapshot (0 for root) */
    uint32_t flags;              /* Snapshot flags */
    uint8_t reserved[236];
} __attribute__((packed)) autofs_snapshot_t;

/**
 * @brief Extended attribute entry
 */
typedef struct autofs_xattr {
    char name[256];              /* Attribute name */
    uint32_t value_len;          /* Value length */
    uint8_t value[3836];         /* Value data */
} __attribute__((packed)) autofs_xattr_t;

/**
 * @brief In-memory filesystem structure
 */
typedef struct autofs_fs {
    int fd;                      /* Device file descriptor */
    autofs_superblock_t *sb;     /* Superblock */

    /* Bitmaps */
    uint8_t *block_bitmap;
    uint8_t *inode_bitmap;

    /* Journal */
    void *journal_buffer;
    uint64_t current_transaction_id;

    /* Caches */
    void *inode_cache;
    void *dentry_cache;
    void *page_cache;

    /* Mount info */
    bool read_only;
    char *device_path;

    /* Statistics */
    struct {
        uint64_t reads;
        uint64_t writes;
        uint64_t cache_hits;
        uint64_t cache_misses;
    } stats;
} autofs_fs_t;

/**
 * @brief File descriptor
 */
typedef struct autofs_file {
    autofs_fs_t *fs;
    uint64_t ino;
    autofs_inode_t *inode;
    uint64_t offset;
    int flags;
} autofs_file_t;

/**
 * @brief Directory handle
 */
typedef struct autofs_dir {
    autofs_fs_t *fs;
    uint64_t ino;
    autofs_inode_t *inode;
    uint64_t offset;
} autofs_dir_t;

/* Core filesystem operations */
autofs_fs_t* autofs_mount(const char *device, bool read_only);
int autofs_unmount(autofs_fs_t *fs);
int autofs_mkfs(const char *device, uint64_t size, const char *label);
int autofs_fsck(const char *device, bool repair);

/* File operations */
int autofs_open(autofs_fs_t *fs, const char *path, int flags, uint64_t *ino);
ssize_t autofs_read(autofs_file_t *file, void *buf, size_t count);
ssize_t autofs_write(autofs_file_t *file, const void *buf, size_t count);
int autofs_close(autofs_file_t *file);
int autofs_truncate(autofs_fs_t *fs, uint64_t ino, uint64_t size);
int autofs_unlink(autofs_fs_t *fs, const char *path);

/* Directory operations */
int autofs_mkdir(autofs_fs_t *fs, const char *path, uint32_t mode);
int autofs_rmdir(autofs_fs_t *fs, const char *path);
autofs_dir_t* autofs_opendir(autofs_fs_t *fs, const char *path);
autofs_dirent_t* autofs_readdir(autofs_dir_t *dir);
int autofs_closedir(autofs_dir_t *dir);

/* Inode operations */
autofs_inode_t* autofs_get_inode(autofs_fs_t *fs, uint64_t ino);
int autofs_put_inode(autofs_fs_t *fs, autofs_inode_t *inode);
uint64_t autofs_alloc_inode(autofs_fs_t *fs);
int autofs_free_inode(autofs_fs_t *fs, uint64_t ino);

/* Block operations */
uint64_t autofs_alloc_block(autofs_fs_t *fs);
int autofs_free_block(autofs_fs_t *fs, uint64_t block);
ssize_t autofs_read_block(autofs_fs_t *fs, uint64_t block, void *buf);
ssize_t autofs_write_block(autofs_fs_t *fs, uint64_t block, const void *buf);

/* Journal operations */
int autofs_journal_init(autofs_fs_t *fs);
int autofs_journal_begin(autofs_fs_t *fs);
int autofs_journal_log(autofs_fs_t *fs, journal_op_t op, uint64_t block, const void *data);
int autofs_journal_commit(autofs_fs_t *fs);
int autofs_journal_abort(autofs_fs_t *fs);
int autofs_journal_recover(autofs_fs_t *fs);

/* Copy-on-write operations */
int autofs_cow_write(autofs_fs_t *fs, uint64_t ino, uint64_t block_idx, const void *data);
int autofs_cow_ref_inc(autofs_fs_t *fs, uint64_t ino);
int autofs_cow_ref_dec(autofs_fs_t *fs, uint64_t ino);

/* Snapshot operations */
autofs_snapshot_t* autofs_snapshot_create(autofs_fs_t *fs, const char *name);
int autofs_snapshot_delete(autofs_fs_t *fs, uint64_t snapshot_id);
int autofs_snapshot_restore(autofs_fs_t *fs, uint64_t snapshot_id);
autofs_snapshot_t** autofs_snapshot_list(autofs_fs_t *fs, uint64_t *count);

/* Compression operations */
ssize_t autofs_compress(compress_algo_t algo, const void *src, size_t src_len,
                        void *dst, size_t dst_len);
ssize_t autofs_decompress(compress_algo_t algo, const void *src, size_t src_len,
                          void *dst, size_t dst_len);

/* Encryption operations */
int autofs_encrypt(const void *key, size_t key_len, const void *plaintext,
                   size_t plain_len, void *ciphertext, size_t *cipher_len);
int autofs_decrypt(const void *key, size_t key_len, const void *ciphertext,
                   size_t cipher_len, void *plaintext, size_t *plain_len);
int autofs_set_encrypted(autofs_fs_t *fs, uint64_t ino, const void *key, size_t key_len);

/* Extended attributes */
int autofs_xattr_set(autofs_fs_t *fs, uint64_t ino, const char *name,
                     const void *value, size_t size);
ssize_t autofs_xattr_get(autofs_fs_t *fs, uint64_t ino, const char *name,
                         void *value, size_t size);
ssize_t autofs_xattr_list(autofs_fs_t *fs, uint64_t ino, char *list, size_t size);
int autofs_xattr_remove(autofs_fs_t *fs, uint64_t ino, const char *name);

/* Cache operations */
int autofs_cache_init(autofs_fs_t *fs);
void autofs_cache_destroy(autofs_fs_t *fs);
void autofs_cache_invalidate(autofs_fs_t *fs, uint64_t ino);
void autofs_cache_flush(autofs_fs_t *fs);

/* Utility functions */
int autofs_path_lookup(autofs_fs_t *fs, const char *path, uint64_t *ino);
uint64_t autofs_checksum(const void *data, size_t len);
void autofs_print_stats(autofs_fs_t *fs);

#endif /* AUTOFS_H */

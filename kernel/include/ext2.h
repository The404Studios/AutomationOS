/**
 * ext2 Filesystem Driver
 *
 * Implementation of the Second Extended Filesystem for AutomationOS
 */

#ifndef EXT2_H
#define EXT2_H

#include "types.h"
#include "vfs.h"

// ext2 Magic number
#define EXT2_SUPER_MAGIC 0xEF53

// Block size
#define EXT2_MIN_BLOCK_SIZE 1024
#define EXT2_MAX_BLOCK_SIZE 4096

// Inode constants
#define EXT2_GOOD_OLD_INODE_SIZE 128
#define EXT2_ROOT_INO 2

// File type constants (stored in inode mode)
#define EXT2_S_IFREG  0x8000  // Regular file
#define EXT2_S_IFDIR  0x4000  // Directory
#define EXT2_S_IFLNK  0xA000  // Symbolic link
#define EXT2_S_IFCHR  0x2000  // Character device
#define EXT2_S_IFBLK  0x6000  // Block device

// Directory entry types
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

// ext2 Superblock (1024 bytes)
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;        // Total number of inodes
    uint32_t s_blocks_count;        // Total number of blocks
    uint32_t s_r_blocks_count;      // Reserved blocks count
    uint32_t s_free_blocks_count;   // Free blocks count
    uint32_t s_free_inodes_count;   // Free inodes count
    uint32_t s_first_data_block;    // First data block
    uint32_t s_log_block_size;      // Block size (log2(block_size) - 10)
    uint32_t s_log_frag_size;       // Fragment size
    uint32_t s_blocks_per_group;    // Blocks per group
    uint32_t s_frags_per_group;     // Fragments per group
    uint32_t s_inodes_per_group;    // Inodes per group
    uint32_t s_mtime;               // Mount time
    uint32_t s_wtime;               // Write time
    uint16_t s_mnt_count;           // Mount count
    uint16_t s_max_mnt_count;       // Maximal mount count
    uint16_t s_magic;               // Magic signature (0xEF53)
    uint16_t s_state;               // File system state
    uint16_t s_errors;              // Behaviour when detecting errors
    uint16_t s_minor_rev_level;     // Minor revision level
    uint32_t s_lastcheck;           // Time of last check
    uint32_t s_checkinterval;       // Max time between checks
    uint32_t s_creator_os;          // OS
    uint32_t s_rev_level;           // Revision level
    uint16_t s_def_resuid;          // Default uid for reserved blocks
    uint16_t s_def_resgid;          // Default gid for reserved blocks
    uint32_t s_first_ino;           // First non-reserved inode
    uint16_t s_inode_size;          // Size of inode structure
    uint16_t s_block_group_nr;      // Block group number of this superblock
    uint32_t s_feature_compat;      // Compatible feature set
    uint32_t s_feature_incompat;    // Incompatible feature set
    uint32_t s_feature_ro_compat;   // Readonly-compatible feature set
    uint8_t  s_uuid[16];            // 128-bit uuid for volume
    char     s_volume_name[16];     // Volume name
    char     s_last_mounted[64];    // Directory where last mounted
    uint32_t s_algorithm_usage_bitmap; // For compression
    uint8_t  s_prealloc_blocks;     // Nr of blocks to preallocate
    uint8_t  s_prealloc_dir_blocks; // Nr to preallocate for dirs
    uint16_t s_padding1;
    uint8_t  s_reserved[204];       // Padding to 1024 bytes
} ext2_superblock_t;

// ext2 Block Group Descriptor
typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;       // Block bitmap block
    uint32_t bg_inode_bitmap;       // Inode bitmap block
    uint32_t bg_inode_table;        // Inode table block
    uint16_t bg_free_blocks_count;  // Free blocks count
    uint16_t bg_free_inodes_count;  // Free inodes count
    uint16_t bg_used_dirs_count;    // Directories count
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_block_group_desc_t;

// ext2 Inode (128 bytes for old format)
typedef struct __attribute__((packed)) {
    uint16_t i_mode;                // File mode
    uint16_t i_uid;                 // Owner Uid
    uint32_t i_size;                // Size in bytes
    uint32_t i_atime;               // Access time
    uint32_t i_ctime;               // Creation time
    uint32_t i_mtime;               // Modification time
    uint32_t i_dtime;               // Deletion time
    uint16_t i_gid;                 // Group Id
    uint16_t i_links_count;         // Links count
    uint32_t i_blocks;              // Blocks count (in 512-byte sectors)
    uint32_t i_flags;               // File flags
    uint32_t i_osd1;                // OS dependent 1
    uint32_t i_block[15];           // Pointers to blocks
    uint32_t i_generation;          // File version (for NFS)
    uint32_t i_file_acl;            // File ACL
    uint32_t i_dir_acl;             // Directory ACL (or size_high for files)
    uint32_t i_faddr;               // Fragment address
    uint8_t  i_osd2[12];            // OS dependent 2
} ext2_inode_t;

// ext2 Directory Entry
typedef struct __attribute__((packed)) {
    uint32_t inode;                 // Inode number
    uint16_t rec_len;               // Directory entry length
    uint8_t  name_len;              // Name length
    uint8_t  file_type;             // File type
    char     name[];                // File name (variable length)
} ext2_dir_entry_t;

// ext2 private filesystem data
typedef struct {
    ext2_superblock_t* superblock;
    ext2_block_group_desc_t* group_desc;
    uint32_t block_size;
    uint32_t groups_count;
    void* block_device;             // Block device handle
} ext2_fs_data_t;

// ext2 initialization and operations
void ext2_init(void);
vfs_superblock_t* ext2_mount(const char* source, uint32_t flags);
void ext2_unmount(vfs_superblock_t* sb);
int ext2_detect(const char* source);

#endif // EXT2_H

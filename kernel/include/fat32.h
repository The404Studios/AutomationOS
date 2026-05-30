/**
 * FAT32 Filesystem Driver
 *
 * Implementation of the FAT32 filesystem for AutomationOS
 */

#ifndef FAT32_H
#define FAT32_H

#include "types.h"
#include "vfs.h"

// FAT32 Boot Sector signature
#define FAT32_SIGNATURE 0xAA55

// FAT Entry values
#define FAT32_FREE_CLUSTER      0x00000000
#define FAT32_EOC               0x0FFFFFF8  // End of cluster chain
#define FAT32_BAD_CLUSTER       0x0FFFFFF7

// FAT32 file attributes
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LONG_NAME  0x0F

// FAT32 Boot Sector (BIOS Parameter Block)
typedef struct __attribute__((packed)) {
    uint8_t  jmp_boot[3];           // Jump instruction
    char     oem_name[8];           // OEM name
    uint16_t bytes_per_sector;      // Bytes per sector
    uint8_t  sectors_per_cluster;   // Sectors per cluster
    uint16_t reserved_sectors;      // Reserved sectors
    uint8_t  num_fats;              // Number of FATs
    uint16_t root_entry_count;      // Root entries (0 for FAT32)
    uint16_t total_sectors_16;      // Total sectors (0 for FAT32)
    uint8_t  media_type;            // Media descriptor
    uint16_t fat_size_16;           // FAT size (0 for FAT32)
    uint16_t sectors_per_track;     // Sectors per track
    uint16_t num_heads;             // Number of heads
    uint32_t hidden_sectors;        // Hidden sectors
    uint32_t total_sectors_32;      // Total sectors

    // FAT32-specific fields
    uint32_t fat_size_32;           // FAT size in sectors
    uint16_t ext_flags;             // Extended flags
    uint16_t fs_version;            // Filesystem version
    uint32_t root_cluster;          // Root directory cluster
    uint16_t fs_info;               // FSInfo sector
    uint16_t backup_boot_sector;    // Backup boot sector
    uint8_t  reserved[12];          // Reserved
    uint8_t  drive_number;          // Drive number
    uint8_t  reserved1;             // Reserved
    uint8_t  boot_signature;        // Boot signature (0x29)
    uint32_t volume_id;             // Volume ID
    char     volume_label[11];      // Volume label
    char     fs_type[8];            // Filesystem type ("FAT32   ")
    uint8_t  boot_code[420];        // Boot code
    uint16_t signature;             // Boot signature (0xAA55)
} fat32_boot_sector_t;

// FAT32 Directory Entry (32 bytes)
typedef struct __attribute__((packed)) {
    char     name[11];              // 8.3 filename
    uint8_t  attr;                  // File attributes
    uint8_t  nt_reserved;           // Reserved for Windows NT
    uint8_t  create_time_tenth;     // Creation time (tenths of second)
    uint16_t create_time;           // Creation time
    uint16_t create_date;           // Creation date
    uint16_t access_date;           // Last access date
    uint16_t first_cluster_hi;      // High word of first cluster
    uint16_t write_time;            // Last write time
    uint16_t write_date;            // Last write date
    uint16_t first_cluster_lo;      // Low word of first cluster
    uint32_t file_size;             // File size in bytes
} fat32_dir_entry_t;

// FAT32 Long File Name entry
typedef struct __attribute__((packed)) {
    uint8_t  order;                 // Order of this entry
    uint16_t name1[5];              // Characters 1-5
    uint8_t  attr;                  // Attributes (always 0x0F)
    uint8_t  type;                  // Type (always 0)
    uint8_t  checksum;              // Checksum
    uint16_t name2[6];              // Characters 6-11
    uint16_t first_cluster;         // First cluster (always 0)
    uint16_t name3[2];              // Characters 12-13
} fat32_lfn_entry_t;

// FAT32 private filesystem data
typedef struct {
    fat32_boot_sector_t* boot_sector;
    uint32_t* fat;                  // File Allocation Table (cached)
    uint32_t bytes_per_cluster;
    uint32_t first_data_sector;
    uint32_t data_sectors;
    uint32_t total_clusters;
    void* block_device;             // Block device handle
} fat32_fs_data_t;

// FAT32 initialization and operations
void fat32_init(void);
vfs_superblock_t* fat32_mount(const char* source, uint32_t flags);
void fat32_unmount(vfs_superblock_t* sb);
int fat32_detect(const char* source);

#endif // FAT32_H

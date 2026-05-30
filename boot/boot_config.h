/**
 * Boot Configuration Parser
 * Parses /boot/boot.conf for boot entries
 */

#ifndef BOOT_CONFIG_H
#define BOOT_CONFIG_H

#include <stdint.h>

#define MAX_BOOT_ENTRIES 16
#define MAX_PATH_LEN 256
#define MAX_TITLE_LEN 64
#define MAX_OPTIONS_LEN 256

typedef struct {
    char title[MAX_TITLE_LEN];
    char kernel_path[MAX_PATH_LEN];
    char initrd_path[MAX_PATH_LEN];
    char options[MAX_OPTIONS_LEN];
    uint32_t timeout;            // Seconds to wait
    uint8_t is_default;
} boot_entry_t;

typedef struct {
    boot_entry_t entries[MAX_BOOT_ENTRIES];
    uint32_t entry_count;
    uint32_t default_entry;
    uint32_t timeout;            // Global timeout
} boot_config_t;

// Parse boot configuration from file
int boot_config_parse(const char* config_data, uint64_t size, boot_config_t* config);

// Load boot configuration from filesystem
int boot_config_load(void* fs_root, boot_config_t* config);

#endif // BOOT_CONFIG_H

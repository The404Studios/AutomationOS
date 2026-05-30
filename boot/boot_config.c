/**
 * Boot Configuration Parser Implementation
 * Simple INI-style parser for boot.conf
 */

#include "boot_config.h"

// Simple string utilities (no libc in bootloader)
static int str_len(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int str_cmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}

static int str_ncmp(const char* a, const char* b, int n) {
    while (n > 0 && *a && *b && *a == *b) {
        a++;
        b++;
        n--;
    }
    return (n == 0) ? 0 : (*a - *b);
}

static void str_cpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static char* str_trim(char* s) {
    // Trim leading whitespace
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }

    // Trim trailing whitespace
    char* end = s + str_len(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = 0;
        end--;
    }

    return s;
}

static int parse_int(const char* s) {
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

/**
 * Parse boot configuration from file contents
 *
 * Format:
 * [Entry Title]
 * kernel=/boot/kernel.elf
 * initrd=/boot/initrd.img
 * options=quiet splash
 * default=yes
 * timeout=5
 */
int boot_config_parse(const char* config_data, uint64_t size, boot_config_t* config) {
    if (!config_data || !config || size == 0) {
        return -1;
    }

    // Initialize config
    config->entry_count = 0;
    config->default_entry = 0;
    config->timeout = 5;  // Default 5 seconds

    // Parse line by line
    const char* line_start = config_data;
    const char* end = config_data + size;
    boot_entry_t* current_entry = NULL;

    while (line_start < end) {
        // Find end of line
        const char* line_end = line_start;
        while (line_end < end && *line_end != '\n') {
            line_end++;
        }

        // Copy line to buffer
        char line[512];
        int line_len = line_end - line_start;
        if (line_len > 511) line_len = 511;

        for (int i = 0; i < line_len; i++) {
            line[i] = line_start[i];
        }
        line[line_len] = 0;

        // Trim whitespace
        char* trimmed = str_trim(line);

        // Skip empty lines and comments
        if (trimmed[0] == 0 || trimmed[0] == '#') {
            line_start = line_end + 1;
            continue;
        }

        // Check for section header [Title]
        if (trimmed[0] == '[') {
            char* title_end = trimmed + 1;
            while (*title_end && *title_end != ']') {
                title_end++;
            }

            if (*title_end == ']') {
                *title_end = 0;

                // Create new entry
                if (config->entry_count < MAX_BOOT_ENTRIES) {
                    current_entry = &config->entries[config->entry_count++];

                    // Initialize entry
                    str_cpy(current_entry->title, trimmed + 1, MAX_TITLE_LEN);
                    current_entry->kernel_path[0] = 0;
                    current_entry->initrd_path[0] = 0;
                    current_entry->options[0] = 0;
                    current_entry->timeout = 0;
                    current_entry->is_default = 0;
                }
            }
        }
        // Check for key=value
        else if (current_entry) {
            char* equals = trimmed;
            while (*equals && *equals != '=') {
                equals++;
            }

            if (*equals == '=') {
                *equals = 0;
                char* key = str_trim(trimmed);
                char* value = str_trim(equals + 1);

                if (str_cmp(key, "kernel") == 0) {
                    str_cpy(current_entry->kernel_path, value, MAX_PATH_LEN);
                }
                else if (str_cmp(key, "initrd") == 0) {
                    str_cpy(current_entry->initrd_path, value, MAX_PATH_LEN);
                }
                else if (str_cmp(key, "options") == 0) {
                    str_cpy(current_entry->options, value, MAX_OPTIONS_LEN);
                }
                else if (str_cmp(key, "default") == 0) {
                    if (str_cmp(value, "yes") == 0 || str_cmp(value, "true") == 0 || str_cmp(value, "1") == 0) {
                        current_entry->is_default = 1;
                        config->default_entry = config->entry_count - 1;
                    }
                }
                else if (str_cmp(key, "timeout") == 0) {
                    current_entry->timeout = parse_int(value);
                }
            }
        }
        // Global options (outside any section)
        else {
            char* equals = trimmed;
            while (*equals && *equals != '=') {
                equals++;
            }

            if (*equals == '=') {
                *equals = 0;
                char* key = str_trim(trimmed);
                char* value = str_trim(equals + 1);

                if (str_cmp(key, "timeout") == 0) {
                    config->timeout = parse_int(value);
                }
            }
        }

        line_start = line_end + 1;
    }

    return 0;
}

/**
 * Load boot configuration from filesystem
 * This will be implemented in the main bootloader with UEFI file I/O
 */
int boot_config_load(void* fs_root, boot_config_t* config) {
    // TODO: Implement UEFI file loading in main bootloader
    // For now, return error
    return -1;
}

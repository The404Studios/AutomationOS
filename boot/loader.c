#include "boot.h"

// Simplified UEFI structures
typedef struct {
    uint8_t magic[16];
} efi_table_header_t;

typedef struct {
    efi_table_header_t hdr;
    void* firmware_vendor;
    uint32_t firmware_revision;
    void* console_in_handle;
    void* con_in;
    void* console_out_handle;
    void* con_out;
    // ... more fields
} efi_system_table_t;

static boot_info_t boot_info;
static memory_map_entry_t memory_map[256];

void* memset(void* s, int c, unsigned long n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void efi_main(void* image_handle, efi_system_table_t* systab) {
    // Clear boot info
    memset(&boot_info, 0, sizeof(boot_info));
    memset(memory_map, 0, sizeof(memory_map));

    // TODO: Get memory map from UEFI
    // For now, create a simple map
    memory_map[0].base = 0x100000;      // 1MB
    memory_map[0].length = 0x1F00000;   // ~31MB
    memory_map[0].type = 1;             // Usable

    boot_info.memory_map = memory_map;
    boot_info.memory_map_count = 1;

    // TODO: Setup graphics mode
    boot_info.framebuffer_addr = (void*)0xFD000000;
    boot_info.framebuffer_width = 1024;
    boot_info.framebuffer_height = 768;
    boot_info.framebuffer_pitch = 1024 * 4;

    // TODO: Load kernel from disk
    // For now, assume kernel is loaded at 0x100000
    void (*kernel_entry)(boot_info_t*) = (void*)0xFFFFFFFF80100000;

    // Jump to kernel
    kernel_entry(&boot_info);

    // Should not reach here
    while (1);
}

#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} memory_map_entry_t;

typedef struct {
    memory_map_entry_t* memory_map;
    uint32_t memory_map_count;
    void* framebuffer_addr;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;
    void* kernel_entry;
} boot_info_t;

#endif

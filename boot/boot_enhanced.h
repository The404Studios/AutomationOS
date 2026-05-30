/**
 * Enhanced Boot Information Structure
 * Passed from bootloader to kernel
 */

#ifndef BOOT_ENHANCED_H
#define BOOT_ENHANCED_H

#include <stdint.h>

#define BOOT_MAGIC 0xB001B001
#define BOOT_VERSION 1

// Memory map entry types (from UEFI spec)
#define MEMORY_TYPE_RESERVED           0
#define MEMORY_TYPE_LOADER_CODE        1
#define MEMORY_TYPE_LOADER_DATA        2
#define MEMORY_TYPE_BOOT_CODE          3
#define MEMORY_TYPE_BOOT_DATA          4
#define MEMORY_TYPE_RUNTIME_CODE       5
#define MEMORY_TYPE_RUNTIME_DATA       6
#define MEMORY_TYPE_AVAILABLE          7
#define MEMORY_TYPE_UNUSABLE           8
#define MEMORY_TYPE_ACPI_RECLAIM       9
#define MEMORY_TYPE_ACPI_NVS          10
#define MEMORY_TYPE_MMIO              11
#define MEMORY_TYPE_MMIO_PORT         12

typedef struct {
    uint64_t base;           // Physical address
    uint64_t length;         // Size in bytes
    uint32_t type;           // Memory type
    uint32_t reserved;
} memory_map_entry_t;

// ACPI RSDP pointer structure
typedef struct {
    char signature[8];       // "RSD PTR "
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;   // 32-bit physical address of RSDT
    // ACPI 2.0+ fields
    uint32_t length;
    uint64_t xsdt_address;   // 64-bit physical address of XSDT
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) rsdp_t;

// Enhanced boot information structure
typedef struct {
    uint32_t magic;              // 0xB001B001
    uint32_t version;            // Boot info version

    // Memory map
    memory_map_entry_t* memory_map;
    uint32_t memory_map_count;
    uint64_t total_memory;       // Total RAM in bytes

    // Initial ramdisk (initrd)
    uint64_t initrd_addr;        // Physical address
    uint64_t initrd_size;        // Size in bytes

    // Framebuffer (Graphics Output Protocol)
    uint64_t framebuffer_addr;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;  // Bytes per scanline
    uint32_t framebuffer_bpp;    // Bits per pixel
    uint32_t pixels_per_scanline;
    uint64_t framebuffer_size;

    // ACPI tables
    uint64_t rsdp_addr;          // RSDP physical address

    // Kernel command line
    char cmdline[1024];

    // Boot time (in milliseconds)
    uint64_t boot_time_ms;

    // Kernel entry point (for reference)
    void* kernel_entry;

    // Loader information
    char loader_name[64];        // "AutomationOS UEFI Bootloader"
    uint32_t loader_version;
} boot_info_t;

// ELF64 Structures
typedef struct {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;          // Program header offset
    uint64_t e_shoff;          // Section header offset
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;          // Number of program headers
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;         // Offset in file
    uint64_t p_vaddr;          // Virtual address
    uint64_t p_paddr;          // Physical address
    uint64_t p_filesz;         // Size in file
    uint64_t p_memsz;          // Size in memory
    uint64_t p_align;
} Elf64_Phdr;

// ELF constants
#define ELF_MAGIC 0x464C457F   // "\x7FELF"
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4

// UEFI Types
typedef void* EFI_HANDLE;
typedef uint64_t EFI_STATUS;
typedef uint64_t UINTN;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;

#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR 1
#define EFI_INVALID_PARAMETER 2
#define EFI_UNSUPPORTED 3
#define EFI_BAD_BUFFER_SIZE 4
#define EFI_BUFFER_TOO_SMALL 5
#define EFI_NOT_READY 6
#define EFI_DEVICE_ERROR 7
#define EFI_WRITE_PROTECTED 8
#define EFI_OUT_OF_RESOURCES 9
#define EFI_NOT_FOUND 14

// UEFI GUID
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} EFI_GUID;

// UEFI Memory Types
typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

// UEFI Memory Descriptor
typedef struct {
    uint32_t Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

// Page table constants
#define PAGE_SIZE 4096
#define PAGE_SIZE_2MB (2 * 1024 * 1024)
#define PAGE_PRESENT (1ULL << 0)
#define PAGE_WRITE (1ULL << 1)
#define PAGE_USER (1ULL << 2)
#define PAGE_SIZE_BIT (1ULL << 7)

#define KERNEL_VMA 0xFFFFFFFF80000000ULL  // Kernel higher half

#endif // BOOT_ENHANCED_H

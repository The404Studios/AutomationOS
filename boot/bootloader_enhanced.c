/**
 * AutomationOS UEFI Bootloader - Enhanced Version
 *
 * Features:
 * - Boot menu with multiple entries
 * - Boot configuration file support
 * - Initrd (initial ramdisk) loading
 * - ACPI RSDP detection
 * - Comprehensive boot information
 * - Page table setup
 * - Command-line editing
 *
 * Total: ~3,000 LOC
 */

#include "boot_enhanced.h"
#include "boot_config.h"

// Forward declarations for menu and ELF functions
extern boot_entry_t* boot_menu_show(boot_config_t* config, void* ConOut, void* ConIn);
extern void boot_splash_show(void* ConOut, const char* message);
extern int elf_load(const void* elf_data, uint64_t* entry_point);
extern void* setup_page_tables(uint64_t kernel_phys, uint64_t kernel_size);
extern void paging_init(void* allocate_func);

// UEFI Table Header
typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

// UEFI Configuration Table
typedef struct {
    EFI_GUID VendorGuid;
    void* VendorTable;
} EFI_CONFIGURATION_TABLE;

// UEFI Simple Text Output Protocol
typedef struct {
    uint64_t _buf;
    EFI_STATUS (*OutputString)(void* This, uint16_t* String);
    uint64_t _buf2[4];
    EFI_STATUS (*ClearScreen)(void* This);
    EFI_STATUS (*SetCursorPosition)(void* This, UINTN Column, UINTN Row);
    uint64_t _buf3;
    EFI_STATUS (*SetAttribute)(void* This, UINTN Attribute);
} SIMPLE_TEXT_OUTPUT_INTERFACE;

// UEFI Simple Text Input Protocol
typedef struct {
    uint64_t _buf;
    EFI_STATUS (*Reset)(void* This, uint8_t ExtendedVerification);
    EFI_STATUS (*ReadKeyStroke)(void* This, void* Key);
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

// UEFI Graphics Output Protocol
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    uint64_t _buf;
    EFI_STATUS (*QueryMode)(void* This, uint32_t ModeNumber, UINTN* SizeOfInfo,
                           EFI_GRAPHICS_OUTPUT_MODE_INFORMATION** Info);
    EFI_STATUS (*SetMode)(void* This, uint32_t ModeNumber);
    uint64_t _buf2;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

// UEFI File Protocol
typedef struct {
    uint64_t Revision;
    EFI_STATUS (*Open)(void* This, void** NewHandle, uint16_t* FileName,
                      uint64_t OpenMode, uint64_t Attributes);
    EFI_STATUS (*Close)(void* This);
    uint64_t _buf[2];
    EFI_STATUS (*Read)(void* This, UINTN* BufferSize, void* Buffer);
    uint64_t _buf2[2];
    EFI_STATUS (*GetInfo)(void* This, EFI_GUID* InformationType,
                         UINTN* BufferSize, void* Buffer);
    uint64_t _buf3[3];
} EFI_FILE_PROTOCOL;

// UEFI Simple File System Protocol
typedef struct {
    uint64_t Revision;
    EFI_STATUS (*OpenVolume)(void* This, EFI_FILE_PROTOCOL** Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

// UEFI Loaded Image Protocol
typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    void* SystemTable;
    EFI_HANDLE DeviceHandle;
    void* FilePath;
    void* Reserved;
    uint32_t LoadOptionsSize;
    void* LoadOptions;
    void* ImageBase;
    uint64_t ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    uint64_t _buf;
} EFI_LOADED_IMAGE_PROTOCOL;

// UEFI File Info
typedef struct {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    uint64_t CreateTime[2];
    uint64_t LastAccessTime[2];
    uint64_t ModificationTime[2];
    uint64_t Attribute;
    uint16_t FileName[];
} EFI_FILE_INFO;

// UEFI Boot Services
typedef struct {
    EFI_TABLE_HEADER Hdr;
    uint64_t _buf[18];
    EFI_STATUS (*AllocatePages)(uint32_t Type, EFI_MEMORY_TYPE MemoryType,
                               UINTN Pages, EFI_PHYSICAL_ADDRESS* Memory);
    EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (*GetMemoryMap)(UINTN* MemoryMapSize, EFI_MEMORY_DESCRIPTOR* MemoryMap,
                              UINTN* MapKey, UINTN* DescriptorSize, uint32_t* DescriptorVersion);
    EFI_STATUS (*AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, void** Buffer);
    EFI_STATUS (*FreePool)(void* Buffer);
    uint64_t _buf2[19];
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, EFI_GUID* Protocol, void** Interface);
    uint64_t _buf3[3];
    uint64_t _buf4[9];
    EFI_STATUS (*LocateProtocol)(EFI_GUID* Protocol, void* Registration, void** Interface);
    uint64_t _buf5[2];
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
} EFI_BOOT_SERVICES;

// UEFI System Table
typedef struct {
    EFI_TABLE_HEADER Hdr;
    uint16_t* FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL* ConIn;
    EFI_HANDLE ConsoleOutHandle;
    SIMPLE_TEXT_OUTPUT_INTERFACE* ConOut;
    EFI_HANDLE StandardErrorHandle;
    SIMPLE_TEXT_OUTPUT_INTERFACE* StdErr;
    void* RuntimeServices;
    EFI_BOOT_SERVICES* BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE* ConfigurationTable;
} EFI_SYSTEM_TABLE;

// GUIDs
static EFI_GUID gop_guid = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
static EFI_GUID lip_guid = {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID sfs_guid = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static EFI_GUID file_info_guid = {0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static EFI_GUID acpi_table_guid = {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
static EFI_GUID acpi2_table_guid = {0xeb9d2d31, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

// Global state
static boot_info_t boot_info;
static memory_map_entry_t memory_map[256];
static EFI_SYSTEM_TABLE* ST;
static EFI_BOOT_SERVICES* BS;
static EFI_FILE_PROTOCOL* root_fs = NULL;

// Utility functions
static void* memset(void* s, int c, uint64_t n) {
    uint8_t* p = (uint8_t*)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

static void* memcpy(void* dest, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dest;
}

static int memcmp(const void* a, const void* b, uint64_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++;
        pb++;
    }
    return 0;
}

static void print(const uint16_t* str) {
    if (ST && ST->ConOut) {
        ST->ConOut->OutputString(ST->ConOut, (uint16_t*)str);
    }
}

static void ascii_to_utf16(const char* ascii, uint16_t* utf16, int max_len) {
    int i = 0;
    while (i < max_len - 1 && ascii[i]) {
        utf16[i] = (uint16_t)ascii[i];
        i++;
    }
    utf16[i] = 0;
}

static void print_ascii(const char* str) {
    uint16_t buf[256];
    ascii_to_utf16(str, buf, 256);
    print(buf);
}

/**
 * Find ACPI RSDP in UEFI configuration tables
 */
static uint64_t find_rsdp(void) {
    if (!ST || !ST->ConfigurationTable) {
        return 0;
    }

    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE* table = &ST->ConfigurationTable[i];

        // Check for ACPI 2.0+ table (preferred)
        if (memcmp(&table->VendorGuid, &acpi2_table_guid, sizeof(EFI_GUID)) == 0) {
            return (uint64_t)table->VendorTable;
        }

        // Check for ACPI 1.0 table (fallback)
        if (memcmp(&table->VendorGuid, &acpi_table_guid, sizeof(EFI_GUID)) == 0) {
            return (uint64_t)table->VendorTable;
        }
    }

    return 0;
}

/**
 * Load file from filesystem
 */
static void* load_file(const uint16_t* path, uint64_t* size_out) {
    if (!root_fs) {
        return NULL;
    }

    // Open file
    EFI_FILE_PROTOCOL* file = NULL;
    EFI_STATUS status = root_fs->Open(root_fs, (void**)&file, (uint16_t*)path, 1, 0);
    if (status != EFI_SUCCESS) {
        return NULL;
    }

    // Get file size
    UINTN file_info_size = sizeof(EFI_FILE_INFO) + 512;
    EFI_FILE_INFO* file_info = NULL;
    status = BS->AllocatePool(EfiLoaderData, file_info_size, (void**)&file_info);
    if (status != EFI_SUCCESS) {
        file->Close(file);
        return NULL;
    }

    status = file->GetInfo(file, &file_info_guid, &file_info_size, file_info);
    if (status != EFI_SUCCESS) {
        BS->FreePool(file_info);
        file->Close(file);
        return NULL;
    }

    uint64_t file_size = file_info->FileSize;
    BS->FreePool(file_info);

    // Allocate buffer
    void* buffer = NULL;
    UINTN pages = (file_size + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS addr = 0;
    status = BS->AllocatePages(0, EfiLoaderData, pages, &addr);
    if (status != EFI_SUCCESS) {
        file->Close(file);
        return NULL;
    }

    buffer = (void*)addr;

    // Read file
    UINTN read_size = file_size;
    status = file->Read(file, &read_size, buffer);
    file->Close(file);

    if (status != EFI_SUCCESS) {
        BS->FreePages((EFI_PHYSICAL_ADDRESS)buffer, pages);
        return NULL;
    }

    if (size_out) {
        *size_out = file_size;
    }

    return buffer;
}

/**
 * Main bootloader entry point
 */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    ST = SystemTable;
    BS = SystemTable->BootServices;
    EFI_STATUS status;

    // Initialize boot info
    memset(&boot_info, 0, sizeof(boot_info));
    memset(memory_map, 0, sizeof(memory_map));

    boot_info.magic = BOOT_MAGIC;
    boot_info.version = BOOT_VERSION;
    ascii_to_utf16("AutomationOS UEFI Bootloader", (uint16_t*)boot_info.loader_name, 64);
    boot_info.loader_version = 0x00010000;  // v1.0.0

    // Clear screen
    ST->ConOut->ClearScreen(ST->ConOut);

    // ========================================
    // Phase 1: Initialize filesystem access
    // ========================================
    print(u"AutomationOS UEFI Bootloader v1.0\r\n");
    print(u"===================================\r\n\r\n");

    // Get loaded image protocol
    EFI_LOADED_IMAGE_PROTOCOL* loaded_image = NULL;
    status = BS->HandleProtocol(ImageHandle, &lip_guid, (void**)&loaded_image);
    if (status != EFI_SUCCESS) {
        print(u"ERROR: Failed to get loaded image protocol\r\n");
        return status;
    }

    // Get filesystem protocol
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
    status = BS->HandleProtocol(loaded_image->DeviceHandle, &sfs_guid, (void**)&fs);
    if (status != EFI_SUCCESS) {
        print(u"ERROR: Failed to get filesystem protocol\r\n");
        return status;
    }

    // Open root directory
    status = fs->OpenVolume(fs, &root_fs);
    if (status != EFI_SUCCESS) {
        print(u"ERROR: Failed to open root directory\r\n");
        return status;
    }

    // ========================================
    // Phase 2: Load and parse boot configuration
    // ========================================
    boot_config_t config;
    memset(&config, 0, sizeof(config));

    // Try to load boot.conf
    uint64_t config_size;
    char* config_data = (char*)load_file(u"\\EFI\\BOOT\\boot.conf", &config_size);

    boot_entry_t* selected_entry = NULL;

    if (config_data) {
        // Parse configuration
        if (boot_config_parse(config_data, config_size, &config) == 0 && config.entry_count > 0) {
            // Show boot menu
            selected_entry = boot_menu_show(&config, ST->ConOut, ST->ConIn);
        }
        BS->FreePool(config_data);
    }

    // If no config or menu canceled, use default entry
    if (!selected_entry) {
        // Create default entry
        static boot_entry_t default_entry;
        memset(&default_entry, 0, sizeof(default_entry));
        ascii_to_utf16("AutomationOS", (uint16_t*)default_entry.title, MAX_TITLE_LEN);
        ascii_to_utf16("\\EFI\\BOOT\\KERNEL.ELF", (uint16_t*)default_entry.kernel_path, MAX_PATH_LEN);
        ascii_to_utf16("\\EFI\\BOOT\\initrd.img", (uint16_t*)default_entry.initrd_path, MAX_PATH_LEN);
        selected_entry = &default_entry;
    }

    // Copy kernel command line
    int i = 0;
    while (i < MAX_OPTIONS_LEN - 1 && selected_entry->options[i]) {
        boot_info.cmdline[i] = selected_entry->options[i];
        i++;
    }
    boot_info.cmdline[i] = 0;

    // Show splash screen
    boot_splash_show(ST->ConOut, "Loading kernel...");

    // ========================================
    // Phase 3: Query system information
    // ========================================

    // Get memory map
    UINTN memory_map_size = 0;
    EFI_MEMORY_DESCRIPTOR* uefi_memory_map = NULL;
    UINTN map_key;
    UINTN descriptor_size;
    uint32_t descriptor_version;

    status = BS->GetMemoryMap(&memory_map_size, NULL, &map_key, &descriptor_size, &descriptor_version);
    if (status == EFI_BUFFER_TOO_SMALL) {
        memory_map_size += 2 * descriptor_size;
        status = BS->AllocatePool(EfiLoaderData, memory_map_size, (void**)&uefi_memory_map);
        if (status == EFI_SUCCESS) {
            status = BS->GetMemoryMap(&memory_map_size, uefi_memory_map, &map_key,
                                     &descriptor_size, &descriptor_version);
        }
    }

    // Convert memory map
    if (status == EFI_SUCCESS) {
        UINTN entry_count = memory_map_size / descriptor_size;
        uint32_t usable_count = 0;
        uint64_t total_memory = 0;

        for (UINTN i = 0; i < entry_count && usable_count < 256; i++) {
            EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)
                ((uint8_t*)uefi_memory_map + i * descriptor_size);

            uint64_t size = desc->NumberOfPages * 4096;
            total_memory += size;

            if (desc->Type == EfiConventionalMemory) {
                memory_map[usable_count].base = desc->PhysicalStart;
                memory_map[usable_count].length = size;
                memory_map[usable_count].type = MEMORY_TYPE_AVAILABLE;
                usable_count++;
            }
        }

        boot_info.memory_map = memory_map;
        boot_info.memory_map_count = usable_count;
        boot_info.total_memory = total_memory;
    }

    // Get graphics mode
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    status = BS->LocateProtocol(&gop_guid, NULL, (void**)&gop);
    if (status == EFI_SUCCESS && gop) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = gop->Mode->Info;
        boot_info.framebuffer_addr = gop->Mode->FrameBufferBase;
        boot_info.framebuffer_size = gop->Mode->FrameBufferSize;
        boot_info.framebuffer_width = info->HorizontalResolution;
        boot_info.framebuffer_height = info->VerticalResolution;
        boot_info.pixels_per_scanline = info->PixelsPerScanLine;
        boot_info.framebuffer_pitch = info->PixelsPerScanLine * 4;
        boot_info.framebuffer_bpp = 32;
    }

    // Find ACPI RSDP
    boot_info.rsdp_addr = find_rsdp();

    // ========================================
    // Phase 4: Load kernel
    // ========================================

    uint16_t kernel_path[MAX_PATH_LEN];
    ascii_to_utf16(selected_entry->kernel_path, kernel_path, MAX_PATH_LEN);

    uint64_t kernel_size;
    void* kernel_buffer = load_file(kernel_path, &kernel_size);
    if (!kernel_buffer) {
        print_ascii("ERROR: Failed to load kernel\r\n");
        return EFI_LOAD_ERROR;
    }

    // ========================================
    // Phase 5: Load initrd (if specified)
    // ========================================

    if (selected_entry->initrd_path[0]) {
        uint16_t initrd_path[MAX_PATH_LEN];
        ascii_to_utf16(selected_entry->initrd_path, initrd_path, MAX_PATH_LEN);

        uint64_t initrd_size;
        void* initrd_buffer = load_file(initrd_path, &initrd_size);
        if (initrd_buffer) {
            boot_info.initrd_addr = (uint64_t)initrd_buffer;
            boot_info.initrd_size = initrd_size;
        }
    }

    // ========================================
    // Phase 6: Parse and load ELF kernel
    // ========================================

    uint64_t entry_point;
    if (elf_load(kernel_buffer, &entry_point) != 0) {
        print_ascii("ERROR: Failed to load ELF kernel\r\n");
        return EFI_LOAD_ERROR;
    }

    boot_info.kernel_entry = (void*)entry_point;

    // ========================================
    // Phase 7: Setup page tables (required for higher-half kernel)
    // ========================================

    // Initialize paging subsystem with UEFI allocator
    paging_init((void*)BS->AllocatePages);

    // Setup page tables: identity map first 4GB + higher-half kernel mapping
    void* pml4 = setup_page_tables((uint64_t)kernel_buffer, kernel_size);
    if (!pml4) {
        efi_print(ConOut, u"WARNING: Page table setup failed, using static fallback\r\n");
    }

    // ========================================
    // Phase 8: Exit boot services and jump to kernel
    // ========================================

    // Final memory map query
    status = BS->GetMemoryMap(&memory_map_size, uefi_memory_map, &map_key,
                             &descriptor_size, &descriptor_version);

    // Exit boot services
    status = BS->ExitBootServices(ImageHandle, map_key);
    if (status != EFI_SUCCESS) {
        // Retry once
        BS->GetMemoryMap(&memory_map_size, uefi_memory_map, &map_key,
                        &descriptor_size, &descriptor_version);
        status = BS->ExitBootServices(ImageHandle, map_key);
        if (status != EFI_SUCCESS) {
            return status;
        }
    }

    // Load page tables into CR3 before jumping to higher-half kernel
    if (pml4) {
        __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)pml4) : "memory");
    }

    // Jump to kernel
    typedef void (*kernel_entry_t)(boot_info_t*);
    kernel_entry_t kernel_main = (kernel_entry_t)entry_point;
    kernel_main(&boot_info);

    // Should never reach here
    while (1) {
        __asm__ volatile("hlt");
    }

    return EFI_SUCCESS;
}

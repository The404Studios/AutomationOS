#include "boot.h"

// UEFI Table Header
typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

// UEFI Simple Text Output Protocol
typedef struct {
    void* _buf;
    EFI_STATUS (*OutputString)(void* This, uint16_t* String);
    void* _buf2[4];
    EFI_STATUS (*ClearScreen)(void* This);
    void* _buf3[2];
} SIMPLE_TEXT_OUTPUT_INTERFACE;

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
    void* _buf;
    EFI_STATUS (*QueryMode)(void* This, uint32_t ModeNumber, UINTN* SizeOfInfo,
                           EFI_GRAPHICS_OUTPUT_MODE_INFORMATION** Info);
    EFI_STATUS (*SetMode)(void* This, uint32_t ModeNumber);
    void* _buf2;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

// UEFI File Protocol
typedef struct {
    uint64_t Revision;
    EFI_STATUS (*Open)(void* This, void** NewHandle, uint16_t* FileName,
                      uint64_t OpenMode, uint64_t Attributes);
    EFI_STATUS (*Close)(void* This);
    void* _buf[2];
    EFI_STATUS (*Read)(void* This, UINTN* BufferSize, void* Buffer);
    void* _buf2[2];
    EFI_STATUS (*GetInfo)(void* This, EFI_GUID* InformationType,
                         UINTN* BufferSize, void* Buffer);
    void* _buf3[3];
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
    void* _buf;
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
    void* _buf[18];
    EFI_STATUS (*AllocatePages)(uint32_t Type, EFI_MEMORY_TYPE MemoryType,
                               UINTN Pages, EFI_PHYSICAL_ADDRESS* Memory);
    EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (*GetMemoryMap)(UINTN* MemoryMapSize, EFI_MEMORY_DESCRIPTOR* MemoryMap,
                              UINTN* MapKey, UINTN* DescriptorSize, uint32_t* DescriptorVersion);
    EFI_STATUS (*AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, void** Buffer);
    EFI_STATUS (*FreePool)(void* Buffer);
    void* _buf2[19];
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, EFI_GUID* Protocol, void** Interface);
    void* _buf3[3];
    void* _buf4[9];
    EFI_STATUS (*LocateProtocol)(EFI_GUID* Protocol, void* Registration, void** Interface);
    void* _buf5[2];
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
} EFI_BOOT_SERVICES;

// UEFI System Table
typedef struct {
    EFI_TABLE_HEADER Hdr;
    uint16_t* FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void* ConIn;
    EFI_HANDLE ConsoleOutHandle;
    SIMPLE_TEXT_OUTPUT_INTERFACE* ConOut;
    EFI_HANDLE StandardErrorHandle;
    SIMPLE_TEXT_OUTPUT_INTERFACE* StdErr;
    void* RuntimeServices;
    EFI_BOOT_SERVICES* BootServices;
    UINTN NumberOfTableEntries;
    void* ConfigurationTable;
} EFI_SYSTEM_TABLE;

// GUIDs
static EFI_GUID gop_guid = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
static EFI_GUID lip_guid = {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID sfs_guid = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static EFI_GUID file_info_guid = {0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

// Boot info
static boot_info_t boot_info;
static memory_map_entry_t memory_map[256];
static EFI_SYSTEM_TABLE* ST;
static EFI_BOOT_SERVICES* BS;

// Utility functions
void* memset(void* s, int c, unsigned long n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void* memcpy(void* dest, const void* src, unsigned long n) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    while (n--) *d++ = *s++;
    return dest;
}

void print(const uint16_t* str) {
    if (ST && ST->ConOut) {
        ST->ConOut->OutputString(ST->ConOut, (uint16_t*)str);
    }
}

// Main entry point
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    ST = SystemTable;
    BS = SystemTable->BootServices;
    EFI_STATUS status;

    // Clear boot info
    memset(&boot_info, 0, sizeof(boot_info));
    memset(memory_map, 0, sizeof(memory_map));

    print(u"AutomationOS UEFI Bootloader\r\n");
    print(u"=============================\r\n");

    // ========================================
    // TASK 1: Query UEFI Memory Map
    // ========================================
    print(u"[1/4] Querying memory map...\r\n");

    UINTN memory_map_size = 0;
    EFI_MEMORY_DESCRIPTOR* uefi_memory_map = NULL;
    UINTN map_key;
    UINTN descriptor_size;
    uint32_t descriptor_version;

    // First call to get size
    status = BS->GetMemoryMap(&memory_map_size, uefi_memory_map, &map_key,
                             &descriptor_size, &descriptor_version);

    if (status == EFI_BUFFER_TOO_SMALL) {
        // Add extra space for allocations
        memory_map_size += 2 * descriptor_size;

        // Allocate buffer
        status = BS->AllocatePool(EfiLoaderData, memory_map_size, (void**)&uefi_memory_map);
        if (status != EFI_SUCCESS) {
            print(u"ERROR: Failed to allocate memory map buffer\r\n");
            return status;
        }

        // Get actual memory map
        status = BS->GetMemoryMap(&memory_map_size, uefi_memory_map, &map_key,
                                 &descriptor_size, &descriptor_version);
        if (status != EFI_SUCCESS) {
            print(u"ERROR: Failed to get memory map\r\n");
            return status;
        }
    }

    // Convert UEFI memory map to our format (keep usable entries)
    UINTN entry_count = memory_map_size / descriptor_size;
    uint32_t usable_count = 0;

    for (UINTN i = 0; i < entry_count && usable_count < 256; i++) {
        EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)
            ((uint8_t*)uefi_memory_map + i * descriptor_size);

        // Only include usable memory types
        if (desc->Type == EfiConventionalMemory) {
            memory_map[usable_count].base = desc->PhysicalStart;
            memory_map[usable_count].length = desc->NumberOfPages * 4096;
            memory_map[usable_count].type = 1; // Usable
            usable_count++;
        }
    }

    boot_info.memory_map = memory_map;
    boot_info.memory_map_count = usable_count;
    print(u"  Memory map acquired: ");
    // TODO: print count
    print(u" usable regions\r\n");

    // ========================================
    // TASK 2: Setup Graphics Mode
    // ========================================
    print(u"[2/4] Setting up graphics...\r\n");

    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    status = BS->LocateProtocol(&gop_guid, NULL, (void**)&gop);

    if (status != EFI_SUCCESS || !gop) {
        print(u"WARNING: Graphics Output Protocol not found\r\n");
        // Set defaults
        boot_info.framebuffer_addr = 0xFD000000;
        boot_info.framebuffer_size = 0x300000;
        boot_info.framebuffer_width = 1024;
        boot_info.framebuffer_height = 768;
        boot_info.framebuffer_pitch = 1024 * 4;
        boot_info.pixels_per_scanline = 1024;
    } else {
        // Get current mode information
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = gop->Mode->Info;
        boot_info.framebuffer_addr = gop->Mode->FrameBufferBase;
        boot_info.framebuffer_size = gop->Mode->FrameBufferSize;
        boot_info.framebuffer_width = info->HorizontalResolution;
        boot_info.framebuffer_height = info->VerticalResolution;
        boot_info.pixels_per_scanline = info->PixelsPerScanLine;
        boot_info.framebuffer_pitch = info->PixelsPerScanLine * 4;

        print(u"  Graphics mode: ");
        // TODO: print resolution
        print(u"\r\n");
    }

    // ========================================
    // TASK 3: Load Kernel ELF
    // ========================================
    print(u"[3/4] Loading kernel...\r\n");

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
    EFI_FILE_PROTOCOL* root = NULL;
    status = fs->OpenVolume(fs, &root);
    if (status != EFI_SUCCESS) {
        print(u"ERROR: Failed to open root directory\r\n");
        return status;
    }

    // Open kernel file
    EFI_FILE_PROTOCOL* kernel_file = NULL;
    status = root->Open(root, (void**)&kernel_file, u"\\EFI\\BOOT\\KERNEL.ELF", 1, 0);
    if (status != EFI_SUCCESS) {
        print(u"ERROR: Failed to open kernel file\r\n");
        // Try alternate location
        status = root->Open(root, (void**)&kernel_file, u"\\KERNEL.ELF", 1, 0);
        if (status != EFI_SUCCESS) {
            print(u"ERROR: Kernel not found\r\n");
            return status;
        }
    }

    // Get file info for size
    UINTN file_info_size = sizeof(EFI_FILE_INFO) + 256;
    EFI_FILE_INFO* file_info = NULL;
    status = BS->AllocatePool(EfiLoaderData, file_info_size, (void**)&file_info);
    if (status != EFI_SUCCESS) {
        print(u"ERROR: Failed to allocate file info buffer\r\n");
        return status;
    }

    status = kernel_file->GetInfo(kernel_file, &file_info_guid, &file_info_size, file_info);
    if (status != EFI_SUCCESS) {
        print(u"ERROR: Failed to get file info\r\n");
        return status;
    }

    UINTN kernel_size = file_info->FileSize;
    print(u"  Kernel size: ");
    // TODO: print size
    print(u" bytes\r\n");

    // Free file info buffer (LEAK-002 fix)
    BS->FreePool(file_info);
    file_info = NULL;

    // Allocate buffer for kernel
    void* kernel_buffer = NULL;
    UINTN pages = (kernel_size + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS kernel_addr = 0;

    status = BS->AllocatePages(1, EfiLoaderData, pages, &kernel_addr);
    if (status != EFI_SUCCESS) {
        print(u"ERROR: Failed to allocate kernel buffer\r\n");
        return status;
    }

    kernel_buffer = (void*)kernel_addr;

    // Read kernel file
    status = kernel_file->Read(kernel_file, &kernel_size, kernel_buffer);
    if (status != EFI_SUCCESS) {
        print(u"ERROR: Failed to read kernel file\r\n");
        return status;
    }

    print(u"  Kernel loaded\r\n");

    // Close file
    kernel_file->Close(kernel_file);
    root->Close(root);

    // ========================================
    // TASK 4: Parse ELF and Load Segments
    // ========================================
    print(u"[4/4] Parsing ELF...\r\n");

    Elf64_Ehdr* elf = (Elf64_Ehdr*)kernel_buffer;

    // Verify ELF magic
    if (elf->e_ident[0] != 0x7F || elf->e_ident[1] != 'E' ||
        elf->e_ident[2] != 'L' || elf->e_ident[3] != 'F') {
        print(u"ERROR: Invalid ELF magic\r\n");
        return EFI_LOAD_ERROR;
    }

    // Verify 64-bit ELF
    if (elf->e_ident[4] != 2) {
        print(u"ERROR: Not a 64-bit ELF\r\n");
        return EFI_LOAD_ERROR;
    }

    // Load program headers
    Elf64_Phdr* phdr = (Elf64_Phdr*)((uint8_t*)kernel_buffer + elf->e_phoff);

    for (uint16_t i = 0; i < elf->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            // Allocate memory for segment
            UINTN seg_pages = (phdr[i].p_memsz + 4095) / 4096;
            EFI_PHYSICAL_ADDRESS seg_addr = phdr[i].p_paddr;

            status = BS->AllocatePages(1, EfiLoaderData, seg_pages, &seg_addr);
            if (status != EFI_SUCCESS) {
                print(u"ERROR: Failed to allocate segment memory\r\n");
                return status;
            }

            // Copy segment data
            memcpy((void*)seg_addr,
                   (uint8_t*)kernel_buffer + phdr[i].p_offset,
                   phdr[i].p_filesz);

            // Zero BSS if needed
            if (phdr[i].p_memsz > phdr[i].p_filesz) {
                memset((void*)(seg_addr + phdr[i].p_filesz), 0,
                       phdr[i].p_memsz - phdr[i].p_filesz);
            }
        }
    }

    uint64_t kernel_entry = elf->e_entry;
    boot_info.kernel_entry = (void*)kernel_entry;
    print(u"  Entry point: 0x");
    // TODO: print address
    print(u"\r\n");

    // Free kernel load buffer (LEAK-003 fix)
    // The kernel has been copied to proper addresses, we don't need the temp buffer
    BS->FreePages(kernel_addr, pages);
    kernel_buffer = NULL;
    print(u"  Freed temporary kernel buffer\r\n");

    // ========================================
    // Exit Boot Services and Jump to Kernel
    // ========================================
    print(u"\r\nExiting boot services...\r\n");

    status = BS->ExitBootServices(ImageHandle, map_key);
    if (status != EFI_SUCCESS) {
        print(u"ERROR: Failed to exit boot services\r\n");
        // Try refreshing memory map
        memory_map_size = 0;
        BS->GetMemoryMap(&memory_map_size, NULL, &map_key, &descriptor_size, &descriptor_version);
        memory_map_size += 2 * descriptor_size;
        status = BS->GetMemoryMap(&memory_map_size, uefi_memory_map, &map_key,
                                 &descriptor_size, &descriptor_version);
        status = BS->ExitBootServices(ImageHandle, map_key);
        if (status != EFI_SUCCESS) {
            return status;
        }
    }

    // We're now in a hostile environment - no more UEFI services

    // ========================================
    // Setup page tables: identity map + higher-half
    // ========================================
    // The kernel is linked at 0xFFFFFFFF80000000 (higher half).
    // We need page tables that map both:
    //   1. Identity: 0x0 -> 0x0 (first 4GB, for bootloader transition)
    //   2. Higher half: 0xFFFFFFFF80000000 -> physical kernel address
    // paging.c provides setup_page_tables() for this purpose.
    extern void paging_init(void* allocate_func);
    extern void* setup_page_tables(uint64_t kernel_phys, uint64_t kernel_size);

    // Note: paging_init was designed to use UEFI AllocatePages.
    // After ExitBootServices we cannot call UEFI, so we set up a simple
    // static page table instead using inline assembly.
    {
        // Use the physical addresses from the ELF load.
        // Build minimal page tables for the transition.
        // PML4, PDPT (low), PDPT (high), PD entries -- statically allocated
        // We use 2MB pages for simplicity.
        static uint64_t pml4_table[512] __attribute__((aligned(4096)));
        static uint64_t pdpt_low[512]   __attribute__((aligned(4096)));
        static uint64_t pdpt_high[512]  __attribute__((aligned(4096)));
        static uint64_t pd_table[512]   __attribute__((aligned(4096)));

        // Zero all tables
        for (int i = 0; i < 512; i++) {
            pml4_table[i] = 0;
            pdpt_low[i] = 0;
            pdpt_high[i] = 0;
            pd_table[i] = 0;
        }

        // Map first 1GB using 2MB pages in pd_table
        for (int i = 0; i < 512; i++) {
            // Each entry maps 2MB: present + write + page-size (2MB)
            pd_table[i] = ((uint64_t)i * 0x200000) | 0x83; // P + RW + PS
        }

        // PDPT low: entry 0 -> pd_table (covers 0..1GB)
        pdpt_low[0] = ((uint64_t)&pd_table[0]) | 0x03; // P + RW

        // For full 4GB identity map, add entries 1-3 as 1GB pages
        // (if CPU supports 1GB pages; otherwise use 2MB via more PDs)
        pdpt_low[1] = (1ULL * 0x40000000) | 0x83; // 1GB page
        pdpt_low[2] = (2ULL * 0x40000000) | 0x83; // 1GB page
        pdpt_low[3] = (3ULL * 0x40000000) | 0x83; // 1GB page

        // PDPT high: reuse pd_table for the higher-half mapping
        // 0xFFFFFFFF80000000 -> PML4 index 511, PDPT index 510
        pdpt_high[510] = ((uint64_t)&pd_table[0]) | 0x03; // P + RW

        // PML4: entry 0 -> pdpt_low (identity), entry 511 -> pdpt_high (higher half)
        pml4_table[0]   = ((uint64_t)&pdpt_low[0])  | 0x03; // P + RW
        pml4_table[511] = ((uint64_t)&pdpt_high[0]) | 0x03; // P + RW

        // Load new page tables into CR3
        __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)&pml4_table[0]) : "memory");
    }

    // Jump to kernel (entry point is the higher-half virtual address)
    typedef void (*kernel_entry_t)(boot_info_t*);
    kernel_entry_t kernel_main = (kernel_entry_t)kernel_entry;
    kernel_main(&boot_info);

    // Should never reach here
    while (1) {
        __asm__ volatile ("hlt");
    }

    return EFI_SUCCESS;
}

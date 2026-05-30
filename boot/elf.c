/**
 * ELF64 Loader
 * Parse and load ELF64 kernel images
 *
 * Total: ~600 LOC
 */

#include "boot_enhanced.h"

// String utilities
static void* memcpy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void* memset(void* dst, int c, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

// ELF information structure
typedef struct {
    uint64_t entry_point;
    uint64_t load_addr;
    uint64_t load_size;
    uint64_t mem_size;
    uint32_t segment_count;
    struct {
        uint64_t vaddr;
        uint64_t paddr;
        uint64_t filesz;
        uint64_t memsz;
        uint32_t flags;
    } segments[16];
} elf_info_t;

/**
 * Validate ELF header
 *
 * @param elf_data Pointer to ELF file data
 * @return 0 on success, -1 on error
 */
int elf_validate(const void* elf_data) {
    if (!elf_data) {
        return -1;
    }

    const Elf64_Ehdr* hdr = (const Elf64_Ehdr*)elf_data;

    // Check ELF magic
    if (hdr->e_ident[0] != 0x7F ||
        hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L' ||
        hdr->e_ident[3] != 'F') {
        return -1;
    }

    // Check 64-bit
    if (hdr->e_ident[4] != 2) {  // ELFCLASS64
        return -1;
    }

    // Check little-endian
    if (hdr->e_ident[5] != 1) {  // ELFDATA2LSB
        return -1;
    }

    // Check x86-64
    if (hdr->e_machine != 0x3E) {  // EM_X86_64
        return -1;
    }

    // Check executable
    if (hdr->e_type != 2) {  // ET_EXEC
        return -1;
    }

    return 0;
}

/**
 * Parse ELF headers
 *
 * @param elf_data Pointer to ELF file data
 * @param info Output ELF information
 * @return 0 on success, -1 on error
 */
int elf_parse(const void* elf_data, elf_info_t* info) {
    if (!elf_data || !info) {
        return -1;
    }

    if (elf_validate(elf_data) != 0) {
        return -1;
    }

    const Elf64_Ehdr* hdr = (const Elf64_Ehdr*)elf_data;

    // Extract entry point
    info->entry_point = hdr->e_entry;
    info->segment_count = 0;
    info->load_addr = 0;
    info->load_size = 0;
    info->mem_size = 0;

    // Parse program headers
    const Elf64_Phdr* phdr = (const Elf64_Phdr*)((const uint8_t*)elf_data + hdr->e_phoff);

    uint64_t min_addr = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t max_addr = 0;

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            // Store segment info
            if (info->segment_count < 16) {
                info->segments[info->segment_count].vaddr = phdr[i].p_vaddr;
                info->segments[info->segment_count].paddr = phdr[i].p_paddr;
                info->segments[info->segment_count].filesz = phdr[i].p_filesz;
                info->segments[info->segment_count].memsz = phdr[i].p_memsz;
                info->segments[info->segment_count].flags = phdr[i].p_flags;
                info->segment_count++;
            }

            // Track address range
            if (phdr[i].p_vaddr < min_addr) {
                min_addr = phdr[i].p_vaddr;
            }

            uint64_t seg_end = phdr[i].p_vaddr + phdr[i].p_memsz;
            if (seg_end > max_addr) {
                max_addr = seg_end;
            }
        }
    }

    info->load_addr = min_addr;
    info->mem_size = max_addr - min_addr;

    return 0;
}

/**
 * Load ELF segments into memory
 *
 * @param elf_data Pointer to ELF file data
 * @param info ELF information (from elf_parse)
 * @return 0 on success, -1 on error
 */
int elf_load_segments(const void* elf_data, const elf_info_t* info) {
    if (!elf_data || !info) {
        return -1;
    }

    const Elf64_Ehdr* hdr = (const Elf64_Ehdr*)elf_data;
    const Elf64_Phdr* phdr = (const Elf64_Phdr*)((const uint8_t*)elf_data + hdr->e_phoff);

    // Load each PT_LOAD segment
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            // Copy file contents
            const uint8_t* src = (const uint8_t*)elf_data + phdr[i].p_offset;
            uint8_t* dst = (uint8_t*)phdr[i].p_vaddr;

            memcpy(dst, src, phdr[i].p_filesz);

            // Zero BSS (if memsz > filesz)
            if (phdr[i].p_memsz > phdr[i].p_filesz) {
                uint64_t bss_size = phdr[i].p_memsz - phdr[i].p_filesz;
                memset(dst + phdr[i].p_filesz, 0, bss_size);
            }
        }
    }

    return 0;
}

/**
 * Complete ELF load: parse and load
 *
 * @param elf_data Pointer to ELF file data
 * @param entry_point Output: kernel entry point
 * @return 0 on success, -1 on error
 */
int elf_load(const void* elf_data, uint64_t* entry_point) {
    if (!elf_data || !entry_point) {
        return -1;
    }

    // Parse ELF
    elf_info_t info;
    if (elf_parse(elf_data, &info) != 0) {
        return -1;
    }

    // Load segments
    if (elf_load_segments(elf_data, &info) != 0) {
        return -1;
    }

    *entry_point = info.entry_point;
    return 0;
}

/**
 * Print ELF information (for debugging)
 *
 * @param info ELF information
 * @param print_func Function to print strings
 */
void elf_print_info(const elf_info_t* info, void (*print_func)(const char*)) {
    if (!info || !print_func) {
        return;
    }

    print_func("ELF Information:\n");

    // Entry point
    char buf[128];
    buf[0] = ' '; buf[1] = ' ';
    buf[2] = 'E'; buf[3] = 'n'; buf[4] = 't'; buf[5] = 'r'; buf[6] = 'y';
    buf[7] = ':'; buf[8] = ' '; buf[9] = '0'; buf[10] = 'x';

    // Convert entry point to hex
    uint64_t val = info->entry_point;
    for (int i = 0; i < 16; i++) {
        int digit = (val >> (60 - i * 4)) & 0xF;
        buf[11 + i] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
    }
    buf[27] = '\n';
    buf[28] = 0;
    print_func(buf);

    // Load address
    buf[2] = 'L'; buf[3] = 'o'; buf[4] = 'a'; buf[5] = 'd';
    buf[6] = ' '; buf[7] = 'A'; buf[8] = 'd'; buf[9] = 'd';
    buf[10] = 'r'; buf[11] = ':'; buf[12] = ' '; buf[13] = '0'; buf[14] = 'x';

    val = info->load_addr;
    for (int i = 0; i < 16; i++) {
        int digit = (val >> (60 - i * 4)) & 0xF;
        buf[15 + i] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
    }
    buf[31] = '\n';
    buf[32] = 0;
    print_func(buf);

    // Segment count
    print_func("  Segments: ");
    buf[0] = '0' + (info->segment_count / 10);
    buf[1] = '0' + (info->segment_count % 10);
    buf[2] = '\n';
    buf[3] = 0;
    print_func(buf);
}

/**
 * Get ELF info string
 *
 * @param elf_data Pointer to ELF file
 * @param buffer Output buffer
 * @param size Buffer size
 * @return 0 on success, -1 on error
 */
int elf_get_info_string(const void* elf_data, char* buffer, uint64_t size) {
    if (!elf_data || !buffer || size == 0) {
        return -1;
    }

    elf_info_t info;
    if (elf_parse(elf_data, &info) != 0) {
        return -1;
    }

    // Format: "Entry: 0xXXXX, Size: XXXX KB, Segments: X"
    char* p = buffer;
    const char* entry_str = "Entry: 0x";
    while (*entry_str && size > 1) {
        *p++ = *entry_str++;
        size--;
    }

    // Entry point (hex)
    uint64_t val = info.entry_point;
    for (int i = 0; i < 16 && size > 1; i++) {
        int digit = (val >> (60 - i * 4)) & 0xF;
        *p++ = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
        size--;
    }

    const char* size_str = ", Size: ";
    while (*size_str && size > 1) {
        *p++ = *size_str++;
        size--;
    }

    // Memory size (KB)
    uint64_t kb = info.mem_size / 1024;
    char num[32];
    int digits = 0;
    if (kb == 0) {
        num[digits++] = '0';
    } else {
        uint64_t temp = kb;
        while (temp > 0) {
            num[digits++] = '0' + (temp % 10);
            temp /= 10;
        }
    }

    // Reverse digits
    for (int i = digits - 1; i >= 0 && size > 1; i--) {
        *p++ = num[i];
        size--;
    }

    const char* kb_str = " KB, Segments: ";
    while (*kb_str && size > 1) {
        *p++ = *kb_str++;
        size--;
    }

    // Segment count
    if (size > 2) {
        *p++ = '0' + (info.segment_count / 10);
        *p++ = '0' + (info.segment_count % 10);
        size -= 2;
    }

    if (size > 0) {
        *p = 0;
    }

    return 0;
}

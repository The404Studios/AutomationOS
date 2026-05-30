#ifndef ELF_H
#define ELF_H

#include "types.h"

/**
 * ELF64 Loader
 * ============
 *
 * Loads ELF64 executables from initrd into user mode.
 */

// ELF64 identification indices
#define EI_MAG0       0  // 0x7F
#define EI_MAG1       1  // 'E'
#define EI_MAG2       2  // 'L'
#define EI_MAG3       3  // 'F'
#define EI_CLASS      4  // File class (1=32-bit, 2=64-bit)
#define EI_DATA       5  // Data encoding (1=LSB, 2=MSB)
#define EI_VERSION    6  // ELF version (1)
#define EI_OSABI      7  // OS/ABI identification
#define EI_ABIVERSION 8  // ABI version
#define EI_PAD        9  // Start of padding bytes
#define EI_NIDENT     16 // Size of e_ident[]

// ELF magic number
#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

// ELF class
#define ELFCLASS32 1  // 32-bit objects
#define ELFCLASS64 2  // 64-bit objects

// ELF data encoding
#define ELFDATA2LSB 1 // Little endian
#define ELFDATA2MSB 2 // Big endian

// ELF object file types
#define ET_NONE   0      // No file type
#define ET_REL    1      // Relocatable file
#define ET_EXEC   2      // Executable file
#define ET_DYN    3      // Shared object file
#define ET_CORE   4      // Core file

// ELF machine types
#define EM_NONE    0     // No machine
#define EM_386     3     // Intel 80386
#define EM_X86_64  62    // AMD x86-64 architecture

// ELF version
#define EV_NONE    0     // Invalid version
#define EV_CURRENT 1     // Current version

// Program header types
#define PT_NULL    0     // Unused entry
#define PT_LOAD    1     // Loadable segment
#define PT_DYNAMIC 2     // Dynamic linking information
#define PT_INTERP  3     // Interpreter pathname
#define PT_NOTE    4     // Auxiliary information
#define PT_SHLIB   5     // Reserved
#define PT_PHDR    6     // Program header table
#define PT_TLS     7     // Thread-local storage

// Program header flags
#define PF_X       0x1   // Execute
#define PF_W       0x2   // Write
#define PF_R       0x4   // Read

// ELF64 header structure
typedef struct {
    uint8_t  e_ident[EI_NIDENT];  // Magic number and other info
    uint16_t e_type;               // Object file type
    uint16_t e_machine;            // Architecture
    uint32_t e_version;            // Object file version
    uint64_t e_entry;              // Entry point virtual address
    uint64_t e_phoff;              // Program header table file offset
    uint64_t e_shoff;              // Section header table file offset
    uint32_t e_flags;              // Processor-specific flags
    uint16_t e_ehsize;             // ELF header size in bytes
    uint16_t e_phentsize;          // Program header table entry size
    uint16_t e_phnum;              // Program header table entry count
    uint16_t e_shentsize;          // Section header table entry size
    uint16_t e_shnum;              // Section header table entry count
    uint16_t e_shstrndx;           // Section header string table index
} PACKED elf64_ehdr_t;

// ELF64 program header structure
typedef struct {
    uint32_t p_type;               // Segment type
    uint32_t p_flags;              // Segment flags
    uint64_t p_offset;             // Segment file offset
    uint64_t p_vaddr;              // Segment virtual address
    uint64_t p_paddr;              // Segment physical address
    uint64_t p_filesz;             // Segment size in file
    uint64_t p_memsz;              // Segment size in memory
    uint64_t p_align;              // Segment alignment
} PACKED elf64_phdr_t;

/**
 * Load ELF64 executable from initrd
 *
 * @param path Path to ELF file in initrd
 * @param argc Argument count
 * @param argv Argument vector
 * @param entry_out Output: entry point address
 * @param stack_out Output: initial stack pointer
 * @return 0 on success, negative error code on failure
 */
int elf_load(const char* path, int argc, char** argv,
             uint64_t* entry_out, uint64_t* stack_out);

/**
 * Validate ELF64 header
 *
 * @param ehdr ELF header to validate
 * @return 1 if valid, 0 if invalid
 */
int elf_validate_header(const elf64_ehdr_t* ehdr);

// Error codes
#define ELF_SUCCESS       0
#define ELF_ERR_NOT_FOUND -1  // File not found in initrd
#define ELF_ERR_INVALID   -2  // Invalid ELF format
#define ELF_ERR_ARCH      -3  // Wrong architecture
#define ELF_ERR_NOMEM     -4  // Out of memory
#define ELF_ERR_PERM      -5  // Permission denied

/**
 * Print ELF file information (debugging)
 *
 * @param path Path to ELF file in initrd
 */
void elf_print_info(const char* path);

/**
 * Run ELF loader test suite
 */
void elf_run_tests(void);

/**
 * Load and execute ELF from memory buffer
 *
 * Takes raw ELF binary data (e.g., extracted from initrd TAR),
 * creates a process, loads the ELF into it, and adds it to the scheduler.
 *
 * @param elf_data Pointer to raw ELF binary in memory
 * @param elf_size Size of ELF binary in bytes
 * @param name Process name (for display purposes)
 * @return PID on success, negative error code on failure
 */
int elf_load_and_exec(void* elf_data, size_t elf_size, const char* name);

#endif // ELF_H

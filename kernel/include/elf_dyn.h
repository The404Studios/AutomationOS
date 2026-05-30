#ifndef ELF_DYN_H
#define ELF_DYN_H

#include "types.h"

/**
 * ELF64 Dynamic Linking Structures
 * =================================
 *
 * Defines structures and constants for ELF dynamic linking,
 * including dynamic section entries, symbol tables, relocation
 * entries, and auxiliary vectors.
 */

// Section header types
#define SHT_NULL          0
#define SHT_PROGBITS      1
#define SHT_SYMTAB        2
#define SHT_STRTAB        3
#define SHT_RELA          4
#define SHT_HASH          5
#define SHT_DYNAMIC       6
#define SHT_NOTE          7
#define SHT_NOBITS        8
#define SHT_REL           9
#define SHT_SHLIB         10
#define SHT_DYNSYM        11
#define SHT_GNU_HASH      0x6ffffff6

// Dynamic section entry tags
#define DT_NULL           0   // Marks end of dynamic section
#define DT_NEEDED         1   // Name of needed library
#define DT_PLTRELSZ       2   // Size in bytes of PLT relocs
#define DT_PLTGOT         3   // Processor-defined value (GOT address)
#define DT_HASH           4   // Address of symbol hash table
#define DT_STRTAB         5   // Address of string table
#define DT_SYMTAB         6   // Address of symbol table
#define DT_RELA           7   // Address of Rela relocs
#define DT_RELASZ         8   // Total size of Rela relocs
#define DT_RELAENT        9   // Size of one Rela reloc
#define DT_STRSZ          10  // Size of string table
#define DT_SYMENT         11  // Size of one symbol table entry
#define DT_INIT           12  // Address of init function
#define DT_FINI           13  // Address of termination function
#define DT_SONAME         14  // Name of shared object
#define DT_RPATH          15  // Library search path (deprecated)
#define DT_SYMBOLIC       16  // Start symbol search here
#define DT_REL            17  // Address of Rel relocs
#define DT_RELSZ          18  // Total size of Rel relocs
#define DT_RELENT         19  // Size of one Rel reloc
#define DT_PLTREL         20  // Type of reloc in PLT
#define DT_DEBUG          21  // For debugging
#define DT_TEXTREL        22  // Reloc might modify .text
#define DT_JMPREL         23  // Address of PLT relocs
#define DT_BIND_NOW       24  // Process relocations of object
#define DT_INIT_ARRAY     25  // Array with addresses of init fct
#define DT_FINI_ARRAY     26  // Array with addresses of fini fct
#define DT_INIT_ARRAYSZ   27  // Size in bytes of DT_INIT_ARRAY
#define DT_FINI_ARRAYSZ   28  // Size in bytes of DT_FINI_ARRAY
#define DT_RUNPATH        29  // Library search path
#define DT_FLAGS          30  // Flags for the object being loaded
#define DT_ENCODING       32  // Start of encoded range
#define DT_PREINIT_ARRAY  32  // Array with addresses of preinit fct
#define DT_PREINIT_ARRAYSZ 33 // size in bytes of DT_PREINIT_ARRAY
#define DT_GNU_HASH       0x6ffffef5 // GNU-style hash table
#define DT_VERSYM         0x6ffffff0
#define DT_RELACOUNT      0x6ffffff9
#define DT_RELCOUNT       0x6ffffffa
#define DT_FLAGS_1        0x6ffffffb // State flags
#define DT_VERDEF         0x6ffffffc // Address of version definition table
#define DT_VERDEFNUM      0x6ffffffd // Number of version definitions
#define DT_VERNEED        0x6ffffffe // Address of version dependency table
#define DT_VERNEEDNUM     0x6fffffff // Number of version dependencies

// DT_FLAGS values
#define DF_ORIGIN         0x00000001
#define DF_SYMBOLIC       0x00000002
#define DF_TEXTREL        0x00000004
#define DF_BIND_NOW       0x00000008
#define DF_STATIC_TLS     0x00000010

// DT_FLAGS_1 values
#define DF_1_NOW          0x00000001
#define DF_1_GLOBAL       0x00000002
#define DF_1_GROUP        0x00000004
#define DF_1_NODELETE     0x00000008
#define DF_1_PIE          0x08000000

// Symbol binding types
#define STB_LOCAL         0   // Local symbol
#define STB_GLOBAL        1   // Global symbol
#define STB_WEAK          2   // Weak symbol
#define STB_GNU_UNIQUE    10  // Unique symbol

// Symbol types
#define STT_NOTYPE        0   // Symbol type is unspecified
#define STT_OBJECT        1   // Symbol is a data object
#define STT_FUNC          2   // Symbol is a code object
#define STT_SECTION       3   // Symbol associated with a section
#define STT_FILE          4   // Symbol's name is file name
#define STT_COMMON        5   // Symbol is a common data object
#define STT_TLS           6   // Symbol is thread-local data object
#define STT_GNU_IFUNC     10  // Symbol is indirect code object

// Symbol visibility
#define STV_DEFAULT       0   // Default symbol visibility rules
#define STV_INTERNAL      1   // Processor specific hidden class
#define STV_HIDDEN        2   // Sym unavailable in other modules
#define STV_PROTECTED     3   // Not preemptible, not exported

// Special section indices
#define SHN_UNDEF         0
#define SHN_ABS           0xfff1
#define SHN_COMMON        0xfff2

// Relocation types for x86-64
#define R_X86_64_NONE           0  // No relocation
#define R_X86_64_64             1  // Direct 64 bit
#define R_X86_64_PC32           2  // PC relative 32 bit signed
#define R_X86_64_GOT32          3  // 32 bit GOT entry
#define R_X86_64_PLT32          4  // 32 bit PLT address
#define R_X86_64_COPY           5  // Copy symbol at runtime
#define R_X86_64_GLOB_DAT       6  // Create GOT entry
#define R_X86_64_JUMP_SLOT      7  // Create PLT entry
#define R_X86_64_RELATIVE       8  // Adjust by program base
#define R_X86_64_GOTPCREL       9  // 32 bit signed PC relative offset to GOT
#define R_X86_64_32             10 // Direct 32 bit zero extended
#define R_X86_64_32S            11 // Direct 32 bit sign extended
#define R_X86_64_16             12 // Direct 16 bit zero extended
#define R_X86_64_PC16           13 // 16 bit sign extended pc relative
#define R_X86_64_8              14 // Direct 8 bit sign extended
#define R_X86_64_PC8            15 // 8 bit sign extended pc relative
#define R_X86_64_DTPMOD64       16 // ID of module containing symbol
#define R_X86_64_DTPOFF64       17 // Offset in module's TLS block
#define R_X86_64_TPOFF64        18 // Offset in initial TLS block
#define R_X86_64_TLSGD          19 // 32 bit signed PC relative offset to two GOT entries for GD symbol
#define R_X86_64_TLSLD          20 // 32 bit signed PC relative offset to two GOT entries for LD symbol
#define R_X86_64_DTPOFF32       21 // Offset in TLS block
#define R_X86_64_GOTTPOFF       22 // 32 bit signed PC relative offset to GOT entry for IE symbol
#define R_X86_64_TPOFF32        23 // Offset in initial TLS block
#define R_X86_64_PC64           24 // PC relative 64 bit
#define R_X86_64_GOTOFF64       25 // 64 bit offset to GOT
#define R_X86_64_GOTPC32        26 // 32 bit signed pc relative offset to GOT
#define R_X86_64_SIZE32         32 // Size of symbol plus 32-bit addend
#define R_X86_64_SIZE64         33 // Size of symbol plus 64-bit addend
#define R_X86_64_IRELATIVE      37 // Adjust indirectly by program base

// ELF64 dynamic section entry
typedef struct {
    int64_t  d_tag;     // Dynamic entry type
    union {
        uint64_t d_val; // Integer value
        uint64_t d_ptr; // Address value
    } d_un;
} elf64_dyn_t;

// ELF64 symbol table entry
typedef struct {
    uint32_t st_name;   // Symbol name (string tbl index)
    uint8_t  st_info;   // Symbol type and binding
    uint8_t  st_other;  // Symbol visibility
    uint16_t st_shndx;  // Section index
    uint64_t st_value;  // Symbol value
    uint64_t st_size;   // Symbol size
} elf64_sym_t;

// ELF64 relocation entries
typedef struct {
    uint64_t r_offset;  // Address
    uint64_t r_info;    // Relocation type and symbol index
} elf64_rel_t;

typedef struct {
    uint64_t r_offset;  // Address
    uint64_t r_info;    // Relocation type and symbol index
    int64_t  r_addend;  // Addend
} elf64_rela_t;

// ELF64 section header
typedef struct {
    uint32_t sh_name;      // Section name (string tbl index)
    uint32_t sh_type;      // Section type
    uint64_t sh_flags;     // Section flags
    uint64_t sh_addr;      // Section virtual addr at execution
    uint64_t sh_offset;    // Section file offset
    uint64_t sh_size;      // Section size in bytes
    uint32_t sh_link;      // Link to another section
    uint32_t sh_info;      // Additional section information
    uint64_t sh_addralign; // Section alignment
    uint64_t sh_entsize;   // Entry size if section holds table
} elf64_shdr_t;

// Macros for manipulating symbol info
#define ELF64_ST_BIND(info)          ((info) >> 4)
#define ELF64_ST_TYPE(info)          ((info) & 0xf)
#define ELF64_ST_INFO(bind, type)    (((bind) << 4) + ((type) & 0xf))
#define ELF64_ST_VISIBILITY(other)   ((other) & 0x3)

// Macros for manipulating relocation info
#define ELF64_R_SYM(info)            ((info) >> 32)
#define ELF64_R_TYPE(info)           ((info) & 0xffffffff)
#define ELF64_R_INFO(sym, type)      (((uint64_t)(sym) << 32) + (type))

// Auxiliary vector types (for passing info from kernel to ld.so)
#define AT_NULL         0   // End of vector
#define AT_IGNORE       1   // Entry should be ignored
#define AT_EXECFD       2   // File descriptor of program
#define AT_PHDR         3   // Program headers for program
#define AT_PHENT        4   // Size of program header entry
#define AT_PHNUM        5   // Number of program headers
#define AT_PAGESZ       6   // System page size
#define AT_BASE         7   // Base address of interpreter
#define AT_FLAGS        8   // Flags
#define AT_ENTRY        9   // Entry point of program
#define AT_NOTELF       10  // Program is not ELF
#define AT_UID          11  // Real uid
#define AT_EUID         12  // Effective uid
#define AT_GID          13  // Real gid
#define AT_EGID         14  // Effective gid
#define AT_PLATFORM     15  // String identifying platform
#define AT_HWCAP        16  // Machine dependent hints
#define AT_CLKTCK       17  // Frequency of times()
#define AT_SECURE       23  // Secure mode boolean
#define AT_BASE_PLATFORM 24 // String identifying real platform
#define AT_RANDOM       25  // Address of 16 random bytes
#define AT_HWCAP2       26  // Extension of AT_HWCAP
#define AT_EXECFN       31  // Filename of executable

typedef struct {
    uint64_t a_type;   // Entry type
    union {
        uint64_t a_val;
    } a_un;
} elf64_auxv_t;

#endif // ELF_DYN_H

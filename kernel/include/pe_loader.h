/**
 * PE Loader for AutomationOS
 *
 * Native Windows PE file format loading and execution.
 * Supports .exe, .dll, and dynamic linking.
 */

#ifndef PE_LOADER_H
#define PE_LOADER_H

#include "types.h"

// PE signature and magic numbers
#define PE_SIGNATURE 0x00004550  // "PE\0\0"
#define DOS_SIGNATURE 0x5A4D      // "MZ"
#define PE32_MAGIC 0x010B
#define PE32PLUS_MAGIC 0x020B

// Machine types
#define IMAGE_FILE_MACHINE_I386   0x014C
#define IMAGE_FILE_MACHINE_AMD64  0x8664
#define IMAGE_FILE_MACHINE_ARM64  0xAA64

// Characteristics
#define IMAGE_FILE_EXECUTABLE_IMAGE      0x0002
#define IMAGE_FILE_LARGE_ADDRESS_AWARE   0x0020
#define IMAGE_FILE_DLL                   0x2000

// Section characteristics
#define IMAGE_SCN_CNT_CODE               0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA   0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_MEM_EXECUTE            0x20000000
#define IMAGE_SCN_MEM_READ               0x40000000
#define IMAGE_SCN_MEM_WRITE              0x80000000

// DLL process attach/detach
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3
#define DLL_PROCESS_DETACH 0

// Maximum limits
#define PE_MAX_SECTIONS 96
#define PE_MAX_IMPORTS 256
#define PE_MAX_DLL_NAME 256

/**
 * DOS Header (MZ header)
 */
typedef struct {
    uint16_t e_magic;      // "MZ"
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;     // Offset to PE header
} __attribute__((packed)) dos_header_t;

/**
 * COFF File Header
 */
typedef struct {
    uint16_t machine;
    uint16_t number_of_sections;
    uint32_t time_date_stamp;
    uint32_t pointer_to_symbol_table;
    uint32_t number_of_symbols;
    uint16_t size_of_optional_header;
    uint16_t characteristics;
} __attribute__((packed)) coff_header_t;

/**
 * Data Directory
 */
typedef struct {
    uint32_t virtual_address;
    uint32_t size;
} __attribute__((packed)) data_directory_t;

#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16

// Directory entry indices
#define IMAGE_DIRECTORY_ENTRY_EXPORT         0
#define IMAGE_DIRECTORY_ENTRY_IMPORT         1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE       2
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION      3
#define IMAGE_DIRECTORY_ENTRY_SECURITY       4
#define IMAGE_DIRECTORY_ENTRY_BASERELOC      5
#define IMAGE_DIRECTORY_ENTRY_DEBUG          6
#define IMAGE_DIRECTORY_ENTRY_ARCHITECTURE   7
#define IMAGE_DIRECTORY_ENTRY_GLOBALPTR      8
#define IMAGE_DIRECTORY_ENTRY_TLS            9
#define IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG    10
#define IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT   11
#define IMAGE_DIRECTORY_ENTRY_IAT            12
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT   13
#define IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR 14

/**
 * Optional Header (PE32+)
 */
typedef struct {
    uint16_t magic;
    uint8_t  major_linker_version;
    uint8_t  minor_linker_version;
    uint32_t size_of_code;
    uint32_t size_of_initialized_data;
    uint32_t size_of_uninitialized_data;
    uint32_t address_of_entry_point;
    uint32_t base_of_code;

    // PE32+ specific (64-bit)
    uint64_t image_base;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint16_t major_os_version;
    uint16_t minor_os_version;
    uint16_t major_image_version;
    uint16_t minor_image_version;
    uint16_t major_subsystem_version;
    uint16_t minor_subsystem_version;
    uint32_t win32_version_value;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t checksum;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint64_t size_of_stack_reserve;
    uint64_t size_of_stack_commit;
    uint64_t size_of_heap_reserve;
    uint64_t size_of_heap_commit;
    uint32_t loader_flags;
    uint32_t number_of_rva_and_sizes;
    data_directory_t data_directory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} __attribute__((packed)) optional_header_t;

/**
 * Section Header
 */
typedef struct {
    char     name[8];
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t size_of_raw_data;
    uint32_t pointer_to_raw_data;
    uint32_t pointer_to_relocations;
    uint32_t pointer_to_line_numbers;
    uint16_t number_of_relocations;
    uint16_t number_of_line_numbers;
    uint32_t characteristics;
} __attribute__((packed)) section_header_t;

/**
 * Import Descriptor
 */
typedef struct {
    uint32_t original_first_thunk;  // RVA to import name table
    uint32_t time_date_stamp;
    uint32_t forwarder_chain;
    uint32_t name;                  // RVA to DLL name
    uint32_t first_thunk;           // RVA to import address table
} __attribute__((packed)) import_descriptor_t;

/**
 * Export Directory
 */
typedef struct {
    uint32_t characteristics;
    uint32_t time_date_stamp;
    uint16_t major_version;
    uint16_t minor_version;
    uint32_t name;
    uint32_t base;
    uint32_t number_of_functions;
    uint32_t number_of_names;
    uint32_t address_of_functions;
    uint32_t address_of_names;
    uint32_t address_of_name_ordinals;
} __attribute__((packed)) export_directory_t;

/**
 * Base Relocation Block
 */
typedef struct {
    uint32_t virtual_address;
    uint32_t size_of_block;
} __attribute__((packed)) base_relocation_t;

// Relocation types
#define IMAGE_REL_BASED_ABSOLUTE       0
#define IMAGE_REL_BASED_HIGH           1
#define IMAGE_REL_BASED_LOW            2
#define IMAGE_REL_BASED_HIGHLOW        3
#define IMAGE_REL_BASED_HIGHADJ        4
#define IMAGE_REL_BASED_DIR64          10

/**
 * TLS Directory
 */
typedef struct {
    uint64_t start_address_of_raw_data;
    uint64_t end_address_of_raw_data;
    uint64_t address_of_index;
    uint64_t address_of_callbacks;
    uint32_t size_of_zero_fill;
    uint32_t characteristics;
} __attribute__((packed)) tls_directory_t;

/**
 * Parsed PE File Structure
 */
typedef struct pe_file {
    // File data
    void *file_data;
    size_t file_size;

    // Headers
    dos_header_t *dos_header;
    uint32_t pe_signature;
    coff_header_t *coff_header;
    optional_header_t *optional_header;

    // Sections
    section_header_t *sections[PE_MAX_SECTIONS];
    uint32_t section_count;

    // Imports
    import_descriptor_t *import_descriptors;
    uint32_t import_count;

    // Exports
    export_directory_t *export_directory;
    uint32_t *export_functions;
    uint32_t *export_names;
    uint16_t *export_ordinals;

    // Relocations
    base_relocation_t *relocations;

    // TLS
    tls_directory_t *tls_directory;

    // Image info
    uint64_t entry_point;
    uint64_t image_base;
    uint32_t image_size;
    bool is_dll;

    // Loaded image
    void *loaded_base;
    bool is_loaded;
} pe_file_t;

/**
 * DLL Handle
 */
typedef struct dll_handle {
    pe_file_t *pe;
    void *base;
    char name[PE_MAX_DLL_NAME];
    uint32_t ref_count;
    struct dll_handle *next;
} dll_handle_t;

// DllMain function pointer type
typedef int (*dll_main_t)(void *instance, uint32_t reason, void *reserved);

/**
 * PE Loader Functions
 */

// Parse PE file from memory or disk
pe_file_t* pe_parse(const void *data, size_t size);
pe_file_t* pe_parse_file(const char *path);

// Free PE structure
void pe_free(pe_file_t *pe);

// Load and execute PE file
int pe_load_and_execute(const char *exe_path);

// Load PE into memory
int pe_load(pe_file_t *pe, void *preferred_base);

// Map sections
int pe_map_sections(pe_file_t *pe);

// Resolve imports
int pe_resolve_imports(pe_file_t *pe);

// Apply relocations
int pe_apply_relocations(pe_file_t *pe, void *new_base);

// Get RVA as pointer
void* pe_rva_to_ptr(pe_file_t *pe, uint32_t rva);

// Section lookup
section_header_t* pe_find_section(pe_file_t *pe, const char *name);
section_header_t* pe_rva_to_section(pe_file_t *pe, uint32_t rva);

/**
 * DLL Loader Functions
 */

// Initialize DLL subsystem
void dll_init(void);

// Load DLL
dll_handle_t* dll_load(const char *dll_name);

// Free DLL
void dll_free(dll_handle_t *dll);

// Get procedure address
void* dll_get_proc_address(dll_handle_t *dll, const char *func_name);
void* dll_get_proc_by_ordinal(dll_handle_t *dll, uint16_t ordinal);

// DLL search
char* dll_find_path(const char *dll_name);

/**
 * Utility Functions
 */

// Validate PE file
bool pe_validate(const void *data, size_t size);

// Get section protection flags
int pe_section_to_prot(uint32_t characteristics);

// Print PE info (debug)
void pe_print_info(pe_file_t *pe);

#endif // PE_LOADER_H

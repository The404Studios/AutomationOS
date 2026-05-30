/**
 * Dynamic Linker Header
 * ======================
 *
 * Main header file for the AutomationOS dynamic linker (ld.so).
 * Defines data structures and function prototypes for ELF dynamic linking.
 */

#ifndef LINKER_H
#define LINKER_H

#include <stdint.h>
#include <stddef.h>

// Include ELF definitions
#include "../../kernel/include/elf.h"
#include "../../kernel/include/elf_dyn.h"

// Configuration
#define MAX_SHARED_OBJECTS  64   // Maximum number of loaded shared objects
#define MAX_DEPENDENCIES    32   // Maximum dependencies per object
#define MAX_SEARCH_PATHS    16   // Maximum library search paths
#define MAX_PATH_LENGTH     256  // Maximum path length

// Relocation modes
#define RTLD_LAZY    1  // Lazy symbol binding
#define RTLD_NOW     2  // Immediate symbol binding
#define RTLD_GLOBAL  4  // Make symbols available for subsequently loaded objects
#define RTLD_LOCAL   8  // Symbols not available for symbol resolution of subsequently loaded objects

// Function pointer types for init/fini
typedef void (*init_func_t)(void);
typedef void (*fini_func_t)(void);

/**
 * Shared Object Structure
 *
 * Represents a loaded shared library or executable.
 */
typedef struct shared_object {
    char name[MAX_PATH_LENGTH];     // Object name (filename)
    char path[MAX_PATH_LENGTH];     // Full path to file
    uint64_t base_addr;              // Base load address

    // ELF header and program headers
    elf64_ehdr_t* ehdr;              // ELF header
    elf64_phdr_t* phdr;              // Program headers

    // Dynamic section info
    elf64_dyn_t* dynamic;            // PT_DYNAMIC segment
    char* strtab;                    // String table
    elf64_sym_t* symtab;             // Symbol table
    uint32_t* hash;                  // ELF hash table
    uint32_t* gnu_hash;              // GNU hash table

    size_t strtab_size;              // String table size
    size_t symtab_entsize;           // Symbol table entry size
    size_t num_symbols;              // Number of symbols

    // Relocations
    elf64_rela_t* rela;              // RELA relocations
    size_t relasz;                   // Size of RELA section
    size_t relaent;                  // Size of one RELA entry

    elf64_rel_t* rel;                // REL relocations
    size_t relsz;                    // Size of REL section
    size_t relent;                   // Size of one REL entry

    // PLT/GOT
    uint64_t* pltgot;                // PLT/GOT table
    size_t pltrelsz;                 // Size of PLT relocations
    uint32_t pltrel;                 // Type of PLT relocation (DT_REL or DT_RELA)
    void* jmprel;                    // PLT relocation table

    // Init/fini functions
    init_func_t init;                // DT_INIT function
    fini_func_t fini;                // DT_FINI function
    init_func_t* init_array;         // DT_INIT_ARRAY
    size_t init_array_size;          // DT_INIT_ARRAYSZ
    fini_func_t* fini_array;         // DT_FINI_ARRAY
    size_t fini_array_size;          // DT_FINI_ARRAYSZ

    // Dependencies
    uint64_t needed_names[MAX_DEPENDENCIES];  // DT_NEEDED string table offsets
    char* needed[MAX_DEPENDENCIES];           // DT_NEEDED strings
    struct shared_object* deps[MAX_DEPENDENCIES];  // Resolved dependency pointers
    int num_needed;                  // Number of DT_NEEDED entries
    int num_deps;                    // Number of resolved dependencies

    // Library paths
    uint64_t soname_offset;          // DT_SONAME string table offset
    uint64_t rpath_offset;           // DT_RPATH string table offset
    uint64_t runpath_offset;         // DT_RUNPATH string table offset
    char* soname;                    // DT_SONAME
    char* rpath;                     // DT_RPATH
    char* runpath;                   // DT_RUNPATH

    // Flags
    uint64_t flags;                  // DT_FLAGS
    uint64_t flags_1;                // DT_FLAGS_1
    int bind_now;                    // DT_BIND_NOW flag

    // State
    int loaded;                      // Object has been loaded into memory
    int relocated;                   // Relocations have been applied
    int initialized;                 // Init functions have been called

    // Reference counting (for dlopen/dlclose)
    int refcount;

    // Memory mapping info
    void* load_addr;                 // Start of mapped region
    size_t load_size;                // Size of mapped region
} shared_object_t;

/**
 * Symbol Information
 *
 * Result of symbol lookup.
 */
typedef struct {
    shared_object_t* object;         // Object containing the symbol
    elf64_sym_t* symbol;             // Symbol table entry
} symbol_info_t;

/**
 * Linker Context
 *
 * Global state for the dynamic linker.
 */
typedef struct {
    shared_object_t* main_object;    // Main executable
    shared_object_t* objects[MAX_SHARED_OBJECTS];  // All loaded objects
    int num_objects;                 // Number of loaded objects

    char search_paths[MAX_SEARCH_PATHS][MAX_PATH_LENGTH];  // Library search paths
    int num_search_paths;

    // Global symbol table (for RTLD_GLOBAL objects)
    // For simplicity, we search objects in load order

    int debug;                       // Debug mode flag
} linker_context_t;

// Global linker context (for resolver access)
extern linker_context_t* g_linker_ctx;

// === Function Prototypes ===

// linker.c - Main linker functions
int linker_init(linker_context_t* ctx);
shared_object_t* linker_load_object(linker_context_t* ctx, const char* path, int mode);
int linker_load_dependencies(linker_context_t* ctx, shared_object_t* so, int mode);
int linker_relocate_all(linker_context_t* ctx, int mode);
int linker_run_initializers(linker_context_t* ctx);
int linker_run_finalizers(linker_context_t* ctx);
void linker_debug(const char* fmt, ...);

// elf_dyn.c - Dynamic section parsing
int elf_parse_dynamic(shared_object_t* so, elf64_dyn_t* dynamic_segment, uint64_t base_addr);
elf64_dyn_t* elf_find_dynamic_segment(elf64_ehdr_t* ehdr, uint64_t base_addr);
int elf_validate_dynamic(shared_object_t* so);

// symbol_resolution.c - Symbol lookup
elf64_sym_t* linker_lookup_symbol_in_object(shared_object_t* so, const char* name);
symbol_info_t* linker_lookup_symbol_global(linker_context_t* ctx, const char* name, int skip_main);
symbol_info_t* linker_lookup_symbol_in_deps(linker_context_t* ctx, shared_object_t* so, const char* name);
uint64_t linker_get_symbol_addr(shared_object_t* so, elf64_sym_t* sym);
int linker_symbol_is_weak(elf64_sym_t* sym);
int linker_symbol_is_global(elf64_sym_t* sym);

// relocation.c - Relocation processing
int linker_relocate_object(linker_context_t* ctx, shared_object_t* so, int mode);
int linker_relocate_rela(linker_context_t* ctx, shared_object_t* so);
int linker_relocate_plt(linker_context_t* ctx, shared_object_t* so, int lazy);

// plt_got.c - PLT/GOT management
int plt_got_init(shared_object_t* so, uint64_t resolver);
uint64_t plt_lazy_resolver(shared_object_t* so, uint64_t reloc_index);
int plt_setup_lazy_binding(linker_context_t* ctx, shared_object_t* so);
uint64_t plt_get_entry_addr(shared_object_t* so, const char* sym_name);
void plt_got_dump(shared_object_t* so, size_t num_entries);
int plt_is_plt_addr(shared_object_t* so, uint64_t addr);

// dlopen.c - Dynamic loading API
void* dlopen(const char* filename, int flag);
void* dlsym(void* handle, const char* symbol);
int dlclose(void* handle);
char* dlerror(void);

#endif // LINKER_H

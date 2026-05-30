/**
 * ELF Dynamic Section Parser
 * ===========================
 *
 * Parses PT_DYNAMIC segment and extracts DT_* entries for dynamic linking.
 */

#include "linker.h"
#include <string.h>

/**
 * Parse PT_DYNAMIC segment
 *
 * Extracts all relevant DT_* entries and stores them in the shared object structure.
 * This includes symbol tables, string tables, relocation tables, hash tables, etc.
 *
 * @param so Shared object structure to populate
 * @param dynamic_segment Pointer to PT_DYNAMIC segment data
 * @param base_addr Base load address of the shared object
 * @return 0 on success, negative error code on failure
 */
int elf_parse_dynamic(shared_object_t* so, elf64_dyn_t* dynamic_segment, uint64_t base_addr) {
    if (!so || !dynamic_segment) {
        return -1;
    }

    elf64_dyn_t* dyn = dynamic_segment;

    // First pass: extract addresses relative to base
    while (dyn->d_tag != DT_NULL) {
        switch (dyn->d_tag) {
            case DT_NEEDED:
                // Record dependency (we'll handle this in a second pass)
                if (so->num_needed < MAX_DEPENDENCIES) {
                    so->needed_names[so->num_needed] = dyn->d_un.d_val;
                    so->num_needed++;
                }
                break;

            case DT_STRTAB:
                so->strtab = (char*)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_SYMTAB:
                so->symtab = (elf64_sym_t*)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_HASH:
                so->hash = (uint32_t*)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_GNU_HASH:
                so->gnu_hash = (uint32_t*)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_STRSZ:
                so->strtab_size = dyn->d_un.d_val;
                break;

            case DT_SYMENT:
                so->symtab_entsize = dyn->d_un.d_val;
                break;

            case DT_PLTGOT:
                so->pltgot = (uint64_t*)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_PLTRELSZ:
                so->pltrelsz = dyn->d_un.d_val;
                break;

            case DT_PLTREL:
                so->pltrel = dyn->d_un.d_val;
                break;

            case DT_JMPREL:
                so->jmprel = (void*)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_RELA:
                so->rela = (elf64_rela_t*)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_RELASZ:
                so->relasz = dyn->d_un.d_val;
                break;

            case DT_RELAENT:
                so->relaent = dyn->d_un.d_val;
                break;

            case DT_REL:
                so->rel = (elf64_rel_t*)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_RELSZ:
                so->relsz = dyn->d_un.d_val;
                break;

            case DT_RELENT:
                so->relent = dyn->d_un.d_val;
                break;

            case DT_INIT:
                so->init = (init_func_t)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_FINI:
                so->fini = (fini_func_t)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_INIT_ARRAY:
                so->init_array = (init_func_t*)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_INIT_ARRAYSZ:
                so->init_array_size = dyn->d_un.d_val;
                break;

            case DT_FINI_ARRAY:
                so->fini_array = (fini_func_t*)(base_addr + dyn->d_un.d_ptr);
                break;

            case DT_FINI_ARRAYSZ:
                so->fini_array_size = dyn->d_un.d_val;
                break;

            case DT_SONAME:
                so->soname_offset = dyn->d_un.d_val;
                break;

            case DT_RPATH:
                so->rpath_offset = dyn->d_un.d_val;
                break;

            case DT_RUNPATH:
                so->runpath_offset = dyn->d_un.d_val;
                break;

            case DT_FLAGS:
                so->flags = dyn->d_un.d_val;
                break;

            case DT_FLAGS_1:
                so->flags_1 = dyn->d_un.d_val;
                break;

            case DT_BIND_NOW:
                so->bind_now = 1;
                break;

            default:
                // Unknown or unhandled tag, ignore
                break;
        }
        dyn++;
    }

    // Resolve string pointers
    if (so->strtab) {
        if (so->soname_offset) {
            so->soname = so->strtab + so->soname_offset;
        }
        if (so->rpath_offset) {
            so->rpath = so->strtab + so->rpath_offset;
        }
        if (so->runpath_offset) {
            so->runpath = so->strtab + so->runpath_offset;
        }

        // Resolve dependency names
        for (int i = 0; i < so->num_needed; i++) {
            so->needed[i] = so->strtab + so->needed_names[i];
        }
    }

    // Calculate number of symbols from hash table if available
    if (so->hash) {
        // Standard ELF hash table format:
        // [0] = nbucket
        // [1] = nchain (= number of symbols)
        so->num_symbols = so->hash[1];
    }

    return 0;
}

/**
 * Find PT_DYNAMIC segment in ELF program headers
 *
 * @param ehdr ELF header
 * @param base_addr Base address where ELF is loaded
 * @return Pointer to PT_DYNAMIC segment, or NULL if not found
 */
elf64_dyn_t* elf_find_dynamic_segment(elf64_ehdr_t* ehdr, uint64_t base_addr) {
    if (!ehdr) {
        return NULL;
    }

    elf64_phdr_t* phdr = (elf64_phdr_t*)((uint8_t*)ehdr + ehdr->e_phoff);

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            return (elf64_dyn_t*)(base_addr + phdr[i].p_vaddr);
        }
    }

    return NULL;
}

/**
 * Validate dynamic section consistency
 *
 * Checks that required entries are present and values are sane.
 *
 * @param so Shared object to validate
 * @return 0 if valid, negative error code if invalid
 */
int elf_validate_dynamic(shared_object_t* so) {
    if (!so) {
        return -1;
    }

    // Must have symbol table and string table
    if (!so->symtab || !so->strtab) {
        return -2;
    }

    // Must have at least one hash table (regular or GNU)
    if (!so->hash && !so->gnu_hash) {
        return -3;
    }

    // If we have relocations, must have correct sizes
    if (so->rela && so->relaent != sizeof(elf64_rela_t)) {
        return -4;
    }

    if (so->rel && so->relent != sizeof(elf64_rel_t)) {
        return -5;
    }

    // If we have PLT relocations, must have PLT info
    if (so->pltrelsz > 0 && !so->jmprel) {
        return -6;
    }

    return 0;
}

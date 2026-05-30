/**
 * PE Loader Implementation
 *
 * Native Windows PE file parsing, loading, and execution for AutomationOS.
 * Supports PE32+ (64-bit) executables and DLLs.
 */

#include <kernel/pe_loader.h>
#include <kernel/process.h>
#include <kernel/memory.h>
#include <kernel/vfs.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// External Win32 API stubs
extern void win32_init(void);
extern int win32_syscall_dispatch(uint32_t syscall_num, void *args);

/**
 * Validate PE file structure
 */
bool pe_validate(const void *data, size_t size) {
    if (!data || size < sizeof(dos_header_t)) {
        return false;
    }

    dos_header_t *dos = (dos_header_t *)data;

    // Check MZ signature
    if (dos->e_magic != DOS_SIGNATURE) {
        printf("PE: Invalid DOS signature: 0x%04x\n", dos->e_magic);
        return false;
    }

    // Check PE header offset
    if (dos->e_lfanew >= size - sizeof(uint32_t) - sizeof(coff_header_t)) {
        printf("PE: Invalid PE header offset: 0x%x\n", dos->e_lfanew);
        return false;
    }

    // Check PE signature
    uint32_t *pe_sig = (uint32_t *)((uint8_t *)data + dos->e_lfanew);
    if (*pe_sig != PE_SIGNATURE) {
        printf("PE: Invalid PE signature: 0x%08x\n", *pe_sig);
        return false;
    }

    return true;
}

/**
 * Parse PE file from memory
 */
pe_file_t* pe_parse(const void *data, size_t size) {
    if (!pe_validate(data, size)) {
        return NULL;
    }

    pe_file_t *pe = calloc(1, sizeof(pe_file_t));
    if (!pe) {
        return NULL;
    }

    // Allocate file data copy
    pe->file_data = malloc(size);
    if (!pe->file_data) {
        free(pe);
        return NULL;
    }
    memcpy(pe->file_data, data, size);
    pe->file_size = size;

    // Parse DOS header
    pe->dos_header = (dos_header_t *)pe->file_data;

    // Parse PE signature and COFF header
    uint8_t *base = (uint8_t *)pe->file_data;
    pe->pe_signature = *(uint32_t *)(base + pe->dos_header->e_lfanew);
    pe->coff_header = (coff_header_t *)(base + pe->dos_header->e_lfanew + 4);

    // Parse optional header
    if (pe->coff_header->size_of_optional_header > 0) {
        pe->optional_header = (optional_header_t *)((uint8_t *)pe->coff_header + sizeof(coff_header_t));

        // Extract key info
        pe->entry_point = pe->optional_header->address_of_entry_point;
        pe->image_base = pe->optional_header->image_base;
        pe->image_size = pe->optional_header->size_of_image;
        pe->is_dll = (pe->coff_header->characteristics & IMAGE_FILE_DLL) != 0;
    }

    // Parse sections
    section_header_t *sections = (section_header_t *)((uint8_t *)pe->optional_header +
                                                      pe->coff_header->size_of_optional_header);
    pe->section_count = pe->coff_header->number_of_sections;

    if (pe->section_count > PE_MAX_SECTIONS) {
        printf("PE: Too many sections: %u\n", pe->section_count);
        pe->section_count = PE_MAX_SECTIONS;
    }

    for (uint32_t i = 0; i < pe->section_count; i++) {
        pe->sections[i] = &sections[i];
    }

    // Parse import directory
    if (pe->optional_header) {
        data_directory_t *import_dir = &pe->optional_header->data_directory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (import_dir->size > 0) {
            pe->import_descriptors = (import_descriptor_t *)pe_rva_to_ptr(pe, import_dir->virtual_address);

            // Count imports
            import_descriptor_t *desc = pe->import_descriptors;
            while (desc && desc->name != 0) {
                pe->import_count++;
                desc++;
                if (pe->import_count >= PE_MAX_IMPORTS) break;
            }
        }

        // Parse export directory
        data_directory_t *export_dir = &pe->optional_header->data_directory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (export_dir->size > 0) {
            pe->export_directory = (export_directory_t *)pe_rva_to_ptr(pe, export_dir->virtual_address);

            if (pe->export_directory) {
                pe->export_functions = (uint32_t *)pe_rva_to_ptr(pe, pe->export_directory->address_of_functions);
                pe->export_names = (uint32_t *)pe_rva_to_ptr(pe, pe->export_directory->address_of_names);
                pe->export_ordinals = (uint16_t *)pe_rva_to_ptr(pe, pe->export_directory->address_of_name_ordinals);
            }
        }

        // Parse relocations
        data_directory_t *reloc_dir = &pe->optional_header->data_directory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (reloc_dir->size > 0) {
            pe->relocations = (base_relocation_t *)pe_rva_to_ptr(pe, reloc_dir->virtual_address);
        }

        // Parse TLS directory
        data_directory_t *tls_dir = &pe->optional_header->data_directory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (tls_dir->size > 0) {
            pe->tls_directory = (tls_directory_t *)pe_rva_to_ptr(pe, tls_dir->virtual_address);
        }
    }

    printf("PE: Parsed %s (%u sections, %u imports)\n",
           pe->is_dll ? "DLL" : "EXE",
           pe->section_count,
           pe->import_count);

    return pe;
}

/**
 * Parse PE file from disk
 */
pe_file_t* pe_parse_file(const char *path) {
    // Open file
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("PE: Failed to open file: %s\n", path);
        return NULL;
    }

    // Get file size
    vfs_stat_t st;
    if (vfs_fstat(fd, &st) < 0) {
        vfs_close(fd);
        return NULL;
    }

    // Read file
    void *data = malloc(st.st_size);
    if (!data) {
        vfs_close(fd);
        return NULL;
    }

    if (vfs_read(fd, data, st.st_size) != st.st_size) {
        free(data);
        vfs_close(fd);
        return NULL;
    }

    vfs_close(fd);

    // Parse
    pe_file_t *pe = pe_parse(data, st.st_size);
    free(data);

    return pe;
}

/**
 * Free PE structure
 */
void pe_free(pe_file_t *pe) {
    if (!pe) return;

    if (pe->loaded_base && pe->is_loaded) {
        vmm_free(current_process->vmm, pe->loaded_base, pe->image_size);
    }

    if (pe->file_data) {
        free(pe->file_data);
    }

    free(pe);
}

/**
 * Convert RVA to file pointer
 */
void* pe_rva_to_ptr(pe_file_t *pe, uint32_t rva) {
    if (rva == 0) return NULL;

    // Find section containing RVA
    for (uint32_t i = 0; i < pe->section_count; i++) {
        section_header_t *sec = pe->sections[i];

        if (rva >= sec->virtual_address &&
            rva < sec->virtual_address + sec->virtual_size) {

            uint32_t offset = rva - sec->virtual_address;

            if (pe->is_loaded) {
                // Return pointer in loaded image
                return (uint8_t *)pe->loaded_base + sec->virtual_address + offset;
            } else {
                // Return pointer in file data
                return (uint8_t *)pe->file_data + sec->pointer_to_raw_data + offset;
            }
        }
    }

    return NULL;
}

/**
 * Find section by name
 */
section_header_t* pe_find_section(pe_file_t *pe, const char *name) {
    for (uint32_t i = 0; i < pe->section_count; i++) {
        if (strncmp(pe->sections[i]->name, name, 8) == 0) {
            return pe->sections[i];
        }
    }
    return NULL;
}

/**
 * Find section containing RVA
 */
section_header_t* pe_rva_to_section(pe_file_t *pe, uint32_t rva) {
    for (uint32_t i = 0; i < pe->section_count; i++) {
        section_header_t *sec = pe->sections[i];
        if (rva >= sec->virtual_address &&
            rva < sec->virtual_address + sec->virtual_size) {
            return sec;
        }
    }
    return NULL;
}

/**
 * Convert section characteristics to memory protection flags
 */
int pe_section_to_prot(uint32_t characteristics) {
    int prot = 0;

    if (characteristics & IMAGE_SCN_MEM_READ) {
        prot |= PROT_READ;
    }
    if (characteristics & IMAGE_SCN_MEM_WRITE) {
        prot |= PROT_WRITE;
    }
    if (characteristics & IMAGE_SCN_MEM_EXECUTE) {
        prot |= PROT_EXEC;
    }

    // Default to read if no flags set
    if (prot == 0) {
        prot = PROT_READ;
    }

    return prot;
}

/**
 * Map PE sections into memory
 */
int pe_map_sections(pe_file_t *pe) {
    if (!pe->loaded_base) {
        printf("PE: No loaded base address\n");
        return -1;
    }

    uint8_t *base = (uint8_t *)pe->loaded_base;

    // Copy headers
    memcpy(base, pe->file_data, pe->optional_header->size_of_headers);

    // Map each section
    for (uint32_t i = 0; i < pe->section_count; i++) {
        section_header_t *sec = pe->sections[i];

        char name[9] = {0};
        strncpy(name, sec->name, 8);

        printf("PE: Mapping section %s at RVA 0x%x (size 0x%x)\n",
               name, sec->virtual_address, sec->virtual_size);

        void *dest = base + sec->virtual_address;

        // Copy section data
        if (sec->size_of_raw_data > 0) {
            void *src = (uint8_t *)pe->file_data + sec->pointer_to_raw_data;
            size_t copy_size = sec->size_of_raw_data < sec->virtual_size ?
                              sec->size_of_raw_data : sec->virtual_size;
            memcpy(dest, src, copy_size);
        }

        // Zero remaining space (BSS)
        if (sec->virtual_size > sec->size_of_raw_data) {
            size_t zero_size = sec->virtual_size - sec->size_of_raw_data;
            memset(dest + sec->size_of_raw_data, 0, zero_size);
        }

        // Set memory protection
        int prot = pe_section_to_prot(sec->characteristics);
        vmm_protect(current_process->vmm, dest, sec->virtual_size, prot);
    }

    return 0;
}

/**
 * Apply base relocations
 */
int pe_apply_relocations(pe_file_t *pe, void *new_base) {
    if (!pe->relocations) {
        return 0; // No relocations needed
    }

    uint64_t delta = (uint64_t)new_base - pe->image_base;
    if (delta == 0) {
        return 0; // Loaded at preferred base, no relocations needed
    }

    printf("PE: Applying relocations (delta: 0x%lx)\n", delta);

    uint8_t *base = (uint8_t *)pe->loaded_base;
    base_relocation_t *reloc = pe->relocations;

    while (reloc->virtual_address != 0) {
        uint32_t num_entries = (reloc->size_of_block - sizeof(base_relocation_t)) / sizeof(uint16_t);
        uint16_t *entries = (uint16_t *)((uint8_t *)reloc + sizeof(base_relocation_t));

        for (uint32_t i = 0; i < num_entries; i++) {
            uint16_t entry = entries[i];
            uint16_t type = entry >> 12;
            uint16_t offset = entry & 0xFFF;

            void *target = base + reloc->virtual_address + offset;

            switch (type) {
                case IMAGE_REL_BASED_ABSOLUTE:
                    // Skip
                    break;

                case IMAGE_REL_BASED_HIGHLOW:
                    *(uint32_t *)target += (uint32_t)delta;
                    break;

                case IMAGE_REL_BASED_DIR64:
                    *(uint64_t *)target += delta;
                    break;

                default:
                    printf("PE: Unknown relocation type: %u\n", type);
                    break;
            }
        }

        reloc = (base_relocation_t *)((uint8_t *)reloc + reloc->size_of_block);
    }

    return 0;
}

/**
 * Resolve imports
 */
int pe_resolve_imports(pe_file_t *pe) {
    if (!pe->import_descriptors || pe->import_count == 0) {
        return 0; // No imports
    }

    printf("PE: Resolving %u imports\n", pe->import_count);

    import_descriptor_t *desc = pe->import_descriptors;

    for (uint32_t i = 0; i < pe->import_count; i++, desc++) {
        if (desc->name == 0) break;

        // Get DLL name
        char *dll_name = (char *)pe_rva_to_ptr(pe, desc->name);
        if (!dll_name) {
            printf("PE: Failed to get DLL name for import %u\n", i);
            continue;
        }

        printf("PE: Loading import DLL: %s\n", dll_name);

        // Load DLL
        dll_handle_t *dll = dll_load(dll_name);
        if (!dll) {
            printf("PE: Failed to load DLL: %s\n", dll_name);
            return -1;
        }

        // Process import address table
        uint64_t *iat = (uint64_t *)pe_rva_to_ptr(pe, desc->first_thunk);
        uint64_t *int_ptr = desc->original_first_thunk ?
                           (uint64_t *)pe_rva_to_ptr(pe, desc->original_first_thunk) : iat;

        uint32_t func_count = 0;
        while (*int_ptr) {
            uint64_t entry = *int_ptr;
            void *func_addr = NULL;

            // Check if import by ordinal
            if (entry & 0x8000000000000000ULL) {
                uint16_t ordinal = entry & 0xFFFF;
                func_addr = dll_get_proc_by_ordinal(dll, ordinal);
                printf("  Import by ordinal: %u -> %p\n", ordinal, func_addr);
            } else {
                // Import by name
                uint32_t *name_data = (uint32_t *)pe_rva_to_ptr(pe, entry & 0x7FFFFFFF);
                if (name_data) {
                    char *func_name = (char *)(name_data + 1); // Skip hint
                    func_addr = dll_get_proc_address(dll, func_name);
                    printf("  Import %s -> %p\n", func_name, func_addr);
                }
            }

            if (!func_addr) {
                printf("PE: Failed to resolve import\n");
                return -1;
            }

            *iat = (uint64_t)func_addr;

            int_ptr++;
            iat++;
            func_count++;
        }

        printf("PE: Resolved %u functions from %s\n", func_count, dll_name);
    }

    return 0;
}

/**
 * Load PE into memory
 */
int pe_load(pe_file_t *pe, void *preferred_base) {
    if (!pe || !pe->optional_header) {
        return -1;
    }

    // Allocate memory for image
    void *base = preferred_base;
    if (!base) {
        base = (void *)pe->image_base;
    }

    base = vmm_alloc_at(current_process->vmm, base, pe->image_size,
                        PROT_READ | PROT_WRITE | PROT_EXEC);
    if (!base) {
        printf("PE: Failed to allocate memory at preferred base\n");
        // Try allocating anywhere
        base = vmm_alloc(current_process->vmm, pe->image_size,
                        PROT_READ | PROT_WRITE | PROT_EXEC);
        if (!base) {
            printf("PE: Failed to allocate memory for image\n");
            return -1;
        }
    }

    pe->loaded_base = base;
    pe->is_loaded = true;

    printf("PE: Loaded image at %p (size 0x%x)\n", base, pe->image_size);

    // Map sections
    if (pe_map_sections(pe) < 0) {
        return -1;
    }

    // Apply relocations if needed
    if (base != (void *)pe->image_base) {
        if (pe_apply_relocations(pe, base) < 0) {
            return -1;
        }
    }

    // Resolve imports
    if (pe_resolve_imports(pe) < 0) {
        return -1;
    }

    return 0;
}

/**
 * Load and execute PE file
 */
int pe_load_and_execute(const char *exe_path) {
    printf("PE: Loading executable: %s\n", exe_path);

    // Initialize Win32 subsystem
    static bool win32_initialized = false;
    if (!win32_initialized) {
        win32_init();
        dll_init();
        win32_initialized = true;
    }

    // Parse PE file
    pe_file_t *pe = pe_parse_file(exe_path);
    if (!pe) {
        printf("PE: Failed to parse PE file\n");
        return -1;
    }

    if (pe->is_dll) {
        printf("PE: Cannot execute DLL directly\n");
        pe_free(pe);
        return -1;
    }

    // Create process
    process_t *proc = process_create(exe_path);
    if (!proc) {
        printf("PE: Failed to create process\n");
        pe_free(pe);
        return -1;
    }

    process_switch(proc);

    // Load PE
    if (pe_load(pe, NULL) < 0) {
        printf("PE: Failed to load PE file\n");
        pe_free(pe);
        process_destroy(proc);
        return -1;
    }

    // Set entry point
    uint64_t entry = (uint64_t)pe->loaded_base + pe->entry_point;
    proc->entry_point = (void *)entry;

    printf("PE: Entry point: %p\n", proc->entry_point);

    // Call TLS callbacks if present
    if (pe->tls_directory) {
        uint64_t *callbacks = (uint64_t *)pe->tls_directory->address_of_callbacks;
        if (callbacks) {
            while (*callbacks) {
                void (*callback)(void *, uint32_t, void *) = (void *)(*callbacks);
                callback(pe->loaded_base, DLL_PROCESS_ATTACH, NULL);
                callbacks++;
            }
        }
    }

    // Start execution
    printf("PE: Starting execution\n");
    scheduler_add_process(proc);

    return 0;
}

/**
 * Print PE information (debug)
 */
void pe_print_info(pe_file_t *pe) {
    if (!pe) return;

    printf("=== PE File Information ===\n");
    printf("Type: %s\n", pe->is_dll ? "DLL" : "EXE");
    printf("Machine: 0x%04x\n", pe->coff_header->machine);
    printf("Image Base: 0x%lx\n", pe->image_base);
    printf("Image Size: 0x%x (%u bytes)\n", pe->image_size, pe->image_size);
    printf("Entry Point: 0x%lx\n", pe->entry_point);
    printf("Sections: %u\n", pe->section_count);

    for (uint32_t i = 0; i < pe->section_count; i++) {
        section_header_t *sec = pe->sections[i];
        char name[9] = {0};
        strncpy(name, sec->name, 8);
        printf("  [%u] %-8s RVA: 0x%08x Size: 0x%08x Flags: 0x%08x\n",
               i, name, sec->virtual_address, sec->virtual_size, sec->characteristics);
    }

    printf("Imports: %u\n", pe->import_count);

    if (pe->export_directory) {
        printf("Exports: %u functions, %u names\n",
               pe->export_directory->number_of_functions,
               pe->export_directory->number_of_names);
    }

    printf("===========================\n");
}

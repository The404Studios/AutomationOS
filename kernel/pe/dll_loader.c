/**
 * DLL Loader for AutomationOS
 *
 * Dynamic library loading and function resolution.
 * Implements LoadLibrary and GetProcAddress equivalents.
 */

#include <kernel/pe_loader.h>
#include <kernel/process.h>
#include <kernel/memory.h>
#include <kernel/vfs.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// DLL cache (loaded DLLs)
static dll_handle_t *dll_cache_head = NULL;
static mutex_t dll_cache_lock;

// DLL search paths
static const char *dll_search_paths[] = {
    "/windows/system32/",
    "/windows/syswow64/",
    "/windows/",
    "./",
    NULL
};

/**
 * Initialize DLL subsystem
 */
void dll_init(void) {
    mutex_init(&dll_cache_lock);
    dll_cache_head = NULL;
    printf("DLL: Subsystem initialized\n");
}

/**
 * Find DLL in search paths
 */
char* dll_find_path(const char *dll_name) {
    if (!dll_name) {
        return NULL;
    }

    // Check if absolute path
    if (dll_name[0] == '/' || dll_name[0] == '\\' ||
        (dll_name[1] == ':' && (dll_name[2] == '/' || dll_name[2] == '\\'))) {
        // Absolute path, check if exists
        vfs_stat_t st;
        if (vfs_stat(dll_name, &st) == 0) {
            return strdup(dll_name);
        }
        return NULL;
    }

    // Search in paths
    char path[512];
    for (int i = 0; dll_search_paths[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", dll_search_paths[i], dll_name);

        vfs_stat_t st;
        if (vfs_stat(path, &st) == 0) {
            return strdup(path);
        }
    }

    // Try current process directory
    if (current_process && current_process->exe_path) {
        // Extract directory from exe path
        char *last_slash = strrchr(current_process->exe_path, '/');
        if (!last_slash) {
            last_slash = strrchr(current_process->exe_path, '\\');
        }

        if (last_slash) {
            size_t dir_len = last_slash - current_process->exe_path + 1;
            snprintf(path, sizeof(path), "%.*s%s", (int)dir_len, current_process->exe_path, dll_name);

            vfs_stat_t st;
            if (vfs_stat(path, &st) == 0) {
                return strdup(path);
            }
        }
    }

    printf("DLL: Failed to find DLL: %s\n", dll_name);
    return NULL;
}

/**
 * Get DLL from cache
 */
static dll_handle_t* dll_cache_get(const char *path) {
    mutex_lock(&dll_cache_lock);

    dll_handle_t *dll = dll_cache_head;
    while (dll) {
        if (strcmp(dll->name, path) == 0) {
            mutex_unlock(&dll_cache_lock);
            return dll;
        }
        dll = dll->next;
    }

    mutex_unlock(&dll_cache_lock);
    return NULL;
}

/**
 * Add DLL to cache
 */
static void dll_cache_add(dll_handle_t *dll) {
    mutex_lock(&dll_cache_lock);

    dll->next = dll_cache_head;
    dll_cache_head = dll;

    mutex_unlock(&dll_cache_lock);
}

/**
 * Remove DLL from cache
 */
static void dll_cache_remove(dll_handle_t *dll) {
    mutex_lock(&dll_cache_lock);

    dll_handle_t **prev = &dll_cache_head;
    dll_handle_t *curr = dll_cache_head;

    while (curr) {
        if (curr == dll) {
            *prev = curr->next;
            break;
        }
        prev = &curr->next;
        curr = curr->next;
    }

    mutex_unlock(&dll_cache_lock);
}

/**
 * Load DLL
 */
dll_handle_t* dll_load(const char *dll_name) {
    if (!dll_name) {
        return NULL;
    }

    printf("DLL: Loading %s\n", dll_name);

    // Find DLL path
    char *path = dll_find_path(dll_name);
    if (!path) {
        // Try with .dll extension if not present
        if (!strstr(dll_name, ".dll") && !strstr(dll_name, ".DLL")) {
            char dll_name_ext[256];
            snprintf(dll_name_ext, sizeof(dll_name_ext), "%s.dll", dll_name);
            path = dll_find_path(dll_name_ext);
        }

        if (!path) {
            printf("DLL: Failed to find: %s\n", dll_name);
            return NULL;
        }
    }

    // Check if already loaded
    dll_handle_t *dll = dll_cache_get(path);
    if (dll) {
        printf("DLL: Already loaded: %s (ref_count=%u)\n", path, dll->ref_count);
        dll->ref_count++;
        free(path);
        return dll;
    }

    // Parse PE file
    pe_file_t *pe = pe_parse_file(path);
    if (!pe) {
        printf("DLL: Failed to parse: %s\n", path);
        free(path);
        return NULL;
    }

    if (!pe->is_dll) {
        printf("DLL: File is not a DLL: %s\n", path);
        pe_free(pe);
        free(path);
        return NULL;
    }

    // Load PE into memory
    if (pe_load(pe, NULL) < 0) {
        printf("DLL: Failed to load: %s\n", path);
        pe_free(pe);
        free(path);
        return NULL;
    }

    // Create DLL handle
    dll = calloc(1, sizeof(dll_handle_t));
    if (!dll) {
        pe_free(pe);
        free(path);
        return NULL;
    }

    dll->pe = pe;
    dll->base = pe->loaded_base;
    strncpy(dll->name, path, PE_MAX_DLL_NAME - 1);
    dll->ref_count = 1;

    free(path);

    // Call DllMain with DLL_PROCESS_ATTACH
    if (pe->entry_point) {
        uint64_t entry = (uint64_t)pe->loaded_base + pe->entry_point;
        dll_main_t dll_main = (dll_main_t)entry;

        printf("DLL: Calling DllMain(DLL_PROCESS_ATTACH) at %p\n", dll_main);

        int result = dll_main(dll->base, DLL_PROCESS_ATTACH, NULL);
        if (!result) {
            printf("DLL: DllMain returned FALSE\n");
            free(dll);
            pe_free(pe);
            return NULL;
        }
    }

    // Add to cache
    dll_cache_add(dll);

    printf("DLL: Loaded successfully: %s at %p\n", dll->name, dll->base);
    return dll;
}

/**
 * Free DLL
 */
void dll_free(dll_handle_t *dll) {
    if (!dll) return;

    dll->ref_count--;

    if (dll->ref_count > 0) {
        printf("DLL: Decremented ref_count for %s to %u\n", dll->name, dll->ref_count);
        return;
    }

    printf("DLL: Freeing %s\n", dll->name);

    // Call DllMain with DLL_PROCESS_DETACH
    if (dll->pe && dll->pe->entry_point) {
        uint64_t entry = (uint64_t)dll->base + dll->pe->entry_point;
        dll_main_t dll_main = (dll_main_t)entry;

        printf("DLL: Calling DllMain(DLL_PROCESS_DETACH)\n");
        dll_main(dll->base, DLL_PROCESS_DETACH, NULL);
    }

    // Remove from cache
    dll_cache_remove(dll);

    // Free PE
    if (dll->pe) {
        pe_free(dll->pe);
    }

    free(dll);
}

/**
 * Get procedure address by name
 */
void* dll_get_proc_address(dll_handle_t *dll, const char *func_name) {
    if (!dll || !dll->pe || !func_name) {
        return NULL;
    }

    export_directory_t *exports = dll->pe->export_directory;
    if (!exports) {
        printf("DLL: No exports in %s\n", dll->name);
        return NULL;
    }

    // Linear search through export names
    for (uint32_t i = 0; i < exports->number_of_names; i++) {
        uint32_t name_rva = dll->pe->export_names[i];
        char *name = (char *)pe_rva_to_ptr(dll->pe, name_rva);

        if (name && strcmp(name, func_name) == 0) {
            // Found function
            uint16_t ordinal = dll->pe->export_ordinals[i];
            uint32_t func_rva = dll->pe->export_functions[ordinal];

            void *func_addr = (uint8_t *)dll->base + func_rva;

            // Check for forwarded export
            data_directory_t *export_dir = &dll->pe->optional_header->data_directory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (func_rva >= export_dir->virtual_address &&
                func_rva < export_dir->virtual_address + export_dir->size) {
                // Forwarded export: "DLL.Function"
                char *forward = (char *)pe_rva_to_ptr(dll->pe, func_rva);
                printf("DLL: Forwarded export: %s -> %s\n", func_name, forward);

                // Parse forward string
                char forward_dll[256], forward_func[256];
                if (sscanf(forward, "%255[^.].%255s", forward_dll, forward_func) == 2) {
                    // Load forwarded DLL
                    dll_handle_t *forward_dll_handle = dll_load(forward_dll);
                    if (forward_dll_handle) {
                        return dll_get_proc_address(forward_dll_handle, forward_func);
                    }
                }

                return NULL;
            }

            return func_addr;
        }
    }

    printf("DLL: Function not found: %s in %s\n", func_name, dll->name);
    return NULL;
}

/**
 * Get procedure address by ordinal
 */
void* dll_get_proc_by_ordinal(dll_handle_t *dll, uint16_t ordinal) {
    if (!dll || !dll->pe) {
        return NULL;
    }

    export_directory_t *exports = dll->pe->export_directory;
    if (!exports) {
        printf("DLL: No exports in %s\n", dll->name);
        return NULL;
    }

    // Check if ordinal is in range
    uint32_t index = ordinal - exports->base;
    if (index >= exports->number_of_functions) {
        printf("DLL: Invalid ordinal: %u in %s\n", ordinal, dll->name);
        return NULL;
    }

    uint32_t func_rva = dll->pe->export_functions[index];
    if (func_rva == 0) {
        printf("DLL: Ordinal %u not exported from %s\n", ordinal, dll->name);
        return NULL;
    }

    void *func_addr = (uint8_t *)dll->base + func_rva;

    // Check for forwarded export
    data_directory_t *export_dir = &dll->pe->optional_header->data_directory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (func_rva >= export_dir->virtual_address &&
        func_rva < export_dir->virtual_address + export_dir->size) {
        // Forwarded export
        char *forward = (char *)pe_rva_to_ptr(dll->pe, func_rva);
        printf("DLL: Forwarded export (ordinal %u): %s\n", ordinal, forward);

        // Parse and load
        char forward_dll[256], forward_func[256];
        if (sscanf(forward, "%255[^.].%255s", forward_dll, forward_func) == 2) {
            dll_handle_t *forward_dll_handle = dll_load(forward_dll);
            if (forward_dll_handle) {
                // Check if forwarded function is by ordinal
                if (forward_func[0] == '#') {
                    uint16_t forward_ordinal = atoi(forward_func + 1);
                    return dll_get_proc_by_ordinal(forward_dll_handle, forward_ordinal);
                } else {
                    return dll_get_proc_address(forward_dll_handle, forward_func);
                }
            }
        }

        return NULL;
    }

    return func_addr;
}

/**
 * Print DLL cache information (debug)
 */
void dll_print_cache(void) {
    printf("=== DLL Cache ===\n");

    mutex_lock(&dll_cache_lock);

    dll_handle_t *dll = dll_cache_head;
    int count = 0;

    while (dll) {
        printf("[%d] %s\n", count, dll->name);
        printf("    Base: %p\n", dll->base);
        printf("    Ref Count: %u\n", dll->ref_count);

        if (dll->pe && dll->pe->export_directory) {
            printf("    Exports: %u functions\n", dll->pe->export_directory->number_of_functions);
        }

        dll = dll->next;
        count++;
    }

    mutex_unlock(&dll_cache_lock);

    printf("Total: %d DLLs loaded\n", count);
    printf("=================\n");
}

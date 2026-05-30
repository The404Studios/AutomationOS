/**
 * Dynamic Loading API (dlopen/dlsym/dlclose)
 * ===========================================
 *
 * Implements POSIX dynamic loading API for runtime loading of shared libraries.
 */

#include "linker.h"
#include <string.h>

// Error message buffer
static char dl_error_buf[256];
static int dl_error_set = 0;

/**
 * Set dlerror message
 */
static void set_dlerror(const char* msg) {
    size_t len = 0;
    const char* s = msg;
    while (*s && len < sizeof(dl_error_buf) - 1) {
        dl_error_buf[len++] = *s++;
    }
    dl_error_buf[len] = '\0';
    dl_error_set = 1;
}

/**
 * Clear dlerror message
 */
static void clear_dlerror(void) {
    dl_error_set = 0;
    dl_error_buf[0] = '\0';
}

/**
 * dlopen - Load a shared library at runtime
 *
 * @param filename Path to shared library (or NULL for main program)
 * @param flag Flags (RTLD_LAZY, RTLD_NOW, RTLD_GLOBAL, RTLD_LOCAL)
 * @return Handle to loaded library, or NULL on error
 */
void* dlopen(const char* filename, int flag) {
    clear_dlerror();

    if (!g_linker_ctx) {
        set_dlerror("Linker not initialized");
        return NULL;
    }

    // NULL filename means return handle to main program
    if (!filename) {
        if (g_linker_ctx->main_object) {
            g_linker_ctx->main_object->refcount++;
            return (void*)g_linker_ctx->main_object;
        } else {
            set_dlerror("Main program not loaded");
            return NULL;
        }
    }

    // Find library path
    char path[MAX_PATH_LENGTH];
    for (const char* p = filename; *p; p++) {
        if (*p == '/') {
            // Absolute or relative path
            size_t len = 0;
            const char* s = filename;
            while (*s && len < sizeof(path) - 1) {
                path[len++] = *s++;
            }
            path[len] = '\0';
            goto found_path;
        }
    }

    // Search in library paths
    int found = 0;
    for (int i = 0; i < g_linker_ctx->num_search_paths; i++) {
        size_t len = 0;
        const char* s = g_linker_ctx->search_paths[i];
        while (*s && len < sizeof(path) - 1) {
            path[len++] = *s++;
        }
        if (len > 0 && path[len-1] != '/') {
            path[len++] = '/';
        }
        s = filename;
        while (*s && len < sizeof(path) - 1) {
            path[len++] = *s++;
        }
        path[len] = '\0';

        // Check if file exists (would need syscall)
        // For now, assume first path works
        found = 1;
        break;
    }

    if (!found) {
        set_dlerror("Library not found");
        return NULL;
    }

found_path:
    // Determine mode
    int mode = (flag & RTLD_NOW) ? RTLD_NOW : RTLD_LAZY;

    // Load the library
    shared_object_t* so = linker_load_object(g_linker_ctx, path, mode);
    if (!so) {
        set_dlerror("Failed to load library");
        return NULL;
    }

    // Load dependencies
    int ret = linker_load_dependencies(g_linker_ctx, so, mode);
    if (ret < 0) {
        set_dlerror("Failed to load dependencies");
        return NULL;
    }

    // Relocate if not already done
    if (!so->relocated) {
        ret = linker_relocate_object(g_linker_ctx, so, mode);
        if (ret < 0) {
            set_dlerror("Relocation failed");
            return NULL;
        }
    }

    // Run initializers if not already done
    if (!so->initialized) {
        // Run DT_INIT_ARRAY
        if (so->init_array && so->init_array_size > 0) {
            size_t num_init = so->init_array_size / sizeof(init_func_t);
            for (size_t i = 0; i < num_init; i++) {
                if (so->init_array[i]) {
                    so->init_array[i]();
                }
            }
        }

        // Run DT_INIT
        if (so->init) {
            so->init();
        }

        so->initialized = 1;
    }

    return (void*)so;
}

/**
 * dlsym - Get address of symbol in dynamically loaded library
 *
 * @param handle Handle from dlopen (or RTLD_DEFAULT/RTLD_NEXT)
 * @param symbol Symbol name
 * @return Address of symbol, or NULL if not found
 */
void* dlsym(void* handle, const char* symbol) {
    clear_dlerror();

    if (!g_linker_ctx) {
        set_dlerror("Linker not initialized");
        return NULL;
    }

    if (!symbol) {
        set_dlerror("NULL symbol name");
        return NULL;
    }

    // Special handles
    #define RTLD_DEFAULT  ((void*)0)
    #define RTLD_NEXT     ((void*)-1)

    shared_object_t* so = NULL;

    if (handle == RTLD_DEFAULT) {
        // Search in global scope
        symbol_info_t* sym_info = linker_lookup_symbol_global(g_linker_ctx, symbol, 0);
        if (sym_info) {
            return (void*)linker_get_symbol_addr(sym_info->object, sym_info->symbol);
        }
    } else if (handle == RTLD_NEXT) {
        // Search in next objects after caller (not implemented)
        set_dlerror("RTLD_NEXT not yet implemented");
        return NULL;
    } else {
        // Regular handle
        so = (shared_object_t*)handle;

        // Validate handle
        int valid = 0;
        for (int i = 0; i < g_linker_ctx->num_objects; i++) {
            if (g_linker_ctx->objects[i] == so) {
                valid = 1;
                break;
            }
        }

        if (!valid) {
            set_dlerror("Invalid handle");
            return NULL;
        }

        // Look up symbol in this object and its dependencies
        symbol_info_t* sym_info = linker_lookup_symbol_in_deps(g_linker_ctx, so, symbol);
        if (sym_info) {
            return (void*)linker_get_symbol_addr(sym_info->object, sym_info->symbol);
        }
    }

    set_dlerror("Symbol not found");
    return NULL;
}

/**
 * dlclose - Close a dynamically loaded library
 *
 * @param handle Handle from dlopen
 * @return 0 on success, non-zero on error
 */
int dlclose(void* handle) {
    clear_dlerror();

    if (!g_linker_ctx) {
        set_dlerror("Linker not initialized");
        return -1;
    }

    if (!handle) {
        set_dlerror("NULL handle");
        return -1;
    }

    shared_object_t* so = (shared_object_t*)handle;

    // Validate handle
    int valid = 0;
    for (int i = 0; i < g_linker_ctx->num_objects; i++) {
        if (g_linker_ctx->objects[i] == so) {
            valid = 1;
            break;
        }
    }

    if (!valid) {
        set_dlerror("Invalid handle");
        return -1;
    }

    // Decrement reference count
    so->refcount--;

    // If refcount reaches zero, run finalizers and unload
    if (so->refcount == 0) {
        // Run DT_FINI
        if (so->fini) {
            so->fini();
        }

        // Run DT_FINI_ARRAY
        if (so->fini_array && so->fini_array_size > 0) {
            size_t num_fini = so->fini_array_size / sizeof(fini_func_t);
            for (size_t i = 0; i < num_fini; i++) {
                if (so->fini_array[i]) {
                    so->fini_array[i]();
                }
            }
        }

        // TODO: Unmap memory (would need syscall_munmap)
        // For now, just mark as unloaded
        so->loaded = 0;
        so->initialized = 0;
    }

    return 0;
}

/**
 * dlerror - Get human-readable error message
 *
 * @return Error message, or NULL if no error
 */
char* dlerror(void) {
    if (dl_error_set) {
        dl_error_set = 0;  // Clear error after reading
        return dl_error_buf;
    }
    return NULL;
}

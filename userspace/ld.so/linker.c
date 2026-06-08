/**
 * Dynamic Linker Main Implementation
 * ===================================
 *
 * Core dynamic linker logic for loading shared libraries,
 * resolving dependencies, and preparing programs for execution.
 */

#include "linker.h"
#include <string.h>
#include <stdarg.h>

// Simple implementations of needed libc functions
static void* memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    while (n--) *d++ = *s++;
    return dest;
}

static size_t strlen(const char* s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

static int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

static char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

static char* strcat(char* dest, const char* src) {
    char* d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

static char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dest[i] = src[i];
    dest[i] = '\0';
    return dest;
}

/* Bounded concatenation: appends src to dest, never writes past dest[maxlen-1]. */
static char* strncat_safe(char* dest, const char* src, size_t maxlen) {
    size_t dlen = strlen(dest);
    if (dlen >= maxlen - 1) return dest;
    size_t remaining = maxlen - dlen - 1;
    size_t i;
    for (i = 0; i < remaining && src[i]; i++)
        dest[dlen + i] = src[i];
    dest[dlen + i] = '\0';
    return dest;
}

// Global linker context
linker_context_t* g_linker_ctx = NULL;

// Debug output buffer (simple implementation)
static char debug_buf[1024];

/**
 * Debug printf (simple implementation)
 */
void linker_debug(const char* fmt, ...) {
    if (!g_linker_ctx || !g_linker_ctx->debug) {
        return;
    }

    // Simple format string handling
    // For now, just print the format string
    // TODO: Implement proper printf-style formatting

    // Write to stdout (fd 1)
    // syscall_write(1, fmt, strlen(fmt));
}

/**
 * Initialize linker context
 *
 * @param ctx Linker context to initialize
 * @return 0 on success, negative error code on failure
 */
int linker_init(linker_context_t* ctx) {
    if (!ctx) {
        return -1;
    }

    memset(ctx, 0, sizeof(linker_context_t));

    // Set default search paths
    strncpy(ctx->search_paths[0], "/lib", MAX_PATH_LENGTH);
    strncpy(ctx->search_paths[1], "/usr/lib", MAX_PATH_LENGTH);
    strncpy(ctx->search_paths[2], "/lib64", MAX_PATH_LENGTH);
    strncpy(ctx->search_paths[3], "/usr/lib64", MAX_PATH_LENGTH);
    ctx->num_search_paths = 4;

    // Set global context
    g_linker_ctx = ctx;

    return 0;
}

/**
 * Find library file in search paths
 *
 * @param ctx Linker context
 * @param name Library name (e.g., "libc.so.6")
 * @param path_out Buffer to store full path
 * @return 0 if found, negative error code if not found
 */
static int find_library(linker_context_t* ctx, const char* name, char* path_out) {
    // If name contains '/', it's a path - use as-is
    for (const char* p = name; *p; p++) {
        if (*p == '/') {
            strncpy(path_out, name, MAX_PATH_LENGTH);
            return 0;
        }
    }

    // Search in library paths
    for (int i = 0; i < ctx->num_search_paths; i++) {
        strncpy(path_out, ctx->search_paths[i], MAX_PATH_LENGTH);
        strncat_safe(path_out, "/", MAX_PATH_LENGTH);
        strncat_safe(path_out, name, MAX_PATH_LENGTH);

        // Check if file exists (would need syscall_stat or similar)
        // For now, assume it exists
        return 0;
    }

    return -1;  // Not found
}

/**
 * Allocate a new shared object structure
 *
 * @param ctx Linker context
 * @return Pointer to allocated object, or NULL on failure
 */
static shared_object_t* alloc_shared_object(linker_context_t* ctx) {
    if (ctx->num_objects >= MAX_SHARED_OBJECTS) {
        return NULL;
    }

    // Allocate memory (would need syscall_mmap or heap allocator)
    // For now, use a simple static allocation
    static shared_object_t object_pool[MAX_SHARED_OBJECTS];
    shared_object_t* so = &object_pool[ctx->num_objects];
    memset(so, 0, sizeof(shared_object_t));

    ctx->objects[ctx->num_objects++] = so;
    so->refcount = 1;

    return so;
}

/**
 * Load ELF file into memory
 *
 * Maps the ELF file into memory and parses program headers.
 *
 * @param path Path to ELF file
 * @param so Shared object structure to populate
 * @return 0 on success, negative error code on failure
 */
static int load_elf_file(const char* path, shared_object_t* so) {
    // Open file (would need syscall_open)
    // Read ELF header
    // Map PT_LOAD segments
    // This is a placeholder - actual implementation needs kernel syscalls

    linker_debug("Loading ELF: %s\n", path);

    // For now, return error - needs kernel integration
    return -1;
}

/**
 * Load a shared object and its dependencies
 *
 * @param ctx Linker context
 * @param path Path to shared object
 * @param mode Loading mode (RTLD_LAZY, RTLD_NOW, etc.)
 * @return Pointer to loaded object, or NULL on failure
 */
shared_object_t* linker_load_object(linker_context_t* ctx, const char* path, int mode) {
    if (!ctx || !path) {
        return NULL;
    }

    linker_debug("linker_load_object: %s (mode=0x%x)\n", path, mode);

    // Check if already loaded
    for (int i = 0; i < ctx->num_objects; i++) {
        if (strcmp(ctx->objects[i]->path, path) == 0) {
            ctx->objects[i]->refcount++;
            return ctx->objects[i];
        }
    }

    // Allocate new shared object
    shared_object_t* so = alloc_shared_object(ctx);
    if (!so) {
        linker_debug("ERROR: Failed to allocate shared object\n");
        return NULL;
    }

    // Set name and path (bounded to MAX_PATH_LENGTH)
    strncpy(so->path, path, MAX_PATH_LENGTH);
    const char* name = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    strncpy(so->name, name, MAX_PATH_LENGTH);

    // Load ELF file
    int ret = load_elf_file(path, so);
    if (ret < 0) {
        linker_debug("ERROR: Failed to load ELF file\n");
        return NULL;
    }

    // Find and parse PT_DYNAMIC segment
    elf64_dyn_t* dynamic = elf_find_dynamic_segment(so->ehdr, so->base_addr);
    if (dynamic) {
        ret = elf_parse_dynamic(so, dynamic, so->base_addr);
        if (ret < 0) {
            linker_debug("ERROR: Failed to parse dynamic section\n");
            return NULL;
        }

        ret = elf_validate_dynamic(so);
        if (ret < 0) {
            linker_debug("ERROR: Invalid dynamic section\n");
            return NULL;
        }
    }

    so->loaded = 1;

    return so;
}

/**
 * Load all dependencies of a shared object
 *
 * @param ctx Linker context
 * @param so Shared object
 * @param mode Loading mode
 * @return 0 on success, negative error code on failure
 */
int linker_load_dependencies(linker_context_t* ctx, shared_object_t* so, int mode) {
    if (!ctx || !so) {
        return -1;
    }

    linker_debug("Loading dependencies for %s (%d needed)\n", so->name, so->num_needed);

    for (int i = 0; i < so->num_needed; i++) {
        const char* dep_name = so->needed[i];
        if (!dep_name) {
            continue;
        }

        linker_debug("  Dependency: %s\n", dep_name);

        // Find library path
        char dep_path[MAX_PATH_LENGTH];
        int ret = find_library(ctx, dep_name, dep_path);
        if (ret < 0) {
            linker_debug("ERROR: Dependency not found: %s\n", dep_name);
            return -2;
        }

        // Load dependency
        shared_object_t* dep = linker_load_object(ctx, dep_path, mode);
        if (!dep) {
            linker_debug("ERROR: Failed to load dependency: %s\n", dep_name);
            return -3;
        }

        // Add to dependency list
        so->deps[so->num_deps++] = dep;

        // Recursively load dependencies of dependency
        ret = linker_load_dependencies(ctx, dep, mode);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

/**
 * Relocate all loaded objects
 *
 * @param ctx Linker context
 * @param mode Relocation mode (RTLD_LAZY or RTLD_NOW)
 * @return 0 on success, negative error code on failure
 */
int linker_relocate_all(linker_context_t* ctx, int mode) {
    if (!ctx) {
        return -1;
    }

    linker_debug("Relocating all objects (mode=%s)\n",
                 (mode == RTLD_NOW) ? "RTLD_NOW" : "RTLD_LAZY");

    // Relocate in reverse load order (dependencies first)
    for (int i = ctx->num_objects - 1; i >= 0; i--) {
        shared_object_t* so = ctx->objects[i];

        if (so->relocated) {
            continue;
        }

        int ret = linker_relocate_object(ctx, so, mode);
        if (ret < 0) {
            linker_debug("ERROR: Failed to relocate %s\n", so->name);
            return ret;
        }
    }

    return 0;
}

/**
 * Run initializer functions for all loaded objects
 *
 * @param ctx Linker context
 * @return 0 on success, negative error code on failure
 */
int linker_run_initializers(linker_context_t* ctx) {
    if (!ctx) {
        return -1;
    }

    linker_debug("Running initializers\n");

    // Run in load order (dependencies first)
    for (int i = 0; i < ctx->num_objects; i++) {
        shared_object_t* so = ctx->objects[i];

        if (so->initialized) {
            continue;
        }

        // Run DT_INIT_ARRAY functions
        if (so->init_array && so->init_array_size > 0) {
            size_t num_init = so->init_array_size / sizeof(init_func_t);
            for (size_t j = 0; j < num_init; j++) {
                if (so->init_array[j]) {
                    linker_debug("  Calling init_array[%zu] for %s\n", j, so->name);
                    so->init_array[j]();
                }
            }
        }

        // Run DT_INIT function
        if (so->init) {
            linker_debug("  Calling init for %s\n", so->name);
            so->init();
        }

        so->initialized = 1;
    }

    return 0;
}

/**
 * Run finalizer functions for all loaded objects
 *
 * @param ctx Linker context
 * @return 0 on success, negative error code on failure
 */
int linker_run_finalizers(linker_context_t* ctx) {
    if (!ctx) {
        return -1;
    }

    linker_debug("Running finalizers\n");

    // Run in reverse load order (reverse of init order)
    for (int i = ctx->num_objects - 1; i >= 0; i--) {
        shared_object_t* so = ctx->objects[i];

        if (!so->initialized) {
            continue;
        }

        // Run DT_FINI function
        if (so->fini) {
            linker_debug("  Calling fini for %s\n", so->name);
            so->fini();
        }

        // Run DT_FINI_ARRAY functions
        if (so->fini_array && so->fini_array_size > 0) {
            size_t num_fini = so->fini_array_size / sizeof(fini_func_t);
            for (size_t j = 0; j < num_fini; j++) {
                if (so->fini_array[j]) {
                    linker_debug("  Calling fini_array[%zu] for %s\n", j, so->name);
                    so->fini_array[j]();
                }
            }
        }
    }

    return 0;
}

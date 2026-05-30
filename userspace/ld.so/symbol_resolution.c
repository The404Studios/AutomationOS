/**
 * Symbol Resolution Engine
 * =========================
 *
 * Implements symbol lookup across shared objects with proper scoping rules.
 * Supports both standard ELF hash and GNU hash tables for fast symbol lookup.
 */

#include "linker.h"
#include <string.h>

/**
 * Standard ELF hash function
 *
 * Computes hash value for symbol name using the classic ELF hash algorithm.
 *
 * @param name Symbol name
 * @return Hash value
 */
static uint32_t elf_hash(const char* name) {
    uint32_t h = 0, g;

    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        if ((g = h & 0xf0000000)) {
            h ^= g >> 24;
        }
        h &= ~g;
    }

    return h;
}

/**
 * GNU hash function
 *
 * Computes hash value using the GNU hash algorithm (faster than ELF hash).
 *
 * @param name Symbol name
 * @return Hash value
 */
static uint32_t gnu_hash(const char* name) {
    uint32_t h = 5381;

    while (*name) {
        h = (h << 5) + h + (uint8_t)*name++;
    }

    return h;
}

/**
 * Lookup symbol using standard ELF hash table
 *
 * @param so Shared object to search
 * @param name Symbol name
 * @return Symbol table entry if found, NULL otherwise
 */
static elf64_sym_t* elf_hash_lookup(shared_object_t* so, const char* name) {
    if (!so->hash || !so->symtab || !so->strtab) {
        return NULL;
    }

    uint32_t nbucket = so->hash[0];
    uint32_t* bucket = &so->hash[2];
    uint32_t* chain = &so->hash[2 + nbucket];

    uint32_t h = elf_hash(name);
    uint32_t idx = bucket[h % nbucket];

    while (idx != 0) {
        elf64_sym_t* sym = &so->symtab[idx];
        const char* sym_name = so->strtab + sym->st_name;

        if (strcmp(name, sym_name) == 0) {
            // Found, but check if it's defined
            if (sym->st_shndx != SHN_UNDEF) {
                return sym;
            }
        }

        idx = chain[idx];
    }

    return NULL;
}

/**
 * Lookup symbol using GNU hash table
 *
 * @param so Shared object to search
 * @param name Symbol name
 * @return Symbol table entry if found, NULL otherwise
 */
static elf64_sym_t* gnu_hash_lookup(shared_object_t* so, const char* name) {
    if (!so->gnu_hash || !so->symtab || !so->strtab) {
        return NULL;
    }

    uint32_t* hash_table = so->gnu_hash;
    uint32_t nbuckets = hash_table[0];
    uint32_t symoffset = hash_table[1];
    uint32_t bloom_size = hash_table[2];
    uint32_t bloom_shift = hash_table[3];

    uint64_t* bloom = (uint64_t*)&hash_table[4];
    uint32_t* buckets = (uint32_t*)&bloom[bloom_size];
    uint32_t* chain = &buckets[nbuckets];

    uint32_t h = gnu_hash(name);

    // Bloom filter test
    uint64_t word = bloom[(h / 64) % bloom_size];
    uint64_t mask = (1ULL << (h % 64)) | (1ULL << ((h >> bloom_shift) % 64));

    if ((word & mask) != mask) {
        return NULL;  // Definitely not present
    }

    // Check bucket
    uint32_t idx = buckets[h % nbuckets];
    if (idx < symoffset) {
        return NULL;
    }

    // Walk chain
    while (1) {
        elf64_sym_t* sym = &so->symtab[idx];
        const char* sym_name = so->strtab + sym->st_name;
        uint32_t chain_hash = chain[idx - symoffset];

        if ((h | 1) == (chain_hash | 1)) {
            if (strcmp(name, sym_name) == 0) {
                if (sym->st_shndx != SHN_UNDEF) {
                    return sym;
                }
            }
        }

        // Check if this is the last entry in chain
        if (chain_hash & 1) {
            break;
        }

        idx++;
    }

    return NULL;
}

/**
 * Lookup symbol in a single shared object
 *
 * Tries GNU hash first (if available), then falls back to ELF hash.
 *
 * @param so Shared object to search
 * @param name Symbol name
 * @return Symbol table entry if found, NULL otherwise
 */
elf64_sym_t* linker_lookup_symbol_in_object(shared_object_t* so, const char* name) {
    if (!so || !name) {
        return NULL;
    }

    // Try GNU hash first (faster)
    if (so->gnu_hash) {
        elf64_sym_t* sym = gnu_hash_lookup(so, name);
        if (sym) {
            return sym;
        }
    }

    // Fall back to standard ELF hash
    if (so->hash) {
        return elf_hash_lookup(so, name);
    }

    return NULL;
}

/**
 * Lookup symbol globally across all loaded shared objects
 *
 * Implements proper symbol search order:
 * 1. Main executable (unless skip_main is set)
 * 2. Dependencies in breadth-first order
 * 3. Global scope
 *
 * @param ctx Linker context
 * @param name Symbol name
 * @param skip_main If non-zero, skip searching the main executable
 * @return Symbol information (object + symbol entry) if found, NULL otherwise
 */
symbol_info_t* linker_lookup_symbol_global(linker_context_t* ctx, const char* name, int skip_main) {
    if (!ctx || !name) {
        return NULL;
    }

    // Search in main executable first (unless skipped)
    if (!skip_main && ctx->main_object) {
        elf64_sym_t* sym = linker_lookup_symbol_in_object(ctx->main_object, name);
        if (sym) {
            static symbol_info_t info;
            info.object = ctx->main_object;
            info.symbol = sym;
            return &info;
        }
    }

    // Search in loaded shared objects (in load order)
    for (int i = 0; i < ctx->num_objects; i++) {
        shared_object_t* so = ctx->objects[i];

        // Skip main executable if we already searched it
        if (so == ctx->main_object && !skip_main) {
            continue;
        }

        elf64_sym_t* sym = linker_lookup_symbol_in_object(so, name);
        if (sym) {
            static symbol_info_t info;
            info.object = so;
            info.symbol = sym;
            return &info;
        }
    }

    return NULL;
}

/**
 * Lookup symbol in dependencies of a specific shared object
 *
 * Implements DT_SYMBOLIC semantics (search the object itself first).
 *
 * @param ctx Linker context
 * @param so Shared object whose dependencies to search
 * @param name Symbol name
 * @return Symbol information if found, NULL otherwise
 */
symbol_info_t* linker_lookup_symbol_in_deps(linker_context_t* ctx, shared_object_t* so, const char* name) {
    if (!ctx || !so || !name) {
        return NULL;
    }

    // If DT_SYMBOLIC, search in this object first
    if (so->flags & DF_SYMBOLIC) {
        elf64_sym_t* sym = linker_lookup_symbol_in_object(so, name);
        if (sym) {
            static symbol_info_t info;
            info.object = so;
            info.symbol = sym;
            return &info;
        }
    }

    // Search in dependencies
    for (int i = 0; i < so->num_deps; i++) {
        shared_object_t* dep = so->deps[i];
        elf64_sym_t* sym = linker_lookup_symbol_in_object(dep, name);
        if (sym) {
            static symbol_info_t info;
            info.object = dep;
            info.symbol = sym;
            return &info;
        }
    }

    // Fall back to global search
    return linker_lookup_symbol_global(ctx, name, 0);
}

/**
 * Get symbol address
 *
 * Calculates the final runtime address of a symbol.
 *
 * @param so Shared object containing the symbol
 * @param sym Symbol table entry
 * @return Runtime address of symbol
 */
uint64_t linker_get_symbol_addr(shared_object_t* so, elf64_sym_t* sym) {
    if (!so || !sym) {
        return 0;
    }

    // Absolute symbols don't need relocation
    if (sym->st_shndx == SHN_ABS) {
        return sym->st_value;
    }

    // Undefined symbols have no address
    if (sym->st_shndx == SHN_UNDEF) {
        return 0;
    }

    // Regular symbol: add base address
    return so->base_addr + sym->st_value;
}

/**
 * Check if symbol is weak
 *
 * @param sym Symbol table entry
 * @return Non-zero if symbol is weak, zero otherwise
 */
int linker_symbol_is_weak(elf64_sym_t* sym) {
    return ELF64_ST_BIND(sym->st_info) == STB_WEAK;
}

/**
 * Check if symbol is global
 *
 * @param sym Symbol table entry
 * @return Non-zero if symbol is global, zero otherwise
 */
int linker_symbol_is_global(elf64_sym_t* sym) {
    uint8_t bind = ELF64_ST_BIND(sym->st_info);
    return bind == STB_GLOBAL || bind == STB_WEAK;
}

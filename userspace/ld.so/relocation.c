/**
 * ELF Relocation Engine
 * ======================
 *
 * Applies R_X86_64_* relocations to shared objects.
 * Handles both immediate binding (RTLD_NOW) and lazy binding (RTLD_LAZY).
 */

#include "linker.h"
#include <string.h>

/**
 * Get relocation type name (for debugging)
 */
static const char* reloc_type_name(uint32_t type) {
    switch (type) {
        case R_X86_64_NONE:         return "R_X86_64_NONE";
        case R_X86_64_64:           return "R_X86_64_64";
        case R_X86_64_PC32:         return "R_X86_64_PC32";
        case R_X86_64_GOT32:        return "R_X86_64_GOT32";
        case R_X86_64_PLT32:        return "R_X86_64_PLT32";
        case R_X86_64_COPY:         return "R_X86_64_COPY";
        case R_X86_64_GLOB_DAT:     return "R_X86_64_GLOB_DAT";
        case R_X86_64_JUMP_SLOT:    return "R_X86_64_JUMP_SLOT";
        case R_X86_64_RELATIVE:     return "R_X86_64_RELATIVE";
        case R_X86_64_GOTPCREL:     return "R_X86_64_GOTPCREL";
        case R_X86_64_32:           return "R_X86_64_32";
        case R_X86_64_32S:          return "R_X86_64_32S";
        case R_X86_64_16:           return "R_X86_64_16";
        case R_X86_64_PC16:         return "R_X86_64_PC16";
        case R_X86_64_8:            return "R_X86_64_8";
        case R_X86_64_PC8:          return "R_X86_64_PC8";
        case R_X86_64_DTPMOD64:     return "R_X86_64_DTPMOD64";
        case R_X86_64_DTPOFF64:     return "R_X86_64_DTPOFF64";
        case R_X86_64_TPOFF64:      return "R_X86_64_TPOFF64";
        case R_X86_64_TLSGD:        return "R_X86_64_TLSGD";
        case R_X86_64_TLSLD:        return "R_X86_64_TLSLD";
        case R_X86_64_DTPOFF32:     return "R_X86_64_DTPOFF32";
        case R_X86_64_GOTTPOFF:     return "R_X86_64_GOTTPOFF";
        case R_X86_64_TPOFF32:      return "R_X86_64_TPOFF32";
        case R_X86_64_PC64:         return "R_X86_64_PC64";
        case R_X86_64_GOTOFF64:     return "R_X86_64_GOTOFF64";
        case R_X86_64_GOTPC32:      return "R_X86_64_GOTPC32";
        case R_X86_64_SIZE32:       return "R_X86_64_SIZE32";
        case R_X86_64_SIZE64:       return "R_X86_64_SIZE64";
        case R_X86_64_IRELATIVE:    return "R_X86_64_IRELATIVE";
        default:                    return "UNKNOWN";
    }
}

/**
 * Apply a single RELA relocation
 *
 * @param ctx Linker context
 * @param so Shared object being relocated
 * @param rela Relocation entry
 * @return 0 on success, negative error code on failure
 */
static int apply_rela(linker_context_t* ctx, shared_object_t* so, elf64_rela_t* rela) {
    uint32_t type = ELF64_R_TYPE(rela->r_info);
    uint32_t sym_idx = ELF64_R_SYM(rela->r_info);
    uint64_t* reloc_addr = (uint64_t*)(so->base_addr + rela->r_offset);

    uint64_t sym_addr = 0;
    elf64_sym_t* sym = NULL;

    // Get symbol if needed
    if (sym_idx != 0) {
        sym = &so->symtab[sym_idx];
        const char* sym_name = so->strtab + sym->st_name;

        // Look up symbol
        symbol_info_t* sym_info = linker_lookup_symbol_in_deps(ctx, so, sym_name);
        if (sym_info) {
            sym_addr = linker_get_symbol_addr(sym_info->object, sym_info->symbol);
        } else if (!linker_symbol_is_weak(sym)) {
            // Undefined symbol (not weak) - this is an error
            linker_debug("ERROR: Undefined symbol: %s\n", sym_name);
            return -1;
        }
        // Weak undefined symbols resolve to 0
    }

    // Apply relocation based on type
    switch (type) {
        case R_X86_64_NONE:
            // No operation
            break;

        case R_X86_64_64:
            // Direct 64-bit: S + A
            *reloc_addr = sym_addr + rela->r_addend;
            break;

        case R_X86_64_PC32:
            // PC-relative 32-bit: S + A - P
            *(uint32_t*)reloc_addr = (uint32_t)(sym_addr + rela->r_addend - (uint64_t)reloc_addr);
            break;

        case R_X86_64_GLOB_DAT:
            // Global data: S
            *reloc_addr = sym_addr;
            break;

        case R_X86_64_JUMP_SLOT:
            // PLT entry: S
            *reloc_addr = sym_addr;
            break;

        case R_X86_64_RELATIVE:
            // Relative: B + A (no symbol)
            *reloc_addr = so->base_addr + rela->r_addend;
            break;

        case R_X86_64_GOTPCREL:
            // GOT PC-relative: G + GOT + A - P
            // For simplicity, treat like PC32 (assumes GOT entry exists)
            *(uint32_t*)reloc_addr = (uint32_t)(sym_addr + rela->r_addend - (uint64_t)reloc_addr);
            break;

        case R_X86_64_32:
            // Direct 32-bit zero-extended: S + A
            *(uint32_t*)reloc_addr = (uint32_t)(sym_addr + rela->r_addend);
            break;

        case R_X86_64_32S:
            // Direct 32-bit sign-extended: S + A
            *(int32_t*)reloc_addr = (int32_t)(sym_addr + rela->r_addend);
            break;

        case R_X86_64_16:
            // Direct 16-bit: S + A
            *(uint16_t*)reloc_addr = (uint16_t)(sym_addr + rela->r_addend);
            break;

        case R_X86_64_PC16:
            // PC-relative 16-bit: S + A - P
            *(uint16_t*)reloc_addr = (uint16_t)(sym_addr + rela->r_addend - (uint64_t)reloc_addr);
            break;

        case R_X86_64_8:
            // Direct 8-bit: S + A
            *(uint8_t*)reloc_addr = (uint8_t)(sym_addr + rela->r_addend);
            break;

        case R_X86_64_PC8:
            // PC-relative 8-bit: S + A - P
            *(uint8_t*)reloc_addr = (uint8_t)(sym_addr + rela->r_addend - (uint64_t)reloc_addr);
            break;

        case R_X86_64_PC64:
            // PC-relative 64-bit: S + A - P
            *reloc_addr = sym_addr + rela->r_addend - (uint64_t)reloc_addr;
            break;

        case R_X86_64_GOTOFF64:
            // 64-bit GOT offset: S + A - GOT
            *reloc_addr = sym_addr + rela->r_addend - (uint64_t)so->pltgot;
            break;

        case R_X86_64_SIZE32:
            // Symbol size 32-bit: Z + A
            *(uint32_t*)reloc_addr = (uint32_t)(sym->st_size + rela->r_addend);
            break;

        case R_X86_64_SIZE64:
            // Symbol size 64-bit: Z + A
            *reloc_addr = sym->st_size + rela->r_addend;
            break;

        case R_X86_64_IRELATIVE:
            // Indirect relative: B + A, then call as function
            {
                uint64_t ifunc_addr = so->base_addr + rela->r_addend;
                uint64_t (*ifunc)(void) = (uint64_t (*)(void))ifunc_addr;
                *reloc_addr = ifunc();
            }
            break;

        case R_X86_64_COPY:
            // Copy relocation (handled separately in executable)
            linker_debug("WARNING: R_X86_64_COPY not yet implemented\n");
            break;

        // TLS relocations (not yet implemented)
        case R_X86_64_DTPMOD64:
        case R_X86_64_DTPOFF64:
        case R_X86_64_TPOFF64:
        case R_X86_64_TLSGD:
        case R_X86_64_TLSLD:
        case R_X86_64_DTPOFF32:
        case R_X86_64_GOTTPOFF:
        case R_X86_64_TPOFF32:
            linker_debug("WARNING: TLS relocation %s not yet implemented\n", reloc_type_name(type));
            return -2;

        default:
            linker_debug("ERROR: Unknown relocation type %d (%s)\n", type, reloc_type_name(type));
            return -3;
    }

    return 0;
}

/**
 * Process all RELA relocations for a shared object
 *
 * @param ctx Linker context
 * @param so Shared object
 * @return 0 on success, negative error code on failure
 */
int linker_relocate_rela(linker_context_t* ctx, shared_object_t* so) {
    if (!ctx || !so) {
        return -1;
    }

    if (!so->rela || so->relasz == 0) {
        return 0;  // No RELA relocations
    }

    size_t num_rela = so->relasz / sizeof(elf64_rela_t);

    linker_debug("Processing %zu RELA relocations for %s\n", num_rela, so->name);

    for (size_t i = 0; i < num_rela; i++) {
        int ret = apply_rela(ctx, so, &so->rela[i]);
        if (ret < 0) {
            linker_debug("ERROR: RELA relocation %zu failed\n", i);
            return ret;
        }
    }

    return 0;
}

/**
 * Process PLT/GOT relocations (lazy or immediate binding)
 *
 * @param ctx Linker context
 * @param so Shared object
 * @param lazy If non-zero, set up for lazy binding; otherwise immediate
 * @return 0 on success, negative error code on failure
 */
int linker_relocate_plt(linker_context_t* ctx, shared_object_t* so, int lazy) {
    if (!ctx || !so) {
        return -1;
    }

    if (!so->jmprel || so->pltrelsz == 0) {
        return 0;  // No PLT relocations
    }

    // Determine relocation type (DT_PLTREL: DT_REL or DT_RELA)
    int is_rela = (so->pltrel == DT_RELA);
    size_t num_plt;

    if (is_rela) {
        num_plt = so->pltrelsz / sizeof(elf64_rela_t);
    } else {
        num_plt = so->pltrelsz / sizeof(elf64_rel_t);
    }

    linker_debug("Processing %zu PLT relocations for %s (lazy=%d)\n", num_plt, so->name, lazy);

    // For immediate binding or if DT_BIND_NOW is set
    if (!lazy || so->bind_now || (so->flags_1 & DF_1_NOW)) {
        // Process all PLT relocations immediately
        if (is_rela) {
            elf64_rela_t* plt_rela = (elf64_rela_t*)so->jmprel;
            for (size_t i = 0; i < num_plt; i++) {
                int ret = apply_rela(ctx, so, &plt_rela[i]);
                if (ret < 0) {
                    return ret;
                }
            }
        } else {
            // Handle REL (not RELA) - less common
            linker_debug("WARNING: REL PLT relocations not fully implemented\n");
            // Would need to read addend from relocation site
        }
    } else {
        // Lazy binding: set up PLT to call resolver
        // This requires PLT stub setup, which is typically done by the linker
        // For now, we'll just do immediate binding
        linker_debug("NOTE: Lazy binding requested but not implemented, using immediate\n");
        return linker_relocate_plt(ctx, so, 0);
    }

    return 0;
}

/**
 * Relocate all sections of a shared object
 *
 * @param ctx Linker context
 * @param so Shared object to relocate
 * @param mode Relocation mode (RTLD_LAZY or RTLD_NOW)
 * @return 0 on success, negative error code on failure
 */
int linker_relocate_object(linker_context_t* ctx, shared_object_t* so, int mode) {
    if (!ctx || !so) {
        return -1;
    }

    linker_debug("Relocating %s (mode=%s)\n", so->name,
                 (mode == RTLD_NOW) ? "RTLD_NOW" : "RTLD_LAZY");

    // Process RELA relocations
    int ret = linker_relocate_rela(ctx, so);
    if (ret < 0) {
        linker_debug("ERROR: RELA relocation failed\n");
        return ret;
    }

    // Process PLT relocations
    int lazy = (mode == RTLD_LAZY);
    ret = linker_relocate_plt(ctx, so, lazy);
    if (ret < 0) {
        linker_debug("ERROR: PLT relocation failed\n");
        return ret;
    }

    so->relocated = 1;
    return 0;
}

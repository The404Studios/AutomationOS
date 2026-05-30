/**
 * ELF64 Loader - Simplified Version (DISABLED)
 *
 * DISABLED: Full ELF loader implementation is in kernel/fs/elf_loader.c.
 * This file is kept for reference but compiles to an empty translation unit
 * to avoid duplicate symbol errors (elf_load, elf_validate_header).
 */

#ifdef ELF_USE_SIMPLE_LOADER  /* Only compile if full ELF loader is not available */

#include "../include/kernel.h"
#include "../include/elf.h"
#include "../include/initrd.h"
#include "../include/mem.h"
#include "../include/string.h"

int elf_validate_header(const elf64_ehdr_t* ehdr) {
    if (!ehdr) return 0;
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) return 0;
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) return 0;
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) return 0;
    if (ehdr->e_machine != EM_X86_64) return 0;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return 0;
    return 1;
}

int elf_load(const char* path, int argc, char** argv,
             uint64_t* entry_out, uint64_t* stack_out) {
    (void)argc; (void)argv; (void)stack_out;
    kprintf("[ELF] Loading %s (simple loader)...\n", path);
    uint64_t file_size = 0;
    void* file_data = initrd_get_file(path, &file_size);
    if (!file_data) return ELF_ERR_NOT_FOUND;
    const elf64_ehdr_t* ehdr = (const elf64_ehdr_t*)file_data;
    if (!elf_validate_header(ehdr)) return ELF_ERR_INVALID;
    *entry_out = ehdr->e_entry;
    return ELF_SUCCESS;
}

#endif /* ELF_USE_SIMPLE_LOADER */

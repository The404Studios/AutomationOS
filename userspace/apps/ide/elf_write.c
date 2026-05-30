/*
 * elf_write.c -- AutomationOS native toolchain ELF64 emitter.
 *
 * Final stage of the on-device pipeline (see tc.h):
 *   machine code --elf_write--> minimal static ELF64 the OS loader runs.
 *
 * Freestanding: no libc, no malloc, no stdio. Uses only fixed-size byte
 * stores into the caller-provided output buffer. Every multi-byte field is
 * written explicitly in little-endian order at its exact ELF offset, so the
 * result is correct regardless of host endianness or struct packing.
 *
 * TARGET ABI (must match kernel/fs/exec.c + userspace.ld):
 *   - Static ELF64, ET_EXEC, EM_X86_64 (62), little-endian.
 *   - Exactly one PT_LOAD: p_offset=0, p_vaddr=0x200000, p_filesz=p_memsz=
 *     whole file, flags = R|W|X.
 *   - Headers (Ehdr 64 + 1 Phdr 56 = 120 = TC_ELF_HDR_SZ) sit at segment
 *     start; code begins right after at file offset 120.
 *   - e_entry = 0x200000 + 120 = first emitted instruction (_start).
 */

#include "tc.h"

/* ---- ELF constants (kept local so this file needs no system headers) ---- */
#define EI_NIDENT          16
#define ELFCLASS64         2
#define ELFDATA2LSB        1
#define EV_CURRENT         1
#define ELFOSABI_SYSV      0
#define ET_EXEC            2
#define EM_X86_64_LOCAL    62
#define PT_LOAD            1
#define PF_X_LOCAL         1
#define PF_W_LOCAL         2
#define PF_R_LOCAL         4

/* ELF64 header / program-header field offsets (canonical layout). */
#define EHDR_SIZE          64
#define PHDR_SIZE          56

/* Offsets within the 64-byte Elf64_Ehdr. */
#define E_TYPE_OFF         16   /* uint16 */
#define E_MACHINE_OFF      18   /* uint16 */
#define E_VERSION_OFF      20   /* uint32 */
#define E_ENTRY_OFF        24   /* uint64 */
#define E_PHOFF_OFF        32   /* uint64 */
#define E_SHOFF_OFF        40   /* uint64 */
#define E_FLAGS_OFF        48   /* uint32 */
#define E_EHSIZE_OFF       52   /* uint16 */
#define E_PHENTSIZE_OFF    54   /* uint16 */
#define E_PHNUM_OFF        56   /* uint16 */
#define E_SHENTSIZE_OFF    58   /* uint16 */
#define E_SHNUM_OFF        60   /* uint16 */
#define E_SHSTRNDX_OFF     62   /* uint16 */

/* Offsets within the 56-byte Elf64_Phdr (relative to phdr start = +64). */
#define P_TYPE_OFF         0    /* uint32 */
#define P_FLAGS_OFF        4    /* uint32 */
#define P_OFFSET_OFF       8    /* uint64 */
#define P_VADDR_OFF        16   /* uint64 */
#define P_PADDR_OFF        24   /* uint64 */
#define P_FILESZ_OFF       32   /* uint64 */
#define P_MEMSZ_OFF        40   /* uint64 */
#define P_ALIGN_OFF        48   /* uint64 */

/* ---- little-endian byte-store helpers (no host-endianness assumption) ---- */

static void put8(uint8_t* p, uint8_t v) {
    p[0] = v;
}

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put64(uint8_t* p, uint64_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
    p[4] = (uint8_t)((v >> 32) & 0xFF);
    p[5] = (uint8_t)((v >> 40) & 0xFF);
    p[6] = (uint8_t)((v >> 48) & 0xFF);
    p[7] = (uint8_t)((v >> 56) & 0xFF);
}

/*
 * elf_write -- emit a minimal static ELF64 wrapping `code`.
 *
 * Layout: [Elf64_Ehdr = 64][one Elf64_Phdr = 56][code...]  (120 + code_len)
 *
 * Returns total byte length, or <0 on bad args / capacity overflow.
 */
int elf_write(const uint8_t* code, int code_len, uint8_t* elf_out, int elf_cap) {
    /* Argument validation (NULL/bounds-safe). */
    if (!elf_out || elf_cap < 0) {
        return -1;
    }
    if (code_len < 0) {
        return -1;
    }
    /* code may only be NULL if there is nothing to copy. */
    if (code_len > 0 && !code) {
        return -1;
    }

    /* Total file size: 120-byte header block + the code. Guard the addition
     * against integer overflow before comparing to capacity. */
    if (code_len > 0x7FFFFFFF - TC_ELF_HDR_SZ) {
        return -1;
    }
    int total = TC_ELF_HDR_SZ + code_len;   /* == 120 + code_len */
    if (total > elf_cap) {
        return -2;
    }

    /* The header block is exactly 64 + 56 = 120 = TC_ELF_HDR_SZ. */
    uint8_t* eh = elf_out;            /* Elf64_Ehdr at offset 0   */
    uint8_t* ph = elf_out + EHDR_SIZE; /* Elf64_Phdr at offset 64 */

    /* ---- e_ident[16] ---- */
    put8(eh + 0, 0x7F);
    put8(eh + 1, 'E');
    put8(eh + 2, 'L');
    put8(eh + 3, 'F');
    put8(eh + 4, ELFCLASS64);     /* EI_CLASS   = 64-bit         */
    put8(eh + 5, ELFDATA2LSB);    /* EI_DATA    = little-endian  */
    put8(eh + 6, EV_CURRENT);     /* EI_VERSION = 1              */
    put8(eh + 7, ELFOSABI_SYSV);  /* EI_OSABI   = SysV (0)       */
    put8(eh + 8, 0);              /* EI_ABIVERSION               */
    put8(eh + 9, 0);              /* EI_PAD ...                  */
    put8(eh + 10, 0);
    put8(eh + 11, 0);
    put8(eh + 12, 0);
    put8(eh + 13, 0);
    put8(eh + 14, 0);
    put8(eh + 15, 0);

    /* ---- remaining Ehdr fields (little-endian, exact offsets) ---- */
    put16(eh + E_TYPE_OFF,      ET_EXEC);                /* e_type      = ET_EXEC  */
    put16(eh + E_MACHINE_OFF,   EM_X86_64_LOCAL);        /* e_machine   = 62       */
    put32(eh + E_VERSION_OFF,   EV_CURRENT);             /* e_version   = 1        */
    put64(eh + E_ENTRY_OFF,     (uint64_t)TC_ENTRY_VADDR); /* e_entry   = 0x200078 */
    put64(eh + E_PHOFF_OFF,     EHDR_SIZE);              /* e_phoff     = 64       */
    put64(eh + E_SHOFF_OFF,     0);                      /* e_shoff     = 0        */
    put32(eh + E_FLAGS_OFF,     0);                      /* e_flags     = 0        */
    put16(eh + E_EHSIZE_OFF,    EHDR_SIZE);              /* e_ehsize    = 64       */
    put16(eh + E_PHENTSIZE_OFF, PHDR_SIZE);              /* e_phentsize = 56       */
    put16(eh + E_PHNUM_OFF,     1);                      /* e_phnum     = 1        */
    put16(eh + E_SHENTSIZE_OFF, 0);                      /* e_shentsize = 0        */
    put16(eh + E_SHNUM_OFF,     0);                      /* e_shnum     = 0        */
    put16(eh + E_SHSTRNDX_OFF,  0);                      /* e_shstrndx  = 0        */

    /* ---- single PT_LOAD program header ---- */
    put32(ph + P_TYPE_OFF,   PT_LOAD);                            /* p_type   = PT_LOAD   */
    put32(ph + P_FLAGS_OFF,  PF_R_LOCAL | PF_W_LOCAL | PF_X_LOCAL); /* p_flags = R|W|X (7) */
    put64(ph + P_OFFSET_OFF, 0);                                 /* p_offset = 0         */
    put64(ph + P_VADDR_OFF,  (uint64_t)TC_BASE_VADDR);           /* p_vaddr  = 0x200000  */
    put64(ph + P_PADDR_OFF,  (uint64_t)TC_BASE_VADDR);           /* p_paddr  = 0x200000  */
    put64(ph + P_FILESZ_OFF, (uint64_t)total);                  /* p_filesz = 120+code  */
    put64(ph + P_MEMSZ_OFF,  (uint64_t)total);                  /* p_memsz  = 120+code  */
    put64(ph + P_ALIGN_OFF,  0x1000);                            /* p_align  = 4096      */

    /* ---- append code right after the 120-byte header block ---- */
    {
        uint8_t* dst = elf_out + TC_ELF_HDR_SZ;
        for (int i = 0; i < code_len; i++) {
            dst[i] = code[i];
        }
    }

    return total;
}

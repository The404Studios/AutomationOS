/*
 * tc.h -- AutomationOS native toolchain (the IDE builds its own programs).
 *
 * Pipeline:   C source --(existing lexer/parser)--> AST --cc_compile--> asm text
 *             asm text --as_assemble--> x86-64 machine code (absolute @ fixed base)
 *             machine code --elf_write--> static ELF64 the OS loader runs (SYS_SPAWN)
 *
 * Languages: C (subset) and x86-64 ASM (Intel subset) are real, on-device.
 *            C++ is attempted best-effort as C; C# is unsupported (no CLR).
 *
 * Freestanding throughout (no libc/malloc): callers pass fixed buffers.
 *
 * ===========================================================================
 *  TARGET ABI (must match kernel/fs/exec.c + userspace.ld)
 * ===========================================================================
 *  - Static ELF64, ET_EXEC, e_machine = EM_X86_64 (62), little-endian.
 *  - One PT_LOAD: p_offset=0, p_vaddr=0x200000, p_filesz=p_memsz=whole file,
 *    flags = R|W|X. The ELF headers (Ehdr 64 + 1 Phdr 56 = 120 bytes) sit at
 *    the start of the segment; code begins right after them.
 *  - e_entry = 0x200000 + 120 = first emitted instruction (must be _start).
 *  - Entered with RSP 16-byte aligned near 0x7FFFFFFFE000. No argc/argv.
 *  - Syscalls: `syscall`; number in RAX; args RDI,RSI,RDX,R10,R8,R9; ret RAX.
 *    SYS_EXIT=0 (rdi=code), SYS_WRITE=3 (rdi=fd, rsi=buf, rdx=len).
 *
 * ===========================================================================
 *  SUPPORTED ASSEMBLY SUBSET (Intel syntax, one instruction per line)
 *  The C codegen emits ONLY this subset; the assembler accepts exactly it.
 * ===========================================================================
 *  Directives:  `section .text` | `section .data` ; `global NAME` (ignored ok)
 *  Labels:      `name:`  (defines a label at the current address)
 *  Data:        `db <bytes/ "str" , ...>` ; `dq <imm/label>` (in .data)
 *  Registers:   rax rbx rcx rdx rsi rdi rbp rsp r8..r15 ; al (low byte) for char
 *  Operands:    register | immediate (decimal or 0x..) | label |
 *               memory `[reg]`, `[reg+disp]`, `[reg-disp]`  (disp decimal)
 *  Instructions the codegen uses (encode these):
 *    mov (reg,reg / reg,imm / reg,[mem] / [mem],reg / reg,label[=abs imm64] /
 *         byte: movzx reg,byte [mem] ; mov byte [mem], al)
 *    lea reg, [reg+disp] ; push reg ; pop reg
 *    add sub imul (reg,reg / reg,imm) ; cqo ; idiv reg
 *    and or xor (reg,reg / reg,imm) ; shl shr (reg,imm/cl) ; neg reg ; not reg
 *    cmp (reg,reg / reg,imm) ; test reg,reg
 *    setcc al (sete setne setl setle setg setge) ; movzx rax, al
 *    jmp label ; je/jne/jl/jle/jg/jge label (rel32) ; call label ; ret ; leave
 *    syscall
 *  Label addressing is ABSOLUTE (non-PIE, fixed base): `mov rax, label` loads
 *  the label's absolute address as imm64; call/jmp/jcc use rel32 from next ip.
 * ===========================================================================
 */
#ifndef IDE_TC_H
#define IDE_TC_H

#include <stdint.h>
#include "ide_ast.h"

#define TC_BASE_VADDR   0x200000ULL
#define TC_ELF_HDR_SZ   120
#define TC_ENTRY_VADDR  (TC_BASE_VADDR + TC_ELF_HDR_SZ)
#define TC_SYS_EXIT     0
#define TC_SYS_WRITE    3

typedef enum { LANG_UNKNOWN = 0, LANG_C, LANG_ASM, LANG_CPP, LANG_CSHARP } TcLang;

typedef struct { int line; char msg[120]; } TcDiag;
#define TC_MAXDIAG  64
#define TC_ASM_CAP  (128 * 1024)
#define TC_CODE_CAP (64 * 1024)
#define TC_ELF_CAP  (96 * 1024)

typedef struct {
    int    ok;                       /* 1 = produced a runnable ELF        */
    TcLang lang;
    char   out_path[160];            /* ELF written here                   */
    char   out_dir[160];             /* the build/ folder the ELF lives in */
    int    code_len, elf_len;
    char   asm_preview[1536];        /* first ~lines of generated asm      */
    TcDiag diags[TC_MAXDIAG]; int ndiags;
    char   message[160];             /* summary / why unsupported          */
} TcResult;

/* extension -> language */
TcLang tc_lang_of(const char* path);

/* Read `path`, build it, write the ELF (same dir, basename without extension),
 * fill *res, return res->ok. (tc_driver.c) */
int tc_build(const char* path, TcResult* res);

/* ---- individual stages (driver + host tests call these) ---- */
/* C AST -> Intel-subset asm text. 1=ok. (cc_codegen.c) */
int cc_compile(const AstNode* tu, char* asm_out, int asm_cap,
               TcDiag* diags, int* ndiags);
/* asm text -> machine code, labels resolved at `base`. 1=ok, *code_len set. (as_x64.c) */
int as_assemble(const char* text, uint64_t base, uint8_t* code_out, int code_cap,
                int* code_len, TcDiag* diags, int* ndiags);
/* machine code -> static ELF64. Returns ELF byte length, or <0 on error. (elf_write.c) */
int elf_write(const uint8_t* code, int code_len, uint8_t* elf_out, int elf_cap);

#endif /* IDE_TC_H */

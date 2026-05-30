/*
 * as_x64.c -- native x86-64 assembler for the AutomationOS toolchain.
 *
 * Turns Intel-syntax assembly text (the exact subset the C codegen emits, see
 * tc.h "SUPPORTED ASSEMBLY SUBSET") into machine code at a fixed absolute base.
 *
 * Freestanding: no libc / malloc / stdio. STATIC scratch buffers only. All
 * string / number helpers are local. Two-pass:
 *   pass 1 -- lay .text from `base`, then .data right after; collect labels.
 *   pass 2 -- encode each line, resolving labels to absolute addrs / rel32.
 * A single encode routine `enc_line` runs in both passes: when `S->out` is NULL
 * it only measures (advances the cursor), so pass-1 sizes == pass-2 emission.
 */
#include <stdint.h>
#include "tc.h"

/* ----------------------------------------------------------------------- *
 *  Local helpers (no libc).
 * ----------------------------------------------------------------------- */

static int as_isspace(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int as_isdigit(char c) { return c >= '0' && c <= '9'; }
static int as_ishex(char c)   { return as_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static char as_lower(char c)  { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int as_isidc(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$';
}

/* case-insensitive compare of NUL-terminated token */
static int as_ieq(const char* a, const char* b) {
    while (*a && *b) {
        if (as_lower(*a) != as_lower(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int as_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int as_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void as_cpy(char* d, const char* s, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* append decimal of v to dst (which is NUL terminated), bounded by cap */
static void as_app(char* dst, int cap, const char* s) {
    int n = as_strlen(dst);
    int i = 0;
    while (s[i] && n < cap - 1) dst[n++] = s[i++];
    dst[n] = 0;
}

/* ----------------------------------------------------------------------- *
 *  Token / line model.
 * ----------------------------------------------------------------------- */

#define AS_MAXTOK   64    /* per-line raw token bytes */
#define AS_MAXLABEL 256   /* symbol table size */
#define AS_NAMELEN  64

typedef struct { char name[AS_NAMELEN]; uint64_t addr; } AsSym;

typedef struct {
    const char* p;        /* cursor into source line                       */
} AsLex;

/* A decoded operand. */
typedef enum {
    OP_NONE = 0,
    OP_REG,    /* 64-bit register, .reg = 0..15                            */
    OP_AL,     /* the AL byte register                                     */
    OP_CL,     /* the CL register (for shift by cl)                        */
    OP_IMM,    /* immediate; .imm holds value, .has_label if symbolic      */
    OP_MEM,    /* [reg], [reg+disp], [reg-disp]; .reg + .disp + .has_disp  */
    OP_LABELREF /* a bare symbol used as an immediate (mov reg,label)      */
} AsOpKind;

typedef struct {
    AsOpKind kind;
    int      reg;        /* register number 0..15 (for REG / MEM base)     */
    long     disp;       /* memory displacement                            */
    int      has_disp;
    long     imm;        /* immediate value                                */
    int      has_label;  /* IMM/LABELREF: value came from a label (abs)    */
    char     label[AS_NAMELEN];
    int      is_byte;    /* operand carried a `byte` size override (mem)   */
} AsOperand;

/* Assembler state shared across the encode routine. */
typedef struct {
    uint8_t* out;     /* NULL => measure-only (pass 1)                     */
    int      cap;     /* code_cap                                          */
    int      pos;     /* current byte offset written/measured              */
    uint64_t base;    /* absolute base of .text                            */
    uint64_t cur;     /* absolute address of current instruction           */
    AsSym*   syms;
    int      nsyms;
    TcDiag*  diags;
    int*     ndiags;
    int      fatal;
    int      line;    /* current source line (1-based)                     */
} AsState;

/* ----------------------------------------------------------------------- *
 *  Diagnostics.
 * ----------------------------------------------------------------------- */

static void as_diag(AsState* S, int line, const char* msg) {
    if (!S->diags || !S->ndiags) return;
    if (*S->ndiags >= TC_MAXDIAG) return;
    TcDiag* d = &S->diags[*S->ndiags];
    d->line = line;
    as_cpy(d->msg, msg, (int)sizeof(d->msg));
    (*S->ndiags)++;
}

static void as_diag2(AsState* S, int line, const char* msg, const char* extra) {
    char buf[120];
    buf[0] = 0;
    as_app(buf, (int)sizeof(buf), msg);
    if (extra) { as_app(buf, (int)sizeof(buf), " '"); as_app(buf, (int)sizeof(buf), extra); as_app(buf, (int)sizeof(buf), "'"); }
    as_diag(S, line, buf);
}

/* ----------------------------------------------------------------------- *
 *  Symbol table.
 * ----------------------------------------------------------------------- */

static int as_sym_add(AsState* S, const char* name, uint64_t addr) {
    if (S->nsyms >= AS_MAXLABEL) return 0;
    /* dedup: redefinition just updates (codegen labels are unique anyway) */
    for (int i = 0; i < S->nsyms; i++) {
        if (as_streq(S->syms[i].name, name)) { S->syms[i].addr = addr; return 1; }
    }
    as_cpy(S->syms[S->nsyms].name, name, AS_NAMELEN);
    S->syms[S->nsyms].addr = addr;
    S->nsyms++;
    return 1;
}

static int as_sym_find(AsState* S, const char* name, uint64_t* out) {
    for (int i = 0; i < S->nsyms; i++) {
        if (as_streq(S->syms[i].name, name)) { *out = S->syms[i].addr; return 1; }
    }
    return 0;
}

/* ----------------------------------------------------------------------- *
 *  Lexing primitives.
 * ----------------------------------------------------------------------- */

static void lx_skipws(AsLex* L) { while (*L->p && as_isspace(*L->p)) L->p++; }

/* Read an identifier/mnemonic token into out (lowercased? no -- keep raw). */
static int lx_ident(AsLex* L, char* out, int cap) {
    lx_skipws(L);
    int n = 0;
    while (*L->p && as_isidc(*L->p)) {
        if (n < cap - 1) out[n] = *L->p;
        n++; L->p++;
    }
    if (n == 0) { out[0] = 0; return 0; }
    if (n > cap - 1) n = cap - 1;
    out[n] = 0;
    return 1;
}

/* Parse a numeric literal (decimal, 0x.., optional leading -). */
static int lx_number(AsLex* L, long* out) {
    lx_skipws(L);
    const char* p = L->p;
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }
    if (!as_isdigit(*p)) return 0;
    long val = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        if (!as_ishex(*p)) return 0;
        while (as_ishex(*p)) {
            int d;
            if (*p <= '9') d = *p - '0';
            else d = (as_lower(*p) - 'a') + 10;
            val = val * 16 + d;
            p++;
        }
    } else {
        while (as_isdigit(*p)) { val = val * 10 + (*p - '0'); p++; }
    }
    if (neg) val = -val;
    *out = val;
    L->p = p;
    return 1;
}

/* ----------------------------------------------------------------------- *
 *  Register names.
 * ----------------------------------------------------------------------- */

/* Returns 0..15 for a 64-bit reg name, or -1.  *is_al set for "al". */
static int reg_num(const char* s) {
    static const char* names[16] = {
        "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15"
    };
    for (int i = 0; i < 16; i++) if (as_ieq(s, names[i])) return i;
    return -1;
}

/* ----------------------------------------------------------------------- *
 *  Operand parsing.
 * ----------------------------------------------------------------------- */

/* Parse a single operand starting at L. Returns 1 on success. */
static int parse_operand(AsState* S, AsLex* L, AsOperand* op) {
    op->kind = OP_NONE;
    op->reg = 0; op->disp = 0; op->has_disp = 0;
    op->imm = 0; op->has_label = 0; op->label[0] = 0; op->is_byte = 0;
    lx_skipws(L);
    if (!*L->p) return 0;

    /* optional size override: "byte" / "qword" preceding a [mem] */
    /* peek an identifier without consuming if it isn't a size keyword */
    if ((L->p[0]=='b'||L->p[0]=='B'||L->p[0]=='q'||L->p[0]=='Q'||L->p[0]=='d'||L->p[0]=='D'||L->p[0]=='w'||L->p[0]=='W')) {
        const char* save = L->p;
        char kw[16];
        if (lx_ident(L, kw, sizeof(kw))) {
            if (as_ieq(kw, "byte")) { op->is_byte = 1; lx_skipws(L); }
            else if (as_ieq(kw, "qword") || as_ieq(kw, "dword") || as_ieq(kw, "word")) { lx_skipws(L); }
            else { L->p = save; }
        } else { L->p = save; }
        /* allow optional "ptr" keyword */
        if (op->is_byte || L->p != save) {
            const char* s2 = L->p;
            char kw2[16];
            if (lx_ident(L, kw2, sizeof(kw2))) {
                if (!as_ieq(kw2, "ptr")) L->p = s2;
                lx_skipws(L);
            } else L->p = s2;
        }
    }

    /* memory operand [ ... ] */
    if (*L->p == '[') {
        L->p++;
        char rname[AS_NAMELEN];
        if (!lx_ident(L, rname, sizeof(rname))) {
            as_diag(S, S->line, "bad memory operand: expected register");
            return 0;
        }
        int r = reg_num(rname);
        if (r < 0) {
            as_diag2(S, S->line, "bad memory base register", rname);
            return 0;
        }
        op->kind = OP_MEM;
        op->reg = r;
        lx_skipws(L);
        if (*L->p == '+' || *L->p == '-') {
            int neg = (*L->p == '-');
            L->p++;
            long d;
            if (!lx_number(L, &d)) {
                as_diag(S, S->line, "bad memory displacement");
                return 0;
            }
            op->disp = neg ? -d : d;
            op->has_disp = 1;
        }
        lx_skipws(L);
        if (*L->p != ']') {
            as_diag(S, S->line, "memory operand missing ']'");
            return 0;
        }
        L->p++;
        return 1;
    }

    /* numeric immediate */
    {
        const char* save = L->p;
        long v;
        if ((*L->p == '+' || *L->p == '-' || as_isdigit(*L->p)) && lx_number(L, &v)) {
            op->kind = OP_IMM;
            op->imm = v;
            return 1;
        }
        L->p = save;
    }

    /* identifier: register, al/cl, or label reference */
    {
        char id[AS_NAMELEN];
        if (!lx_ident(L, id, sizeof(id))) {
            as_diag(S, S->line, "expected operand");
            return 0;
        }
        if (as_ieq(id, "al")) { op->kind = OP_AL; return 1; }
        if (as_ieq(id, "cl")) { op->kind = OP_CL; return 1; }
        int r = reg_num(id);
        if (r >= 0) { op->kind = OP_REG; op->reg = r; return 1; }
        /* otherwise a label reference -> absolute imm64 */
        op->kind = OP_LABELREF;
        as_cpy(op->label, id, AS_NAMELEN);
        op->has_label = 1;
        return 1;
    }
}

/* Resolve a label-bearing operand's value in pass 2 (no-op in pass 1). */
static long resolve_label(AsState* S, AsOperand* op) {
    if (!op->has_label) return op->imm;
    uint64_t a;
    if (S->out) { /* pass 2: must resolve */
        if (as_sym_find(S, op->label, &a)) return (long)a;
        as_diag2(S, S->line, "undefined label", op->label);
        return 0;
    }
    /* pass 1: any 8-byte value works for sizing; imm64 form is fixed-length */
    return 0;
}

/* ----------------------------------------------------------------------- *
 *  Byte emission.
 * ----------------------------------------------------------------------- */

static void emit_b(AsState* S, uint8_t b) {
    if (S->pos < S->cap) {
        if (S->out) S->out[S->pos] = b;
    } else {
        S->fatal = 1;   /* overflow */
    }
    S->pos++;
}
static void emit_imm16(AsState* S, long v) {
    emit_b(S, (uint8_t)(v & 0xff));
    emit_b(S, (uint8_t)((v >> 8) & 0xff));
}
static void emit_imm32(AsState* S, long v) {
    emit_b(S, (uint8_t)(v & 0xff));
    emit_b(S, (uint8_t)((v >> 8) & 0xff));
    emit_b(S, (uint8_t)((v >> 16) & 0xff));
    emit_b(S, (uint8_t)((v >> 24) & 0xff));
}
static void emit_imm64(AsState* S, uint64_t v) {
    for (int i = 0; i < 8; i++) emit_b(S, (uint8_t)((v >> (i * 8)) & 0xff));
}

/* REX prefix.  w/r/x/b are 0/1. Always emitted when forced (e.g. W=1). */
static void emit_rex(AsState* S, int w, int r, int x, int b) {
    emit_b(S, (uint8_t)(0x40 | (w << 3) | (r << 2) | (x << 1) | b));
}

/* int8 fits? */
static int fits_i8(long v)  { return v >= -128 && v <= 127; }
static int fits_i32(long v) { return v >= -2147483647L - 1 && v <= 2147483647L; }

/*
 * Encode a register-direct ModRM (mod=11): reg field = `regfield`,
 * rm = `rm`. Caller already emitted REX (with R bit = regfield>>3,
 * B bit = rm>>3).
 */
static void emit_modrm_rr(AsState* S, int regfield, int rm) {
    emit_b(S, (uint8_t)(0xC0 | ((regfield & 7) << 3) | (rm & 7)));
}

/*
 * Encode a memory ModRM for [base], [base+disp], [base-disp].
 * regfield = opcode reg/extension. base = 0..15. disp + has_disp from operand.
 * Handles RSP (needs SIB) and RBP (needs disp8=0 even when no disp).
 */
static void emit_modrm_mem(AsState* S, int regfield, int base, long disp, int has_disp) {
    int rmlow = base & 7;
    int mod;
    int need_disp32 = 0, need_disp8 = 0;

    if (!has_disp) {
        /* [rbp]/[r13] cannot be encoded with mod=00,rm=101 (=RIP/disp32),
         * so force disp8=0. */
        if (rmlow == 5) { mod = 1; need_disp8 = 1; disp = 0; }
        else { mod = 0; }
    } else if (fits_i8(disp)) {
        mod = 1; need_disp8 = 1;
    } else {
        mod = 2; need_disp32 = 1;
    }

    emit_b(S, (uint8_t)((mod << 6) | ((regfield & 7) << 3) | rmlow));
    /* RSP/R12 (rmlow==4) require a SIB byte: scale=0,index=100(none),base */
    if (rmlow == 4) {
        emit_b(S, (uint8_t)(0x00 | (4 << 3) | rmlow));
    }
    if (need_disp8)  emit_b(S, (uint8_t)(disp & 0xff));
    if (need_disp32) emit_imm32(S, disp);
}

/* ----------------------------------------------------------------------- *
 *  Condition-code tables (setcc / jcc).
 * ----------------------------------------------------------------------- */

/* returns the cc nibble for the suffix, or -1.  e/z=4 ne/nz=5 l=C le=E g=F ge=D */
static int cc_of(const char* suf) {
    if (as_ieq(suf, "e")  || as_ieq(suf, "z"))  return 0x4;
    if (as_ieq(suf, "ne") || as_ieq(suf, "nz")) return 0x5;
    if (as_ieq(suf, "l"))  return 0xC;
    if (as_ieq(suf, "le")) return 0xE;
    if (as_ieq(suf, "g"))  return 0xF;
    if (as_ieq(suf, "ge")) return 0xD;
    return -1;
}

/* ----------------------------------------------------------------------- *
 *  ALU group helpers (add/sub/and/or/xor/cmp): one /digit + opcodes.
 * ----------------------------------------------------------------------- */

typedef struct {
    uint8_t rr_op;   /* opcode for `op r/m64, r64`  (e.g. add=01)          */
    int     ext;     /* /digit for the 81/83 imm form                      */
} AluInfo;

/* fill *out, return 1 if `m` is an ALU mnemonic */
static int alu_lookup(const char* m, AluInfo* out) {
    if (as_ieq(m, "add")) { out->rr_op = 0x01; out->ext = 0; return 1; }
    if (as_ieq(m, "or"))  { out->rr_op = 0x09; out->ext = 1; return 1; }
    if (as_ieq(m, "and")) { out->rr_op = 0x21; out->ext = 4; return 1; }
    if (as_ieq(m, "sub")) { out->rr_op = 0x29; out->ext = 5; return 1; }
    if (as_ieq(m, "xor")) { out->rr_op = 0x31; out->ext = 6; return 1; }
    if (as_ieq(m, "cmp")) { out->rr_op = 0x39; out->ext = 7; return 1; }
    return 0;
}

/* ----------------------------------------------------------------------- *
 *  Operand list parser: parse up to 2 comma-separated operands.
 * ----------------------------------------------------------------------- */

static int parse_ops(AsState* S, AsLex* L, AsOperand* a, AsOperand* b, int* count) {
    *count = 0;
    a->kind = OP_NONE; b->kind = OP_NONE;
    lx_skipws(L);
    if (!*L->p || *L->p == ';' || *L->p == '#') return 1;  /* no operands */
    if (!parse_operand(S, L, a)) return 0;
    *count = 1;
    lx_skipws(L);
    if (*L->p == ',') {
        L->p++;
        if (!parse_operand(S, L, b)) return 0;
        *count = 2;
    }
    lx_skipws(L);
    return 1;
}

/* ----------------------------------------------------------------------- *
 *  Instruction encoders.
 * ----------------------------------------------------------------------- */

/* mov encodings */
static void enc_mov(AsState* S, AsOperand* d, AsOperand* s, int nops) {
    if (nops != 2) { as_diag(S, S->line, "mov needs 2 operands"); return; }

    /* mov byte [mem], al   -> 88 /r */
    if (d->kind == OP_MEM && d->is_byte && s->kind == OP_AL) {
        int b = d->reg;
        if (b >= 8) emit_rex(S, 0, 0, 0, 1);
        emit_b(S, 0x88);
        emit_modrm_mem(S, 0 /*al*/, b, d->disp, d->has_disp);
        return;
    }
    /* mov reg, reg  -> 89 /r (store src into dst): REX.W, src in reg, dst in rm */
    if (d->kind == OP_REG && s->kind == OP_REG) {
        emit_rex(S, 1, s->reg >> 3, 0, d->reg >> 3);
        emit_b(S, 0x89);
        emit_modrm_rr(S, s->reg, d->reg);
        return;
    }
    /* mov reg, [mem]  -> 8B /r */
    if (d->kind == OP_REG && s->kind == OP_MEM) {
        emit_rex(S, 1, d->reg >> 3, 0, s->reg >> 3);
        emit_b(S, 0x8B);
        emit_modrm_mem(S, d->reg, s->reg, s->disp, s->has_disp);
        return;
    }
    /* mov [mem], reg  -> 89 /r */
    if (d->kind == OP_MEM && s->kind == OP_REG) {
        emit_rex(S, 1, s->reg >> 3, 0, d->reg >> 3);
        emit_b(S, 0x89);
        emit_modrm_mem(S, s->reg, d->reg, d->disp, d->has_disp);
        return;
    }
    /* mov reg, label  -> imm64 absolute (REX.W B8+rd io) */
    if (d->kind == OP_REG && s->kind == OP_LABELREF) {
        long v = resolve_label(S, s);
        emit_rex(S, 1, 0, 0, d->reg >> 3);
        emit_b(S, (uint8_t)(0xB8 + (d->reg & 7)));
        emit_imm64(S, (uint64_t)v);
        return;
    }
    /* mov reg, imm */
    if (d->kind == OP_REG && s->kind == OP_IMM) {
        long v = s->imm;
        if (fits_i32(v)) {
            /* REX.W C7 /0 id  (sign-extended imm32) */
            emit_rex(S, 1, 0, 0, d->reg >> 3);
            emit_b(S, 0xC7);
            emit_modrm_rr(S, 0, d->reg);
            emit_imm32(S, v);
        } else {
            /* REX.W B8+rd io  (full imm64) */
            emit_rex(S, 1, 0, 0, d->reg >> 3);
            emit_b(S, (uint8_t)(0xB8 + (d->reg & 7)));
            emit_imm64(S, (uint64_t)v);
        }
        return;
    }
    as_diag(S, S->line, "unsupported mov form");
}

/* movzx reg, byte [mem]  | movzx rax, al */
static void enc_movzx(AsState* S, AsOperand* d, AsOperand* s, int nops) {
    if (nops != 2 || d->kind != OP_REG) { as_diag(S, S->line, "bad movzx"); return; }
    if (s->kind == OP_MEM) {
        /* REX.W 0F B6 /r */
        emit_rex(S, 1, d->reg >> 3, 0, s->reg >> 3);
        emit_b(S, 0x0F); emit_b(S, 0xB6);
        emit_modrm_mem(S, d->reg, s->reg, s->disp, s->has_disp);
        return;
    }
    if (s->kind == OP_AL) {
        /* movzx r64, al : REX.W 0F B6 /r, rm = al (0) */
        emit_rex(S, 1, d->reg >> 3, 0, 0);
        emit_b(S, 0x0F); emit_b(S, 0xB6);
        emit_modrm_rr(S, d->reg, 0);
        return;
    }
    as_diag(S, S->line, "unsupported movzx form");
}

/* lea reg, [mem]  -> 8D /r */
static void enc_lea(AsState* S, AsOperand* d, AsOperand* s, int nops) {
    if (nops != 2 || d->kind != OP_REG || s->kind != OP_MEM) {
        as_diag(S, S->line, "bad lea"); return;
    }
    emit_rex(S, 1, d->reg >> 3, 0, s->reg >> 3);
    emit_b(S, 0x8D);
    emit_modrm_mem(S, d->reg, s->reg, s->disp, s->has_disp);
}

/* push reg / pop reg */
static void enc_push(AsState* S, AsOperand* a, int nops) {
    if (nops != 1 || a->kind != OP_REG) { as_diag(S, S->line, "bad push"); return; }
    if (a->reg >= 8) emit_rex(S, 0, 0, 0, 1);
    emit_b(S, (uint8_t)(0x50 + (a->reg & 7)));
}
static void enc_pop(AsState* S, AsOperand* a, int nops) {
    if (nops != 1 || a->kind != OP_REG) { as_diag(S, S->line, "bad pop"); return; }
    if (a->reg >= 8) emit_rex(S, 0, 0, 0, 1);
    emit_b(S, (uint8_t)(0x58 + (a->reg & 7)));
}

/* add/sub/and/or/xor/cmp  reg,reg | reg,imm */
static void enc_alu(AsState* S, AluInfo* ai, AsOperand* d, AsOperand* s, int nops) {
    if (nops != 2 || d->kind != OP_REG) { as_diag(S, S->line, "bad alu form"); return; }
    if (s->kind == OP_REG) {
        emit_rex(S, 1, s->reg >> 3, 0, d->reg >> 3);
        emit_b(S, ai->rr_op);
        emit_modrm_rr(S, s->reg, d->reg);
        return;
    }
    if (s->kind == OP_IMM || s->kind == OP_LABELREF) {
        long v = resolve_label(S, s);
        emit_rex(S, 1, 0, 0, d->reg >> 3);
        if (!s->has_label && fits_i8(v)) {
            emit_b(S, 0x83);
            emit_modrm_rr(S, ai->ext, d->reg);
            emit_b(S, (uint8_t)(v & 0xff));
        } else {
            emit_b(S, 0x81);
            emit_modrm_rr(S, ai->ext, d->reg);
            emit_imm32(S, v);
        }
        return;
    }
    as_diag(S, S->line, "bad alu operand");
}

/* imul reg,reg -> 0F AF /r */
static void enc_imul(AsState* S, AsOperand* d, AsOperand* s, int nops) {
    if (nops != 2 || d->kind != OP_REG || s->kind != OP_REG) {
        as_diag(S, S->line, "bad imul"); return;
    }
    emit_rex(S, 1, d->reg >> 3, 0, s->reg >> 3);
    emit_b(S, 0x0F); emit_b(S, 0xAF);
    emit_modrm_rr(S, d->reg, s->reg);
}

/* F7 /digit unary: neg(/3) not(/2) idiv(/7) ; also mul/div not needed */
static void enc_f7(AsState* S, int ext, AsOperand* a, int nops) {
    if (nops != 1 || a->kind != OP_REG) { as_diag(S, S->line, "bad unary op"); return; }
    emit_rex(S, 1, 0, 0, a->reg >> 3);
    emit_b(S, 0xF7);
    emit_modrm_rr(S, ext, a->reg);
}

/* shl/shr reg,imm8 (C1 /ext ib) | reg,cl (D3 /ext) */
static void enc_shift(AsState* S, int ext, AsOperand* d, AsOperand* s, int nops) {
    if (nops != 2 || d->kind != OP_REG) { as_diag(S, S->line, "bad shift"); return; }
    if (s->kind == OP_CL) {
        emit_rex(S, 1, 0, 0, d->reg >> 3);
        emit_b(S, 0xD3);
        emit_modrm_rr(S, ext, d->reg);
        return;
    }
    if (s->kind == OP_IMM) {
        emit_rex(S, 1, 0, 0, d->reg >> 3);
        emit_b(S, 0xC1);
        emit_modrm_rr(S, ext, d->reg);
        emit_b(S, (uint8_t)(s->imm & 0xff));
        return;
    }
    as_diag(S, S->line, "shift needs imm8 or cl");
}

/* test reg,reg -> 85 /r */
static void enc_test(AsState* S, AsOperand* d, AsOperand* s, int nops) {
    if (nops != 2 || d->kind != OP_REG || s->kind != OP_REG) {
        as_diag(S, S->line, "bad test"); return;
    }
    emit_rex(S, 1, s->reg >> 3, 0, d->reg >> 3);
    emit_b(S, 0x85);
    emit_modrm_rr(S, s->reg, d->reg);
}

/* setcc al -> 0F (90+cc) /0 (rm = al = 0) */
static void enc_setcc(AsState* S, int cc, AsOperand* a, int nops) {
    if (nops != 1 || a->kind != OP_AL) { as_diag(S, S->line, "setcc needs al"); return; }
    emit_b(S, 0x0F);
    emit_b(S, (uint8_t)(0x90 + cc));
    emit_b(S, (uint8_t)(0xC0 | (0 << 3) | 0)); /* mod=11, /0, rm=al */
}

/* jmp/jcc/call rel32. target resolved in pass 2; pass 1 emits a fixed 5/6 bytes. */
static void enc_jmp(AsState* S, AsOperand* a, int nops) {
    if (nops != 1) { as_diag(S, S->line, "jmp needs a target"); return; }
    long disp = 0;
    /* opcode (E9) + rel32: next-ip = cur + 5 */
    if (S->out) {
        uint64_t target;
        const char* name = (a->kind == OP_LABELREF) ? a->label : 0;
        if (!name || !as_sym_find(S, name, &target)) {
            as_diag2(S, S->line, "undefined jump target", name);
            target = S->cur + 5;
        }
        disp = (long)((int64_t)target - (int64_t)(S->cur + 5));
    }
    emit_b(S, 0xE9);
    emit_imm32(S, disp);
}
static void enc_jcc(AsState* S, int cc, AsOperand* a, int nops) {
    if (nops != 1) { as_diag(S, S->line, "jcc needs a target"); return; }
    long disp = 0;
    if (S->out) {
        uint64_t target;
        const char* name = (a->kind == OP_LABELREF) ? a->label : 0;
        if (!name || !as_sym_find(S, name, &target)) {
            as_diag2(S, S->line, "undefined jump target", name);
            target = S->cur + 6;
        }
        disp = (long)((int64_t)target - (int64_t)(S->cur + 6));
    }
    emit_b(S, 0x0F);
    emit_b(S, (uint8_t)(0x80 + cc));
    emit_imm32(S, disp);
}
static void enc_call(AsState* S, AsOperand* a, int nops) {
    if (nops != 1) { as_diag(S, S->line, "call needs a target"); return; }
    long disp = 0;
    if (S->out) {
        uint64_t target;
        const char* name = (a->kind == OP_LABELREF) ? a->label : 0;
        if (!name || !as_sym_find(S, name, &target)) {
            as_diag2(S, S->line, "undefined call target", name);
            target = S->cur + 5;
        }
        disp = (long)((int64_t)target - (int64_t)(S->cur + 5));
    }
    emit_b(S, 0xE8);
    emit_imm32(S, disp);
}

/* ----------------------------------------------------------------------- *
 *  Data directives (db / dq).  These live in .data and emit bytes/qwords.
 *  Parsed directly off the raw operand text (commas, "strings", numbers).
 * ----------------------------------------------------------------------- */

static void enc_db(AsState* S, AsLex* L) {
    for (;;) {
        lx_skipws(L);
        if (!*L->p || *L->p == ';' || *L->p == '#') break;
        if (*L->p == '"' || *L->p == '\'') {
            char q = *L->p; L->p++;
            while (*L->p && *L->p != q) {
                char c = *L->p;
                if (c == '\\' && L->p[1]) {
                    L->p++;
                    char e = *L->p;
                    switch (e) {
                        case 'n': c = '\n'; break;
                        case 't': c = '\t'; break;
                        case 'r': c = '\r'; break;
                        case '0': c = '\0'; break;
                        case '\\': c = '\\'; break;
                        case '"': c = '"'; break;
                        case '\'': c = '\''; break;
                        default: c = e; break;
                    }
                }
                emit_b(S, (uint8_t)c);
                L->p++;
            }
            if (*L->p == q) L->p++;
        } else {
            long v;
            if (lx_number(L, &v)) {
                emit_b(S, (uint8_t)(v & 0xff));
            } else {
                as_diag(S, S->line, "bad db operand");
                /* skip to next comma to avoid infinite loop */
                while (*L->p && *L->p != ',') L->p++;
            }
        }
        lx_skipws(L);
        if (*L->p == ',') { L->p++; continue; }
        break;
    }
}

static void enc_dq(AsState* S, AsLex* L) {
    for (;;) {
        lx_skipws(L);
        if (!*L->p || *L->p == ';' || *L->p == '#') break;
        long v;
        if (as_isdigit(*L->p) || *L->p == '+' || *L->p == '-') {
            if (lx_number(L, &v)) emit_imm64(S, (uint64_t)v);
            else { as_diag(S, S->line, "bad dq operand"); while (*L->p && *L->p != ',') L->p++; }
        } else {
            /* label -> absolute address (imm64). pass1: 0 */
            char id[AS_NAMELEN];
            if (lx_ident(L, id, sizeof(id))) {
                uint64_t addr = 0;
                if (S->out) {
                    if (!as_sym_find(S, id, &addr)) {
                        as_diag2(S, S->line, "undefined dq label", id);
                        addr = 0;
                    }
                }
                emit_imm64(S, addr);
            } else {
                as_diag(S, S->line, "bad dq operand");
                while (*L->p && *L->p != ',') L->p++;
            }
        }
        lx_skipws(L);
        if (*L->p == ',') { L->p++; continue; }
        break;
    }
}

/* ----------------------------------------------------------------------- *
 *  Per-line dispatch (runs in both passes via S->out NULL/non-NULL).
 *  Returns nothing; advances S->pos.  Lines: blank/comment, directive,
 *  label (possibly with trailing instruction), or instruction.
 * ----------------------------------------------------------------------- */

static void enc_line(AsState* S, const char* line, int in_data) {
    AsLex Lx; Lx.p = line;
    AsLex* L = &Lx;
    lx_skipws(L);
    if (!*L->p || *L->p == ';' || *L->p == '#') return;

    /* read first token */
    const char* tokstart = L->p;
    char tok[AS_MAXTOK];
    if (!lx_ident(L, tok, sizeof(tok))) {
        /* not an identifier start (and not blank/comment): error */
        as_diag(S, S->line, "unexpected character at start of line");
        return;
    }

    /* label definition: "name:" */
    lx_skipws(L);
    if (*L->p == ':') {
        L->p++;
        /* the rest of the line (if any) may be another instruction */
        lx_skipws(L);
        if (!*L->p || *L->p == ';' || *L->p == '#') return;
        /* recurse on remainder */
        enc_line(S, L->p, in_data);
        return;
    }
    L->p = tokstart;  /* rewind; re-lex below as mnemonic/directive */

    /* directives handled by caller (section/global) are skipped here, but
     * be defensive: ignore them if they reach us. */
    if (as_ieq(tok, "section") || as_ieq(tok, "global") ||
        as_ieq(tok, "extern")  || as_ieq(tok, "default") ||
        as_ieq(tok, "bits")) {
        return;
    }

    /* data directives */
    if (as_ieq(tok, "db")) { L->p = tokstart + as_strlen("db"); enc_db(S, L); return; }
    if (as_ieq(tok, "dq")) { L->p = tokstart + as_strlen("dq"); enc_dq(S, L); return; }
    if (as_ieq(tok, "dw") || as_ieq(tok, "dd")) {
        /* not in the documented subset, but tolerate as data sizes */
        L->p = tokstart + 2;
        for (;;) {
            lx_skipws(L);
            if (!*L->p || *L->p == ';') break;
            long v;
            if (lx_number(L, &v)) {
                if (as_ieq(tok, "dw")) emit_imm16(S, v); else emit_imm32(S, v);
            } else { while (*L->p && *L->p != ',') L->p++; }
            lx_skipws(L);
            if (*L->p == ',') { L->p++; continue; }
            break;
        }
        return;
    }

    /* re-lex the mnemonic (lowercase comparison via as_ieq) */
    char mn[AS_MAXTOK];
    lx_ident(L, mn, sizeof(mn));

    /* parse operands */
    AsOperand a, b; int nops;
    if (!parse_ops(S, L, &a, &b, &nops)) {
        /* operand parse already logged; skip line */
        return;
    }

    /* zero-operand instructions */
    if (as_ieq(mn, "ret"))     { if (nops==0) emit_b(S, 0xC3); else as_diag(S,S->line,"ret takes no operands"); return; }
    if (as_ieq(mn, "leave"))   { if (nops==0) emit_b(S, 0xC9); else as_diag(S,S->line,"leave takes no operands"); return; }
    if (as_ieq(mn, "syscall")) { if (nops==0){ emit_b(S,0x0F); emit_b(S,0x05);} else as_diag(S,S->line,"syscall takes no operands"); return; }
    if (as_ieq(mn, "cqo"))     { if (nops==0){ emit_rex(S,1,0,0,0); emit_b(S,0x99);} else as_diag(S,S->line,"cqo takes no operands"); return; }
    if (as_ieq(mn, "nop"))     { if (nops==0) emit_b(S, 0x90); return; }

    /* movement */
    if (as_ieq(mn, "mov"))   { enc_mov(S, &a, &b, nops); return; }
    if (as_ieq(mn, "movzx")) { enc_movzx(S, &a, &b, nops); return; }
    if (as_ieq(mn, "lea"))   { enc_lea(S, &a, &b, nops); return; }
    if (as_ieq(mn, "push"))  { enc_push(S, &a, nops); return; }
    if (as_ieq(mn, "pop"))   { enc_pop(S, &a, nops); return; }

    /* alu group */
    {
        AluInfo ai;
        if (alu_lookup(mn, &ai)) { enc_alu(S, &ai, &a, &b, nops); return; }
    }
    if (as_ieq(mn, "imul")) { enc_imul(S, &a, &b, nops); return; }
    if (as_ieq(mn, "idiv")) { enc_f7(S, 7, &a, nops); return; }
    if (as_ieq(mn, "neg"))  { enc_f7(S, 3, &a, nops); return; }
    if (as_ieq(mn, "not"))  { enc_f7(S, 2, &a, nops); return; }
    if (as_ieq(mn, "shl") || as_ieq(mn, "sal")) { enc_shift(S, 4, &a, &b, nops); return; }
    if (as_ieq(mn, "shr"))  { enc_shift(S, 5, &a, &b, nops); return; }
    if (as_ieq(mn, "sar"))  { enc_shift(S, 7, &a, &b, nops); return; }
    if (as_ieq(mn, "test")) { enc_test(S, &a, &b, nops); return; }

    /* setcc: mnemonic begins with "set" */
    if (mn[0]=='s' && mn[1]=='e' && mn[2]=='t' && mn[3]) {
        int cc = cc_of(mn + 3);
        if (cc >= 0) { enc_setcc(S, cc, &a, nops); return; }
    }

    /* control flow */
    if (as_ieq(mn, "jmp"))  { enc_jmp(S, &a, nops); return; }
    if (as_ieq(mn, "call")) { enc_call(S, &a, nops); return; }
    if (mn[0]=='j' && mn[1]) {
        int cc = cc_of(mn + 1);
        if (cc >= 0) { enc_jcc(S, cc, &a, nops); return; }
    }

    as_diag2(S, S->line, "unknown mnemonic", mn);
}

/* ----------------------------------------------------------------------- *
 *  Line iteration over the whole source, splitting on '\n'.
 *  We copy each logical line into a bounded scratch buffer (NUL-terminated)
 *  and strip a trailing comment (';').  '#' is only a comment at line start
 *  in our subset, but ';' is the canonical comment char.
 * ----------------------------------------------------------------------- */

#define AS_LINEBUF 512

/* classify a line's directive for section tracking. returns:
 *  0 = none, 1 = section .text, 2 = section .data */
static int line_section(const char* line) {
    AsLex Lx; Lx.p = line; AsLex* L = &Lx;
    lx_skipws(L);
    char tok[AS_MAXTOK];
    const char* save = L->p;
    if (!lx_ident(L, tok, sizeof(tok))) return 0;
    if (!as_ieq(tok, "section")) { L->p = save; return 0; }
    char name[AS_MAXTOK];
    if (!lx_ident(L, name, sizeof(name))) return 0;
    if (as_ieq(name, ".text") || as_ieq(name, "text")) return 1;
    if (as_ieq(name, ".data") || as_ieq(name, "data") ||
        as_ieq(name, ".rodata") || as_ieq(name, ".bss")) return 2;
    return 1; /* unknown section name: treat as text */
}

/* Does this line define a label?  If so, copy its name into out and return the
 * remaining pointer (after the colon) so we can also measure a trailing insn. */
static const char* line_label(const char* line, char* out, int cap) {
    AsLex Lx; Lx.p = line; AsLex* L = &Lx;
    lx_skipws(L);
    const char* save = L->p;
    char tok[AS_NAMELEN];
    if (!lx_ident(L, tok, sizeof(tok))) { out[0]=0; return 0; }
    lx_skipws(L);
    if (*L->p == ':') {
        as_cpy(out, tok, cap);
        L->p++;
        return L->p;
    }
    L->p = save;
    out[0] = 0;
    return 0;
}

/* copy logical line [src, src_end) into buf, strip trailing ';' comment.
 * returns buf. */
static void grab_line(const char* src, int len, char* buf) {
    int n = 0;
    int instr = 0; char q = 0;
    for (int i = 0; i < len && n < AS_LINEBUF - 1; i++) {
        char c = src[i];
        if (instr) {
            buf[n++] = c;
            if (c == q) instr = 0;
            continue;
        }
        if (c == '"' || c == '\'') { instr = 1; q = c; buf[n++] = c; continue; }
        if (c == ';') break;                 /* comment to end of line */
        buf[n++] = c;
    }
    buf[n] = 0;
}

/* ----------------------------------------------------------------------- *
 *  Public entry point.
 * ----------------------------------------------------------------------- */

int as_assemble(const char* text, uint64_t base, uint8_t* code_out, int code_cap,
                int* code_len, TcDiag* diags, int* ndiags) {
    static AsSym symtab[AS_MAXLABEL];
    static char linebuf[AS_LINEBUF];

    AsState S;
    S.out = 0; S.cap = code_cap; S.pos = 0; S.base = base; S.cur = base;
    S.syms = symtab; S.nsyms = 0;
    S.diags = diags; S.ndiags = ndiags; S.fatal = 0; S.line = 0;

    if (code_len) *code_len = 0;
    if (ndiags) *ndiags = 0;
    if (!text || !code_out || code_cap <= 0) return 0;

    /* ---------------------------------------------------------------- *
     * PASS 1: two sweeps over the source.  Sweep A measures the size of
     * .text (lines belonging to the text section) so we know where .data
     * begins; Sweep B measures .data and assigns label addresses.
     * Because the encode routine produces fixed-length output regardless
     * of label values (imm64 for labels, rel32 for branches), pass-1 sizes
     * exactly match pass-2 emission.
     * ---------------------------------------------------------------- */

    /* --- Sweep A: size of .text --- */
    int text_size = 0;
    {
        const char* p = text;
        int sect = 1;  /* default .text */
        while (*p) {
            const char* nl = p;
            while (*nl && *nl != '\n') nl++;
            int len = (int)(nl - p);
            grab_line(p, len, linebuf);

            int sc = line_section(linebuf);
            if (sc == 1) sect = 1;
            else if (sc == 2) sect = 2;
            else if (sect == 1) {
                /* measure this text line */
                S.out = 0; S.pos = 0; S.cur = base; S.fatal = 0;
                /* avoid double-counting diags in sizing sweep */
                TcDiag* sd = S.diags; int* snd = S.ndiags;
                S.diags = 0; S.ndiags = 0;
                char lbl[AS_NAMELEN];
                const char* rest = line_label(linebuf, lbl, sizeof(lbl));
                enc_line(&S, rest ? rest : linebuf, 0);
                S.diags = sd; S.ndiags = snd;
                text_size += S.pos;
            }

            if (!*nl) break;
            p = nl + 1;
        }
    }

    uint64_t data_base = base + (uint64_t)text_size;

    /* --- Sweep B: assign label addresses across both sections --- */
    {
        const char* p = text;
        int sect = 1;
        uint64_t taddr = base;        /* running .text address */
        uint64_t daddr = data_base;   /* running .data address */
        int lineno = 0;
        while (*p) {
            lineno++;
            const char* nl = p;
            while (*nl && *nl != '\n') nl++;
            int len = (int)(nl - p);
            grab_line(p, len, linebuf);

            int sc = line_section(linebuf);
            if (sc == 1) { sect = 1; if (!*nl) break; p = nl + 1; continue; }
            if (sc == 2) { sect = 2; if (!*nl) break; p = nl + 1; continue; }

            /* label? */
            char lbl[AS_NAMELEN];
            const char* rest = line_label(linebuf, lbl, sizeof(lbl));
            uint64_t here = (sect == 1) ? taddr : daddr;
            if (rest && lbl[0]) {
                as_sym_add(&S, lbl, here);
            }
            const char* body = rest ? rest : linebuf;

            /* measure the body to advance the section cursor */
            S.out = 0; S.pos = 0; S.cur = here; S.fatal = 0;
            S.line = lineno;
            TcDiag* sd = S.diags; int* snd = S.ndiags;
            S.diags = 0; S.ndiags = 0;   /* silence sizing diagnostics */
            enc_line(&S, body, sect == 2);
            S.diags = sd; S.ndiags = snd;

            if (sect == 1) taddr += S.pos; else daddr += S.pos;

            if (!*nl) break;
            p = nl + 1;
        }
    }

    /* ---------------------------------------------------------------- *
     * PASS 2: emit.  Walk again, encoding into code_out with labels
     * resolved.  .text emitted first (offset 0), then .data.
     * ---------------------------------------------------------------- */
    int total = 0;
    for (int phase = 1; phase <= 2; phase++) {
        const char* p = text;
        int sect = 1;
        int lineno = 0;
        uint64_t addr = (phase == 1) ? base : data_base;
        S.out = code_out;
        S.pos = total;           /* continue appending after .text */
        while (*p) {
            lineno++;
            const char* nl = p;
            while (*nl && *nl != '\n') nl++;
            int len = (int)(nl - p);
            grab_line(p, len, linebuf);

            int sc = line_section(linebuf);
            if (sc == 1) { sect = 1; if (!*nl) break; p = nl + 1; continue; }
            if (sc == 2) { sect = 2; if (!*nl) break; p = nl + 1; continue; }

            if (sect == phase) {
                char lbl[AS_NAMELEN];
                const char* rest = line_label(linebuf, lbl, sizeof(lbl));
                const char* body = rest ? rest : linebuf;
                S.cur = addr;
                S.line = lineno;
                int before = S.pos;
                enc_line(&S, body, sect == 2);
                addr += (uint64_t)(S.pos - before);
            }

            if (!*nl) break;
            p = nl + 1;
        }
        total = S.pos;
    }

    if (S.fatal) {
        as_diag(&S, 0, "code buffer overflow (output truncated)");
    }
    if (total > code_cap) total = code_cap;

    if (code_len) *code_len = total;
    return (total > 0) ? 1 : 0;
}

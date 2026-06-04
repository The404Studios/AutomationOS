/*
 * cc.h -- shared state for the C-subset compiler backend.
 *
 *   cc_codegen.c : program / functions / statements / globals / _start / builtins
 *   cc_expr.c    : expressions (result in RAX; addresses via cg_addr)
 *   cc_type.c    : type sizes / pointer / struct-ish helpers
 *
 * SUBSET (v1): all integer/pointer types are treated as 64-bit (8 bytes) except
 * `char`/`_Bool` = 1 byte; supported: global + local int/char/pointer vars,
 * functions with <=6 params, return, if/else, while, for, blocks, the operators
 * + - * / % & | ^ << >> ! ~ unary- && || == != < <= > >= = and compound forms,
 * function calls (incl recursion), string literals (-> .data), array index a[i],
 * deref *p / addr-of &x, and builtins sys_write(fd,buf,len) / sys_exit(code) that
 * emit `syscall`. Anything unsupported -> cc_error (best-effort, never crash).
 *
 * Calling convention used by GENERATED code (self-consistent, SysV-like):
 *   args in rdi,rsi,rdx,rcx,r8,r9 ; return in rax ; callee-saved rbp ; frame via
 *   push rbp/mov rbp,rsp/sub rsp,frame ; locals at [rbp-off]; 16-byte aligned
 *   rsp at each `call`. _start: call main; mov rdi,rax; mov rax,1; syscall.
 */
#ifndef IDE_CC_H
#define IDE_CC_H

#include "ide_ast.h"
#include "tc.h"

#define CC_MAXLOCALS   96
#define CC_MAXGLOBALS  64
#define CC_MAXSTR      64
#define CC_NAME        48
/* Upper bound on a global array's element count. Caps the digit accumulator in
 * the "int x[N]" parser so a huge/overflowing N can neither wrap the signed int
 * nor drive a runaway "dq 0"-per-element emit in cc_emit_data_section. */
#define CC_MAXARRLEN   65536

typedef struct { char name[CC_NAME]; int off; int size; } CcLocal;   /* off = -(bytes from rbp) */
typedef struct { char name[CC_NAME]; char type[CC_NAME]; int size; int is_arr; int arrlen; } CcGlobal;
typedef struct { int id; char text[160]; int len; } CcStr;

typedef struct {
    char*   out; int cap; int len;          /* asm text buffer (NUL-kept)   */
    TcDiag* diags; int* ndiags;
    int     label_id;
    /* current function frame */
    CcLocal locals[CC_MAXLOCALS]; int nlocals; int frame_size;
    int     cur_param_count;
    int     break_label, cont_label;        /* -1 if none                   */
    /* module scope */
    CcGlobal globals[CC_MAXGLOBALS]; int nglobals;
    CcStr   strs[CC_MAXSTR]; int nstrs;
    int     ok;
} Cg;

/* ---- emit helpers (cc_codegen.c) ---- */
void cc_emit(Cg* g, const char* line);              /* append line + '\n' */
void cc_emit2(Cg* g, const char* a, const char* b); /* a then b then '\n' */
void cc_emit_num(Cg* g, const char* a, long n, const char* b); /* a + dec(n) + b + '\n' */
int  cc_new_label(Cg* g);                           /* unique N for .LN */
void cc_emit_labeldef(Cg* g, int n);                /* ".LN:" */
CcLocal* cc_find_local(Cg* g, const char* name);
CcGlobal* cc_find_global(Cg* g, const char* name);
int  cc_intern_str(Cg* g, const char* text);        /* returns string id; -> .data */
void cc_error(Cg* g, int line, const char* msg);

/* ---- expression codegen (cc_expr.c) ---- */
void cg_expr(Cg* g, const AstNode* e);   /* evaluate rvalue -> RAX */
void cg_addr(Cg* g, const AstNode* e);   /* lvalue address -> RAX  */

/* ---- type helpers (cc_type.c) ---- */
int  cc_sizeof_type(const char* type_str);  /* char/_Bool=1 else 8 */
int  cc_is_pointer(const char* type_str);   /* contains '*' */
int  cc_elem_size(const char* type_str);    /* element size for ptr/array arithmetic */
/* infer a (very rough) type string for an expression node, into out[cap].
 * Good enough to scale pointer arithmetic + pick byte vs qword loads. */
void cc_infer_type(Cg* g, const AstNode* e, char* out, int cap);

/* ---- struct layout registry (cc_type.c) ----
 * Scan ast_root() for AST_RECORD nodes and lay out their fields sequentially
 * (char/_Bool=1, everything else incl. pointers=8; non-char fields 8-aligned).
 * cc_member_offset returns the byte offset of `field` within `struct_type`
 * ("struct Foo" / "Foo" / "Foo *" all accepted) and writes the field size to
 * field_size_out; unknown type/field -> 0 with size 8 (safe fallback). The
 * registry builds lazily on first query; call cc_build_struct_registry to
 * (re)build eagerly (e.g. after a fresh parse). */
void cc_build_struct_registry(void);
int  cc_member_offset(const char* struct_type, const char* field,
                      int* field_size_out);

/* ---- program driver (cc_codegen.c) ---- */
int  cc_gen_program(Cg* g, const AstNode* tu);  /* emit whole module; 1=ok */

#endif /* IDE_CC_H */

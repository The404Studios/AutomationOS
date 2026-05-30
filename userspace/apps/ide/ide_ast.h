/*
 * ide_ast.h -- real C Abstract Syntax Tree (spans + stable-ish node IDs).
 *
 * The parser (ide_pcore/pdecl/pstmt/pexpr.c) builds this tree from a token
 * stream; ide_parse.c lowers it into the Model the panels render; ide_gen.c
 * edits it (via span-based text splicing) for blueprint->code.
 *
 * Single source of truth = the TEXT; this AST is a parsed projection of it,
 * every node carrying the source SPAN it came from so edits map back to exact
 * byte offsets. Freestanding, arena-allocated, no malloc.
 */
#ifndef IDE_AST_H
#define IDE_AST_H

#include <stdint.h>

typedef enum {
    AST_NONE = 0,
    AST_TU,            /* translation unit (root)                       */
    /* declarations */
    AST_FUNC_DEF,      /* function definition (has a body)              */
    AST_FUNC_PROTO,    /* function prototype (no body)                  */
    AST_PARAM,         /* one parameter                                 */
    AST_VAR_DECL,      /* variable / global declaration                 */
    AST_TYPEDEF,       /* typedef                                       */
    AST_RECORD,        /* struct / union / enum specifier (a tag)       */
    AST_FIELD,         /* struct/union field                            */
    AST_ENUM_CONST,    /* enumerator                                    */
    /* statements */
    AST_COMPOUND,      /* { ... }                                       */
    AST_IF, AST_WHILE, AST_FOR, AST_DO, AST_SWITCH, AST_CASE, AST_DEFAULT,
    AST_RETURN, AST_BREAK, AST_CONTINUE, AST_GOTO, AST_LABEL,
    AST_EXPR_STMT, AST_DECL_STMT, AST_EMPTY_STMT,
    /* expressions */
    AST_BINARY,        /* a OP b   (name = operator text)               */
    AST_UNARY,         /* OP a / a++ (name = operator text)             */
    AST_ASSIGN,        /* a = b / a += b (name = operator text)         */
    AST_TERNARY,       /* c ? a : b                                     */
    AST_CALL,          /* callee(args)   (name = callee ident if simple)*/
    AST_INDEX,         /* a[b]                                          */
    AST_MEMBER,        /* a.b / a->b (name = member, type_str= "." / "->")*/
    AST_IDENT,         /* identifier (name = the id)                    */
    AST_LITERAL,       /* number/string/char (name = literal text)      */
    AST_CAST,          /* (type)expr (type_str = type)                  */
    AST_SIZEOF,        /* sizeof ...                                    */
    AST_INIT_LIST,     /* { a, b, c }                                   */
    AST_COMMA,         /* a , b                                         */
    AST_KIND_COUNT
} AstKind;

typedef struct {
    int start_off, end_off;     /* byte offsets into source, [start,end)  */
    int start_line, start_col;  /* 1-based line, 0-based col              */
    int end_line, end_col;
} Span;

typedef struct AstNode AstNode;
struct AstNode {
    AstKind  kind;
    uint32_t id;          /* deterministic id (allocation order)         */
    Span     span;
    /* Overloaded text slots (meaning depends on kind):
     *  name     = identifier / operator text / literal text / tag name /
     *             decl-or-func-or-param name / member name
     *  type_str = rendered type for FUNC_DEF(ret)/VAR_DECL/PARAM/TYPEDEF/CAST,
     *             or "." / "->" for AST_MEMBER, else ""                  */
    char     name[64];
    char     type_str[96];
    /* tree links (n-ary) */
    AstNode* parent;
    AstNode* first_child;
    AstNode* last_child;
    AstNode* next;        /* next sibling */
    int      nchildren;
};

/* ---- arena lifecycle + tree building (ide_ast.c) ---- */
void     ast_reset(void);                       /* clear arena + id counter      */
AstNode* ast_new(AstKind kind);                 /* zeroed node, next id, no links */
void     ast_add_child(AstNode* parent, AstNode* child);
void     ast_set_root(AstNode* root);
AstNode* ast_root(void);                         /* current root, or 0            */
int      ast_node_count(void);
int      ast_arena_full(void);                   /* 1 if the pool is exhausted    */

/* ---- helpers ---- */
const char* ast_kind_name(AstKind k);            /* short label for debug/inspector */
/* find the AST_FUNC_DEF whose name matches (depth-1 under TU); 0 if none */
AstNode* ast_find_func(const char* name);

#endif /* IDE_AST_H */

/*
 * ide_parser.h -- recursive-descent C parser shared contract.
 *
 * The parser is split across files that are MUTUALLY RECURSIVE; this header
 * declares every cross-layer entry point + the shared cursor API + the Parser
 * state. The cursor-ownership contract below is mandatory -- every layer must
 * obey it or the layers won't compose.
 *
 *   ide_pcore.c  : parser_init, cursor impl, type-name table, parse_translation_unit
 *                  (loops parse_declaration until EOF).
 *   ide_pdecl.c  : parse_declaration -- handles var decls, typedefs, struct/union/enum,
 *                  function PROTOTYPES and full function DEFINITIONS (when a '{' body
 *                  follows the parameter list, parse the body via parse_compound and
 *                  return AST_FUNC_DEF). Also parse_type_and_declarator + param lists.
 *   ide_pstmt.c  : parse_statement, parse_compound (+ panic-mode recovery).
 *   ide_pexpr.c  : parse_expression (comma), parse_assignment (precedence climbing).
 *
 * CURSOR-OWNERSHIP CONTRACT (obey exactly):
 *  - Every parse_X consumes precisely the tokens of its construct and leaves
 *    the cursor on the first token AFTER it.
 *  - parse_statement consumes its terminating ';' (expr/return/decl stmt) OR the
 *    closing '}' (compound). parse_compound consumes the '{' and matching '}'.
 *  - parse_expression / parse_assignment do NOT consume a trailing ';'.
 *  - On error: call pdiag() then precover_to(); return a best-effort node, never
 *    NULL for a required production (use ast_new(AST_NONE) as a placeholder) so
 *    callers never crash. The cursor must always make forward progress (adv at
 *    least once per loop) to avoid infinite loops.
 *  - Type detection uses is_typename() (the "lexer hack"); parse_declaration
 *    registers new typedef names via add_typename().
 */
#ifndef IDE_PARSER_H
#define IDE_PARSER_H

#include "ide_ast.h"
#include "ide_lex.h"     /* Tok, TokKind, lex_tokenize */

#define PARSE_MAX_TOKS   16384
#define PARSE_MAX_TYPES  256
#define PARSE_MAX_DIAGS  128

typedef struct { int line, col, off; char msg[96]; } Diag;

typedef struct {
    const char* src;  int src_len;
    Tok*        toks; int ntoks; int pos;
    char        types[PARSE_MAX_TYPES][64]; int ntypes;   /* typedef + builtin type names */
    Diag        diags[PARSE_MAX_DIAGS];     int ndiags;
} Parser;

/* ---- lifecycle (ide_pcore.c) ---- */
/* Tokenize src into toks[max], seed builtin type names, reset cursor. */
void     parser_init(Parser* p, const char* src, int src_len, Tok* toks, int max_toks);
/* Parse the unit; allocate via ast_*, call ast_set_root(root), return root (AST_TU). */
AstNode* parse_translation_unit(Parser* p);

/* ---- cursor API (ide_pcore.c) : used by ALL layers ---- */
Tok* pk (Parser* p);                 /* current token; returns a TK_EOF token at/after end (never NULL) */
Tok* pk2(Parser* p);                 /* lookahead +1 (TK_EOF past end) */
int  at (Parser* p, TokKind k);      /* current token kind == k */
int  at_punct(Parser* p, const char* s);   /* current token is TK_PUNCT with this exact text */
int  at_kw(Parser* p, const char* s);      /* current token (TK_KW/TK_TYPE/TK_ID) text == s */
Tok* adv(Parser* p);                 /* consume current, return it (clamped at EOF) */
int  eat_punct(Parser* p, const char* s);  /* if at_punct(s): consume, return 1; else 0 */
void expect_punct(Parser* p, const char* s); /* consume if matches else pdiag (no abort) */
void pdiag(Parser* p, const char* msg);      /* record diagnostic at current token */
void precover_to(Parser* p, const char* stop_chars); /* skip until a token whose 1st char is in stop_chars, or EOF */
int  is_typename(Parser* p, const char* s, int len); /* builtin or registered typedef/tag */
void add_typename(Parser* p, const char* s, int len);
/* token text equals literal (helper); tok may be NULL-safe */
int  tok_is(Tok* t, const char* s);
/* build a Span covering [startTok .. endTok] (inclusive of endTok's text) */
Span span_of(Parser* p, Tok* startTok, Tok* endTok);

/* ---- grammar entry points (mutually recursive across files) ---- */
AstNode* parse_declaration(Parser* p);   /* ide_pdecl.c : var/typedef/record/func-proto/func-def, TU or block scope */
/* Parse a type + declarator (one declared entity). Writes rendered type into
 * type_out (cap >=96) and the declared identifier into name_out (cap >=64).
 * Returns a node describing it, or AST_NONE. Used for params/vars/func sigs.
 * (ide_pdecl.c) */
AstNode* parse_type_and_declarator(Parser* p, char* type_out, char* name_out);
AstNode* parse_statement(Parser* p);     /* ide_pstmt.c */
AstNode* parse_compound(Parser* p);      /* ide_pstmt.c : '{' stmts '}' */
AstNode* parse_expression(Parser* p);    /* ide_pexpr.c : full expr incl comma */
AstNode* parse_assignment(Parser* p);    /* ide_pexpr.c : assignment-expression (no comma) */

#endif /* IDE_PARSER_H */

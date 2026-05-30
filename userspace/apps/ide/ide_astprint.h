/*
 * ide_astprint.h -- AST printer + span-based text editing helpers.
 *
 * Two jobs:
 *   1. Render an AST as an indented text tree (inspector SYNTAX tab) and
 *      render a function signature line (blueprint header).
 *   2. Minimal-diff text editing: insert text at an exact byte offset,
 *      shifting the tail right, preserving every surrounding byte
 *      (comments, whitespace, code). Insert-only by design.
 *
 * Freestanding: no libc, no malloc, no stdio. All routines are bounded
 * and NULL-safe; they never overflow the caller's output/edit buffer.
 */
#ifndef IDE_ASTPRINT_H
#define IDE_ASTPRINT_H

#include "ide_ast.h"

/* Render the whole AST as an indented tree, one node per line:
 *   <indent by depth>ast_kind_name(kind)[ 'name'][ : type_str]\n
 * Recurses first_child/next, depth-capped and line-capped. Writes at most
 * cap-1 bytes plus a NUL terminator. Returns bytes written (excluding NUL),
 * or 0 if root/out is NULL or cap < 1. */
int astprint_tree(const AstNode* root, char* out, int cap);

/* Insert NUL-terminated `ins` into buf at byte offset at_off (0..*len),
 * shifting the existing tail right. Updates *len. Returns 1 on success,
 * 0 if it would not fit in cap (buf left unchanged) or on bad args. */
int text_splice(char* buf, int* len, int cap, int at_off, const char* ins);

/* Same as text_splice but with an explicit insert length (ins need not be
 * NUL-terminated; embedded NULs are copied verbatim). Returns 1/0. */
int text_splice_n(char* buf, int* len, int cap, int at_off,
                  const char* ins, int ins_len);

/* Render a function signature line from an AST_FUNC_DEF node:
 *   "<type_str> <name>(<ptype> <pname>, ...)"
 * reading the node's type_str (return) / name and its AST_PARAM children.
 * Bounded to cap (cap-1 bytes + NUL). Returns bytes written, or 0 on bad
 * args. "void" is emitted for an empty parameter list. */
int astprint_signature(const AstNode* funcdef, char* out, int cap);

#endif /* IDE_ASTPRINT_H */

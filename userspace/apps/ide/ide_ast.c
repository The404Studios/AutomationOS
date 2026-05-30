/*
 * ide_ast.c -- static-arena AST builder for the from-scratch C parser.
 *
 * Freestanding: no libc, no malloc, no stdio. All nodes live in a fixed
 * static pool. Allocation never fails fatally: when the pool is exhausted we
 * hand back a permanently-AST_NONE sentinel that is always safe to deref and
 * safe to pass to ast_add_child (it becomes a no-op).
 */
#include "ide_ast.h"

#define AST_POOL_CAP 16384

static AstNode g_nodes[AST_POOL_CAP];
static int     g_count;
static int     g_full;
static AstNode* g_root;

/* Returned when the pool is exhausted; kept AST_NONE with null links so
 * callers can always safely dereference the result. */
static AstNode g_overflow;

/* ---- local freestanding helpers ---- */

static void ast_zero(AstNode* n)
{
    unsigned char* p = (unsigned char*)n;
    unsigned int   i;
    for (i = 0; i < sizeof(AstNode); i++)
        p[i] = 0;
}

static int ast_streq(const char* a, const char* b)
{
    if (!a || !b)
        return 0;
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *a == *b;
}

/* ---- arena lifecycle ---- */

void ast_reset(void)
{
    g_count = 0;
    g_full  = 0;
    g_root  = 0;
    /* Keep the overflow sentinel permanently safe/empty. */
    ast_zero(&g_overflow);
    g_overflow.kind = AST_NONE;
}

AstNode* ast_new(AstKind kind)
{
    AstNode* n;

    if (g_count >= AST_POOL_CAP) {
        g_full = 1;
        /* sentinel must stay AST_NONE with null links */
        ast_zero(&g_overflow);
        g_overflow.kind = AST_NONE;
        return &g_overflow;
    }

    n = &g_nodes[g_count];
    ast_zero(n);
    n->kind = kind;
    g_count++;
    n->id = (uint32_t)g_count; /* id 0 reserved; first real node = 1 */
    return n;
}

void ast_add_child(AstNode* parent, AstNode* child)
{
    /* Guard NULLs and the overflow sentinel (no-op on either). */
    if (!parent || !child)
        return;
    if (parent == &g_overflow || child == &g_overflow)
        return;

    child->parent = parent;
    child->next   = 0;

    if (parent->last_child) {
        parent->last_child->next = child;
        parent->last_child       = child;
    } else {
        parent->first_child = child;
        parent->last_child  = child;
    }
    parent->nchildren++;
}

void ast_set_root(AstNode* root)
{
    g_root = root;
}

AstNode* ast_root(void)
{
    return g_root;
}

int ast_node_count(void)
{
    return g_count;
}

int ast_arena_full(void)
{
    return g_full;
}

/* ---- helpers ---- */

const char* ast_kind_name(AstKind k)
{
    switch (k) {
    case AST_NONE:        return "none";
    case AST_TU:          return "tu";
    /* declarations */
    case AST_FUNC_DEF:    return "func";
    case AST_FUNC_PROTO:  return "proto";
    case AST_PARAM:       return "param";
    case AST_VAR_DECL:    return "var";
    case AST_TYPEDEF:     return "typedef";
    case AST_RECORD:      return "record";
    case AST_FIELD:       return "field";
    case AST_ENUM_CONST:  return "enumc";
    /* statements */
    case AST_COMPOUND:    return "block";
    case AST_IF:          return "if";
    case AST_WHILE:       return "while";
    case AST_FOR:         return "for";
    case AST_DO:          return "do";
    case AST_SWITCH:      return "switch";
    case AST_CASE:        return "case";
    case AST_DEFAULT:     return "default";
    case AST_RETURN:      return "return";
    case AST_BREAK:       return "break";
    case AST_CONTINUE:    return "continue";
    case AST_GOTO:        return "goto";
    case AST_LABEL:       return "label";
    case AST_EXPR_STMT:   return "exprstmt";
    case AST_DECL_STMT:   return "declstmt";
    case AST_EMPTY_STMT:  return "empty";
    /* expressions */
    case AST_BINARY:      return "binop";
    case AST_UNARY:       return "unop";
    case AST_ASSIGN:      return "assign";
    case AST_TERNARY:     return "ternary";
    case AST_CALL:        return "call";
    case AST_INDEX:       return "index";
    case AST_MEMBER:      return "member";
    case AST_IDENT:       return "id";
    case AST_LITERAL:     return "lit";
    case AST_CAST:        return "cast";
    case AST_SIZEOF:      return "sizeof";
    case AST_INIT_LIST:   return "initlist";
    case AST_COMMA:       return "comma";
    case AST_KIND_COUNT:  return "?";
    default:              return "?";
    }
}

AstNode* ast_find_func(const char* name)
{
    AstNode* c;

    if (!g_root || !name)
        return 0;

    for (c = g_root->first_child; c; c = c->next) {
        if (c->kind == AST_FUNC_DEF && ast_streq(c->name, name))
            return c;
    }
    return 0;
}

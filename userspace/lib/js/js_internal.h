/*
 * js_internal.h -- shared internal types for the AutomationOS JS engine.
 * =====================================================================
 *
 * Everything that the lexer, parser, value model, interpreter and builtins
 * must agree on lives here: the value struct, object/array/string/function
 * representations, AST node shapes, the token enum, the arena allocator
 * interface, and the js_vm definition.
 *
 * This header is freestanding: it pulls in NO standard headers. It defines
 * its own fixed-width-ish typedefs.
 */

#ifndef JS_INTERNAL_H
#define JS_INTERNAL_H

#include "js.h"

/* ------------------------------------------------------------------ */
/*  Freestanding primitive types                                       */
/* ------------------------------------------------------------------ */
typedef unsigned char       js_u8;
typedef unsigned short      js_u16;
typedef unsigned int        js_u32;
typedef int                 js_i32;
typedef unsigned long       js_usize;  /* 64-bit on x86_64 */
typedef long                js_isize;
typedef unsigned long long  js_u64;
typedef long long           js_i64;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ------------------------------------------------------------------ */
/*  Tunables                                                           */
/* ------------------------------------------------------------------ */
/* One big static arena per VM. Holds AST + all runtime values + envs. */
#define JS_ARENA_BYTES   (6u * 1024u * 1024u)   /* 6 MB */

#define JS_MAX_INTERN    2048      /* interned string table slots         */
#define JS_MAX_CALL_DEPTH 512      /* recursion guard for interp & parser */
#define JS_ERRMSG_CAP    256       /* size of vm->errmsg                   */
#define JS_NUMBUF_CAP    64        /* scratch for number formatting        */

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
typedef struct js_value   js_value;
typedef struct js_object  js_object;
typedef struct js_string  js_string;
typedef struct js_propmap js_propmap;
typedef struct js_node    js_node;
typedef struct js_env     js_env;
typedef struct js_func    js_func;

/* ------------------------------------------------------------------ */
/*  Tagged value                                                       */
/* ------------------------------------------------------------------ */
typedef enum {
    JS_UNDEFINED = 0,
    JS_NULL,
    JS_BOOL,
    JS_NUMBER,
    JS_STRING,
    JS_OBJECT,      /* plain object */
    JS_ARRAY,       /* array (also a js_object with array flag)        */
    JS_FUNCTION     /* function (closure or native), a js_object too   */
} js_type;

struct js_value {
    js_type type;
    union {
        int        b;     /* JS_BOOL                                   */
        double     n;     /* JS_NUMBER                                 */
        js_string *s;     /* JS_STRING                                 */
        js_object *o;     /* JS_OBJECT/JS_ARRAY/JS_FUNCTION            */
    } u;
};

/* ------------------------------------------------------------------ */
/*  Strings (immutable, length-prefixed, NUL-terminated for convenience)*/
/* ------------------------------------------------------------------ */
struct js_string {
    js_usize len;          /* byte length (excludes terminator)        */
    js_u32   hash;         /* cached FNV-1a hash                        */
    char     data[];       /* flexible array member: len+1 bytes alloc'd */
};

/* ------------------------------------------------------------------ */
/*  Objects, arrays, functions                                        */
/* ------------------------------------------------------------------ */

/* A property slot: key string -> value, kept in an open list. */
typedef struct {
    js_string *key;        /* interned key (NULL == empty slot)        */
    js_value   val;
    js_u8      enumerable;
    js_u32     order;          /* insertion order (for stable iteration) */
} js_prop;

/*
 * Object internal flags.
 */
#define JS_OBJ_ARRAY    0x01
#define JS_OBJ_FUNCTION 0x02
#define JS_OBJ_ERROR    0x04   /* an Error-like object (has .message)  */
/*
 * JS_OBJ_NATIVE -- the object wraps an external C pointer, with optional
 * dynamic property get/set dispatched through a registered native class.
 * Added (minimally) to support DOM bindings (see lib/js/js_native.c and
 * lib/dom/dom_bindings.c). The native_class_id indexes into the vm's
 * native class registry; native_ptr is the C-side host pointer.
 */
#define JS_OBJ_NATIVE   0x08

struct js_object {
    js_u8       flags;

    /* Hash-ish property store: a flat growable array of slots, linear
     * probe by hash. Small objects stay tiny; grows on demand.        */
    js_prop    *props;
    js_usize    nprops;        /* used slots (incl. tombstones=none)   */
    js_usize    cap;           /* allocated slots                      */
    js_u32      order_seq;     /* next insertion-order value           */

    /* Array storage (dense). Used only when JS_OBJ_ARRAY set.         */
    js_value   *elems;
    js_usize    length;        /* array length                         */
    js_usize    ecap;          /* elems capacity                       */

    /* Function payload (only when JS_OBJ_FUNCTION).                    */
    js_func    *fn;

    /* prototype: used for builtin method dispatch (String/Array/Object
     * methods live on shared prototype objects). May be NULL.          */
    js_object  *proto;

    /* Native-object wrapper payload (JS_OBJ_NATIVE). class_id == 0
     * means "no class registered"; native_ptr is interpreted by the
     * class's get/set/method callbacks. */
    int         native_class_id;
    void       *native_ptr;
};

/* Native function signature.
 *   vm    : the engine
 *   thisv : the `this` value of the call
 *   argv  : argument array (count = argc)
 *   out   : where to store the return value (default undefined)
 * returns 0 on success, <0 to signal a thrown error (vm->exception set). */
typedef int (*js_native_fn)(js_vm *vm, js_value thisv,
                            js_value *argv, int argc, js_value *out);

struct js_func {
    js_native_fn native;       /* non-NULL for builtins                */
    const char  *native_name;  /* for stringification of natives       */

    /* Script-defined function: */
    js_node     *params;       /* parameter list (NODE_PARAMLIST)      */
    js_node     *body;         /* function body block                  */
    js_env      *closure;      /* captured defining environment        */
    js_string   *name;         /* function name (may be NULL)          */
    int          nparams;
    int          is_arrow;
};

/* ------------------------------------------------------------------ */
/*  Environments (lexical scopes) -- closures capture these            */
/* ------------------------------------------------------------------ */
typedef struct {
    js_string *name;
    js_value   val;
    js_u8      is_const;
} js_binding;

struct js_env {
    js_env     *parent;
    js_binding *binds;
    js_usize    nbinds;
    js_usize    cap;
};

/* ------------------------------------------------------------------ */
/*  Tokens                                                             */
/* ------------------------------------------------------------------ */
typedef enum {
    T_EOF = 0,
    T_NUMBER, T_STRING, T_TEMPLATE, T_IDENT,
    /* keywords */
    T_VAR, T_LET, T_CONST, T_FUNCTION, T_RETURN,
    T_IF, T_ELSE, T_FOR, T_WHILE, T_DO,
    T_BREAK, T_CONTINUE, T_NEW, T_TYPEOF, T_VOID, T_DELETE,
    T_TRUE, T_FALSE, T_NULL, T_UNDEFINED, T_THIS,
    T_IN, T_OF, T_INSTANCEOF,
    T_THROW, T_TRY, T_CATCH, T_FINALLY,
    /* punctuation / operators */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_SEMI, T_COMMA, T_DOT, T_QUESTION, T_COLON, T_ARROW,
    T_ASSIGN,            /* =  */
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_STARSTAR,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_INC, T_DEC,        /* ++ -- */
    T_EQ, T_NEQ, T_SEQ, T_SNEQ,    /* == != === !== */
    T_LT, T_LE, T_GT, T_GE,
    T_AND, T_OR, T_NOT, T_NULLISH, /* && || ! ?? */
    T_BAND, T_BOR, T_BXOR, T_BNOT, /* & | ^ ~ */
    T_SHL, T_SHR, T_USHR,          /* << >> >>> */
    T_BANDEQ, T_BOREQ, T_BXOREQ,
    T_SHLEQ, T_SHREQ, T_USHREQ,
    T_ANDEQ, T_OREQ,               /* &&= ||= (parsed, basic)          */
    T_ERROR
} js_tok_kind;

typedef struct {
    js_tok_kind kind;
    const char *start;     /* pointer into source                      */
    js_usize    len;       /* raw length in source                     */
    double      num;       /* for T_NUMBER                             */
    js_string  *str;       /* for T_STRING/T_TEMPLATE/T_IDENT (decoded)*/
    int         nl_before; /* a newline preceded this token (for ASI)  */
    int         line;
} js_token;

/* Lexer state. */
typedef struct {
    js_vm      *vm;
    const char *src;
    const char *cur;
    const char *end;
    int         line;
    int         had_nl;    /* newline seen since last token            */
    js_token    tok;       /* current token (peek)                     */
    int         error;
} js_lexer;

/* ------------------------------------------------------------------ */
/*  AST                                                                */
/* ------------------------------------------------------------------ */
typedef enum {
    /* expressions */
    NODE_NUMBER, NODE_STRING, NODE_BOOL, NODE_NULL, NODE_UNDEFINED,
    NODE_IDENT, NODE_THIS,
    NODE_ARRAY, NODE_OBJECT, NODE_FUNCTION, NODE_ARROW,
    NODE_TEMPLATE,
    NODE_UNARY,        /* prefix op (op): - + ! ~ typeof void delete   */
    NODE_UPDATE,       /* ++/-- prefix or postfix                       */
    NODE_BINARY,       /* arithmetic/compare/bitwise/shift              */
    NODE_LOGICAL,      /* && || ??                                      */
    NODE_ASSIGN,       /* = += etc.                                     */
    NODE_COND,         /* ?:                                            */
    NODE_CALL,         /* f(args)                                       */
    NODE_NEW,          /* new f(args)                                   */
    NODE_MEMBER,       /* a.b  (computed=0) or a[b] (computed=1)        */
    NODE_SEQ,          /* a, b                                          */
    NODE_PARAMLIST,    /* function params container                     */

    /* statements */
    NODE_PROGRAM,
    NODE_VARDECL,      /* var/let/const, one declarator                 */
    NODE_BLOCK,
    NODE_EXPRSTMT,
    NODE_IF,
    NODE_FOR,
    NODE_FORIN,        /* for-in / for-of                               */
    NODE_WHILE,
    NODE_DOWHILE,
    NODE_RETURN,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_FUNCDECL,
    NODE_THROW,
    NODE_TRY,
    NODE_EMPTY
} js_node_kind;

/*
 * A single AST node. Children are stored as explicit pointers (a/b/c/d) plus
 * an optional node-list (kids/nkids) for variadic forms (call args, array
 * elems, object props, block statements, param lists). `op` holds a token
 * kind for operators. `str`/`num`/`b` hold literal payloads. `flag` is a
 * per-node-kind bitfield (e.g. computed member, postfix update, var kind).
 */
struct js_node {
    js_node_kind kind;
    js_tok_kind  op;       /* operator token for unary/binary/assign   */
    int          flag;     /* misc per-node flags                      */
    int          line;

    double       num;      /* NODE_NUMBER                              */
    js_string   *str;      /* NODE_STRING/IDENT/keys/func name         */
    int          b;        /* NODE_BOOL                                */

    js_node     *a, *b_, *c, *d;   /* generic children                 */

    js_node    **kids;     /* variadic children                        */
    int          nkids;
    int          kcap;
};

/* flag values for NODE_VARDECL / NODE_FORIN var kind */
#define VK_VAR   0
#define VK_LET   1
#define VK_CONST 2
/* NODE_MEMBER computed flag */
#define MEMBER_COMPUTED 1
/* NODE_UPDATE postfix flag  */
#define UPDATE_POSTFIX  1
/* NODE_FORIN of-loop flag    */
#define FORIN_OF        1
/* object prop kinds packed as paired kids: key,value */

/* ------------------------------------------------------------------ */
/*  Interpreter completion / control flow                             */
/* ------------------------------------------------------------------ */
typedef enum {
    CMP_NORMAL = 0,
    CMP_RETURN,
    CMP_BREAK,
    CMP_CONTINUE,
    CMP_THROW
} js_completion;

/* ------------------------------------------------------------------ */
/*  The VM                                                             */
/* ------------------------------------------------------------------ */
struct js_vm {
    int          initialized;

    /* Arena allocator. */
    js_u8       *arena;
    js_usize     arena_cap;
    js_usize     arena_used;
    js_usize     arena_mark;    /* reset point after builtins installed */
    int          oom;

    /* String intern table (open-addressed). */
    js_string   *intern[JS_MAX_INTERN];
    js_usize     nintern;

    /* Global environment. */
    js_env      *global_env;

    /* Shared prototype objects for builtin methods. */
    js_object   *proto_string;
    js_object   *proto_array;
    js_object   *proto_object;
    js_object   *proto_number;
    js_object   *proto_function;

    /* console.log sink. */
    void        (*emit)(const char *s, js_usize n);

    /* Active exception (when a completion is CMP_THROW). */
    js_value     exception;
    int          has_exception;

    /* Recursion depth guard. */
    int          depth;

    /* PRNG state for Math.random. */
    js_u64       rng;

    /* Error/diagnostic message buffer. */
    char         errmsg[JS_ERRMSG_CAP];

    /* ----- Native-class registry (lib/js/js_native.c). ---------------
     * Slot 0 is reserved ("no class"); first registered class gets id 1.
     * The slot holds an opaque pointer to a (const js_native_class *) so
     * we don't need that struct's full definition here. */
#define JS_MAX_NATIVE_CLASSES 32
    const void *native_classes[JS_MAX_NATIVE_CLASSES];
    int          n_native_classes;

    /* Backing store for the arena (in BSS). */
    js_u8        arena_store[JS_ARENA_BYTES];
};

/* ------------------------------------------------------------------ */
/*  Native-object dispatch hooks (implemented in js_native.c)          */
/*  Called from js_get_prop / js_set_prop in js_value.c.               */
/*  Return 1 if the call handled the property; 0 to fall through to    */
/*  the regular property store / prototype chain.                      */
/* ------------------------------------------------------------------ */
int js_native_dispatch_get(js_vm *vm, js_object *o,
                           js_string *key, js_value *out);
int js_native_dispatch_set(js_vm *vm, js_object *o,
                           js_string *key, js_value v);

/* ================================================================== */
/*  Cross-module function declarations                                 */
/* ================================================================== */

/* --- js_value.c : arena + values + strings + objects --- */
void       *js_arena_alloc(js_vm *vm, js_usize n);
void        js_arena_reset(js_vm *vm);            /* back to mark      */
void        js_arena_mark(js_vm *vm);             /* set mark = used   */

js_string  *js_str_new(js_vm *vm, const char *s, js_usize len);
js_string  *js_str_newz(js_vm *vm, const char *s);     /* NUL-terminated */
js_string  *js_str_intern(js_vm *vm, const char *s, js_usize len);
js_string  *js_str_concat(js_vm *vm, js_string *a, js_string *b);
int         js_str_eq(js_string *a, js_string *b);
js_u32      js_str_hash(const char *s, js_usize len);

js_value    js_mk_undef(void);
js_value    js_mk_null(void);
js_value    js_mk_bool(int b);
js_value    js_mk_num(double n);
js_value    js_mk_str(js_string *s);
js_value    js_mk_strz(js_vm *vm, const char *s);
js_value    js_mk_obj(js_object *o);

js_object  *js_object_new(js_vm *vm);
js_object  *js_array_new(js_vm *vm);
js_object  *js_func_new_native(js_vm *vm, js_native_fn fn, const char *name);

/* property access on objects (string keys) */
int         js_obj_get(js_vm *vm, js_object *o, js_string *key, js_value *out);
int         js_obj_set(js_vm *vm, js_object *o, js_string *key, js_value v);
int         js_obj_has_own(js_object *o, js_string *key);
int         js_obj_delete(js_object *o, js_string *key);
/* fill `out` with up to `cap` live prop pointers in insertion order;
 * returns the number written. */
js_usize    js_obj_ordered(js_object *o, js_prop **out, js_usize cap);

/* array helpers */
int         js_arr_push(js_vm *vm, js_object *a, js_value v);
js_value    js_arr_get(js_object *a, js_usize i);
int         js_arr_set(js_vm *vm, js_object *a, js_usize i, js_value v);

/* generic property get supporting indexes/length/prototype methods */
int         js_get_prop(js_vm *vm, js_value obj, js_string *key, js_value *out);
int         js_set_prop(js_vm *vm, js_value obj, js_string *key, js_value v);

/* --- js_lex.c --- */
void        js_lex_init(js_lexer *lx, js_vm *vm, const char *src, js_usize len);
void        js_lex_next(js_lexer *lx);            /* advance lx->tok   */

/* --- js_parse.c --- */
js_node    *js_parse_program(js_vm *vm, const char *src, js_usize len);
const char *js_parse_error(void);                 /* last parse error  */

/* --- js_interp.c --- */
int         js_run_program(js_vm *vm, js_node *prog, js_value *completion);
js_completion js_eval_node(js_vm *vm, js_node *n, js_env *env, js_value *out);
js_completion js_call_function(js_vm *vm, js_value fnv, js_value thisv,
                               js_value *argv, int argc, js_value *out);
js_env     *js_env_new(js_vm *vm, js_env *parent);
int         js_env_define(js_vm *vm, js_env *e, js_string *name,
                          js_value v, int is_const);
int         js_env_get(js_env *e, js_string *name, js_value *out);
int         js_env_set(js_vm *vm, js_env *e, js_string *name, js_value v);
void        js_throw(js_vm *vm, js_value v);
void        js_throw_str(js_vm *vm, const char *msg);

/* --- coercions (js_value.c) --- */
int         js_truthy(js_value v);
double      js_to_number(js_vm *vm, js_value v);
js_string  *js_to_string(js_vm *vm, js_value v);
js_string  *js_value_to_display(js_vm *vm, js_value v); /* console form */
const char *js_typeof(js_value v);
int         js_strict_eq(js_value a, js_value b);
int         js_loose_eq(js_vm *vm, js_value a, js_value b);

/* number <-> string */
double      js_parse_double(const char *s, js_usize len, int *ok);
js_string  *js_num_to_str(js_vm *vm, double n);
js_usize    js_dtoa(double n, char *buf, js_usize cap);   /* returns len */

/* --- js_builtin.c --- */
void        js_install_builtins(js_vm *vm);

/* math primitives (implemented in js_builtin.c) */
double      js_math_sqrt(double x);
double      js_math_pow(double b, double e);
double      js_math_floor(double x);
double      js_math_ceil(double x);
double      js_math_abs(double x);
double      js_math_sin(double x);
double      js_math_cos(double x);

/* --- shared tiny helpers (js_value.c) --- */
js_usize    js_strlen(const char *s);
void        js_memcpy(void *d, const void *s, js_usize n);
void        js_memset(void *d, int c, js_usize n);
int         js_memcmp(const void *a, const void *b, js_usize n);

/* NaN / Infinity helpers (we cannot use math.h) */
double      js_nan(void);
double      js_inf(int negative);
int         js_isnan(double x);
int         js_isinf(double x);
int         js_isfinite(double x);

#endif /* JS_INTERNAL_H */

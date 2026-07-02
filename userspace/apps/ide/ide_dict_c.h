/*
 * ide_dict_c.h -- C language + patterns dictionary for the Semantic-LEGO IDE
 *                 autocomplete engine (ide_complete.c).
 * =========================================================================
 *
 * A rich, teaching-grade table of C keywords, types, control-flow patterns,
 * and self-contained (no-libc) stdlib-shaped helpers. Every entry carries a
 * one-line signature and a one-line doc that TEACHES -- the doc renders in the
 * autocomplete popup's preview pane so a user (e.g. with aphantasia) never has
 * to recall a signature from memory.
 *
 * Snippet bodies use the SAME tab-stop convention as ide_library.c:
 *   ${N:placeholder}  -> numbered stop with default text
 *   $N                -> bare numbered stop
 *   $0                -> final caret position
 * (expanded by ide_editor_insert_snippet on accept).
 *
 * ASCII ONLY (the 8x16 bitfont has no UTF-8).
 *
 * ============================ ON-DEVICE cc SUBSET ===========================
 * Entries that EXCEED what the on-device compiler (userspace/apps/ide/cc_*.c +
 * tc_driver.c + the as_x64 assembler) can build are marked in their doc with a
 * "[host gcc only ...]" suffix. The on-device cc, verified against the source:
 *   - SINGLE translation unit. NO preprocessor: #include / #define / #ifdef are
 *     tokenized and SKIPPED (ide_lex.c). There is NO libc, NO malloc, NO stdio.
 *     The ONLY I/O is the builtins syscall(n,a1,a2,a3) / sys_write(fd,buf,len) /
 *     sys_exit(code)  (cc_expr.c:719-773).  NO inline asm.
 *   - TYPE MODEL (cc_type.c:322-342): char/_Bool/bool = 1 byte; EVERYTHING ELSE
 *     (int, long, unsigned, uint32_t, pointers, ...) = 8 bytes. So `int` is 64
 *     bits here, and `unsigned int* p; p[i]` strides 8 bytes (cc_expr.c:272-290,
 *     scales by element size). To touch a 32-bit framebuffer you MUST use char*
 *     and write 4 bytes per pixel -- see ide_dict_os.h.
 *   - SUPPORTED: if/else, for, while, return, break, continue; ?: ; && || (short
 *     circuit); + - * / % & | ^ << >> ~ ! and the compound-assign forms; ++ -- ;
 *     sizeof; casts (no-op); &x *p a[i] s.f p->f ; typedef; struct (.,->) ;
 *     pointers; local + file-scope arrays; multiple functions (<=6 args each);
 *     recursion; hex/octal/char literals; string escapes \n \t \r \0 \\ \' \".
 *   - PARSED BUT NOT GENERATED (host gcc only): do-while, switch/case/default,
 *     goto/labels (no codegen case in cc_codegen.c).
 *   - UNSUPPORTED: function pointers, variadic fns, multidimensional arrays,
 *     runtime array/global initializers (globals are zeroed), static LOCALs,
 *     float/double arithmetic (no FPU codegen), binary 0b literals.
 *   - LIMITS: <=64 functions, <=96 locals/fn, <=64 globals, <=64 string
 *     literals, <=6 args/call, 16384 tokens, 16384 AST nodes.
 *
 * ============================ INTEGRATION CONTRACT ==========================
 * Owner of the merge: ide_complete.c.  All three dictionaries (this file,
 * ide_dict_asm.h, ide_dict_os.h) share the IdeDictEntry struct defined below
 * and expose a `const IdeDictEntry IDE_DICT_*[]` array + an IDE_DICT_*_COUNT.
 *
 * Do the following IN ide_complete.c (nothing else needs to change):
 *
 *   (a) After the existing #includes add:
 *           #include "ide_dict_c.h"
 *           #include "ide_dict_asm.h"
 *           #include "ide_dict_os.h"
 *
 *   (b) Add a parallel metadata slot next to g_kind[]/g_snip[]:
 *           static const IdeDictEntry* g_dict[AC_MAX_MATCHES];
 *       and clear it at the top of complete_refresh() (set entries to 0), the
 *       same place ac_count is reset.
 *
 *   (c) Kind char -> CK_* (chip color):
 *           static int dict_ck(char k){ switch(k){
 *             case 't': return CK_TYPE;   case 'f': return CK_FUNC;
 *             case 'm': case 'd': return CK_MACRO; case 's': return CK_SNIPPET;
 *             case 'r': return CK_GLOBAL; default:  return CK_KEYWORD; } }
 *
 *   (d) Merge helper that remembers which entry produced each candidate (try_add
 *       appends at index e->ac_count only when it actually accepts):
 *           static void try_add_dict(Editor* e, const IdeDictEntry* tab, int n,
 *                                    const char* pf, int plen){
 *             for (int i=0;i<n;i++){
 *               int before=e->ac_count;
 *               try_add(e, tab[i].word, dict_ck(tab[i].kind), -1, pf, plen);
 *               if (e->ac_count>before) g_dict[before]=&tab[i];
 *             }
 *           }
 *
 *   (e) In complete_refresh(), add a STEP 5 right AFTER the library-trigger loop
 *       (current ide_complete.c step 4, ~line 90) and BEFORE the visible-rows
 *       clamp (~line 92). Optionally gate ASM by the buffer language:
 *           try_add_dict(e, IDE_DICT_C,   IDE_DICT_C_COUNT,   pf, plen);
 *           try_add_dict(e, IDE_DICT_OS,  IDE_DICT_OS_COUNT,  pf, plen);
 *           try_add_dict(e, IDE_DICT_ASM, IDE_DICT_ASM_COUNT, pf, plen);
 *
 *   (f) Extend complete_preview() (ide_complete.c:110) so EVERY dict entry
 *       teaches in the preview pane. NOTE: the pane is 30 cols wide and clips
 *       each '\n'-split line at ~28 visible chars (ide_editor.c:1674,1728-1734),
 *       so word-wrap to 28. Ready-to-paste composer + wrapper:
 *
 *           static char g_dictpv[640];
 *           static int  pv_word(char* o,int oc,int p,const char* s,int wrap){
 *               int col=0;                                  // append wrapped s
 *               for (int i=0; s[i] && p<oc-1; i++){
 *                   if (s[i]==' ' && col>=wrap){ o[p++]='\n'; col=0; continue; }
 *                   o[p++]=s[i]; col = (s[i]=='\n')?0:col+1;
 *               }
 *               return p;
 *           }
 *           static const char* dict_compose(const IdeDictEntry* d){
 *               int p=0, oc=(int)sizeof(g_dictpv);
 *               if (d->sig)  p=pv_word(g_dictpv,oc,p,d->sig,28);
 *               if (d->doc){ if(p<oc-2){g_dictpv[p++]='\n';g_dictpv[p++]='\n';}
 *                            p=pv_word(g_dictpv,oc,p,d->doc,28); }
 *               if (d->snippet){ if(p<oc-2){g_dictpv[p++]='\n';g_dictpv[p++]='\n';}
 *                            p=pv_word(g_dictpv,oc,p,d->snippet,60); } // raw-ish
 *               g_dictpv[p<oc?p:oc-1]=0; return g_dictpv;
 *           }
 *
 *       then in complete_preview():
 *           if (i<0||i>=AC_MAX_MATCHES) return 0;
 *           if (g_dict[i]) return dict_compose(g_dict[i]);
 *           ... (existing CK_SNIPPET path) ...
 *
 *   (g) Extend complete_accept() (ide_complete.c:117): a dict entry with a
 *       snippet expands it (tab-stops), otherwise inserts the bare word:
 *           if (g_dict[e->ac_sel]){
 *               const IdeDictEntry* d=g_dict[e->ac_sel];
 *               if (d->snippet && d->snippet[0])
 *                   ide_editor_apply_completion(a,d->snippet,1,e->ac_prefix_len);
 *               else
 *                   ide_editor_apply_completion(a,d->word,0,e->ac_prefix_len);
 *               e->ac_active=0; e->ac_count=0; return;
 *           }
 *
 * Freestanding: const char* literals in .rodata; zero .bss.
 */
#ifndef IDE_DICT_C_H
#define IDE_DICT_C_H

/* Shared entry struct (defined once across the three dict headers). */
#ifndef IDE_DICT_ENTRY_DEFINED
#define IDE_DICT_ENTRY_DEFINED
typedef struct {
    const char* word;     /* token typed to summon it (the match key)        */
    const char* sig;      /* one-line signature / grammar / operand shape    */
    const char* doc;      /* one-line teaching doc (renders in popup preview)*/
    const char* snippet;  /* body with ${N}/$0 tab-stops, or 0 (insert word) */
    char        kind;     /* 'k'eyword 't'ype 'f'unc 'm'acro 's'nippet        */
                          /* 'r'egister 'i'nstruction 'd'irective (asm/os)   */
} IdeDictEntry;
#endif

/* ===================================================================== *
 *  C KEYWORDS & TYPES  (kind 'k' / 't')
 * ===================================================================== */
static const IdeDictEntry IDE_DICT_C[] = {
    /* ---- primitive & storage types ---- */
    { "int", "int x;", "Signed integer. On-device cc it is 64-bit (every non-char type is 8 bytes).", 0, 't' },
    { "char", "char c;", "1-byte integer; holds one ASCII byte. The ONLY 1-byte type on-device cc.", 0, 't' },
    { "void", "void f(void);", "No type / no value. Use void* for a typeless pointer.", 0, 't' },
    { "short", "short s;", "Short integer. On-device cc treats it as 8 bytes like int.", 0, 't' },
    { "long", "long n;", "Long integer (8 bytes everywhere here). Good default for syscall args.", 0, 't' },
    { "unsigned", "unsigned u;", "Non-negative integer; wraps mod 2^N instead of overflowing.", 0, 't' },
    { "signed", "signed int x;", "Explicitly signed (the default for int/char-as-number).", 0, 't' },
    { "float", "float f;", "32-bit IEEE float. [host gcc only -- on-device cc has no FPU codegen].", 0, 't' },
    { "double", "double d;", "64-bit IEEE float. [host gcc only -- on-device cc has no FPU codegen].", 0, 't' },
    { "_Bool", "_Bool b;", "1-byte boolean (0 or 1). 'bool' is the same thing.", 0, 't' },
    { "bool", "bool b;", "1-byte true/false. true=1, false=0. (no <stdbool.h> needed on-device).", 0, 't' },
    { "struct", "struct Name { ... };", "Aggregate of named fields. Access with . (value) or -> (pointer).", 0, 'k' },
    { "union", "union U { ... };", "Overlapping fields sharing storage. On-device cc lays it out like a struct.", 0, 'k' },
    { "enum", "enum E { A, B };", "Named integer constants. [on-device cc: enumerator VALUES are not generated -- prefer plain int literals].", 0, 'k' },
    { "typedef", "typedef struct {..} T;", "Give a type a new name. Fully supported on-device cc.", 0, 'k' },
    { "const", "const int N = 8;", "Read-only qualifier. [on-device cc: parsed but IGNORED -- no real const enforcement].", 0, 'k' },
    { "volatile", "volatile int* r;", "Tells the compiler the value can change underfoot (MMIO). [on-device cc: ignored].", 0, 'k' },
    { "static", "static int g;", "File-scope: internal linkage. Inside a function: persistent local. [on-device cc: static LOCALS unsupported; file-scope OK, zero-init].", 0, 'k' },
    { "extern", "extern int g;", "Declares a symbol defined elsewhere. [on-device cc: ignored -- single TU, no linker].", 0, 'k' },
    { "register", "register int i;", "Hint to keep a local in a register. [on-device cc: ignored].", 0, 'k' },
    { "inline", "inline int f(void){..}", "Hint to inline a function. [on-device cc: ignored].", 0, 'k' },

    /* ---- fixed-width types (built into the lexer; no header needed) ---- */
    { "uint8_t", "uint8_t b;", "Unsigned 8-bit. Built into on-device cc lexer (but stored as 8 bytes there).", 0, 't' },
    { "uint16_t", "uint16_t h;", "Unsigned 16-bit. Built-in token; 8-byte storage on-device cc.", 0, 't' },
    { "uint32_t", "uint32_t w;", "Unsigned 32-bit. Built-in token; 8-byte storage on-device cc (mind struct layout!).", 0, 't' },
    { "uint64_t", "uint64_t q;", "Unsigned 64-bit. The natural width on-device.", 0, 't' },
    { "int8_t", "int8_t b;", "Signed 8-bit. Built-in token (8-byte storage on-device cc).", 0, 't' },
    { "int16_t", "int16_t h;", "Signed 16-bit. Built-in token (8-byte storage on-device cc).", 0, 't' },
    { "int32_t", "int32_t w;", "Signed 32-bit. Built-in token (8-byte storage on-device cc).", 0, 't' },
    { "int64_t", "int64_t q;", "Signed 64-bit. Matches the native register width.", 0, 't' },
    { "size_t", "size_t n;", "Unsigned size/count type (pointer-width = 8 bytes). Returned by sizeof.", 0, 't' },

    /* ---- control-flow keywords ---- */
    { "if", "if (cond) { ... }", "Run the block only when cond is non-zero.", 0, 'k' },
    { "else", "else { ... }", "Runs when the preceding if's cond was zero. Chain with 'else if'.", 0, 'k' },
    { "for", "for (init; cond; step)", "Counted loop: init once, test cond, body, step, repeat.", 0, 'k' },
    { "while", "while (cond) { ... }", "Repeat the block while cond stays non-zero (test FIRST).", 0, 'k' },
    { "do", "do { ... } while (c);", "Loop that runs the body at least once. [host gcc only -- on-device cc parses but does not generate do-while].", 0, 'k' },
    { "switch", "switch (x) { case..}", "Multi-way branch on an integer. [host gcc only -- on-device cc parses but does not generate switch; use an if/else chain].", 0, 'k' },
    { "case", "case 1: ...", "A labeled arm inside switch. Fall through unless you break. [host gcc only on-device].", 0, 'k' },
    { "default", "default: ...", "The fallback arm of a switch when no case matched. [host gcc only on-device].", 0, 'k' },
    { "break", "break;", "Leave the innermost loop or switch immediately.", 0, 'k' },
    { "continue", "continue;", "Skip to the next iteration of the innermost loop.", 0, 'k' },
    { "goto", "goto label;", "Jump to a label in the same function. [host gcc only -- on-device cc parses but does not generate goto].", 0, 'k' },
    { "return", "return expr;", "End the function and hand expr back to the caller (left in rax).", 0, 'k' },
    { "sizeof", "sizeof(T) / sizeof x", "Bytes a type or object occupies. On-device cc: char=1, everything else=8.", 0, 'k' },

    /* ---- literals / pseudo-constants ---- */
    { "true", "true", "Boolean 1.", 0, 'm' },
    { "false", "false", "Boolean 0.", 0, 'm' },
    { "NULL", "NULL", "The null pointer. No <stddef.h> on-device: paste the literal ((void*)0).", "((void*)0)", 'm' },

    /* ===================================================================== *
     *  CONTROL-FLOW SNIPPET PATTERNS  (kind 's')
     * ===================================================================== */
    { "forr", "for (i=0; i<n; i++)", "Counted loop over [0,n). Edit the bound and body.",
      "for (int ${1:i} = 0; ${1:i} < ${2:n}; ${1:i}++) {\n    $0\n}", 's' },
    { "forrev", "for (i=n-1; i>=0; i--)", "Reverse counted loop, n-1 down to 0.",
      "for (int ${1:i} = ${2:n} - 1; ${1:i} >= 0; ${1:i}--) {\n    $0\n}", 's' },
    { "forever", "for (;;) { ... }", "Infinite loop. Exit with break/return. The app main-loop shape.",
      "for (;;) {\n    $0\n}", 's' },
    { "grid", "nested for (y, x)", "Row-major 2D scan: outer y, inner x. The drawing/loop-over-pixels shape.",
      "for (int ${1:y} = 0; ${1:y} < ${2:h}; ${1:y}++) {\n"
      "    for (int ${3:x} = 0; ${3:x} < ${4:w}; ${3:x}++) {\n"
      "        $0\n    }\n}", 's' },
    { "whilel", "while (cond) { ... }", "Pre-tested loop. Make sure cond eventually goes zero.",
      "while (${1:cond}) {\n    $0\n}", 's' },
    { "iff", "if (cond) { ... }", "Single guarded block.",
      "if (${1:cond}) {\n    $0\n}", 's' },
    { "ifel", "if (c) {..} else {..}", "Two-way branch.",
      "if (${1:cond}) {\n    $0\n} else {\n}", 's' },
    { "elif", "if/else-if/else chain", "Multi-way branch using else-if (the on-device-safe substitute for switch).",
      "if (${1:x} == ${2:A}) {\n    $0\n} else if (${1:x} == ${3:B}) {\n} else {\n}", 's' },
    { "guard", "if (!ok) return ...;", "Early-out guard: bail at the top so the happy path stays unindented.",
      "if (${1:!ok}) return ${2:-1};\n$0", 's' },
    { "rangeck", "clamp index to bounds", "Reject an out-of-range index before using it (no UB).",
      "if (${1:i} < 0 || ${1:i} >= ${2:n}) return ${3:-1};\n$0", 's' },
    { "dowhile", "do {..} while (c);", "Body-runs-once loop. [host gcc only -- on-device cc cannot generate do-while].",
      "do {\n    $0\n} while (${1:cond});", 's' },
    { "switchc", "switch (x) { cases }", "Jump table on an int. [host gcc only -- on-device use 'elif'].",
      "switch (${1:expr}) {\n    case ${2:0}:\n        $0\n        break;\n    default:\n        break;\n}", 's' },

    /* ---- function / type scaffolding ---- */
    { "funcc", "ret name(args){..}", "Function definition skeleton (<=6 params on-device cc).",
      "${1:int} ${2:name}(${3:void}) {\n    $0\n}", 's' },
    { "mainc", "int main(...)", "Program entry. crt0 turns the return value into SYS_EXIT(code).",
      "int main(int argc, char** argv) {\n    (void)argc; (void)argv;\n    $0\n    return 0;\n}", 's' },
    { "structc", "struct Name {..};", "Define a record type.",
      "struct ${1:Name} {\n    ${2:int} ${3:field};\n};", 's' },
    { "typedefst", "typedef struct {..} T;", "Define + name a record in one go.",
      "typedef struct {\n    ${1:int} ${2:field};\n} ${3:Name};", 's' },
    { "enumc", "enum {A, B, C};", "Name a set of int constants. [on-device cc: values not generated -- use literals].",
      "enum ${1:Name} {\n    ${2:VALUE} = 0,\n};", 's' },

    /* ===================================================================== *
     *  STDLIB-SHAPED HELPERS  (kind 's')  -- there is NO libc; each snippet
     *  INLINES its own freestanding implementation so it works on-device.
     * ===================================================================== */
    { "strlen_", "size_t strlen(s)", "No libc: count bytes up to the NUL. Inlines a local loop.",
      "long ${1:slen}(const char* s) {\n    long n = 0;\n    while (s[n]) n++;\n    return n;\n}$0", 's' },
    { "strcpy_", "char* strcpy(d,s)", "No libc: copy s (incl. NUL) into d. Caller guarantees room.",
      "void ${1:scopy}(char* d, const char* s) {\n    long i = 0;\n    while (s[i]) { d[i] = s[i]; i++; }\n    d[i] = 0;\n}$0", 's' },
    { "strncpy_", "bounded string copy", "No libc: copy at most cap-1 bytes, always NUL-terminate.",
      "void ${1:scopyn}(char* d, const char* s, long cap) {\n    long i = 0;\n    while (i < cap - 1 && s[i]) { d[i] = s[i]; i++; }\n    d[i] = 0;\n}$0", 's' },
    { "strcmp_", "int strcmp(a,b)", "No libc: 0 if equal, else sign of first differing byte.",
      "long ${1:scmp}(const char* a, const char* b) {\n    long i = 0;\n    while (a[i] && a[i] == b[i]) i++;\n    return (long)(unsigned char)a[i] - (long)(unsigned char)b[i];\n}$0", 's' },
    { "streq_", "int streq(a,b)", "No libc: 1 if two strings are equal, else 0.",
      "long ${1:streq}(const char* a, const char* b) {\n    long i = 0;\n    while (a[i] && a[i] == b[i]) i++;\n    return a[i] == b[i];\n}$0", 's' },
    { "strcat_", "char* strcat(d,s)", "No libc: append s to the end of d (d must have room).",
      "void ${1:scat}(char* d, const char* s) {\n    long i = 0; while (d[i]) i++;\n    long j = 0; while (s[j]) { d[i++] = s[j++]; }\n    d[i] = 0;\n}$0", 's' },
    { "strchr_", "char* strchr(s,c)", "No libc: address of first c in s, or NULL.",
      "char* ${1:schr}(char* s, int c) {\n    while (*s) { if (*s == (char)c) return s; s++; }\n    return ((void*)0);\n}$0", 's' },
    { "memcpy_", "void memcpy(d,s,n)", "No libc: byte copy of n bytes (non-overlapping).",
      "void ${1:mcopy}(void* d, const void* s, long n) {\n    char* dp = (char*)d; const char* sp = (const char*)s;\n    for (long i = 0; i < n; i++) dp[i] = sp[i];\n}$0", 's' },
    { "memset_", "void memset(d,v,n)", "No libc: set n bytes of d to v. Use char* so each store is 1 byte.",
      "void ${1:mset}(void* d, int v, long n) {\n    char* dp = (char*)d;\n    for (long i = 0; i < n; i++) dp[i] = (char)v;\n}$0", 's' },
    { "memmove_", "void memmove(d,s,n)", "No libc: overlap-safe copy (picks direction by address order).",
      "void ${1:mmove}(char* d, const char* s, long n) {\n    if (d < s) for (long i = 0; i < n; i++) d[i] = s[i];\n    else for (long i = n - 1; i >= 0; i--) d[i] = s[i];\n}$0", 's' },
    { "atoi_", "int atoi(s)", "No libc: parse a signed decimal string (stops at first non-digit).",
      "long ${1:atoi}(const char* s) {\n    long v = 0, neg = 0;\n    if (*s == '-') { neg = 1; s++; }\n    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }\n    return neg ? -v : v;\n}$0", 's' },
    { "itoa_", "int itoa(v,buf)", "No libc: unsigned int -> decimal string; returns length. Builds reversed then flips.",
      "long ${1:itoa}(unsigned long v, char* buf) {\n    char t[24]; int i = 0;\n    if (v == 0) t[i++] = '0';\n    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }\n    for (int j = 0; j < i; j++) buf[j] = t[i - 1 - j];\n    buf[i] = 0; return i;\n}$0", 's' },
    { "isdigit_", "int isdigit(c)", "No libc: 1 if c is '0'..'9'.",
      "long ${1:isdigit}(int c) { return c >= '0' && c <= '9'; }$0", 's' },
    { "isalpha_", "int isalpha(c)", "No libc: 1 if c is a-z or A-Z.",
      "long ${1:isalpha}(int c) { return (c|32) >= 'a' && (c|32) <= 'z'; }$0", 's' },
    { "tolower_", "int tolower(c)", "No libc: ASCII upper->lower (others unchanged).",
      "long ${1:tolower}(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }$0", 's' },

    /* ===================================================================== *
     *  COMMON ALGORITHM PATTERNS  (kind 's')
     * ===================================================================== */
    { "swap_", "swap two ints", "Exchange a and b through a temporary (no XOR tricks needed).",
      "{ long ${1:tmp} = ${2:a}; ${2:a} = ${3:b}; ${3:b} = ${1:tmp}; }$0", 's' },
    { "clamp_", "clamp v to [lo,hi]", "Pin v into a range. The min/max idiom written out.",
      "if (${1:v} < ${2:lo}) ${1:v} = ${2:lo};\nif (${1:v} > ${3:hi}) ${1:v} = ${3:hi};\n$0", 's' },
    { "min_", "long imin(a,b)", "No libc/macro: smaller of two values (avoids double-eval of a macro).",
      "long ${1:imin}(long a, long b) { return a < b ? a : b; }$0", 's' },
    { "max_", "long imax(a,b)", "No libc/macro: larger of two values.",
      "long ${1:imax}(long a, long b) { return a > b ? a : b; }$0", 's' },
    { "abs_", "long iabs(x)", "No libc: absolute value of a signed integer.",
      "long ${1:iabs}(long x) { return x < 0 ? -x : x; }$0", 's' },
    { "linsearch", "linear search", "Return the index of key in a[0..n), or -1. O(n).",
      "long ${1:find}(long* a, long n, long key) {\n    for (long i = 0; i < n; i++) if (a[i] == key) return i;\n    return -1;\n}$0", 's' },
    { "binsearch", "binary search (sorted)", "O(log n) search of a SORTED array; returns index or -1.",
      "long ${1:bsearch}(long* a, long n, long key) {\n    long lo = 0, hi = n - 1;\n    while (lo <= hi) {\n        long m = (lo + hi) / 2;\n        if (a[m] == key) return m;\n        if (a[m] < key) lo = m + 1; else hi = m - 1;\n    }\n    return -1;\n}$0", 's' },
    { "bubblesort", "bubble sort (in place)", "Simple O(n^2) ascending sort. Fine for small fixed arrays.",
      "for (long ${1:i} = 0; ${1:i} < ${2:n} - 1; ${1:i}++)\n    for (long ${3:j} = 0; ${3:j} < ${2:n} - 1 - ${1:i}; ${3:j}++)\n        if (${4:a}[${3:j}] > ${4:a}[${3:j}+1]) {\n            long t = ${4:a}[${3:j}]; ${4:a}[${3:j}] = ${4:a}[${3:j}+1]; ${4:a}[${3:j}+1] = t;\n        }\n$0", 's' },
    { "gcd_", "long gcd(a,b)", "Euclid's algorithm; greatest common divisor.",
      "long ${1:gcd}(long a, long b) { while (b) { long t = b; b = a % b; a = t; } return a; }$0", 's' },
    { "bitset_", "set/clear/test a bit", "Manipulate bit n of a word: set, clear, toggle, test.",
      "${1:w} |=  (1UL << ${2:n});   /* set   */\n${1:w} &= ~(1UL << ${2:n});   /* clear */\n${1:w} ^=  (1UL << ${2:n});   /* toggle*/\nlong on = (${1:w} >> ${2:n}) & 1;  /* test */$0", 's' },
    { "popcount_", "long popcount(x)", "Count set bits with Kernighan's loop (clears lowest set bit each pass).",
      "long ${1:popcount}(unsigned long x) { long c = 0; while (x) { x &= x - 1; c++; } return c; }$0", 's' },
    { "ringidx", "ring buffer indices", "Wrap head/tail around a power-of-two-or-mod capacity (FIFO).",
      "long ${1:next} = (${2:head} + 1) % ${3:CAP};\nif (${1:next} == ${4:tail}) { /* full */ }\nelse { ${5:buf}[${2:head}] = ${6:v}; ${2:head} = ${1:next}; }$0", 's' },
};
#define IDE_DICT_C_COUNT ((int)(sizeof(IDE_DICT_C) / sizeof(IDE_DICT_C[0])))

#endif /* IDE_DICT_C_H */

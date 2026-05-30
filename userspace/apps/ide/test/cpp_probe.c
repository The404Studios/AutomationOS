/* host-only probe: what does the current toolchain do with C vs C++ input? */
#include <stdio.h>
#include <string.h>
#include "../ide_parser.h"
#include "../tc.h"

static Tok toks[PARSE_MAX_TOKS];
static Parser P;
static char asmb[TC_ASM_CAP];

static int count_funcs(AstNode* tu) {
    int n = 0;
    if (!tu) return 0;
    for (AstNode* c = tu->first_child; c; c = c->next)
        if (c->kind == AST_FUNC_DEF) n++;
    return n;
}

static void try_src(const char* label, const char* src) {
    parser_init(&P, src, (int)strlen(src), toks, PARSE_MAX_TOKS);
    AstNode* tu = parse_translation_unit(&P);
    TcDiag d[64]; int nd = 0;
    int ok = cc_compile(tu, asmb, TC_ASM_CAP, d, &nd);
    printf("[%s]\n  parser diags=%d  funcs found=%d  cc_compile ok=%d  cc diags=%d\n",
           label, P.ndiags, count_funcs(tu), ok, nd);
    for (int i = 0; i < nd && i < 4; i++) printf("    cc: L%d %s\n", d[i].line, d[i].msg);
    for (int i = 0; i < P.ndiags && i < 4; i++) printf("    parse: L%d %s\n", P.diags[i].line, P.diags[i].msg);
}

int main(void) {
    try_src("C-compatible .cpp",
        "int add(int a,int b){return a+b;} int main(){return add(40,2);}");
    try_src("real C++ (class+methods)",
        "class Counter{ int n; public: void inc(){ n=n+1; } int get(){ return n; } };"
        " int main(){ Counter c; c.inc(); return c.get(); }");
    return 0;
}

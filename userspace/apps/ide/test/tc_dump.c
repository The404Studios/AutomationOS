/* host-only: dump OUR assembler's bytes for sum.c so we can disassemble with
 * correct VMAs and verify control-flow / label addresses. */
#include <stdio.h>
#include <string.h>
#include "../ide_parser.h"
#include "../tc.h"

static Tok toks[PARSE_MAX_TOKS];
static Parser P;
static char asmbuf[TC_ASM_CAP];
static unsigned char code[TC_CODE_CAP];

int main(void) {
    const char* src =
        "int main(void){int s=0;int i=1;while(i<=10){s=s+i;i=i+1;}return s;}";
    parser_init(&P, src, (int)strlen(src), toks, PARSE_MAX_TOKS);
    AstNode* tu = parse_translation_unit(&P);
    TcDiag d[64]; int nd = 0;
    int ok = cc_compile(tu, asmbuf, TC_ASM_CAP, d, &nd);
    printf("=== cc_compile ok=%d ndiag=%d ===\n%s\n", ok, nd, asmbuf);
    int clen = 0; nd = 0;
    ok = as_assemble(asmbuf, TC_ENTRY_VADDR, code, TC_CODE_CAP, &clen, d, &nd);
    printf("=== as_assemble ok=%d len=%d ndiag=%d ===\n", ok, clen, nd);
    for (int i = 0; i < nd; i++) printf("  diag L%d: %s\n", d[i].line, d[i].msg);
    FILE* f = fopen("/tmp/our.bin", "wb");
    if (f) { fwrite(code, 1, clen, f); fclose(f); }
    printf("wrote /tmp/our.bin (%d bytes), base=0x%llx\n", clen,
           (unsigned long long)TC_ENTRY_VADDR);
    return 0;
}

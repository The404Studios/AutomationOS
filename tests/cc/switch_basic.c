/* cc regression: switch/case/default codegen (CC-SWITCH-0).
 * EXPECT: 20  -- switch(x==2) takes case 2.
 * The parser already emitted AST_SWITCH/CASE/DEFAULT (ide_pstmt.c); this proves
 * the new gen_switch() lowers them to a compare-and-jump table: for each case it
 * emits `mov rcx, rax` + `cmp rcx, <val>` + `je .L<label>`, then `jmp .L<default>`,
 * then the bodies in source order (so `break` exits via the switch end label).
 * Pre-fix symptom: cc errored "unsupported statement" on the switch. */
int main(void) {
    int x = 2;
    int r = 0;
    switch (x) {
        case 1: r = 10; break;
        case 2: r = 20; break;
        case 3: r = 30; break;
        default: r = 99; break;
    }
    return r;
}

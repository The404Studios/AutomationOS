/* cc regression: every declarator in a multi-declarator must survive
 * (CC-MULTIDECL-0). EXPECT (x + y): 30.
 *
 * Pre-fix symptom: the parser modelled only the first declarator and discarded
 * the rest ("int x, y;" kept only x), so y compiled to an "unknown identifier".
 * The .data must therefore contain BOTH initializers (dq 10 and dq 20). */
int x = 10, y = 20;

int main(void) {
    return x + y;
}

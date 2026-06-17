/* cc regression: a mixed-width struct lays its init out by field size -- char
 * as `db`, int as `dq`, with the alignment gap zero-padded (CC-STRUCTINIT-0).
 * EXPECT (m.c + m.i): 72.
 * The char field MUST emit `db 65` (not `dq 65`) so the int lands at offset 8. */
struct M { char c; int i; };
struct M m = {65, 7};

int main(void) {
    return m.c + m.i;
}

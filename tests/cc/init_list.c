/* cc regression: brace init-list values survive + array index in a loop (B3 + B4).
 * EXPECT: 10   (1 + 2 + 3 + 4)
 * Pre-fix symptom: the array was zeroed at .data emit time, so the sum was 0.
 *
 * NB: deliberately uses SEPARATE declarations + plain assignment. The
 * multi-declarator form `int s = 0, i;` and the `+=` compound-assign hit
 * UNRELATED cc-subset limits (tracked as a follow-up: CC-MULTIDECL-COMPOUND-0),
 * so this case stays focused on the array initializer + indexing fix. */
int a[4] = {1, 2, 3, 4};

int main(void) {
    int s = 0;
    int i = 0;
    for (i = 0; i < 4; i++) {
        s = s + a[i];
    }
    return s;
}

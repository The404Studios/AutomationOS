/* cc regression: pointer-from-array decay + pointer arithmetic + deref.
 * EXPECT (*(p + 2)): 9.
 * Verifies `p = a` (array decays to its address), `p + 2` element scaling, and
 * the `*(p + 2)` load. The array's values (3, 6, 9) must reach .data. */
int a[3] = {3, 6, 9};

int main(void) {
    int *p;
    p = a;
    return *(p + 2);
}

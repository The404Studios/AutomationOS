/* cc regression: a const-qualified global array initializer must be emitted
 * (B4 follow-on). EXPECT (t[3]): 8.
 * Verifies `const` qualifier + brace-init + constant indexing together. */
const int t[5] = {2, 4, 6, 8, 10};

int main(void) {
    return t[3];
}

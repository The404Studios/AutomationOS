/*
 * fact.c -- iterative factorial of 5.
 *
 * 5 * 4 * 3 * 2 * 1 = 120, so the process exits with code 120.
 * Demonstrates a multiply loop with a decrementing counter.
 */
int main(void) {
    int n = 5;
    int f = 1;
    while (n > 1) {
        f = f * n;
        n = n - 1;
    }
    return f;
}

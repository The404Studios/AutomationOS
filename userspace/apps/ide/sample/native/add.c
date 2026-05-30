/*
 * add.c -- function call test.
 *
 * main calls add(40, 2), so the process exits with code 42.
 * Exercises the generated SysV-like call convention (args in
 * rdi/rsi, return in rax).
 */
int add(int a, int b) {
    return a + b;
}

int main(void) {
    return add(40, 2);
}

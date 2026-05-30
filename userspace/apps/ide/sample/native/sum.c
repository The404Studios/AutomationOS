/*
 * sum.c -- sum the integers 1..10 and return the total.
 *
 * 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 = 55, so the process
 * exits with code 55. Build with (B), run with (R) in the IDE.
 */
int main(void) {
    int s = 0;
    int i = 1;
    while (i <= 10) {
        s = s + i;
        i = i + 1;
    }
    return s;
}

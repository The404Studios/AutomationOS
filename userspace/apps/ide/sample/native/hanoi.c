/*
 * hanoi.c -- Towers of Hanoi, solved recursively, printed move by move.
 *
 * A game the on-device toolchain can compile and run. Self-contained: no
 * #include, no libc, integer-only. Output goes through the sys_write builtin
 * one byte at a time via a 1-byte global (sidesteps array-to-pointer decay).
 *
 * Showcases: recursion, multi-arg calls, globals, while loops, division/modulo.
 * Build it in the IDE (press B) or from a terminal:  cc /usr/src/native/hanoi.c -o /tmp/hanoi
 */

char g_ch;

void putc1(int c) {
    g_ch = c;
    sys_write(1, &g_ch, 1);
}

void puts0(char *s) {
    int n;
    n = 0;
    while (s[n] != 0) n = n + 1;
    sys_write(1, s, n);
}

void puti(int v) {
    char tmp[24];
    int n;
    int neg;
    neg = 0;
    if (v < 0) { neg = 1; v = 0 - v; }
    n = 0;
    if (v == 0) { tmp[0] = '0'; n = 1; }
    while (v > 0) {
        tmp[n] = '0' + (v % 10);
        n = n + 1;
        v = v / 10;
    }
    if (neg) putc1('-');
    while (n > 0) {
        n = n - 1;
        putc1(tmp[n]);
    }
}

int g_moves;

void move(int disk, int from, int to) {
    puts0("  move disk ");
    puti(disk);
    puts0("  :  peg ");
    puti(from);
    puts0(" -> peg ");
    puti(to);
    putc1('\n');
    g_moves = g_moves + 1;
}

void hanoi(int n, int from, int to, int via) {
    if (n == 0) return;
    hanoi(n - 1, from, via, to);
    move(n, from, to);
    hanoi(n - 1, via, to, from);
}

int main() {
    puts0("Towers of Hanoi -- 4 disks, peg 1 -> peg 3\n\n");
    g_moves = 0;
    hanoi(4, 1, 3, 2);
    putc1('\n');
    puts0("solved in ");
    puti(g_moves);
    puts0(" moves (2^4 - 1 = 15)\n");
    return 0;
}

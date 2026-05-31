/*
 * life.c -- Conway's Game of Life. A zero-player game: seed a glider and a
 * blinker, then print six generations of a 16x24 world.
 *
 * Self-contained for the on-device toolchain: no #include, no libc, integer
 * only. The board lives in two global char arrays; output is one byte at a
 * time via a 1-byte global (sidesteps array-to-pointer decay in calls).
 *
 * Showcases: 2-D arrays in 1-D, nested loops, neighbour counting, double
 * buffering. Build in the IDE (B) or:  cc /usr/src/native/life.c -o /tmp/life
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
    n = 0;
    if (v == 0) { tmp[0] = '0'; n = 1; }
    while (v > 0) {
        tmp[n] = '0' + (v % 10);
        n = n + 1;
        v = v / 10;
    }
    while (n > 0) {
        n = n - 1;
        putc1(tmp[n]);
    }
}

/* 16 rows x 24 cols, flattened */
char cur[384];
char nxt[384];

int at(int r, int c) {
    return r * 24 + c;
}

int neighbours(int r, int c) {
    int dr;
    int dc;
    int cnt;
    int rr;
    int cc;
    cnt = 0;
    dr = 0 - 1;
    while (dr <= 1) {
        dc = 0 - 1;
        while (dc <= 1) {
            if (dr != 0 || dc != 0) {
                rr = r + dr;
                cc = c + dc;
                if (rr >= 0 && rr < 16 && cc >= 0 && cc < 24) {
                    if (cur[at(rr, cc)] != 0) cnt = cnt + 1;
                }
            }
            dc = dc + 1;
        }
        dr = dr + 1;
    }
    return cnt;
}

void draw() {
    int r;
    int c;
    r = 0;
    while (r < 16) {
        c = 0;
        while (c < 24) {
            if (cur[at(r, c)] != 0) putc1('#');
            else putc1('.');
            c = c + 1;
        }
        putc1('\n');
        r = r + 1;
    }
}

void step() {
    int r;
    int c;
    int n;
    int i;
    r = 0;
    while (r < 16) {
        c = 0;
        while (c < 24) {
            n = neighbours(r, c);
            if (cur[at(r, c)] != 0) {
                if (n == 2 || n == 3) nxt[at(r, c)] = 1;
                else nxt[at(r, c)] = 0;
            } else {
                if (n == 3) nxt[at(r, c)] = 1;
                else nxt[at(r, c)] = 0;
            }
            c = c + 1;
        }
        r = r + 1;
    }
    i = 0;
    while (i < 384) {
        cur[i] = nxt[i];
        i = i + 1;
    }
}

void seed(int r, int c) {
    cur[at(r, c)] = 1;
}

int main() {
    int g;
    int i;
    i = 0;
    while (i < 384) { cur[i] = 0; i = i + 1; }

    /* a glider, top-left */
    seed(0, 1);
    seed(1, 2);
    seed(2, 0);
    seed(2, 1);
    seed(2, 2);

    /* a blinker, middle */
    seed(8, 10);
    seed(8, 11);
    seed(8, 12);

    g = 0;
    while (g < 6) {
        puts0("-- generation ");
        puti(g);
        putc1('\n');
        draw();
        putc1('\n');
        step();
        g = g + 1;
    }
    puts0("done.\n");
    return 0;
}

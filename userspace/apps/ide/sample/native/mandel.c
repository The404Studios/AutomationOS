/*
 * mandel.c -- the Mandelbrot set in ASCII, with NO floating point.
 *
 * The on-device toolchain has no floats, so this renders the set using
 * fixed-point integers (scale 1<<12 = 4096). Every pixel iterates
 * z = z*z + c until it escapes, and the iteration count picks a shade.
 *
 * Self-contained: no #include, no libc. Output is one byte at a time via a
 * 1-byte global. Build in the IDE (B) or:  cc /usr/src/native/mandel.c -o /tmp/mandel
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

int main() {
    int W;
    int H;
    int S;
    int MAXIT;
    int px;
    int py;
    int x0;
    int y0;
    int dx;
    int dy;
    int cre;
    int cim;
    int zre;
    int zim;
    int zre2;
    int zim2;
    int it;
    int t;

    W = 70;
    H = 32;
    S = 4096;          /* fixed-point scale: 1.0 == 4096 */
    MAXIT = 64;

    /* viewport: real in [-2.5, 1.0], imag in [-1.25, 1.25] (times S) */
    x0 = 0 - 10240;            /* -2.5 * 4096 */
    y0 = 0 - 5120;             /* -1.25 * 4096 */
    dx = 14336 / 70;           /*  (3.5 * 4096) / W */
    dy = 10240 / 32;           /*  (2.5 * 4096) / H */

    puts0("Mandelbrot set -- integer fixed-point, no FPU\n\n");

    py = 0;
    while (py < H) {
        px = 0;
        while (px < W) {
            cre = x0 + px * dx;
            cim = y0 + py * dy;
            zre = 0;
            zim = 0;
            zre2 = 0;
            zim2 = 0;
            it = 0;
            while (it < MAXIT) {
                zre2 = (zre * zre) / S;
                zim2 = (zim * zim) / S;
                if (zre2 + zim2 > 4 * S) break;   /* escaped the disc of radius 2 */
                t = (zre * zim) / S;
                zim = 2 * t + cim;
                zre = zre2 - zim2 + cre;
                it = it + 1;
            }
            /* it == MAXIT  => point is (probably) in the set;
             * it <  MAXIT  => it escaped after `it` iterations.            */

            if (it >= MAXIT) putc1(' ');
            else if (it > 28) putc1('.');
            else if (it > 18) putc1(',');
            else if (it > 12) putc1(':');
            else if (it > 8)  putc1('-');
            else if (it > 5)  putc1('=');
            else if (it > 3)  putc1('+');
            else if (it > 2)  putc1('*');
            else putc1('#');

            px = px + 1;
        }
        putc1('\n');
        py = py + 1;
    }
    return 0;
}

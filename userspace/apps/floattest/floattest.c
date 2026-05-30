/* floattest -- proves ring-3 float/SSE works end-to-end at RUNTIME. Inputs come
 * through a `volatile` array so the compiler can't constant-fold the math away;
 * what's left is real SSE: scalar mulss/addss/divss, a 2x2 float matmul (the
 * tensor-kernel seed), an auto-vectorizable reduction, and float<->int
 * conversion (cvttss2si). Prints a PASS/FAIL marker the boot log can be checked
 * for. Built with gcc (emits SSE for floats), crt0-linked; inline syscalls only. */

typedef unsigned long size_t;

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static size_t slen(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s) { sc(3 /*SYS_WRITE*/, 1, (long)s, (long)slen(s)); }

static void out_uint(unsigned long v) {
    char buf[24];
    int i = 24;
    buf[--i] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    out(&buf[i]);
}

static float fabsf_(float x) { return x < 0.0f ? -x : x; }
static int near(float a, float b) { return fabsf_(a - b) < 0.001f; }

int main(void) {
    int ok = 1;

    /* volatile source => real SSE loads at runtime, no constant folding. */
    volatile float Vin[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    /* 1. scalar float: mul, add, div. */
    float a  = Vin[0] + 0.5f;            /* 1.5  */
    float b  = Vin[1];                   /* 2.0  */
    float r1 = a * b + 0.25f;            /* 3.25 */
    float r2 = (Vin[7] + 2.0f) / 4.0f;   /* 2.50 */
    if (!near(r1, 3.25f)) ok = 0;
    if (!near(r2, 2.50f)) ok = 0;

    /* 2. tiny 2x2 float matmul C = A*B (the tensor-kernel foundation). */
    float A[2][2] = {{Vin[0], Vin[1]}, {Vin[2], Vin[3]}};   /* {{1,2},{3,4}} */
    float B[2][2] = {{Vin[4], Vin[5]}, {Vin[6], Vin[7]}};   /* {{5,6},{7,8}} */
    float C[2][2];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            float s = 0.0f;
            for (int k = 0; k < 2; k++) s += A[i][k] * B[k][j];
            C[i][j] = s;
        }
    /* expected {{19,22},{43,50}} */
    if (!near(C[0][0], 19.0f) || !near(C[0][1], 22.0f) ||
        !near(C[1][0], 43.0f) || !near(C[1][1], 50.0f)) ok = 0;

    /* 3. array reduction (auto-vectorizable to packed SSE at -O2). */
    float v[8];
    for (int i = 0; i < 8; i++) v[i] = Vin[i];
    float sum = 0.0f;
    for (int i = 0; i < 8; i++) sum += v[i];   /* 36 */
    if (!near(sum, 36.0f)) ok = 0;

    /* evidence (float->int via SSE conversion) */
    out("FLOATTEST: matmul C00="); out_uint((unsigned long)(C[0][0] + 0.5f));
    out(" C11=");                  out_uint((unsigned long)(C[1][1] + 0.5f));
    out(" vsum=");                 out_uint((unsigned long)(sum + 0.5f));
    out(" r1x100=");               out_uint((unsigned long)(r1 * 100.0f + 0.5f));
    out("\n");

    out(ok ? "FLOATTEST: PASS\n" : "FLOATTEST: FAIL\n");
    return 0;
}

/*
 * p384.c -- NIST P-384 (secp384r1) ECDSA signature verification.
 * ===============================================================
 *
 * Freestanding, no-libc, no-malloc. See p384.h for the public API.
 *
 * Built on the generic modular arithmetic in bignum.c (bn_mod_mul/add/sub and
 * bn_mod_exp for Fermat inversion). Point arithmetic uses Jacobian projective
 * coordinates (X:Y:Z) representing the affine point (X/Z^2, Y/Z^3); the point
 * at infinity is the `inf` flag. Scalar multiplication is straightforward
 * left-to-right double-and-add: every value processed here is PUBLIC (public
 * key, signature, message hash), so timing side-channels are not a concern.
 *
 * Constants are the FIPS 186-4 / SEC2 domain parameters, transcribed verbatim
 * as big-endian hex (cross-checked against authoritative sources). The KAT in
 * p384_selftest() is RFC 6979 Appendix A.2.6 (message "sample", SHA-384).
 */

#include "p384.h"
#include "bignum.h"
#include "sha512.h"

/* ===================================================================== *
 *  Domain parameters (big-endian hex, 48 bytes / 384 bits each).         *
 *  a = p - 3.                                                            *
 * ===================================================================== */
static const char P_HEX[]  = "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff";
static const char A_HEX[]  = "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000fffffffc";
static const char B_HEX[]  = "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef";
static const char N_HEX[]  = "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973";
static const char GX_HEX[] = "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7";
static const char GY_HEX[] = "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f";

static bignum P, A, B, N, GX, GY;
static int g_init = 0;

/* ===================================================================== *
 *  Hex helpers + lazy constant init.                                     *
 * ===================================================================== */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* Parse a 96-char (48-byte) big-endian hex string into a bignum. */
static void bn_from_hex96(bignum *x, const char *h)
{
    unsigned char be[48];
    for (int i = 0; i < 48; i++)
        be[i] = (unsigned char)((hexval(h[2 * i]) << 4) | hexval(h[2 * i + 1]));
    bn_from_bytes(x, be, 48);
}

static void ensure_init(void)
{
    if (g_init) return;
    bn_from_hex96(&P,  P_HEX);
    bn_from_hex96(&A,  A_HEX);
    bn_from_hex96(&B,  B_HEX);
    bn_from_hex96(&N,  N_HEX);
    bn_from_hex96(&GX, GX_HEX);
    bn_from_hex96(&GY, GY_HEX);
    g_init = 1;
}

/* ===================================================================== *
 *  Field arithmetic mod P (thin wrappers; alias-safe -- the bignum mod   *
 *  helpers read all operands before writing the destination).            *
 * ===================================================================== */
static void fmul(bignum *r, const bignum *a, const bignum *b) { bn_mod_mul(r, a, b, &P); }
static void fadd(bignum *r, const bignum *a, const bignum *b) { bn_mod_add(r, a, b, &P); }
static void fsub(bignum *r, const bignum *a, const bignum *b) { bn_mod_sub(r, a, b, &P); }
static void fsqr(bignum *r, const bignum *a)                  { bn_mod_mul(r, a, a, &P); }

/* r = a^-1 mod P  (Fermat: a^(P-2), valid since P is prime). */
static void finv(bignum *r, const bignum *a)
{
    bignum e, two;
    bn_set_u32(&two, 2);
    bn_mod_sub(&e, &P, &two, &P);   /* e = P - 2 */
    bn_mod_exp(r, a, &e, &P);
}

/* ===================================================================== *
 *  Jacobian point arithmetic on y^2 = x^3 + A x + B over GF(P).          *
 * ===================================================================== */
typedef struct { bignum X, Y, Z; int inf; } jpt;

static void jset_inf(jpt *p)
{
    bn_set_u32(&p->X, 1);
    bn_set_u32(&p->Y, 1);
    bn_set_zero(&p->Z);
    p->inf = 1;
}

/* r = 2*p (generic-a doubling). Computes into locals so r may alias p. */
static void jdouble(jpt *r, const jpt *p)
{
    if (p->inf || bn_is_zero(&p->Y)) { jset_inf(r); return; }

    bignum YY, S, M, X2, Z2, Z4, t, Y4, e8, nx, ny, nz;

    fsqr(&YY, &p->Y);                       /* YY = Y^2                       */
    fmul(&S, &p->X, &YY);                    /* S  = X*Y^2                     */
    fadd(&S, &S, &S); fadd(&S, &S, &S);      /* S  = 4*X*Y^2                   */

    fsqr(&X2, &p->X);                        /* X2 = X^2                       */
    fadd(&M, &X2, &X2); fadd(&M, &M, &X2);   /* M  = 3*X^2                     */
    fsqr(&Z2, &p->Z); fsqr(&Z4, &Z2);        /* Z4 = Z^4                       */
    fmul(&t, &A, &Z4); fadd(&M, &M, &t);     /* M  = 3X^2 + a*Z^4             */

    fsqr(&nx, &M);                           /* nx = M^2                       */
    fadd(&t, &S, &S); fsub(&nx, &nx, &t);    /* nx = M^2 - 2S                  */

    fsqr(&Y4, &YY);                          /* Y4 = Y^4                       */
    fadd(&e8, &Y4, &Y4); fadd(&e8, &e8, &e8); fadd(&e8, &e8, &e8); /* 8*Y^4   */
    fsub(&t, &S, &nx); fmul(&ny, &M, &t);    /* ny = M*(S - nx)                */
    fsub(&ny, &ny, &e8);                     /* ny = M*(S-nx) - 8Y^4           */

    fmul(&t, &p->Y, &p->Z); fadd(&nz, &t, &t); /* nz = 2*Y*Z                   */

    bn_copy(&r->X, &nx); bn_copy(&r->Y, &ny); bn_copy(&r->Z, &nz); r->inf = 0;
}

/* r = p + q (full Jacobian addition). Computes into locals so r may alias. */
static void jadd(jpt *r, const jpt *p, const jpt *q)
{
    if (p->inf) { *r = *q; return; }
    if (q->inf) { *r = *p; return; }

    bignum Z1Z1, Z2Z2, U1, U2, S1, S2, t, H, RR;

    fsqr(&Z1Z1, &p->Z); fsqr(&Z2Z2, &q->Z);
    fmul(&U1, &p->X, &Z2Z2);                  /* U1 = X1*Z2^2                  */
    fmul(&U2, &q->X, &Z1Z1);                  /* U2 = X2*Z1^2                  */
    fmul(&t, &q->Z, &Z2Z2); fmul(&S1, &p->Y, &t); /* S1 = Y1*Z2^3             */
    fmul(&t, &p->Z, &Z1Z1); fmul(&S2, &q->Y, &t); /* S2 = Y2*Z1^3             */

    if (bn_cmp(&U1, &U2) == 0) {
        if (bn_cmp(&S1, &S2) != 0) { jset_inf(r); return; } /* P + (-P) = inf */
        jdouble(r, p); return;                              /* P == Q         */
    }

    bignum HH, HHH, U1HH, twoU1HH, s1hhh, d, nx, ny, nz;
    fsub(&H, &U2, &U1);                       /* H = U2 - U1                   */
    fsub(&RR, &S2, &S1);                      /* R = S2 - S1                   */
    fsqr(&HH, &H); fmul(&HHH, &H, &HH);       /* HH = H^2, HHH = H^3           */
    fmul(&U1HH, &U1, &HH);                    /* U1HH = U1*H^2                 */

    fsqr(&nx, &RR); fsub(&nx, &nx, &HHH);     /* nx = R^2 - H^3                */
    fadd(&twoU1HH, &U1HH, &U1HH); fsub(&nx, &nx, &twoU1HH); /* - 2*U1*H^2     */

    fsub(&d, &U1HH, &nx); fmul(&ny, &RR, &d); /* ny = R*(U1H^2 - nx)           */
    fmul(&s1hhh, &S1, &HHH); fsub(&ny, &ny, &s1hhh); /* - S1*H^3              */

    fmul(&t, &p->Z, &q->Z); fmul(&nz, &t, &H);/* nz = Z1*Z2*H                  */

    bn_copy(&r->X, &nx); bn_copy(&r->Y, &ny); bn_copy(&r->Z, &nz); r->inf = 0;
}

/* bit i of scalar k (k stored little-endian in bignum words). */
static int kbit(const bignum *k, int i)
{
    int wi = i >> 5, bi = i & 31;
    if (wi >= k->n) return 0;
    return (int)((k->w[wi] >> bi) & 1u);
}

/* r = k*p via left-to-right double-and-add. */
static void jscalar(jpt *r, const bignum *k, const jpt *p)
{
    jpt acc;
    jset_inf(&acc);
    int bits = bn_bit_length(k);
    for (int i = bits - 1; i >= 0; i--) {
        jdouble(&acc, &acc);
        if (kbit(k, i)) jadd(&acc, &acc, p);
    }
    *r = acc;
}

/* ===================================================================== *
 *  ECDSA verification.                                                    *
 * ===================================================================== */
int p384_ecdsa_verify(const unsigned char pub97[97],
                      const unsigned char *hash, unsigned long hlen,
                      const unsigned char r[48], const unsigned char s[48])
{
    ensure_init();

    bignum rr, ss, e, w, u1, u2;
    bn_from_bytes(&rr, r, 48);
    bn_from_bytes(&ss, s, 48);

    /* 1 <= r,s <= n-1 */
    if (bn_is_zero(&rr) || bn_is_zero(&ss))   return -1;
    if (bn_cmp(&rr, &N) >= 0 || bn_cmp(&ss, &N) >= 0) return -1;

    /* Parse and validate the public key Q. */
    if (pub97[0] != 0x04) return -1;
    jpt Q;
    bn_from_bytes(&Q.X, pub97 + 1,  48);
    bn_from_bytes(&Q.Y, pub97 + 49, 48);
    bn_set_u32(&Q.Z, 1);
    Q.inf = 0;
    if (bn_cmp(&Q.X, &P) >= 0 || bn_cmp(&Q.Y, &P) >= 0) return -1; /* coords < p */
    if (bn_is_zero(&Q.Y) && bn_is_zero(&Q.X))           return -1;
    {   /* on-curve: Y^2 == X^3 + A*X + B (mod P) */
        bignum lhs, rhs, x2, x3, ax;
        fsqr(&lhs, &Q.Y);
        fsqr(&x2, &Q.X); fmul(&x3, &x2, &Q.X);
        fmul(&ax, &A, &Q.X);
        fadd(&rhs, &x3, &ax); fadd(&rhs, &rhs, &B);
        if (bn_cmp(&lhs, &rhs) != 0) return -1;
    }

    /* e = bits2int(hash): leftmost 384 bits (= first 48 bytes), then mod n. */
    {
        unsigned long hn = (hlen > 48) ? 48 : hlen;
        bn_from_bytes(&e, hash, hn);
        bn_mod(&e, &e, &N);
    }

    /* w = s^-1 mod n  (Fermat: s^(n-2), n is prime). */
    {
        bignum nm2, two;
        bn_set_u32(&two, 2);
        bn_mod_sub(&nm2, &N, &two, &N);   /* n - 2 */
        bn_mod_exp(&w, &ss, &nm2, &N);
    }

    bn_mod_mul(&u1, &e,  &w, &N);
    bn_mod_mul(&u2, &rr, &w, &N);

    /* R = u1*G + u2*Q */
    jpt G, R1, R2, Rsum;
    bn_copy(&G.X, &GX); bn_copy(&G.Y, &GY); bn_set_u32(&G.Z, 1); G.inf = 0;
    jscalar(&R1, &u1, &G);
    jscalar(&R2, &u2, &Q);
    jadd(&Rsum, &R1, &R2);
    if (Rsum.inf) return -1;

    /* v = (R.x in affine) mod n;  valid iff v == r. */
    {
        bignum z2, zinv2, xa, v;
        fsqr(&z2, &Rsum.Z);
        finv(&zinv2, &z2);
        fmul(&xa, &Rsum.X, &zinv2);
        bn_mod(&v, &xa, &N);
        if (bn_cmp(&v, &rr) == 0) return 0;
    }
    return -1;
}

/* ===================================================================== *
 *  Built-in KAT: RFC 6979 A.2.6, message "sample", SHA-384.              *
 * ===================================================================== */
int p384_selftest(void)
{
    /* Public key Q (Ux, Uy) from RFC 6979 A.2.6. */
    static const char QX_HEX[] = "ec3a4e415b4e19a4568618029f427fa5da9a8bc4ae92e02e06aae5286b300c64def8f0ea9055866064a254515480bc13";
    static const char QY_HEX[] = "8015d9b72d7d57244ea8ef9ac0c621896708a59367f9dfb9f54ca84b3f1c9db1288b231c3ae0d4fe7344fd2533264720";
    static const char R_HEX[]  = "94edbb92a5ecb8aad4736e56c691916b3f88140666ce9fa73d64c4ea95ad133c81a648152e44acf96e36dd1e80fabe46";
    static const char S_HEX[]  = "99ef4aeb15f178cea1fe40db2603138f130e740a19624526203b6351d0a3a94fa329c145786e679e7b82c71a38628ac8";

    unsigned char pub[97], r[48], s[48], h[48];
    pub[0] = 0x04;
    for (int i = 0; i < 48; i++) {
        pub[1 + i]  = (unsigned char)((hexval(QX_HEX[2*i]) << 4) | hexval(QX_HEX[2*i+1]));
        pub[49 + i] = (unsigned char)((hexval(QY_HEX[2*i]) << 4) | hexval(QY_HEX[2*i+1]));
        r[i]        = (unsigned char)((hexval(R_HEX[2*i])  << 4) | hexval(R_HEX[2*i+1]));
        s[i]        = (unsigned char)((hexval(S_HEX[2*i])  << 4) | hexval(S_HEX[2*i+1]));
    }

    /* hash = SHA-384("sample") */
    static const unsigned char msg[6] = { 's','a','m','p','l','e' };
    sha384(msg, 6, h);

    /* Valid signature must verify. */
    if (p384_ecdsa_verify(pub, h, 48, r, s) != 0) return -1;

    /* A tampered r must be rejected. */
    r[47] ^= 0x01;
    if (p384_ecdsa_verify(pub, h, 48, r, s) == 0) return -1;

    return 0;
}

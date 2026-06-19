/*
 * tls.c -- TLS 1.2 client (RFC 5246/5288/7905), freestanding ring-3 userspace.
 * ==========================================================================
 *
 * See tls.h for the public API, the implemented cipher suites, and -- most
 * importantly -- the SECURITY CAVEATS (chain validation is a separate module).
 *
 * Implementation notes / honest maturity statement
 * ------------------------------------------------
 *   - This client now negotiates MODERN suites: ECDHE (x25519 / secp256r1) key
 *     exchange with AEAD record protection (AES-128/256-GCM and
 *     ChaCha20-Poly1305), authenticated by the server cert's RSA or ECDSA key.
 *     The legacy TLS_RSA_WITH_AES_128_CBC_SHA (0x002F) path is retained only as
 *     a last-resort fallback for ancient servers.
 *   - The ServerKeyExchange signature IS verified against the leaf cert key
 *     (RSA-PKCS1 or ECDSA-P256) over client_random||server_random||params, so
 *     ECDHE parameters cannot be forged for that certificate. Proving the cert
 *     belongs to the host (chain to a trusted root + name match) is delegated
 *     to an OPTIONAL x509_verify_chain() module; absent that, the connection is
 *     flagged encrypted-but-unauthenticated (tls_cert_trusted() == 0).
 *   - It has NOT been fuzzed or run against a wide matrix of servers. Treat any
 *     parsing path as potentially fragile against hostile/malformed inputs.
 *   - NO constant-time discipline: the legacy CBC padding/MAC check is the
 *     classic Lucky-13-class caveat; AEAD tag checks rely on the sibling AEAD
 *     primitives' own verification. Accepted, documented limitation.
 *   - All buffers are fixed/static; we never allocate. Oversized inputs are
 *     rejected with TLS_ERR_BUF rather than truncated silently.
 *
 * Crypto is provided by sibling libraries (compiled separately); we only call:
 *     sha256(), sha256_ctx/init/update/final          (crypto/sha256.h)
 *     sha384(), sha384_ctx/init/update/final          (crypto/sha512.h)
 *     hmac_sha256(), hmac_sha384(), hmac_sha1()        (crypto/hmac.h)
 *     aes_ctx, aes_set_encrypt_key/decrypt_key,
 *       aes_cbc_encrypt/decrypt,
 *       aes_gcm_encrypt/aes_gcm_decrypt                (crypto/aes.h)
 *     chacha20poly1305_encrypt/decrypt                 (crypto/chacha20poly1305.h)
 *     x25519(), x25519_base()                          (crypto/x25519.h)
 *     p256_ecdh(), p256_keygen(), p256_ecdsa_verify()  (crypto/p256.h)
 *     rsa_pubkey, rsa_pubkey_from_bytes,
 *       rsa_pkcs1_encrypt, rsa_pkcs1_verify            (crypto/rsa.h)
 *     x509_extract_pubkey()                            (tls/x509.h)
 *     asn1_* DER reader (to parse SKE sigs / EC SPKI)  (tls/asn1.h)
 *     [optional] x509_verify_chain()                   (tls/x509_verify.h)
 */

#include "tls.h"

#include "../crypto/sha256.h"
#include "../crypto/sha512.h"
#include "../crypto/sha1.h"
#include "../crypto/hmac.h"
#include "../crypto/aes.h"
#include "../crypto/chacha20poly1305.h"
#include "../crypto/x25519.h"
#include "../crypto/p256.h"
#include "../crypto/rsa.h"
#include "x509.h"
#include "asn1.h"

/*
 * OPTIONAL certificate-chain validator (tls/x509_verify.h). When that module is
 * linked in, x509_verify_chain() (returning 0 when the chain is trusted) is
 * called below. We pull in its real 5-argument prototype from the header --
 * critically, the 5th argument is the current time string `now_yyyymmddhhmmss`,
 * which MUST be supplied or the validity-window check sees garbage and rejects
 * every chain (the historical NET-TLS-TRUST-0 bug). We also mark the symbol WEAK
 * here so this object links cleanly even when no validator is provided -- in
 * that case the weak symbol is NULL and we skip the check, leaving the
 * connection flagged encrypted-but-unauthenticated.
 */
#include "x509_verify.h"

__attribute__((weak))
int x509_verify_chain(const unsigned char *const *certs,
                      const unsigned long *lens, int ncerts,
                      const char *hostname,
                      const char *now_yyyymmddhhmmss);

/* ======================================================================== *
 *  Syscall layer
 * ======================================================================== */

#define SYS_YIELD      15
#define SYS_GETTIME    42      /* fill a user rtc_time_t* with wall-clock time */
#define SYS_RANDOM     43
#define SYS_SEND       53
#define SYS_RECV       54
#define SYS_SOCK_POLL  58

#define SC_EAGAIN      (-11)   /* RECV would block: nothing available yet    */

/*
 * 6-arg inline syscall wrapper (n in rax; args in rdi,rsi,rdx,r10,r8).
 * Exactly the wrapper specified for this environment.
 */
static long sc(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 asm("r10") = a4, r8 asm("r8") = a5;
    asm volatile("syscall"
                 : "=a"(r)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                 : "rcx", "r11", "memory");
    return r;
}

static void sys_yield(void)        { sc(SYS_YIELD, 0, 0, 0, 0, 0); }
static void sys_sock_poll(void)    { sc(SYS_SOCK_POLL, 0, 0, 0, 0, 0); }

/* Fill `len` bytes of `buf` with kernel randomness. Returns 0 / negative. */
static int sys_random(void *buf, unsigned long len) {
    long r = sc(SYS_RANDOM, (long)buf, (long)len, 0, 0, 0);
    if (r < 0) return TLS_ERR_CRYPTO;
    /* Some kernels return the count; treat any non-negative as success but
     * insist all bytes were produced when a count is reported. */
    if (r > 0 && (unsigned long)r != len) return TLS_ERR_CRYPTO;
    return 0;
}

/*
 * Broken-down wall-clock time as filled by SYS_GETTIME. The kernel's
 * rtc_time_t (kernel/include/rtc.h) is: uint16_t year; uint8_t month, day,
 * hour, min, sec. We mirror that byte layout EXACTLY so copy_to_user writes the
 * fields where we read them (year little-endian at offset 0, then five bytes).
 * All fields are natural binary (not BCD) on return.
 */
typedef struct {
    unsigned short year;   /* full 4-digit year, e.g. 2026 */
    unsigned char  month;  /* 1..12 */
    unsigned char  day;    /* 1..31 */
    unsigned char  hour;   /* 0..23 */
    unsigned char  min;    /* 0..59 */
    unsigned char  sec;    /* 0..59 */
} tls_rtc_time_t;

/* Lowest plausible "now" if the RTC reads back a nonsense year (e.g. an
 * unset/zeroed CMOS). Using a fixed lower bound keeps the validity-window
 * checks meaningful (notBefore must be <= now) instead of feeding garbage; it
 * NEVER weakens trust -- a verify failure still leaves cert_trusted == 0. This
 * is a compiled-in floor near the project's current era. */
#define TLS_NOW_FALLBACK "20260101000000"

/*
 * tls_build_now -- write the current UTC time as a 14-char "YYYYMMDDHHMMSS"
 * string plus NUL into out[15], exactly the format check_now_format() in
 * x509_verify.c requires. Reads the RTC via SYS_GETTIME; if that fails or the
 * year is implausible (< 2020), falls back to a sane compiled-in lower bound so
 * validity-window checks still function rather than passing garbage.
 */
static void tls_put2(char *p, unsigned int v) {      /* zero-padded 2 digits */
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
}
static void tls_build_now(char out[15]) {
    tls_rtc_time_t t;
    /* zero the struct (mem_set is declared later in this file) */
    {
        unsigned char *z = (unsigned char *)&t;
        for (unsigned long i = 0; i < sizeof t; i++) z[i] = 0;
    }
    long r = sc(SYS_GETTIME, (long)&t, 0, 0, 0, 0);

    /* SYS_GETTIME returns 0 (ESUCCESS) on success; anything else => fall back.
     * Also reject an implausible year so we never feed a bogus window check. */
    if (r != 0 || t.year < 2020 || t.year > 9999 ||
        t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 ||
        t.hour > 23 || t.min > 59 || t.sec > 59) {
        const char *fb = TLS_NOW_FALLBACK;          /* "YYYYMMDDHHMMSS" + NUL */
        for (int i = 0; i < 15; i++) out[i] = fb[i];
        return;
    }

    /* YYYY */
    out[0] = (char)('0' + (t.year / 1000) % 10);
    out[1] = (char)('0' + (t.year / 100)  % 10);
    out[2] = (char)('0' + (t.year / 10)   % 10);
    out[3] = (char)('0' +  t.year         % 10);
    tls_put2(out + 4,  t.month);
    tls_put2(out + 6,  t.day);
    tls_put2(out + 8,  t.hour);
    tls_put2(out + 10, t.min);
    tls_put2(out + 12, t.sec);
    out[14] = '\0';
}

/* ======================================================================== *
 *  Tiny freestanding helpers (no libc)
 * ======================================================================== */

static void *mem_set(void *d, int v, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)v;
    return d;
}
static void *mem_cpy(void *d, const void *s, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}
/* Returns 0 if the first n bytes are equal, nonzero otherwise. NOT const-time. */
static int mem_cmp(const void *a, const void *b, unsigned long n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (unsigned long i = 0; i < n; i++) if (x[i] != y[i]) return 1;
    return 0;
}
static unsigned long str_len(const char *s) {
    unsigned long n = 0; if (!s) return 0; while (s[n]) n++; return n;
}

/* big-endian helpers */
static void put_u16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v;
}
static void put_u24(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v >> 16); p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)v;
}
static unsigned int get_u16(const unsigned char *p) {
    return ((unsigned int)p[0] << 8) | p[1];
}
static unsigned int get_u24(const unsigned char *p) {
    return ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | p[2];
}

/* ======================================================================== *
 *  TLS constants
 * ======================================================================== */

#define TLS_VERSION_MAJOR 3
#define TLS_VERSION_MINOR 3            /* TLS 1.2 == 0x0303 */
#define TLS_VERSION_BE    0x0303

/* ContentType */
#define CT_CHANGE_CIPHER_SPEC 20
#define CT_ALERT              21
#define CT_HANDSHAKE          22
#define CT_APPLICATION_DATA   23

/* HandshakeType */
#define HS_CLIENT_HELLO        1
#define HS_SERVER_HELLO        2
#define HS_CERTIFICATE        11
#define HS_SERVER_KEY_EXCHANGE 12
#define HS_CERTIFICATE_REQUEST 13
#define HS_SERVER_HELLO_DONE  14
#define HS_CLIENT_KEY_EXCHANGE 16
#define HS_FINISHED           20

/* AlertDescription we care about */
#define AL_CLOSE_NOTIFY        0
#define AL_LEVEL_WARNING       1
#define AL_LEVEL_FATAL         2

/* Cipher suites we advertise / support. */
#define SUITE_ECDHE_ECDSA_AES128_GCM_SHA256  0xC02B
#define SUITE_ECDHE_RSA_AES128_GCM_SHA256    0xC02F
#define SUITE_ECDHE_RSA_AES256_GCM_SHA384    0xC030
#define SUITE_ECDHE_RSA_CHACHA20_SHA256      0xCCA8
#define SUITE_ECDHE_ECDSA_CHACHA20_SHA256    0xCCA9
#define SUITE_RSA_AES128_CBC_SHA             0x002F   /* legacy fallback */

/* Named groups (supported_groups / ECDHE curves). */
#define GROUP_X25519     0x001D
#define GROUP_SECP256R1  0x0017

/* EC curve_type in ServerKeyExchange. */
#define EC_NAMED_CURVE   3

/* SignatureAndHashAlgorithm bytes (hash, signature) per RFC 5246 7.4.1.4.1. */
#define SIGALG_RSA_PKCS1_SHA256  0x0401
#define SIGALG_ECDSA_SHA256      0x0403
#define SIGALG_RSA_PKCS1_SHA384  0x0501
#define SIGALG_RSA_PKCS1_SHA512  0x0601
#define SIGALG_RSA_PKCS1_SHA1    0x0201

/* Hash algorithm IDs (high byte of SignatureAndHashAlgorithm). */
#define HASH_SHA1    2
#define HASH_SHA256  4
#define HASH_SHA384  5
#define HASH_SHA512  6
/* Signature algorithm IDs (low byte). */
#define SIG_RSA      1
#define SIG_ECDSA    3

/* AES-128-CBC-SHA fixed sizes (legacy suite). */
#define AES_BLOCK   16
#define MAC_LEN     20            /* HMAC-SHA1 */
#define CBC_ENC_KEY_LEN 16        /* AES-128 */

/* AEAD record sizes. */
#define AEAD_SALT_LEN     4       /* implicit (fixed) nonce salt from key_block */
#define AEAD_EXPLICIT_LEN 8       /* GCM explicit nonce (on the wire)           */
#define AEAD_NONCE_LEN    12      /* full AEAD nonce                            */
#define AEAD_TAG_LEN      16      /* GCM / Poly1305 tag                         */

/* ======================================================================== *
 *  TLS 1.2 PRF (RFC 5246 section 5): P_SHA256 or P_SHA384 (selected per suite)
 * ======================================================================== */
/*
 * P_hash(secret, seed) = HMAC(secret, A(1)+seed) + HMAC(secret, A(2)+seed)+ ...
 *   A(0) = seed
 *   A(i) = HMAC(secret, A(i-1))
 *
 * PRF(secret, label, seed) = P_hash(secret, label + seed).
 *
 * TLS 1.2 uses P_SHA256 for the SHA256 suites and P_SHA384 for the SHA384
 * suites (RFC 5246 5; RFC 5289 for the ECDHE_*_SHA384 suites). We select the
 * underlying HMAC by `use_sha384`. The HMAC output (and thus A() / block) size
 * is 32 for SHA-256 and 48 for SHA-384.
 *
 * We stream into `out` for `outlen` bytes. All buffers are fixed-size; the
 * label+seed are concatenated into a stack buffer bounded by PRF_SEED_MAX.
 */
#define PRF_SEED_MAX 256              /* label(<=32) + 2*random(64) + slack    */
#define PRF_HMAC_MAX 48               /* HMAC-SHA384 digest size               */

/* One HMAC call selecting SHA-256 (32 B) or SHA-384 (48 B) output. */
static void prf_hmac(int use_sha384,
                     const unsigned char *key, unsigned long klen,
                     const unsigned char *msg, unsigned long mlen,
                     unsigned char *out /* 32 or 48 */) {
    if (use_sha384) hmac_sha384(key, klen, msg, mlen, out);
    else            hmac_sha256(key, klen, msg, mlen, out);
}

static int tls_prf_ex(int use_sha384,
                      const unsigned char *secret, unsigned long secret_len,
                      const char *label,
                      const unsigned char *seed, unsigned long seed_len,
                      unsigned char *out, unsigned long outlen) {
    unsigned long hlen = use_sha384 ? 48u : 32u;
    unsigned long label_len = str_len(label);
    if (label_len + seed_len > PRF_SEED_MAX) return TLS_ERR_BUF;

    /* labelseed = label || seed */
    unsigned char labelseed[PRF_SEED_MAX];
    mem_cpy(labelseed, label, label_len);
    mem_cpy(labelseed + label_len, seed, seed_len);
    unsigned long ls_len = label_len + seed_len;

    /* A(1) = HMAC(secret, A(0)=labelseed) */
    unsigned char a[PRF_HMAC_MAX];
    prf_hmac(use_sha384, secret, secret_len, labelseed, ls_len, a);

    /* tmp = A(i) || labelseed  (input to each HMAC for output block) */
    unsigned char tmp[PRF_HMAC_MAX + PRF_SEED_MAX];
    unsigned long pos = 0;
    while (pos < outlen) {
        mem_cpy(tmp, a, hlen);
        mem_cpy(tmp + hlen, labelseed, ls_len);

        unsigned char block[PRF_HMAC_MAX];
        prf_hmac(use_sha384, secret, secret_len, tmp, hlen + ls_len, block);

        unsigned long take = outlen - pos;
        if (take > hlen) take = hlen;
        mem_cpy(out + pos, block, take);
        pos += take;

        /* A(i+1) = HMAC(secret, A(i)) */
        unsigned char a_next[PRF_HMAC_MAX];
        prf_hmac(use_sha384, secret, secret_len, a, hlen, a_next);
        mem_cpy(a, a_next, hlen);
    }
    return 0;
}

/* Backwards-compatible P_SHA256 wrapper (used by the selftest KAT). */
static int tls_prf(const unsigned char *secret, unsigned long secret_len,
                   const char *label,
                   const unsigned char *seed, unsigned long seed_len,
                   unsigned char *out, unsigned long outlen) {
    return tls_prf_ex(0, secret, secret_len, label, seed, seed_len, out, outlen);
}

/* ======================================================================== *
 *  Handshake transcript hash
 * ======================================================================== */
/*
 * The Finished verify_data is PRF(master, "client finished",
 *   Hash(all handshake messages so far), 12) where Hash is SHA-256 for the
 * SHA256 suites and SHA-384 for the SHA384 suites. We do not know the suite
 * until ServerHello, so we run BOTH a SHA-256 and a SHA-384 context over every
 * handshake message and select the right digest once the suite is negotiated.
 *
 * We must hash EVERY handshake message (the 4-byte type+length header + body),
 * in order, EXCLUDING the record-layer headers and the Finished we are about to
 * send (for the client) / verifying against (for the server, include up to but
 * not the server Finished). We snapshot the running ctx without disturbing it.
 */
typedef struct { sha256_ctx c256; sha384_ctx c384; } transcript_t;

static void tr_init(transcript_t *t) {
    sha256_init(&t->c256);
    sha384_init(&t->c384);
}
static void tr_add(transcript_t *t, const void *d, unsigned long n) {
    sha256_update(&t->c256, d, n);
    sha384_update(&t->c384, d, n);
}
/* Snapshot the current transcript hash WITHOUT disturbing the running ctx.
 * Returns the digest length (32 or 48) for the selected hash. */
static unsigned long tr_snapshot(const transcript_t *t, int use_sha384,
                                 unsigned char out[48]) {
    if (use_sha384) {
        sha384_ctx copy = t->c384;     /* struct copy of streaming state */
        sha384_final(&copy, out);
        return 48;
    }
    sha256_ctx copy = t->c256;
    sha256_final(&copy, out);
    return 32;
}

/* ======================================================================== *
 *  Record send / receive (handles TCP reassembly)
 * ======================================================================== */

/* Raw send of `len` bytes; loops until all sent or error. */
static int raw_send_all(int fd, const unsigned char *buf, unsigned long len) {
    unsigned long sent = 0;
    long guard = 0;
    while (sent < len) {
        long r = sc(SYS_SEND, fd, (long)(buf + sent), (long)(len - sent), 0, 0);
        if (r > 0) { sent += (unsigned long)r; guard = 0; continue; }
        if (r == 0 || r == SC_EAGAIN) {
            /* socket buffer full / not ready: poll, yield, retry (bounded) */
            sys_sock_poll();
            sys_yield();
            if (++guard > 1000000) return TLS_ERR_IO;
            continue;
        }
        return TLS_ERR_IO;
    }
    return 0;
}

/*
 * raw_recv_some -- read at least 1 byte (up to cap) from the socket, blocking
 * cooperatively: poll + recv with a large iteration cap, yielding between dry
 * reads. Returns bytes read (>0), 0 if the peer cleanly closed, or TLS_ERR_IO.
 */
static long raw_recv_some(int fd, unsigned char *buf, unsigned long cap) {
    long guard = 0;
    for (;;) {
        sys_sock_poll();
        long r = sc(SYS_RECV, fd, (long)buf, (long)cap, 0, 0);
        if (r > 0) return r;
        if (r == 0) return 0;                 /* peer closed */
        if (r == SC_EAGAIN) {
            sys_yield();
            if (++guard > 5000000) return TLS_ERR_IO;  /* stalled too long */
            continue;
        }
        return TLS_ERR_IO;                     /* hard error */
    }
}

/*
 * recv_record -- read exactly one whole TLS record into c->rbuf and return its
 * fields. We buffer partial reads in c->rbuf across calls.
 *   *ctype  <- ContentType
 *   *body   <- pointer to record body inside c->rbuf (after 5-byte header)
 *   *blen   <- body length
 * Returns 0 on success, 0-length handling: returns TLS_ERR_IO/CLOSED on close.
 * After processing, the caller must call record_consume() to drop it.
 */
static int recv_record(tls_conn_t *c, unsigned char *ctype,
                       unsigned char **body, unsigned int *blen) {
    /* Ensure we have at least the 5-byte header. */
    while (c->rbuf_len < 5) {
        long got = raw_recv_some(c->fd, c->rbuf + c->rbuf_len,
                                 sizeof(c->rbuf) - c->rbuf_len);
        if (got == 0) return TLS_ERR_CLOSED;
        if (got < 0)  return TLS_ERR_IO;
        c->rbuf_len += (unsigned long)got;
    }

    unsigned int rlen = get_u16(c->rbuf + 3);
    if (rlen > TLS_MAX_PLAINTEXT + TLS_RECORD_OVERHEAD) return TLS_ERR_PROTO;
    unsigned long total = 5u + rlen;
    if (total > sizeof(c->rbuf)) return TLS_ERR_BUF;

    /* Ensure we have the whole record body. */
    while (c->rbuf_len < total) {
        long got = raw_recv_some(c->fd, c->rbuf + c->rbuf_len,
                                 sizeof(c->rbuf) - c->rbuf_len);
        if (got == 0) return TLS_ERR_CLOSED;
        if (got < 0)  return TLS_ERR_IO;
        c->rbuf_len += (unsigned long)got;
    }

    *ctype = c->rbuf[0];
    *body  = c->rbuf + 5;
    *blen  = rlen;
    return 0;
}

/* Drop the first whole record (5 + body_len) from c->rbuf, shifting the rest. */
static void record_consume(tls_conn_t *c, unsigned int body_len) {
    unsigned long total = 5u + body_len;
    if (total >= c->rbuf_len) { c->rbuf_len = 0; return; }
    unsigned long rest = c->rbuf_len - total;
    /* move remaining bytes to the front */
    for (unsigned long i = 0; i < rest; i++) c->rbuf[i] = c->rbuf[total + i];
    c->rbuf_len = rest;
}

/* Send a plaintext (pre-handshake / CCS) record. */
static int send_plain_record(tls_conn_t *c, unsigned char ctype,
                             const unsigned char *body, unsigned int blen) {
    if (blen > TLS_MAX_PLAINTEXT) return TLS_ERR_BUF;
    if ((unsigned long)blen + 5 > sizeof(c->wbuf)) return TLS_ERR_BUF;
    c->wbuf[0] = ctype;
    c->wbuf[1] = TLS_VERSION_MAJOR;
    c->wbuf[2] = TLS_VERSION_MINOR;
    put_u16(c->wbuf + 3, blen);
    mem_cpy(c->wbuf + 5, body, blen);
    return raw_send_all(c->fd, c->wbuf, 5u + blen);
}

/* ======================================================================== *
 *  Record protection -- legacy AES-CBC + HMAC-SHA1 (TLS_RSA_WITH_AES_128_CBC_SHA)
 * ======================================================================== */
/*
 * MAC (RFC 5246 6.2.3.1) = HMAC-SHA1(MAC_key,
 *     seq_num(8) || type(1) || version(2) || length(2) || fragment )
 * MAC-then-encrypt: compute MAC over the plaintext, append, PKCS#7-pad to a
 * multiple of the block size, then AES-CBC encrypt (plaintext-MAC-pad) with a
 * fresh per-record explicit IV (which is prepended to the ciphertext).
 */
static void compute_mac(const unsigned char mac_key[MAC_LEN],
                        unsigned long long seq,
                        unsigned char ctype, unsigned int len,
                        const unsigned char *frag,
                        unsigned char out[MAC_LEN]) {
    /* Build the MAC'd header: 8-byte seq + 1 type + 2 version + 2 length. */
    unsigned char hdr[13];
    for (int i = 0; i < 8; i++) hdr[i] = (unsigned char)(seq >> (8 * (7 - i)));
    hdr[8]  = ctype;
    hdr[9]  = TLS_VERSION_MAJOR;
    hdr[10] = TLS_VERSION_MINOR;
    put_u16(hdr + 11, len);

    /* HMAC-SHA1 over hdr || frag. The sibling hmac_sha1 is one-shot, so we
     * stage hdr+frag into a bounded scratch. frag <= TLS_MAX_PLAINTEXT. */
    static unsigned char macbuf[13 + TLS_MAX_PLAINTEXT];
    mem_cpy(macbuf, hdr, 13);
    mem_cpy(macbuf + 13, frag, len);
    hmac_sha1(mac_key, MAC_LEN, macbuf, 13u + len, out);
}

/*
 * cbc_encrypt_and_send -- MAC-then-encrypt `plain` (plen bytes) of content type
 * `ctype` and send it. Bumps write_seq. Layout produced:
 *   explicit_IV(16) || AES-CBC( plaintext || MAC(20) || PKCS#7 pad )
 */
static int cbc_encrypt_and_send(tls_conn_t *c, unsigned char ctype,
                                const unsigned char *plain, unsigned int plen) {
    if (plen > TLS_MAX_PLAINTEXT) return TLS_ERR_BUF;

    /* 1. MAC over the plaintext (with the current write sequence number). */
    unsigned char mac[MAC_LEN];
    compute_mac(c->client_mac_key, c->write_seq, ctype, plen, plain, mac);

    /* 2. Assemble plaintext || MAC, then PKCS#7 pad to AES block multiple. */
    unsigned long body = (unsigned long)plen + MAC_LEN;
    unsigned long pad = AES_BLOCK - (body % AES_BLOCK);   /* 1..16 */
    unsigned long total = body + pad;                      /* multiple of 16 */
    if (5 + AES_BLOCK + total > sizeof(c->wbuf)) return TLS_ERR_BUF;

    /* Build into a scratch (the part that gets encrypted). */
    static unsigned char clear[TLS_MAX_PLAINTEXT + MAC_LEN + AES_BLOCK];
    mem_cpy(clear, plain, plen);
    mem_cpy(clear + plen, mac, MAC_LEN);
    for (unsigned long i = 0; i < pad; i++) clear[body + i] = (unsigned char)(pad - 1);

    /* 3. Fresh explicit IV. */
    unsigned char iv[AES_BLOCK];
    if (sys_random(iv, AES_BLOCK) != 0) return TLS_ERR_CRYPTO;

    /* 4. AES-128-CBC encrypt. aes_cbc_encrypt takes (ctx, iv, in, out, nblocks)
     *    and advances the IV internally; we keep our own copy for the record. */
    aes_ctx ek;
    aes_set_encrypt_key(&ek, c->client_enc_key, 128);
    unsigned char iv_work[AES_BLOCK];
    mem_cpy(iv_work, iv, AES_BLOCK);

    static unsigned char ct[TLS_MAX_PLAINTEXT + MAC_LEN + AES_BLOCK];
    aes_cbc_encrypt(&ek, iv_work, clear, ct, total / AES_BLOCK);

    /* 5. Emit record: header || explicit_IV || ciphertext. */
    unsigned long reclen = AES_BLOCK + total;
    c->wbuf[0] = ctype;
    c->wbuf[1] = TLS_VERSION_MAJOR;
    c->wbuf[2] = TLS_VERSION_MINOR;
    put_u16(c->wbuf + 3, (unsigned int)reclen);
    mem_cpy(c->wbuf + 5, iv, AES_BLOCK);
    mem_cpy(c->wbuf + 5 + AES_BLOCK, ct, total);

    int rc = raw_send_all(c->fd, c->wbuf, 5 + reclen);
    if (rc != 0) return rc;
    c->write_seq++;
    return 0;
}

/*
 * cbc_decrypt_record -- decrypt an inbound encrypted record body (explicit IV
 * prefixed) of content type `ctype` into `out` (cap bytes). Verifies the MAC
 * and strips padding. Bumps read_seq. Returns plaintext length (>=0) or a
 * negative TLS_ERR_*.
 *
 * NOTE: padding & MAC checks here are NOT constant-time (documented caveat).
 */
static long cbc_decrypt_record(tls_conn_t *c, unsigned char ctype,
                               const unsigned char *body, unsigned int blen,
                               unsigned char *out, unsigned long cap) {
    if (blen < AES_BLOCK + AES_BLOCK) return TLS_ERR_PROTO;   /* IV + >=1 block */
    if (blen % AES_BLOCK != 0)        return TLS_ERR_PROTO;

    const unsigned char *iv = body;
    const unsigned char *ct = body + AES_BLOCK;
    unsigned long ctlen = blen - AES_BLOCK;

    aes_ctx dk;
    aes_set_decrypt_key(&dk, c->server_enc_key, 128);
    unsigned char iv_work[AES_BLOCK];
    mem_cpy(iv_work, iv, AES_BLOCK);

    static unsigned char clear[TLS_MAX_PLAINTEXT + MAC_LEN + AES_BLOCK];
    if (ctlen > sizeof(clear)) return TLS_ERR_BUF;
    aes_cbc_decrypt(&dk, iv_work, ct, clear, ctlen / AES_BLOCK);

    /* Strip PKCS#7 padding: last byte is pad_len, plus pad_len padding bytes. */
    unsigned int pad = clear[ctlen - 1];
    unsigned long padbytes = (unsigned long)pad + 1;     /* pad value + length byte */
    if (padbytes > ctlen) return TLS_ERR_PROTO;
    /* (not constant-time) verify all padding bytes equal pad */
    for (unsigned long i = 0; i < padbytes; i++)
        if (clear[ctlen - 1 - i] != pad) return TLS_ERR_PROTO;

    if (ctlen < padbytes + MAC_LEN) return TLS_ERR_PROTO;
    unsigned long plen = ctlen - padbytes - MAC_LEN;
    /* A valid TLS record's plaintext fragment is <= TLS_MAX_PLAINTEXT (2^14).
     * Because `clear` (and thus ctlen) is sized larger than TLS_MAX_PLAINTEXT,
     * an oversized/malformed record could yield plen > TLS_MAX_PLAINTEXT, which
     * would overflow compute_mac()'s macbuf (sized 13 + TLS_MAX_PLAINTEXT) and
     * the out buffer's expectations. Reject such records. */
    if (plen > TLS_MAX_PLAINTEXT) return TLS_ERR_PROTO;
    const unsigned char *rx_mac = clear + plen;

    /* Recompute MAC over the recovered plaintext and compare. */
    unsigned char mac[MAC_LEN];
    compute_mac(c->server_mac_key, c->read_seq, ctype, (unsigned int)plen,
                clear, mac);
    if (mem_cmp(mac, rx_mac, MAC_LEN) != 0) return TLS_ERR_VERIFY;

    if (plen > cap) return TLS_ERR_BUF;
    mem_cpy(out, clear, plen);
    c->read_seq++;
    return (long)plen;
}

/* ======================================================================== *
 *  Record protection -- AEAD (AES-GCM RFC 5288, ChaCha20-Poly1305 RFC 7905)
 * ======================================================================== */
/*
 * AEAD additional data (TLS 1.2, RFC 5246 6.2.3.3 / 5288):
 *     AAD = seq_num(8) || type(1) || version(2) || length(2)
 * where `length` is the PLAINTEXT length (not the ciphertext+tag length).
 */
static void aead_aad(unsigned char aad[13], unsigned long long seq,
                     unsigned char ctype, unsigned int plen) {
    for (int i = 0; i < 8; i++) aad[i] = (unsigned char)(seq >> (8 * (7 - i)));
    aad[8]  = ctype;
    aad[9]  = TLS_VERSION_MAJOR;
    aad[10] = TLS_VERSION_MINOR;
    put_u16(aad + 11, plen);
}

/*
 * Build the 12-byte AEAD nonce for direction `salt` (4-byte implicit salt) and
 * record sequence number `seq`.
 *   AES-GCM (RFC 5288):  nonce = salt(4) || explicit_nonce(8). We use the
 *       8-byte sequence number as the explicit nonce (sent on the wire), so
 *       nonce = salt || seq_be64.
 *   ChaCha20-Poly1305 (RFC 7905): the 64-bit record sequence number is
 *       left-padded to 12 bytes (4 zero bytes || seq_be64) and XORed with the
 *       per-direction 12-byte write IV. For TLS 1.2 the key_block produces a
 *       full 12-byte write IV per side (no separate salt / explicit nonce); we
 *       store that 12-byte IV in client_iv/server_iv.
 */
static void gcm_nonce(unsigned char nonce[12], const unsigned char salt[4],
                      unsigned long long seq) {
    mem_cpy(nonce, salt, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (unsigned char)(seq >> (8 * (7 - i)));
}
static void chacha_nonce(unsigned char nonce[12], const unsigned char wiv[12],
                         unsigned long long seq) {
    unsigned char pad[12];
    mem_set(pad, 0, 4);
    for (int i = 0; i < 8; i++) pad[4 + i] = (unsigned char)(seq >> (8 * (7 - i)));
    for (int i = 0; i < 12; i++) nonce[i] = wiv[i] ^ pad[i];
}

/*
 * aead_encrypt_and_send -- protect `plain` (plen) of content type `ctype` and
 * send it. For GCM the on-the-wire record body is:
 *     explicit_nonce(8) || ciphertext(plen) || tag(16)
 * For ChaCha20-Poly1305 (RFC 7905) there is NO explicit nonce on the wire:
 *     ciphertext(plen) || tag(16)
 * Bumps write_seq.
 */
static int aead_encrypt_and_send(tls_conn_t *c, unsigned char ctype,
                                 const unsigned char *plain, unsigned int plen) {
    if (plen > TLS_MAX_PLAINTEXT) return TLS_ERR_BUF;

    unsigned char aad[13];
    aead_aad(aad, c->write_seq, ctype, plen);

    static unsigned char ct[TLS_MAX_PLAINTEXT];
    unsigned char tag[AEAD_TAG_LEN];

    unsigned int explicit_len = c->aead_chacha ? 0u : AEAD_EXPLICIT_LEN;
    unsigned long reclen = (unsigned long)explicit_len + plen + AEAD_TAG_LEN;
    if (5u + reclen > sizeof(c->wbuf)) return TLS_ERR_BUF;

    if (c->aead_chacha) {
        unsigned char nonce[12];
        chacha_nonce(nonce, c->client_iv, c->write_seq);
        if (chacha20poly1305_encrypt(c->client_enc_key, nonce, aad, 13,
                                     plain, plen, ct, tag) != 0)
            return TLS_ERR_CRYPTO;
    } else {
        unsigned char nonce[12];
        gcm_nonce(nonce, c->client_iv, c->write_seq);   /* salt(4) || seq(8) */
        aes_ctx ek;
        aes_set_encrypt_key(&ek, c->client_enc_key,
                            (int)(c->enc_key_len * 8));
        if (aes_gcm_encrypt(&ek, nonce, aad, 13, plain, plen, ct, tag) != 0)
            return TLS_ERR_CRYPTO;
    }

    /* Emit: record header || [explicit_nonce] || ciphertext || tag. */
    c->wbuf[0] = ctype;
    c->wbuf[1] = TLS_VERSION_MAJOR;
    c->wbuf[2] = TLS_VERSION_MINOR;
    put_u16(c->wbuf + 3, (unsigned int)reclen);
    unsigned long o = 5;
    if (!c->aead_chacha) {
        /* explicit nonce on the wire == the 8-byte sequence number */
        for (int i = 0; i < 8; i++)
            c->wbuf[o + i] = (unsigned char)(c->write_seq >> (8 * (7 - i)));
        o += 8;
    }
    mem_cpy(c->wbuf + o, ct, plen);  o += plen;
    mem_cpy(c->wbuf + o, tag, AEAD_TAG_LEN); o += AEAD_TAG_LEN;

    int rc = raw_send_all(c->fd, c->wbuf, o);
    if (rc != 0) return rc;
    c->write_seq++;
    return 0;
}

/*
 * aead_decrypt_record -- authenticate+decrypt an inbound AEAD record body into
 * `out` (cap). Bumps read_seq on success. Returns plaintext length or negative.
 * On tag-verify failure returns TLS_ERR_VERIFY (a fatal decryption error).
 */
static long aead_decrypt_record(tls_conn_t *c, unsigned char ctype,
                                const unsigned char *body, unsigned int blen,
                                unsigned char *out, unsigned long cap) {
    unsigned int explicit_len = c->aead_chacha ? 0u : AEAD_EXPLICIT_LEN;
    if (blen < explicit_len + AEAD_TAG_LEN) return TLS_ERR_PROTO;

    unsigned long ctlen = (unsigned long)blen - explicit_len - AEAD_TAG_LEN;
    if (ctlen > TLS_MAX_PLAINTEXT) return TLS_ERR_BUF;
    if (ctlen > cap) return TLS_ERR_BUF;

    const unsigned char *ct  = body + explicit_len;
    const unsigned char *tag = body + explicit_len + ctlen;

    unsigned char aad[13];
    aead_aad(aad, c->read_seq, ctype, (unsigned int)ctlen);

    if (c->aead_chacha) {
        unsigned char nonce[12];
        chacha_nonce(nonce, c->server_iv, c->read_seq);
        if (chacha20poly1305_decrypt(c->server_enc_key, nonce, aad, 13,
                                     ct, ctlen, tag, out) != 0)
            return TLS_ERR_VERIFY;
    } else {
        /* GCM nonce = server salt(4) || explicit_nonce-from-the-wire(8). */
        unsigned char nonce[12];
        mem_cpy(nonce, c->server_iv, 4);
        mem_cpy(nonce + 4, body, 8);                 /* explicit nonce on wire */
        aes_ctx dk;
        /* aes_gcm uses the encrypt schedule for the CTR/GHASH core. */
        aes_set_encrypt_key(&dk, c->server_enc_key, (int)(c->enc_key_len * 8));
        if (aes_gcm_decrypt(&dk, nonce, aad, 13, ct, ctlen, tag, out) != 0)
            return TLS_ERR_VERIFY;
    }

    c->read_seq++;
    return (long)ctlen;
}

/* ======================================================================== *
 *  Record protection dispatchers (select AEAD vs legacy CBC by suite)
 * ======================================================================== */
static int encrypt_and_send(tls_conn_t *c, unsigned char ctype,
                            const unsigned char *plain, unsigned int plen) {
    if (c->aead) return aead_encrypt_and_send(c, ctype, plain, plen);
    return cbc_encrypt_and_send(c, ctype, plain, plen);
}
static long decrypt_record(tls_conn_t *c, unsigned char ctype,
                           const unsigned char *body, unsigned int blen,
                           unsigned char *out, unsigned long cap) {
    if (c->aead) return aead_decrypt_record(c, ctype, body, blen, out, cap);
    return cbc_decrypt_record(c, ctype, body, blen, out, cap);
}

/* ======================================================================== *
 *  Handshake message builders
 * ======================================================================== */

/*
 * build_client_hello -- write a ClientHello handshake message (type+len+body)
 * into `out`. Returns total bytes written, or negative on error.
 *
 * Layout (handshake body):
 *   client_version(2) = 0x0303
 *   random(32)
 *   session_id_len(1)=0
 *   cipher_suites_len(2) + suites
 *   compression_len(1)=1 + null(1)=0
 *   extensions_len(2) + [ SNI extension ]
 */
static long build_client_hello(tls_conn_t *c, const char *server_name,
                               unsigned char *out, unsigned long cap) {
    unsigned long p = 4;          /* leave room for handshake header */
    if (cap < 256) return TLS_ERR_BUF;

    /* client_version */
    out[p++] = TLS_VERSION_MAJOR;
    out[p++] = TLS_VERSION_MINOR;

    /* client_random: gmt_unix_time(4) is folded into the 32 random bytes here
     * (RFC permits the whole 32 to be random; we do not have a clock). */
    if (sys_random(c->client_random, TLS_RANDOM_LEN) != 0) return TLS_ERR_CRYPTO;
    mem_cpy(out + p, c->client_random, TLS_RANDOM_LEN); p += TLS_RANDOM_LEN;

    /* session_id (empty) */
    out[p++] = 0;

    /* cipher_suites: modern ECDHE+AEAD first, legacy CBC last. */
    static const unsigned short suites[] = {
        SUITE_ECDHE_ECDSA_AES128_GCM_SHA256,   /* 0xC02B */
        SUITE_ECDHE_RSA_AES128_GCM_SHA256,     /* 0xC02F (broadest compat) */
        SUITE_ECDHE_RSA_AES256_GCM_SHA384,     /* 0xC030 */
        SUITE_ECDHE_RSA_CHACHA20_SHA256,       /* 0xCCA8 */
        SUITE_ECDHE_ECDSA_CHACHA20_SHA256,     /* 0xCCA9 */
        SUITE_RSA_AES128_CBC_SHA,              /* 0x002F (last-resort fallback) */
    };
    unsigned int nsuites = (unsigned int)(sizeof(suites) / sizeof(suites[0]));
    put_u16(out + p, (unsigned int)(nsuites * 2)); p += 2; /* suites length     */
    for (unsigned int i = 0; i < nsuites; i++) { put_u16(out + p, suites[i]); p += 2; }

    /* compression methods: null only */
    out[p++] = 1;
    out[p++] = 0;

    /* extensions */
    unsigned long ext_len_pos = p;
    p += 2;                                       /* placeholder for ext total */
    unsigned long ext_start = p;

    /* --- SNI extension (server_name) RFC 6066 --- */
    unsigned long name_len = str_len(server_name);
    if (name_len > 0 && name_len < 0x3FFF) {
        /* extension_type = 0 (server_name) */
        put_u16(out + p, 0); p += 2;
        /* extension_data length = 2 (list len) + 1 (type) + 2 (name len) + name */
        unsigned int extdata = (unsigned int)(2 + 1 + 2 + name_len);
        put_u16(out + p, extdata); p += 2;
        /* ServerNameList length */
        put_u16(out + p, (unsigned int)(1 + 2 + name_len)); p += 2;
        /* name_type = 0 (host_name) */
        out[p++] = 0;
        /* host_name length + bytes */
        put_u16(out + p, (unsigned int)name_len); p += 2;
        mem_cpy(out + p, server_name, name_len); p += name_len;
    }

    /* --- supported_groups (RFC 8422/7919, ext 0x000A): x25519, secp256r1 --- */
    put_u16(out + p, 0x000A); p += 2;             /* extension_type            */
    put_u16(out + p, 2 + 4);  p += 2;             /* ext_data length           */
    put_u16(out + p, 4);      p += 2;             /* named_group list length   */
    put_u16(out + p, GROUP_X25519);    p += 2;    /* 0x001D                    */
    put_u16(out + p, GROUP_SECP256R1); p += 2;    /* 0x0017                    */

    /* --- ec_point_formats (RFC 8422, ext 0x000B): uncompressed(0) --- */
    put_u16(out + p, 0x000B); p += 2;             /* extension_type            */
    put_u16(out + p, 2);      p += 2;             /* ext_data length           */
    out[p++] = 1;                                 /* formats list length       */
    out[p++] = 0;                                 /* uncompressed              */

    /* --- signature_algorithms (RFC 5246 7.4.1.4.1, ext 0x000D) --- */
    {
        static const unsigned short sigalgs[] = {
            SIGALG_RSA_PKCS1_SHA256,  /* 0x0401 */
            SIGALG_ECDSA_SHA256,      /* 0x0403 */
            SIGALG_RSA_PKCS1_SHA384,  /* 0x0501 */
            SIGALG_RSA_PKCS1_SHA512,  /* 0x0601 */
        };
        unsigned int n = (unsigned int)(sizeof(sigalgs) / sizeof(sigalgs[0]));
        put_u16(out + p, 0x000D); p += 2;         /* extension_type            */
        put_u16(out + p, (unsigned int)(2 + n * 2)); p += 2; /* ext_data len   */
        put_u16(out + p, (unsigned int)(n * 2));  p += 2;    /* list length     */
        for (unsigned int i = 0; i < n; i++) { put_u16(out + p, sigalgs[i]); p += 2; }
    }

    /* --- renegotiation_info (RFC 5746, ext 0xFF01): empty (no renegotiation) */
    put_u16(out + p, 0xFF01); p += 2;             /* extension_type            */
    put_u16(out + p, 1);      p += 2;             /* ext_data length           */
    out[p++] = 0;                                 /* renegotiated_connection[0]*/

    unsigned long ext_total = p - ext_start;
    put_u16(out + ext_len_pos, (unsigned int)ext_total);

    /* handshake header: type + 24-bit length */
    unsigned long body_len = p - 4;
    out[0] = HS_CLIENT_HELLO;
    put_u24(out + 1, (unsigned int)body_len);
    return (long)p;
}

/* ======================================================================== *
 *  Handshake reader: collect handshake messages, dispatching by type.
 * ======================================================================== */
/*
 * The handshake messages (ServerHello, Certificate, ...) arrive inside
 * CT_HANDSHAKE records and may be split across records OR several may share a
 * record. We pull records and walk a handshake-message accumulator. To keep
 * this minimal we copy handshake bytes into a dedicated buffer and parse whole
 * messages out of it.
 */
typedef struct {
    tls_conn_t   *c;
    transcript_t *tr;
    unsigned char hs[TLS_REC_BUF];   /* accumulated handshake bytes */
    unsigned long hs_len;
    unsigned long hs_off;            /* parse cursor */
    int got_ccs;                     /* server ChangeCipherSpec seen */
} hs_reader_t;

/* Pull one more CT_HANDSHAKE (or CCS / alert) record and append handshake
 * bytes to the accumulator. Returns 0 on progress, negative on error. */
static int hs_pump(hs_reader_t *h) {
    unsigned char ctype; unsigned char *body; unsigned int blen;
    int rc = recv_record(h->c, &ctype, &body, &blen);
    if (rc != 0) return rc;

    if (ctype == CT_ALERT) {
        /* alert: level(1) desc(1). Treat fatal as error, close_notify as EOF. */
        int desc = (blen >= 2) ? body[1] : -1;
        record_consume(h->c, blen);
        if (desc == AL_CLOSE_NOTIFY) return TLS_ERR_CLOSED;
        return TLS_ERR_ALERT;
    }
    if (ctype == CT_CHANGE_CIPHER_SPEC) {
        h->got_ccs = 1;
        record_consume(h->c, blen);
        return 0;
    }
    if (ctype != CT_HANDSHAKE) {
        record_consume(h->c, blen);
        return TLS_ERR_PROTO;
    }
    if (h->hs_len + blen > sizeof(h->hs)) { record_consume(h->c, blen); return TLS_ERR_BUF; }
    mem_cpy(h->hs + h->hs_len, body, blen);
    h->hs_len += blen;
    record_consume(h->c, blen);
    return 0;
}

/*
 * hs_next -- return a pointer to the next complete handshake message in the
 * accumulator (type in *type, body pointer in *msg, body length in *mlen),
 * pumping more records as needed. Also feeds the message (header+body) into the
 * transcript hash. Returns 0 on success.
 */
static int hs_next(hs_reader_t *h, unsigned char *type,
                   unsigned char **msg, unsigned int *mlen) {
    for (;;) {
        unsigned long avail = h->hs_len - h->hs_off;
        if (avail >= 4) {
            unsigned char *p = h->hs + h->hs_off;
            unsigned int len = get_u24(p + 1);
            if (avail >= 4u + len) {
                *type = p[0];
                *msg  = p + 4;
                *mlen = len;
                /* Feed the whole handshake message into the transcript. */
                tr_add(h->tr, p, 4u + len);
                h->hs_off += 4u + len;
                return 0;
            }
        }
        int rc = hs_pump(h);
        if (rc != 0) return rc;
    }
}

/* ======================================================================== *
 *  ServerHello / Certificate parsing
 * ======================================================================== */

/*
 * decode_suite -- set the negotiated-suite property fields on `c` from
 * c->cipher. Returns 0 if the suite is one we support, TLS_ERR_SUITE otherwise.
 */
static int decode_suite(tls_conn_t *c) {
    /* defaults */
    c->kx_ecdhe = 0; c->auth_ecdsa = 0; c->aead = 0;
    c->aead_chacha = 0; c->hash_sha384 = 0; c->enc_key_len = CBC_ENC_KEY_LEN;
    switch (c->cipher) {
    case SUITE_ECDHE_ECDSA_AES128_GCM_SHA256:
        c->kx_ecdhe = 1; c->auth_ecdsa = 1; c->aead = 1;
        c->enc_key_len = 16; c->hash_sha384 = 0; return 0;
    case SUITE_ECDHE_RSA_AES128_GCM_SHA256:
        c->kx_ecdhe = 1; c->auth_ecdsa = 0; c->aead = 1;
        c->enc_key_len = 16; c->hash_sha384 = 0; return 0;
    case SUITE_ECDHE_RSA_AES256_GCM_SHA384:
        c->kx_ecdhe = 1; c->auth_ecdsa = 0; c->aead = 1;
        c->enc_key_len = 32; c->hash_sha384 = 1; return 0;
    case SUITE_ECDHE_RSA_CHACHA20_SHA256:
        c->kx_ecdhe = 1; c->auth_ecdsa = 0; c->aead = 1; c->aead_chacha = 1;
        c->enc_key_len = 32; c->hash_sha384 = 0; return 0;
    case SUITE_ECDHE_ECDSA_CHACHA20_SHA256:
        c->kx_ecdhe = 1; c->auth_ecdsa = 1; c->aead = 1; c->aead_chacha = 1;
        c->enc_key_len = 32; c->hash_sha384 = 0; return 0;
    case SUITE_RSA_AES128_CBC_SHA:
        /* legacy: static RSA key transport, CBC+HMAC-SHA1, SHA-256 PRF. */
        c->kx_ecdhe = 0; c->auth_ecdsa = 0; c->aead = 0;
        c->enc_key_len = CBC_ENC_KEY_LEN; c->hash_sha384 = 0; return 0;
    default:
        return TLS_ERR_SUITE;
    }
}

/* Parse ServerHello body; capture server_random and chosen cipher. */
static int parse_server_hello(tls_conn_t *c, const unsigned char *b, unsigned int n) {
    if (n < 2 + 32 + 1) return TLS_ERR_PROTO;
    /* server_version (2) -- must be 0x0303 (we are TLS 1.2 only). */
    unsigned int ver = get_u16(b);
    if (ver != TLS_VERSION_BE) return TLS_ERR_PROTO;
    unsigned long p = 2;
    mem_cpy(c->server_random, b + p, 32); p += 32;
    /* session_id */
    unsigned int sid = b[p++];
    if (p + sid + 2 > n) return TLS_ERR_PROTO;
    p += sid;
    /* cipher_suite (2) */
    c->cipher = (unsigned short)get_u16(b + p); p += 2;
    /* compression_method (1) -- must be null; extensions tail ignored. */
    if (p >= n || b[p] != 0) return TLS_ERR_PROTO;
    /* The chosen suite must be one we advertised / support. */
    return decode_suite(c);
}

/*
 * ec_spki_extract_p256 -- pull an uncompressed secp256r1 public point (65 bytes,
 * leading 0x04) out of a DER X.509 certificate's subjectPublicKeyInfo. The
 * sibling x509 module only extracts RSA keys, so for EC (ECDSA) certs we walk
 * the cert with the asn1.h reader ourselves and copy the BIT STRING payload of
 * the SPKI (which, for an EC key, IS the uncompressed point). We do NOT verify
 * the OID curve here beyond requiring a 65-byte 0x04-prefixed point; mismatched
 * curves will simply fail signature verification later.
 *
 *   Certificate ::= SEQ { tbsCertificate SEQ {
 *       [0] version?, serial INT, sigAlg SEQ, issuer SEQ, validity SEQ,
 *       subject SEQ, SubjectPublicKeyInfo SEQ { algId SEQ, subjectPublicKey BIT } } ... }
 *
 * Returns 0 and writes out[0..64] on success; nonzero otherwise.
 */
static int ec_spki_extract_p256(const unsigned char *der, unsigned long len,
                                unsigned char out[65]) {
    asn1_cur top = { der, der + len };
    asn1_cur cert, tbs;
    if (asn1_enter(&top, ASN1_SEQUENCE, &cert)) return -1;     /* Certificate */
    if (asn1_enter(&cert, ASN1_SEQUENCE, &tbs))  return -1;    /* tbsCertificate */

    /* optional [0] EXPLICIT version */
    int tag; const unsigned char *v; unsigned long vl;
    if (asn1_peek_tlv(&tbs, &tag, &v, &vl) == 0 &&
        tag == ASN1_CONTEXT_CONSTRUCTED(0)) {
        if (asn1_skip(&tbs)) return -1;          /* skip version */
    }
    if (asn1_skip(&tbs)) return -1;              /* serialNumber */
    if (asn1_skip(&tbs)) return -1;              /* signature AlgId */
    if (asn1_skip(&tbs)) return -1;              /* issuer */
    if (asn1_skip(&tbs)) return -1;              /* validity */
    if (asn1_skip(&tbs)) return -1;              /* subject */

    asn1_cur spki, algid;
    if (asn1_enter(&tbs, ASN1_SEQUENCE, &spki))  return -1;    /* SPKI */
    if (asn1_enter(&spki, ASN1_SEQUENCE, &algid)) return -1;   /* AlgorithmIdentifier */
    /* subjectPublicKey BIT STRING -- payload after the unused-bits octet. */
    const unsigned char *bits; unsigned long bits_len; int unused = 0;
    if (asn1_get_bitstring(&spki, &bits, &bits_len, &unused)) return -1;
    if (unused != 0) return -1;
    if (bits_len != 65 || bits[0] != 0x04) return -1;          /* uncompressed P-256 */
    mem_cpy(out, bits, 65);
    return 0;
}

/*
 * parse_certificate -- parse the WHOLE server Certificate handshake message,
 * collecting every certificate DER (leaf first, then intermediates, in the
 * leaf->root order the server sent) into c->chain_der[]/chain_len[], and
 * extract the leaf's public key. For an RSA leaf we pull modulus+exponent via
 * x509_extract_pubkey(); for an EC leaf we pull the uncompressed P-256 point.
 *
 * Certificate body layout (RFC 5246 7.4.2):
 *   certificate_list_len(3)
 *   [ cert_len(3) cert_der(cert_len) ]...
 *
 * Every length is bounds-checked against the message buffer: this is
 * attacker-controlled network data, so a malformed/oversized field must yield
 * TLS_ERR_PROTO, never an out-of-bounds read. The full chain is passed to
 * x509_verify_chain() in tls_client_connect(); without the intermediates almost
 * no real (leaf->intermediate->root) site would validate.
 */
static int parse_certificate(tls_conn_t *c, const unsigned char *b, unsigned int n) {
    if (n < 3) return TLS_ERR_PROTO;
    unsigned int list_len = get_u24(b);
    if (3u + (unsigned long)list_len > n) return TLS_ERR_PROTO;
    if (list_len < 3) return TLS_ERR_PROTO;

    /* Walk the certificate_list, collecting up to TLS_MAX_CHAIN entries. The
     * cursor `off` is relative to the start of the list (b + 3); `end` bounds
     * it. All arithmetic is done in unsigned long to avoid 32-bit overflow. */
    c->chain_count = 0;
    const unsigned char *list = b + 3;
    unsigned long end = list_len;        /* bytes available in the list region */
    unsigned long off = 0;
    while (off + 3u <= end) {
        unsigned int clen = get_u24(list + off);
        off += 3u;
        if ((unsigned long)clen > end - off) return TLS_ERR_PROTO;  /* OOB guard */
        if (clen == 0) return TLS_ERR_PROTO;
        if (c->chain_count < TLS_MAX_CHAIN) {
            c->chain_der[c->chain_count] = list + off;
            c->chain_len[c->chain_count] = clen;
            c->chain_count++;
        }
        /* Beyond TLS_MAX_CHAIN we stop collecting but keep parsing to validate
         * the structure; an absurdly long chain we simply cap. */
        off += clen;
        if (c->chain_count >= TLS_MAX_CHAIN) break;
    }
    if (c->chain_count == 0) return TLS_ERR_PROTO;

    /* The leaf is the first entry. Keep leaf_der/leaf_der_len working for any
     * code that reads them. */
    const unsigned char *der = c->chain_der[0];
    unsigned int cert_len    = (unsigned int)c->chain_len[0];
    c->leaf_der     = der;
    c->leaf_der_len = cert_len;
    c->srv_ec_pub_len = 0;

    if (c->auth_ecdsa) {
        /* ECDSA cert: extract uncompressed secp256r1 point. */
        if (ec_spki_extract_p256(der, cert_len, c->srv_ec_pub) != 0)
            return TLS_ERR_CERT;
        c->srv_ec_pub_len = 65;
        return 0;
    }

    /* RSA cert (used by the RSA-auth ECDHE suites and the legacy suite). */
    c->srv_mod_len = sizeof(c->srv_mod);
    c->srv_exp_len = sizeof(c->srv_exp);
    int rc = x509_extract_pubkey(der, cert_len,
                                 c->srv_mod, &c->srv_mod_len,
                                 c->srv_exp, &c->srv_exp_len);
    if (rc != 0) return TLS_ERR_CERT;
    if (c->srv_mod_len == 0 || c->srv_mod_len > sizeof(c->srv_mod)) return TLS_ERR_CERT;
    return 0;
}

/* ======================================================================== *
 *  Key derivation
 * ======================================================================== */
/*
 * master_secret = PRF(pre_master, "master secret",
 *                     client_random + server_random, 48)
 * key_block     = PRF(master_secret, "key expansion",
 *                     server_random + client_random, needed)
 *
 * The PRF uses P_SHA384 for the SHA384 suite, P_SHA256 otherwise (selected by
 * c->hash_sha384). key_block partitions (RFC 5246 6.3) depend on the suite:
 *
 *   legacy CBC (0x002F):                          fixed_iv_len = 16, mac = 20
 *     client_MAC[20] server_MAC[20]
 *     client_key[16] server_key[16]
 *     client_IV[16]  server_IV[16]   (explicit-IV CBC ignores these, but they
 *                                     are still produced to keep offsets right)
 *   AES-GCM (RFC 5288):                           fixed_iv_len = 4, no MAC
 *     client_key[16|32] server_key[16|32]
 *     client_salt[4]    server_salt[4]            (the implicit nonce salt)
 *   ChaCha20-Poly1305 (RFC 7905, TLS 1.2):        fixed_iv_len = 12, no MAC
 *     client_key[32] server_key[32]
 *     client_IV[12]  server_IV[12]                (full 12-byte write IV/side)
 */
static int derive_keys(tls_conn_t *c, const unsigned char *premaster,
                       unsigned long pm_len) {
    int sha384 = c->hash_sha384;

    unsigned char seed[64];
    /* master secret seed = client_random || server_random */
    mem_cpy(seed, c->client_random, 32);
    mem_cpy(seed + 32, c->server_random, 32);
    int rc = tls_prf_ex(sha384, premaster, pm_len, "master secret", seed, 64,
                        c->master_secret, TLS_MASTER_LEN);
    if (rc != 0) return rc;

    /* key block seed = server_random || client_random */
    unsigned char kseed[64];
    mem_cpy(kseed, c->server_random, 32);
    mem_cpy(kseed + 32, c->client_random, 32);

    /* Determine the per-suite partition sizes. */
    unsigned int mac_len, key_len, iv_len;
    if (!c->aead) {                       /* legacy CBC */
        mac_len = MAC_LEN; key_len = CBC_ENC_KEY_LEN; iv_len = AES_BLOCK;
    } else if (c->aead_chacha) {          /* ChaCha20-Poly1305: 12-byte IV/side */
        mac_len = 0; key_len = c->enc_key_len; iv_len = AEAD_NONCE_LEN; /* 12 */
    } else {                              /* AES-GCM: 4-byte salt/side */
        mac_len = 0; key_len = c->enc_key_len; iv_len = AEAD_SALT_LEN;  /* 4 */
    }

    unsigned long need = 2u * mac_len + 2u * key_len + 2u * iv_len;
    /* Max possible: 2*20 + 2*32 + 2*16 = 136. */
    unsigned char kb[2 * MAC_LEN + 2 * TLS_ENC_KEY_LEN + 2 * AEAD_NONCE_LEN];
    if (need > sizeof(kb)) return TLS_ERR_BUF;
    rc = tls_prf_ex(sha384, c->master_secret, TLS_MASTER_LEN, "key expansion",
                    kseed, 64, kb, need);
    if (rc != 0) return rc;

    unsigned long o = 0;
    if (mac_len) {
        mem_cpy(c->client_mac_key, kb + o, mac_len); o += mac_len;
        mem_cpy(c->server_mac_key, kb + o, mac_len); o += mac_len;
    }
    mem_cpy(c->client_enc_key, kb + o, key_len); o += key_len;
    mem_cpy(c->server_enc_key, kb + o, key_len); o += key_len;
    /* IV/salt: for GCM these are the 4-byte salts; for ChaCha the 12-byte IVs;
     * for CBC unused but stored. They live in client_iv/server_iv (16 bytes). */
    mem_cpy(c->client_iv, kb + o, iv_len); o += iv_len;
    mem_cpy(c->server_iv, kb + o, iv_len); o += iv_len;
    return 0;
}

/* ======================================================================== *
 *  Finished
 * ======================================================================== */
/*
 * verify_data = PRF(master_secret, finished_label,
 *                   Hash(handshake_messages), 12)
 *   Hash = SHA-384 for the SHA384 suite, SHA-256 otherwise (selected by the
 *   negotiated suite). client label = "client finished", server = "server
 *   finished". `th_len` is 32 or 48.
 */
static int compute_verify_data(tls_conn_t *c, const char *label,
                               const unsigned char *transcript_hash,
                               unsigned long th_len,
                               unsigned char out[12]) {
    return tls_prf_ex(c->hash_sha384, c->master_secret, TLS_MASTER_LEN, label,
                      transcript_hash, th_len, out, 12);
}

/* ======================================================================== *
 *  ServerKeyExchange (ECDHE) parsing + signature verification
 * ======================================================================== */
/*
 * verify_ske_signature -- verify the ServerKeyExchange signature over
 *     client_random(32) || server_random(32) || ServerECDHParams
 * using the server certificate's public key. `params` points at the
 * ServerECDHParams bytes (curve_type .. EC public point, length-prefixed) and
 * `params_len` is their length. `sigalg` is the 2-byte SignatureAndHashAlgorithm
 * from the wire (RFC 5246 7.4.3). The hashed message is H(randoms || params)
 * with H chosen by sigalg's hash byte.
 *
 * Returns 0 if the signature verifies; TLS_ERR_SIG otherwise.
 */
static int verify_ske_signature(tls_conn_t *c,
                                const unsigned char *params, unsigned long params_len,
                                unsigned int sigalg,
                                const unsigned char *sig, unsigned long sig_len) {
    unsigned int hash_id = (sigalg >> 8) & 0xFF;
    unsigned int sig_id  = sigalg & 0xFF;

    /* Build the signed message = client_random || server_random || params. */
    static unsigned char signed_msg[64 + 512];
    if (64u + params_len > sizeof(signed_msg)) return TLS_ERR_BUF;
    mem_cpy(signed_msg, c->client_random, 32);
    mem_cpy(signed_msg + 32, c->server_random, 32);
    mem_cpy(signed_msg + 64, params, params_len);
    unsigned long sm_len = 64u + params_len;

    /* Hash the signed message with the algorithm the server declared. */
    unsigned char hash[64];
    unsigned long hlen;
    switch (hash_id) {
    case HASH_SHA256: sha256(signed_msg, sm_len, hash); hlen = 32; break;
    case HASH_SHA384: sha384(signed_msg, sm_len, hash); hlen = 48; break;
    case HASH_SHA512:
        /* sha512.h provides sha512(); reuse the same digest buffer. */
        sha512(signed_msg, sm_len, hash); hlen = 64; break;
    case HASH_SHA1: sha1(signed_msg, sm_len, hash); hlen = 20; break;
    default: return TLS_ERR_SIG;     /* unknown hash */
    }

    if (sig_id == SIG_RSA) {
        if (c->srv_mod_len == 0) return TLS_ERR_SIG;   /* not an RSA cert */
        rsa_pubkey pk;
        rsa_pubkey_from_bytes(&pk, c->srv_mod, c->srv_mod_len,
                              c->srv_exp, c->srv_exp_len);
        int alg = (hash_id == HASH_SHA1) ? RSA_HASH_SHA1 : RSA_HASH_SHA256;
        /* The sibling rsa_pkcs1_verify only encodes SHA-1 / SHA-256 DigestInfo.
         * For SHA-384/512 RSA we cannot construct the right DigestInfo, so we
         * conservatively refuse rather than accept an unverifiable signature. */
        if (hash_id != HASH_SHA1 && hash_id != HASH_SHA256) return TLS_ERR_SIG;
        if (rsa_pkcs1_verify(&pk, sig, sig_len, hash, hlen, alg) != 0)
            return TLS_ERR_SIG;
        return 0;
    }

    if (sig_id == SIG_ECDSA) {
        if (c->srv_ec_pub_len != 65) return TLS_ERR_SIG;  /* not a P-256 cert */
        /* ECDSA signature is DER SEQUENCE { r INTEGER, s INTEGER }. Extract the
         * two 32-byte big-endian scalars (left-pad / strip sign byte). */
        unsigned char r[32], s[32];
        asn1_cur top = { sig, sig + sig_len };
        asn1_cur seq;
        if (asn1_enter(&top, ASN1_SEQUENCE, &seq)) return TLS_ERR_SIG;
        const unsigned char *rv, *sv; unsigned long rl, sl;
        if (asn1_get_integer(&seq, &rv, &rl)) return TLS_ERR_SIG;
        if (asn1_get_integer(&seq, &sv, &sl)) return TLS_ERR_SIG;
        /* strip leading 0x00 sign byte(s) */
        while (rl > 1 && rv[0] == 0) { rv++; rl--; }
        while (sl > 1 && sv[0] == 0) { sv++; sl--; }
        if (rl > 32 || sl > 32 || rl == 0 || sl == 0) return TLS_ERR_SIG;
        mem_set(r, 0, 32); mem_set(s, 0, 32);
        mem_cpy(r + (32 - rl), rv, rl);
        mem_cpy(s + (32 - sl), sv, sl);
        if (p256_ecdsa_verify(c->srv_ec_pub, hash, hlen, r, s) != 0)
            return TLS_ERR_SIG;
        return 0;
    }

    return TLS_ERR_SIG;     /* unknown signature algorithm */
}

/*
 * parse_server_key_exchange -- parse an ECDHE ServerKeyExchange (RFC 4492 5.4):
 *     ECParameters curve_params:
 *         curve_type(1) == named_curve(3)
 *         named_curve(2)
 *     ECPoint public:
 *         length(1) point[length]
 *     SignatureAndHashAlgorithm(2)
 *     signature: length(2) sig[length]
 * Verifies the signature over client_random||server_random||ECDHEParams and, on
 * success, records the server's public point and named group on `c`.
 */
static int parse_server_key_exchange(tls_conn_t *c,
                                     const unsigned char *b, unsigned int n) {
    if (!c->kx_ecdhe) return TLS_ERR_SUITE;      /* SKE only for ECDHE suites */
    unsigned long p = 0;
    if (n < 4) return TLS_ERR_PROTO;

    if (b[p] != EC_NAMED_CURVE) return TLS_ERR_CURVE;
    p += 1;
    unsigned int group = get_u16(b + p); p += 2;
    if (group != GROUP_X25519 && group != GROUP_SECP256R1) return TLS_ERR_CURVE;

    unsigned int point_len = b[p]; p += 1;
    if (p + point_len > n) return TLS_ERR_PROTO;
    /* Validate the point shape per group. */
    if (group == GROUP_X25519) {
        if (point_len != 32) return TLS_ERR_CURVE;
    } else { /* secp256r1 uncompressed */
        if (point_len != 65 || b[p] != 0x04) return TLS_ERR_CURVE;
    }
    const unsigned char *peer = b + p;
    unsigned long params_len = p + point_len;     /* length of ServerECDHParams */
    p += point_len;

    /* SignatureAndHashAlgorithm + signature. */
    if (p + 2 + 2 > n) return TLS_ERR_PROTO;
    unsigned int sigalg = get_u16(b + p); p += 2;
    unsigned int sig_len = get_u16(b + p); p += 2;
    if (p + sig_len > n) return TLS_ERR_PROTO;
    const unsigned char *sig = b + p;

    int rc = verify_ske_signature(c, b, params_len, sigalg, sig, sig_len);
    if (rc != 0) return rc;                        /* abort on bad signature */

    /* Record verified ECDHE parameters. */
    c->named_group  = (unsigned short)group;
    c->peer_pub_len = point_len;
    mem_cpy(c->peer_pub, peer, point_len);
    return 0;
}

/*
 * ecdhe_compute -- generate our ephemeral key for the negotiated group, compute
 * the shared secret with the server's public point, and emit the
 * ClientKeyExchange (our public point, length-prefixed). The premaster secret
 * IS the raw shared secret (32 bytes for x25519 and the P-256 X-coordinate).
 *
 * `pms` receives the premaster; `*pms_len` is set to 32.
 */
static int ecdhe_compute(tls_conn_t *c, transcript_t *tr,
                         unsigned char *pms, unsigned long *pms_len) {
    if (sys_random(c->ecdhe_priv, 32) != 0) return TLS_ERR_CRYPTO;

    if (c->named_group == GROUP_X25519) {
        /* clamp + base mult handled by x25519_base(); derive public + shared. */
        x25519_base(c->ecdhe_pub, c->ecdhe_priv);
        c->ecdhe_pub_len = 32;
        if (c->peer_pub_len != 32) return TLS_ERR_CURVE;
        x25519(pms, c->ecdhe_priv, c->peer_pub);
        /* x25519 of a low-order point yields all-zero; reject (RFC 7748 6.1). */
        int allzero = 1;
        for (int i = 0; i < 32; i++) if (pms[i]) { allzero = 0; break; }
        if (allzero) return TLS_ERR_CRYPTO;
        *pms_len = 32;
    } else { /* secp256r1 */
        /* p256_keygen() produces a fresh private/public pair; it may ignore our
         * pre-filled ecdhe_priv and generate its own. Use its outputs. */
        if (p256_keygen(c->ecdhe_priv, c->ecdhe_pub) != 0) return TLS_ERR_CRYPTO;
        c->ecdhe_pub_len = 65;
        if (c->peer_pub_len != 65) return TLS_ERR_CURVE;
        if (p256_ecdh(pms, c->ecdhe_priv, c->peer_pub) != 0) return TLS_ERR_CRYPTO;
        *pms_len = 32;
    }

    /* ClientKeyExchange: ECPoint public = length(1) || point. */
    static unsigned char cke[4 + 1 + 65];
    unsigned long q = 4;
    cke[q++] = (unsigned char)c->ecdhe_pub_len;
    mem_cpy(cke + q, c->ecdhe_pub, c->ecdhe_pub_len); q += c->ecdhe_pub_len;
    unsigned long body_len = q - 4;
    cke[0] = HS_CLIENT_KEY_EXCHANGE;
    put_u24(cke + 1, (unsigned int)body_len);
    tr_add(tr, cke, q);                              /* transcript */
    return send_plain_record(c, CT_HANDSHAKE, cke, (unsigned int)q);
}

/* ======================================================================== *
 *  Handshake driver
 * ======================================================================== */

int tls_client_connect(tls_conn_t *c, int tcp_fd, const char *server_name) {
    /* Fresh state. */
    mem_set(c, 0, sizeof(*c));
    c->fd = tcp_fd;
    c->write_seq = 0;
    c->read_seq  = 0;

    transcript_t tr;
    tr_init(&tr);

    /* ---- 1. ClientHello ---- */
    {
        static unsigned char ch[512];
        long n = build_client_hello(c, server_name, ch, sizeof(ch));
        if (n < 0) return (int)n;
        tr_add(&tr, ch, (unsigned long)n);                /* transcript */
        int rc = send_plain_record(c, CT_HANDSHAKE, ch, (unsigned int)n);
        if (rc != 0) return rc;
    }

    /* ---- 2. Read ServerHello .. ServerHelloDone ---- */
    hs_reader_t H;
    mem_set(&H, 0, sizeof(H));
    H.c = c; H.tr = &tr;

    int seen_sh = 0, seen_cert = 0, seen_ske = 0, seen_done = 0;
    while (!seen_done) {
        unsigned char type; unsigned char *msg; unsigned int mlen;
        int rc = hs_next(&H, &type, &msg, &mlen);
        if (rc != 0) return rc;
        switch (type) {
        case HS_SERVER_HELLO:
            rc = parse_server_hello(c, msg, mlen);
            if (rc != 0) return rc;
            seen_sh = 1;
            break;
        case HS_CERTIFICATE:
            rc = parse_certificate(c, msg, mlen);
            if (rc != 0) return rc;
            seen_cert = 1;
            break;
        case HS_SERVER_KEY_EXCHANGE:
            /* ECDHE suites carry signed ephemeral params here; the legacy RSA
             * key-transport suite must NOT send this message. */
            if (!c->kx_ecdhe) return TLS_ERR_SUITE;
            rc = parse_server_key_exchange(c, msg, mlen);
            if (rc != 0) return rc;        /* aborts on bad signature/curve */
            seen_ske = 1;
            break;
        case HS_CERTIFICATE_REQUEST:
            /* Client-auth requested; we do not support client certs. We will
             * still proceed (server may make it optional) but we cannot send a
             * Certificate message, so most servers will fail. Document & skip. */
            break;
        case HS_SERVER_HELLO_DONE:
            seen_done = 1;
            break;
        default:
            return TLS_ERR_PROTO;
        }
    }
    if (!seen_sh || !seen_cert) return TLS_ERR_PROTO;
    if (c->kx_ecdhe && !seen_ske) return TLS_ERR_PROTO;   /* ECDHE needs SKE */

    /* ---- 2b. OPTIONAL certificate-chain validation ----
     * The ServerKeyExchange signature (verified above for ECDHE) already binds
     * the ephemeral params to the leaf cert's key, but proving the leaf cert
     * actually belongs to `server_name` and chains to a trusted root requires a
     * CA store -- delegated to x509_verify_chain() if that module is linked. We
     * pass the FULL server-sent chain (leaf + intermediates, as collected in
     * parse_certificate) plus the current UTC time, so leaf->intermediate->root
     * validation can succeed for real sites. If the validator is absent (weak
     * symbol NULL) or rejects the chain, we flag the connection
     * encrypted-but-UNAUTHENTICATED and continue -- a verify FAILURE always
     * leaves cert_trusted == 0 (we never default-trust). */
    c->cert_trusted = 0;
    if (x509_verify_chain && c->chain_count > 0) {
        char now[15];
        tls_build_now(now);     /* "YYYYMMDDHHMMSS" + NUL, from the RTC */
        if (x509_verify_chain(c->chain_der, c->chain_len, c->chain_count,
                              server_name, now) == 0)
            c->cert_trusted = 1;
    }
    /* The chain pointers alias the handshake reader's buffer (valid only here);
     * clear them so nothing dereferences a dangling pointer after we return. */
    c->leaf_der = 0; c->leaf_der_len = 0;
    for (int ci = 0; ci < TLS_MAX_CHAIN; ci++) {
        c->chain_der[ci] = 0; c->chain_len[ci] = 0;
    }
    c->chain_count = 0;

    /* ---- 3. ClientKeyExchange + premaster ---- */
    unsigned char premaster[48];
    unsigned long pm_len;

    if (c->kx_ecdhe) {
        /* ECDHE: generate our ephemeral key, compute the shared secret (the
         * premaster), and send our public point as the ClientKeyExchange. */
        int rc = ecdhe_compute(c, &tr, premaster, &pm_len);
        if (rc != 0) return rc;
    } else {
        /* Legacy static-RSA key transport: PMS = client_version || 46 random,
         * RSA-PKCS#1 v1.5 encrypted to the server's certificate public key. */
        premaster[0] = TLS_VERSION_MAJOR;
        premaster[1] = TLS_VERSION_MINOR;
        if (sys_random(premaster + 2, 46) != 0) return TLS_ERR_CRYPTO;
        pm_len = 48;

        rsa_pubkey pk;
        rsa_pubkey_from_bytes(&pk, c->srv_mod, c->srv_mod_len,
                              c->srv_exp, c->srv_exp_len);
        unsigned long k = c->srv_mod_len;
        if (k == 0 || k > 512) return TLS_ERR_CRYPTO;

        unsigned char rnd[1024];
        unsigned long rndlen = 2 * k;
        if (rndlen > sizeof(rnd)) rndlen = sizeof(rnd);
        if (sys_random(rnd, rndlen) != 0) return TLS_ERR_CRYPTO;

        unsigned char enc[512];
        if (rsa_pkcs1_encrypt(&pk, premaster, 48, rnd, rndlen, enc, k) != 0)
            return TLS_ERR_CRYPTO;
        unsigned long enc_len = k;

        static unsigned char cke[4 + 2 + 512];
        unsigned long p = 4;
        put_u16(cke + p, (unsigned int)enc_len); p += 2;
        mem_cpy(cke + p, enc, enc_len); p += enc_len;
        unsigned long body_len = p - 4;
        cke[0] = HS_CLIENT_KEY_EXCHANGE;
        put_u24(cke + 1, (unsigned int)body_len);
        tr_add(&tr, cke, p);                          /* transcript */
        int rc = send_plain_record(c, CT_HANDSHAKE, cke, (unsigned int)p);
        if (rc != 0) return rc;
    }

    /* ---- 4. Derive keys from the premaster ---- */
    {
        int rc = derive_keys(c, premaster, pm_len);
        if (rc != 0) return rc;
    }

    /* ---- 5a. ChangeCipherSpec (plaintext record) ---- */
    {
        unsigned char ccs = 1;
        int rc = send_plain_record(c, CT_CHANGE_CIPHER_SPEC, &ccs, 1);
        if (rc != 0) return rc;
        /* From here on, our outgoing records are encrypted. write_seq stays 0
         * for the FIRST encrypted record (the Finished). */
    }

    /* ---- 5b. Encrypted client Finished ---- */
    {
        unsigned char th[48];
        unsigned long thl = tr_snapshot(&tr, c->hash_sha384, th);  /* up to (not
                                            * incl) client Finished             */
        unsigned char vd[12];
        int rc = compute_verify_data(c, "client finished", th, thl, vd);
        if (rc != 0) return rc;

        /* Finished handshake message: type(20) len(12) verify_data(12) */
        unsigned char fin[4 + 12];
        fin[0] = HS_FINISHED;
        put_u24(fin + 1, 12);
        mem_cpy(fin + 4, vd, 12);
        /* The client Finished IS part of the transcript for the SERVER's
         * Finished verification, so add it now. */
        tr_add(&tr, fin, sizeof(fin));

        rc = encrypt_and_send(c, CT_HANDSHAKE, fin, sizeof(fin));
        if (rc != 0) return rc;
    }

    /* ---- 6. Read server ChangeCipherSpec + encrypted Finished ---- */
    {
        /* Expect: CCS (plaintext), then an encrypted Finished record. */
        /* 6a. CCS */
        int got_ccs = 0;
        long guard = 0;
        while (!got_ccs) {
            unsigned char ctype; unsigned char *body; unsigned int blen;
            int rc = recv_record(c, &ctype, &body, &blen);
            if (rc != 0) return rc;
            if (ctype == CT_CHANGE_CIPHER_SPEC) {
                got_ccs = 1; record_consume(c, blen);
            } else if (ctype == CT_ALERT) {
                int desc = (blen >= 2) ? body[1] : -1;
                record_consume(c, blen);
                return (desc == AL_CLOSE_NOTIFY) ? TLS_ERR_CLOSED : TLS_ERR_ALERT;
            } else if (ctype == CT_HANDSHAKE) {
                /* Stray plaintext handshake before CCS: protocol violation. */
                record_consume(c, blen);
                return TLS_ERR_PROTO;
            } else {
                record_consume(c, blen);
            }
            if (++guard > 1000) return TLS_ERR_PROTO;
        }

        /* 6b. Encrypted Finished record. */
        unsigned char ctype; unsigned char *body; unsigned int blen;
        int rc = recv_record(c, &ctype, &body, &blen);
        if (rc != 0) return rc;
        if (ctype != CT_HANDSHAKE) { record_consume(c, blen); return TLS_ERR_PROTO; }

        static unsigned char fin_pt[64];
        long pl = decrypt_record(c, CT_HANDSHAKE, body, blen,
                                 fin_pt, sizeof(fin_pt));
        record_consume(c, blen);
        if (pl < 0) return (int)pl;
        if (pl < 4 + 12) return TLS_ERR_PROTO;
        if (fin_pt[0] != HS_FINISHED) return TLS_ERR_PROTO;
        unsigned int vlen = get_u24(fin_pt + 1);
        if (vlen != 12) return TLS_ERR_PROTO;

        /* Verify server verify_data over the transcript INCLUDING our Finished
         * but NOT the server Finished. */
        unsigned char th[48];
        unsigned long thl = tr_snapshot(&tr, c->hash_sha384, th);
        unsigned char expect[12];
        rc = compute_verify_data(c, "server finished", th, thl, expect);
        if (rc != 0) return rc;
        if (mem_cmp(expect, fin_pt + 4, 12) != 0) return TLS_ERR_VERIFY;
    }

    c->established = 1;
    return TLS_OK;
}

/* ======================================================================== *
 *  Application data
 * ======================================================================== */

long tls_write(tls_conn_t *c, const void *buf, unsigned long len) {
    if (!c->established || c->closed) return TLS_ERR_CLOSED;
    const unsigned char *p = (const unsigned char *)buf;
    unsigned long off = 0;
    while (off < len) {
        unsigned long chunk = len - off;
        if (chunk > TLS_MAX_PLAINTEXT) chunk = TLS_MAX_PLAINTEXT;
        int rc = encrypt_and_send(c, CT_APPLICATION_DATA, p + off,
                                  (unsigned int)chunk);
        if (rc != 0) return rc;
        off += chunk;
    }
    return (long)len;
}

long tls_read(tls_conn_t *c, void *buf, unsigned long cap) {
    if (c->closed) return 0;
    if (!c->established) return -1;

    /* Serve from any leftover decrypted plaintext first. */
    if (c->app_off < c->app_len) {
        unsigned long avail = c->app_len - c->app_off;
        unsigned long take = (avail < cap) ? avail : cap;
        mem_cpy(buf, c->app_buf + c->app_off, take);
        c->app_off += take;
        return (long)take;
    }

    /* Otherwise pull and decrypt the next application-data record. We loop past
     * non-application records (e.g. a server-initiated alert). */
    for (;;) {
        unsigned char ctype; unsigned char *body; unsigned int blen;
        int rc = recv_record(c, &ctype, &body, &blen);
        if (rc == TLS_ERR_CLOSED) { c->closed = 1; return 0; }
        if (rc != 0) return -1;

        if (ctype == CT_ALERT) {
            /* Encrypted alert. Decrypt to inspect (close_notify == clean EOF). */
            static unsigned char ad[64];
            long pl = decrypt_record(c, CT_ALERT, body, blen, ad, sizeof(ad));
            record_consume(c, blen);
            if (pl >= 2 && ad[1] == AL_CLOSE_NOTIFY) { c->closed = 1; return 0; }
            c->closed = 1;
            return -1;
        }
        if (ctype != CT_APPLICATION_DATA) {
            /* Ignore unexpected record types post-handshake (e.g. stray CCS). */
            record_consume(c, blen);
            continue;
        }

        long pl = decrypt_record(c, CT_APPLICATION_DATA, body, blen,
                                 c->app_buf, sizeof(c->app_buf));
        record_consume(c, blen);
        if (pl < 0) return -1;
        c->app_len = (unsigned long)pl;
        c->app_off = 0;
        if (pl == 0) continue;                  /* empty record; keep going */

        unsigned long take = (c->app_len < cap) ? c->app_len : cap;
        mem_cpy(buf, c->app_buf, take);
        c->app_off = take;
        return (long)take;
    }
}

void tls_close(tls_conn_t *c) {
    if (c->established && !c->closed) {
        /* close_notify alert: level=warning(1), desc=close_notify(0). */
        unsigned char alert[2] = { AL_LEVEL_WARNING, AL_CLOSE_NOTIFY };
        (void)encrypt_and_send(c, CT_ALERT, alert, 2);   /* best effort */
    }
    c->closed = 1;
    c->established = 0;
}

/* ======================================================================== *
 *  Certificate-trust status (chain validation result)
 * ======================================================================== */
int tls_cert_trusted(const tls_conn_t *c) {
    return (c && c->cert_trusted) ? 1 : 0;
}

/* ======================================================================== *
 *  Offline self-test: TLS 1.2 PRF (P_SHA256 + P_SHA384) known-answer vectors
 * ======================================================================== */
/*
 * Well-known TLS 1.2 SHA-256 PRF test vector (widely published, e.g. the
 * IETF tls-wg test data and mbedTLS' selftest):
 *
 *   secret = 9b be 43 6b a9 40 f0 17 b1 76 52 84 9a 71 db 35
 *   label  = "test label"
 *   seed   = a0 ba 9f 93 6c da 31 18 27 a6 f7 96 ff d5 19 8c
 *   PRF(secret,label,seed) [100 bytes] =
 *     e3 f2 29 ba 72 7b e1 7b 8d 12 26 20 55 7c d4 53
 *     c2 aa b2 1d 07 c3 d4 95 32 9b 52 d4 e6 1e db 5a
 *     6b 30 17 91 e9 0d 35 c9 c9 a4 6b 4e 14 ba f9 af
 *     0f a0 22 f7 07 7d ef 17 ab fd 37 97 c0 56 4b ab
 *     4f bc 91 66 6e 9d ef 9b 97 fc e3 4f 79 67 89 ba
 *     a4 80 82 d1 22 ee 42 c5 a7 2e 5a 51 10 ff f7 01
 *     87 34 7b 66
 *
 * If this passes, our PRF (and therefore master-secret / key-block / Finished
 * derivation) matches the spec.
 */
int tls_selftest(void) {
    static const unsigned char secret[16] = {
        0x9b,0xbe,0x43,0x6b,0xa9,0x40,0xf0,0x17,
        0xb1,0x76,0x52,0x84,0x9a,0x71,0xdb,0x35
    };
    static const unsigned char seed[16] = {
        0xa0,0xba,0x9f,0x93,0x6c,0xda,0x31,0x18,
        0x27,0xa6,0xf7,0x96,0xff,0xd5,0x19,0x8c
    };
    static const unsigned char expect[100] = {
        0xe3,0xf2,0x29,0xba,0x72,0x7b,0xe1,0x7b,0x8d,0x12,0x26,0x20,0x55,0x7c,0xd4,0x53,
        0xc2,0xaa,0xb2,0x1d,0x07,0xc3,0xd4,0x95,0x32,0x9b,0x52,0xd4,0xe6,0x1e,0xdb,0x5a,
        0x6b,0x30,0x17,0x91,0xe9,0x0d,0x35,0xc9,0xc9,0xa4,0x6b,0x4e,0x14,0xba,0xf9,0xaf,
        0x0f,0xa0,0x22,0xf7,0x07,0x7d,0xef,0x17,0xab,0xfd,0x37,0x97,0xc0,0x56,0x4b,0xab,
        0x4f,0xbc,0x91,0x66,0x6e,0x9d,0xef,0x9b,0x97,0xfc,0xe3,0x4f,0x79,0x67,0x89,0xba,
        0xa4,0x80,0x82,0xd1,0x22,0xee,0x42,0xc5,0xa7,0x2e,0x5a,0x51,0x10,0xff,0xf7,0x01,
        0x87,0x34,0x7b,0x66
    };

    unsigned char out[148];
    int rc = tls_prf(secret, sizeof(secret), "test label",
                     seed, sizeof(seed), out, 100);
    if (rc != 0) return TLS_ERR_CRYPTO;
    if (mem_cmp(out, expect, sizeof(expect)) != 0) return TLS_ERR_VERIFY;

    /*
     * P_SHA384 PRF known-answer vector (the SHA-384 companion to the vector
     * above, from the same widely-published mbedTLS TLS 1.2 PRF test set).
     *
     *   secret = b8 0b 73 3d 6c ee fc dc 71 56 6e a4 8e 55 67 df
     *   label  = "test label"
     *   seed   = cd 66 5c f6 a8 44 7d d6 ff 8b 27 55 5e db 74 65
     *   PRF_SHA384(secret,label,seed) [148 bytes] =
     *     7b 0c 18 e9 ce d4 10 ed 18 04 f2 cf a3 4a 33 6a
     *     1c 14 df fb 49 00 bb 5f d7 94 21 07 e8 1c 83 cd
     *     e9 ca 0f aa 60 be 9f e3 4f 82 b1 23 3c 91 46 a0
     *     e5 34 cb 40 0f ed 27 00 88 4f 9d c2 36 f8 0e dd
     *     8b fa 96 11 44 c9 e8 d7 92 ec a7 22 a7 b3 2f c3
     *     d4 16 d4 73 eb c2 c5 fd 4a bf da d0 5d 91 84 25
     *     9b 5b f8 cd 4d 90 fa 0d 31 e2 de c4 79 e4 f1 a2
     *     60 66 f2 ee a9 a6 92 36 a3 e5 26 55 c9 e9 ae e6
     *     91 c8 f3 a2 68 54 30 8d 5e aa 3b e8 5e 09 90 70
     *     3d 73 e5 6f
     */
    {
        static const unsigned char secret384[16] = {
            0xb8,0x0b,0x73,0x3d,0x6c,0xee,0xfc,0xdc,
            0x71,0x56,0x6e,0xa4,0x8e,0x55,0x67,0xdf
        };
        static const unsigned char seed384[16] = {
            0xcd,0x66,0x5c,0xf6,0xa8,0x44,0x7d,0xd6,
            0xff,0x8b,0x27,0x55,0x5e,0xdb,0x74,0x65
        };
        static const unsigned char expect384[148] = {
            0x7b,0x0c,0x18,0xe9,0xce,0xd4,0x10,0xed,0x18,0x04,0xf2,0xcf,0xa3,0x4a,0x33,0x6a,
            0x1c,0x14,0xdf,0xfb,0x49,0x00,0xbb,0x5f,0xd7,0x94,0x21,0x07,0xe8,0x1c,0x83,0xcd,
            0xe9,0xca,0x0f,0xaa,0x60,0xbe,0x9f,0xe3,0x4f,0x82,0xb1,0x23,0x3c,0x91,0x46,0xa0,
            0xe5,0x34,0xcb,0x40,0x0f,0xed,0x27,0x00,0x88,0x4f,0x9d,0xc2,0x36,0xf8,0x0e,0xdd,
            0x8b,0xfa,0x96,0x11,0x44,0xc9,0xe8,0xd7,0x92,0xec,0xa7,0x22,0xa7,0xb3,0x2f,0xc3,
            0xd4,0x16,0xd4,0x73,0xeb,0xc2,0xc5,0xfd,0x4a,0xbf,0xda,0xd0,0x5d,0x91,0x84,0x25,
            0x9b,0x5b,0xf8,0xcd,0x4d,0x90,0xfa,0x0d,0x31,0xe2,0xde,0xc4,0x79,0xe4,0xf1,0xa2,
            0x60,0x66,0xf2,0xee,0xa9,0xa6,0x92,0x36,0xa3,0xe5,0x26,0x55,0xc9,0xe9,0xae,0xe6,
            0x91,0xc8,0xf3,0xa2,0x68,0x54,0x30,0x8d,0x5e,0xaa,0x3b,0xe8,0x5e,0x09,0x90,0x70,
            0x3d,0x73,0xe5,0x6f
        };
        rc = tls_prf_ex(1 /* SHA-384 */, secret384, sizeof(secret384),
                        "test label", seed384, sizeof(seed384),
                        out, sizeof(expect384));
        if (rc != 0) return TLS_ERR_CRYPTO;
        if (mem_cmp(out, expect384, sizeof(expect384)) != 0) return TLS_ERR_VERIFY;
    }

    return TLS_OK;
}

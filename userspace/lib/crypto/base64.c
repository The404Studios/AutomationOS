/*
 * Base64 Encode/Decode + PEM Block Extraction
 * ============================================
 *
 * Freestanding, pure-computation implementation.
 * No syscalls, no libc, no malloc, no standard headers.
 *
 * Conforms to RFC 4648 §4 (standard) and §5 (URL-safe variant).
 *
 * Build flags:
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 */

#include "base64.h"

/* =========================================================================
 * Internal type aliases (no standard headers)
 * ========================================================================= */

typedef b64_u8   u8;
typedef b64_ulong ulong;
typedef b64_long  slong;

/* =========================================================================
 * Encoding table — standard RFC 4648 Base64 alphabet
 * ========================================================================= */

static const char ENC_TABLE[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* =========================================================================
 * Decoding table
 *
 * Maps an ASCII byte → 6-bit value.
 *   0x80  invalid character (reject)
 *   0x40  whitespace (skip: space, \t, \r, \n)
 *   0x41  padding character '='
 *   0x00..0x3F  valid 6-bit value
 * ========================================================================= */

#define DEC_INV  0x80u   /* invalid */
#define DEC_WS   0x40u   /* whitespace — skip */
#define DEC_PAD  0x41u   /* '=' padding */

static const u8 DEC_TABLE[256] = {
    /* 0x00-0x08 */ DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    /* 0x09 \t  */ DEC_WS,
    /* 0x0A \n  */ DEC_WS,
    /* 0x0B     */ DEC_INV,
    /* 0x0C     */ DEC_INV,
    /* 0x0D \r  */ DEC_WS,
    /* 0x0E-0x1F */ DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
                    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
                    DEC_INV,DEC_INV,
    /* 0x20 ' ' */ DEC_WS,
    /* 0x21-0x2B */ DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    /* 0x2B '+' */ 62,
    /* 0x2C     */ DEC_INV,
    /* 0x2D '-' */ 62,   /* URL-safe alias for '+' */
    /* 0x2E     */ DEC_INV,
    /* 0x2F '/' */ 63,
    /* 0x30-0x39 '0'-'9' */ 52,53,54,55,56,57,58,59,60,61,
    /* 0x3A-0x3C */ DEC_INV,DEC_INV,DEC_INV,
    /* 0x3D '=' */ DEC_PAD,
    /* 0x3E-0x3F */ DEC_INV,DEC_INV,
    /* 0x40     */ DEC_INV,
    /* 0x41-0x5A 'A'-'Z' */
     0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,
    /* 0x5B-0x5E */ DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    /* 0x5F '_' */ 63,   /* URL-safe alias for '/' */
    /* 0x60     */ DEC_INV,
    /* 0x61-0x7A 'a'-'z' */
    26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,
    42,43,44,45,46,47,48,49,50,51,
    /* 0x7B-0xFF */ DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,
    DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV,DEC_INV
};

/* =========================================================================
 * Internal helpers (no libc)
 * ========================================================================= */

/* Compare at most `n` bytes of two byte strings; return 0 if equal */
static int mem_eq(const char *a, const char *b, ulong n)
{
    for (ulong i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* Return length of a NUL-terminated string */
static ulong str_len(const char *s)
{
    ulong n = 0;
    while (s[n]) n++;
    return n;
}

/*
 * Search for the first occurrence of the byte sequence `needle` (length
 * `nlen`) inside `haystack` (length `hlen`).  Returns the offset of the
 * first match, or (ulong)-1 if not found.
 */
static ulong mem_find(const char *haystack, ulong hlen,
                      const char *needle,   ulong nlen)
{
    if (nlen == 0) return 0;
    if (nlen > hlen) return (ulong)-1;

    ulong limit = hlen - nlen;
    for (ulong i = 0; i <= limit; i++) {
        if (mem_eq(haystack + i, needle, nlen))
            return i;
    }
    return (ulong)-1;
}

/* =========================================================================
 * base64_encode
 * ========================================================================= */

b64_long base64_encode(const u8 *in, ulong inlen, char *out, ulong outcap)
{
    /* Encoded length: every 3 input bytes → 4 output chars, padded to multiple of 4 */
    ulong outlen = ((inlen + 2) / 3) * 4;

    /* Need outlen chars + 1 NUL */
    if (outcap < outlen + 1)
        return -1;

    const u8 *src = in;
    char      *dst = out;
    ulong      rem = inlen;

    /* Process full 3-byte groups */
    while (rem >= 3) {
        unsigned int v = ((unsigned int)src[0] << 16)
                       | ((unsigned int)src[1] <<  8)
                       |  (unsigned int)src[2];
        dst[0] = ENC_TABLE[(v >> 18) & 0x3F];
        dst[1] = ENC_TABLE[(v >> 12) & 0x3F];
        dst[2] = ENC_TABLE[(v >>  6) & 0x3F];
        dst[3] = ENC_TABLE[ v        & 0x3F];
        src += 3;
        dst += 4;
        rem -= 3;
    }

    /* Handle 1 or 2 remaining bytes with padding */
    if (rem == 1) {
        unsigned int v = (unsigned int)src[0] << 16;
        dst[0] = ENC_TABLE[(v >> 18) & 0x3F];
        dst[1] = ENC_TABLE[(v >> 12) & 0x3F];
        dst[2] = '=';
        dst[3] = '=';
        dst += 4;
    } else if (rem == 2) {
        unsigned int v = ((unsigned int)src[0] << 16)
                       | ((unsigned int)src[1] <<  8);
        dst[0] = ENC_TABLE[(v >> 18) & 0x3F];
        dst[1] = ENC_TABLE[(v >> 12) & 0x3F];
        dst[2] = ENC_TABLE[(v >>  6) & 0x3F];
        dst[3] = '=';
        dst += 4;
    }

    *dst = '\0';
    return (b64_long)outlen;
}

/* =========================================================================
 * base64_decode
 * ========================================================================= */

b64_long base64_decode(const char *in, ulong inlen, u8 *out, ulong outcap)
{
    ulong   wi   = 0;   /* write index into out */
    unsigned int accum = 0;    /* bit accumulator */
    int     bits = 0;  /* valid bits in accum */
    int     pad  = 0;  /* padding '=' characters seen */

    for (ulong ri = 0; ri < inlen; ri++) {
        u8 c  = (u8)in[ri];
        u8 dv = DEC_TABLE[c];

        if (dv == DEC_WS)
            continue;   /* silently skip whitespace */

        if (dv == DEC_PAD) {
            pad++;
            /* Consume up to 2 padding characters */
            if (pad > 2) return -1;
            /* Flush any remaining bits that represent a full byte */
            /* (handled naturally: accum will have < 8 bits, loop ends) */
            continue;
        }

        if (dv == DEC_INV)
            return -1;  /* invalid character */

        if (pad)
            return -1;  /* data after padding */

        accum = (accum << 6) | dv;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            if (wi >= outcap) return -1;
            out[wi++] = (u8)((accum >> bits) & 0xFF);
        }
    }

    return (b64_long)wi;
}

/* =========================================================================
 * pem_extract_der
 * ========================================================================= */

/*
 * Build the BEGIN marker "-----BEGIN <type>-----" into a stack buffer.
 * Returns the marker length, or 0 if `type` is too long.
 *
 * Maximum marker length: 5 + 6 + typelen + 5 = 16 + typelen.
 * We cap type at 127 chars to keep the stack buffer small.
 */
#define PEM_MARKER_MAX  (16 + 127)

static ulong make_begin_marker(const char *type, char *buf)
{
    /* "-----BEGIN " (11) + type + "-----" (5) */
    const char prefix[] = "-----BEGIN ";
    const char suffix[] = "-----";
    ulong tlen = str_len(type);
    if (tlen > 127) return 0;

    ulong pos = 0;
    for (ulong i = 0; prefix[i]; i++) buf[pos++] = prefix[i];
    for (ulong i = 0; i < tlen;  i++) buf[pos++] = type[i];
    for (ulong i = 0; suffix[i]; i++) buf[pos++] = suffix[i];
    buf[pos] = '\0';
    return pos;
}

static ulong make_end_marker(const char *type, char *buf)
{
    /* "-----END " (9) + type + "-----" (5) */
    const char prefix[] = "-----END ";
    const char suffix[] = "-----";
    ulong tlen = str_len(type);
    if (tlen > 127) return 0;

    ulong pos = 0;
    for (ulong i = 0; prefix[i]; i++) buf[pos++] = prefix[i];
    for (ulong i = 0; i < tlen;  i++) buf[pos++] = type[i];
    for (ulong i = 0; suffix[i]; i++) buf[pos++] = suffix[i];
    buf[pos] = '\0';
    return pos;
}

b64_long pem_extract_der(const char *pem, ulong pemlen,
                         const char *type,
                         u8 *out, ulong outcap)
{
    char begin_marker[PEM_MARKER_MAX + 1];
    char end_marker[PEM_MARKER_MAX + 1];

    ulong blen = make_begin_marker(type, begin_marker);
    ulong elen = make_end_marker(type, end_marker);
    if (blen == 0 || elen == 0) return -1;

    /* Find the BEGIN marker */
    ulong bpos = mem_find(pem, pemlen, begin_marker, blen);
    if (bpos == (ulong)-1) return -1;

    /* The base64 body starts after the BEGIN marker (skip any trailing \r\n) */
    ulong body_start = bpos + blen;
    /* Skip \r and/or \n immediately after the marker */
    while (body_start < pemlen &&
           (pem[body_start] == '\r' || pem[body_start] == '\n'))
        body_start++;

    /* Find the END marker (must appear after body_start) */
    ulong epos = mem_find(pem + body_start, pemlen - body_start,
                          end_marker, elen);
    if (epos == (ulong)-1) return -1;

    ulong body_len = epos;   /* bytes between body_start and the END marker */

    /* base64_decode handles embedded newlines/spaces itself */
    return base64_decode(pem + body_start, body_len, out, outcap);
}

/* =========================================================================
 * base64_selftest
 * ========================================================================= */

/*
 * Minimal stack-only string comparison helper
 */
static int cstr_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/*
 * RFC 4648 §10 test vectors:
 *
 *   ""       -> ""
 *   "f"      -> "Zg=="
 *   "fo"     -> "Zm8="
 *   "foo"    -> "Zm9v"
 *   "foob"   -> "Zm9vYg=="
 *   "fooba"  -> "Zm9vYmE="
 *   "foobar" -> "Zm9vYmFy"
 */

/* Check encode("input") == expected_b64 */
static int test_encode(const u8 *input, ulong inlen,
                       const char *expected)
{
    char buf[64];
    b64_long r = base64_encode(input, inlen, buf, sizeof(buf));
    if (r < 0) return 0;
    return cstr_eq(buf, expected);
}

/* Check decode(b64) == expected bytes */
static int test_decode_bytes(const char *b64,
                             const u8 *expected, ulong explen)
{
    u8 buf[64];
    b64_long r = base64_decode(b64, str_len(b64), buf, sizeof(buf));
    if (r < 0 || (ulong)r != explen) return 0;
    for (ulong i = 0; i < explen; i++) {
        if (buf[i] != expected[i]) return 0;
    }
    return 1;
}

int base64_selftest(void)
{
    /* --- RFC 4648 encode vectors ---------------------------------------- */

    if (!test_encode((const u8*)"",      0, ""))       return -1;
    if (!test_encode((const u8*)"f",     1, "Zg=="))   return -1;
    if (!test_encode((const u8*)"fo",    2, "Zm8="))   return -1;
    if (!test_encode((const u8*)"foo",   3, "Zm9v"))   return -1;
    if (!test_encode((const u8*)"foob",  4, "Zm9vYg==")) return -1;
    if (!test_encode((const u8*)"fooba", 5, "Zm9vYmE=")) return -1;
    if (!test_encode((const u8*)"foobar",6, "Zm9vYmFy")) return -1;

    /* --- RFC 4648 decode round-trips ------------------------------------ */

    if (!test_decode_bytes("",         (const u8*)"",      0)) return -1;
    if (!test_decode_bytes("Zg==",     (const u8*)"f",     1)) return -1;
    if (!test_decode_bytes("Zm8=",     (const u8*)"fo",    2)) return -1;
    if (!test_decode_bytes("Zm9v",     (const u8*)"foo",   3)) return -1;
    if (!test_decode_bytes("Zm9vYg==", (const u8*)"foob",  4)) return -1;
    if (!test_decode_bytes("Zm9vYmE=", (const u8*)"fooba", 5)) return -1;
    if (!test_decode_bytes("Zm9vYmFy", (const u8*)"foobar",6)) return -1;

    /* --- Whitespace / newline tolerance in decode ----------------------- */

    if (!test_decode_bytes("Zm9v\nYmFy", (const u8*)"foobar", 6)) return -1;
    if (!test_decode_bytes("Zm9v YmFy",  (const u8*)"foobar", 6)) return -1;

    /* --- URL-safe alphabet (-_) ---------------------------------------- */

    /* "foobar" -> "Zm9vYmFy" (no +/ in this vector, but test +→- / /→_ ) */
    /* Construct a vector that hits + and /: "\xFB\xFF" → "+/8=" (standard)
     * URL-safe: "-_8=" */
    {
        const u8 raw[3] = { 0xFB, 0xFF, 0x00 }; /* last byte 0 to avoid issues */
        /* encode the 3 bytes: 0xFB=11111011, 0xFF=11111111, 0x00=00000000
         * groups: 111110 110111 111111 000000 → 62,55,63,0 → +3/A
         * URL-safe: -3_A */
        char enc[8];
        b64_long er = base64_encode(raw, 3, enc, sizeof(enc));
        if (er != 4) return -1;
        /* Manually swap to URL-safe and verify decode gives same result */
        char url_safe[5];
        url_safe[0] = (enc[0] == '+') ? '-' : (enc[0] == '/') ? '_' : enc[0];
        url_safe[1] = (enc[1] == '+') ? '-' : (enc[1] == '/') ? '_' : enc[1];
        url_safe[2] = (enc[2] == '+') ? '-' : (enc[2] == '/') ? '_' : enc[2];
        url_safe[3] = (enc[3] == '+') ? '-' : (enc[3] == '/') ? '_' : enc[3];
        url_safe[4] = '\0';

        u8 dec[4];
        b64_long dr = base64_decode(url_safe, 4, dec, sizeof(dec));
        if (dr != 3) return -1;
        if (dec[0] != raw[0] || dec[1] != raw[1] || dec[2] != raw[2]) return -1;
    }

    /* --- PEM round-trip ------------------------------------------------- */

    /*
     * Wrap 6 bytes ("foobar") as a TEST PEM block and verify recovery.
     *
     * PEM block constructed on the stack (no heap):
     *   -----BEGIN TEST-----\n
     *   Zm9vYmFy\n
     *   -----END TEST-----\n
     */
    {
        const u8  der_in[6]  = { 'f','o','o','b','a','r' };
        /* Build the PEM string manually to keep it freestanding */
        const char pem[] =
            "-----BEGIN TEST-----\n"
            "Zm9vYmFy\n"
            "-----END TEST-----\n";

        u8       der_out[32];
        b64_long dr = pem_extract_der(pem, str_len(pem), "TEST",
                                      der_out, sizeof(der_out));
        if (dr != 6) return -1;
        for (int i = 0; i < 6; i++) {
            if (der_out[i] != der_in[i]) return -1;
        }
    }

    /* --- Additional PEM: multi-line body, CERTIFICATE label ------------- */

    {
        /*
         * Encode 9 bytes across two lines of base64 (6 bytes → 8 chars first
         * line, 3 bytes → 4 chars second line, for a clean demo):
         *   "foobar" (6 bytes) → "Zm9vYmFy"  (first line)
         *   "foo"   (3 bytes) → "Zm9v"       (second line)
         *   Total: 9 bytes
         */
        const char pem[] =
            "-----BEGIN CERTIFICATE-----\r\n"
            "Zm9vYmFy\r\n"
            "Zm9v\r\n"
            "-----END CERTIFICATE-----\r\n";

        const u8 expected[9] = {'f','o','o','b','a','r','f','o','o'};

        u8       buf[32];
        b64_long dr = pem_extract_der(pem, str_len(pem),
                                      "CERTIFICATE", buf, sizeof(buf));
        if (dr != 9) return -1;
        for (int i = 0; i < 9; i++) {
            if (buf[i] != expected[i]) return -1;
        }
    }

    return 0;   /* all tests passed */
}

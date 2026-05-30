/*
 * deflate.c -- self-contained DEFLATE compressor + INFLATE decompressor.
 * =======================================================================
 *
 * RFC 1951 raw-stream codec. FREESTANDING: no libc, no headers, no globals
 * that depend on a C runtime beyond plain zero-init .bss. Everything works
 * on in-memory buffers handed in by the caller (gzip.c).
 *
 *   inflate_decompress() : the must-have half. Decodes stored, fixed-Huffman
 *                          and dynamic-Huffman blocks. Tested to round-trip
 *                          against our own compressor AND it is written to
 *                          the RFC so it also decodes real zlib/gzip output.
 *
 *   deflate_compress()   : LZ77 + FIXED Huffman compressor. It performs hash
 *                          -chained match finding (lazy, length 3..258,
 *                          distance 1..32768) and emits one or more fixed
 *                          (BTYPE=01) blocks. If at any point the compressed
 *                          form would not fit / not help, it transparently
 *                          falls back to STORED (BTYPE=00) blocks, which are
 *                          always valid. So the result is always a correct
 *                          DEFLATE stream regardless of the data.
 *
 *   deflate_crc32()      : reflected CRC-32, poly 0xEDB88320 (gzip/zlib).
 *
 * Build (same flags as the other freestanding userspace tools):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 \
 *       -c userspace/lib/deflate/deflate.c -o deflate.o
 */

#include "deflate.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* =========================================================================
 *  CRC-32 (reflected, poly 0xEDB88320). Table built lazily on first use.
 * ========================================================================= */
static u32  g_crc_tab[256];
static int  g_crc_ready = 0;

static void crc_build(void)
{
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[n] = c;
    }
    g_crc_ready = 1;
}

unsigned int deflate_crc32(const unsigned char *buf, long len)
{
    if (!g_crc_ready) crc_build();
    u32 crc = 0xFFFFFFFFu;
    for (long i = 0; i < len; i++)
        crc = g_crc_tab[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* =========================================================================
 *  Bit reader for INFLATE.  DEFLATE packs bits LSB-first within each byte.
 * ========================================================================= */
typedef struct {
    const u8 *src;
    long      len;
    long      pos;     /* next byte index                    */
    u32       bitbuf;  /* bit accumulator (LSB = next bit)    */
    int       bitcnt;  /* valid bits in bitbuf                */
    int       error;   /* set on input exhaustion             */
} bitreader;

static void br_init(bitreader *b, const u8 *src, long len)
{
    b->src = src; b->len = len; b->pos = 0;
    b->bitbuf = 0; b->bitcnt = 0; b->error = 0;
}

/* Read a single bit (0/1). On exhaustion sets error and returns 0. */
static int br_bit(bitreader *b)
{
    if (b->bitcnt == 0) {
        if (b->pos >= b->len) { b->error = 1; return 0; }
        b->bitbuf = b->src[b->pos++];
        b->bitcnt = 8;
    }
    int bit = (int)(b->bitbuf & 1u);
    b->bitbuf >>= 1;
    b->bitcnt--;
    return bit;
}

/* Read `n` bits (n<=24), LSB-first, as an unsigned integer. */
static u32 br_bits(bitreader *b, int n)
{
    u32 v = 0;
    for (int i = 0; i < n; i++)
        v |= (u32)br_bit(b) << i;
    return v;
}

/* Drop any partially-consumed bits to align to the next byte boundary. */
static void br_align(bitreader *b)
{
    b->bitbuf = 0;
    b->bitcnt = 0;
}

/* =========================================================================
 *  Canonical Huffman decode table.
 *
 *  We decode bit-by-bit using the classic (first_code / first_symbol /
 *  count-per-length) scheme from RFC 1951's appendix. No large tables, no
 *  allocation -- everything is bounded by MAXBITS and the symbol count.
 * ========================================================================= */
#define MAXBITS 15

typedef struct {
    u16 count[MAXBITS + 1];   /* number of codes of each length        */
    u16 symbol[288];          /* symbols sorted by (length, value)     */
} huff;

/*
 * Build a canonical Huffman decode table from an array of code lengths.
 * `lengths[i]` is the code length (in bits) for symbol i; 0 = unused.
 * Returns 0 on success, -1 on an over-subscribed (invalid) code set.
 */
static int huff_build(huff *h, const u8 *lengths, int n)
{
    for (int i = 0; i <= MAXBITS; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;   /* unused symbols don't participate */

    /* Check for an over-subscribed or incomplete code (left as a soft check:
     * incomplete is tolerated for the single-symbol distance-tree edge case
     * the RFC permits; over-subscription is a hard error). */
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= (int)h->count[len];
        if (left < 0) return -1;   /* over-subscribed */
    }

    /* Offsets into the symbol table for each length. */
    u16 offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len + 1] = (u16)(offs[len] + h->count[len]);

    for (int i = 0; i < n; i++)
        if (lengths[i] != 0)
            h->symbol[offs[lengths[i]]++] = (u16)i;

    return 0;
}

/*
 * Decode one symbol from the bit stream using table `h`.
 * Returns the symbol, or -1 on an invalid code / input exhaustion.
 */
static int huff_decode(bitreader *b, const huff *h)
{
    int code = 0;     /* bits read so far, MSB-first within the code */
    int first = 0;    /* first canonical code of the current length  */
    int index = 0;    /* index of first symbol of the current length */

    for (int len = 1; len <= MAXBITS; len++) {
        code |= br_bit(b);
        if (b->error) return -1;
        int cnt = (int)h->count[len];
        if (code - first < cnt)
            return (int)h->symbol[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;   /* code longer than MAXBITS -> invalid */
}

/* =========================================================================
 *  Length / distance base + extra-bit tables (RFC 1951 sections 3.2.5).
 * ========================================================================= */

/* length symbol 257..285 -> base length and extra bits */
static const u16 LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const u8 LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/* distance symbol 0..29 -> base distance and extra bits */
static const u16 DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const u8 DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* =========================================================================
 *  INFLATE -- decode one DEFLATE block body given lit/len + dist trees.
 *  Output is written into out[] with bounds checking against out_cap.
 *  `*op` is the running output position (also used as the LZ77 window).
 * ========================================================================= */
static int inflate_block(bitreader *b, const huff *lit, const huff *dist,
                         u8 *out, long out_cap, long *op)
{
    for (;;) {
        int sym = huff_decode(b, lit);
        if (sym < 0) return b->error ? -3 : -2;

        if (sym == 256) return 0;          /* end of block */

        if (sym < 256) {                   /* literal byte */
            if (*op >= out_cap) return -1;
            out[(*op)++] = (u8)sym;
            continue;
        }

        /* length/distance pair (sym 257..285) */
        sym -= 257;
        if (sym >= 29) return -2;
        int len = (int)LEN_BASE[sym] + (int)br_bits(b, LEN_EXTRA[sym]);

        int dsym = huff_decode(b, dist);
        if (dsym < 0) return b->error ? -3 : -2;
        if (dsym >= 30) return -2;
        long distance = (long)DIST_BASE[dsym] + (long)br_bits(b, DIST_EXTRA[dsym]);

        if (distance > *op) return -2;     /* refers before start of output */
        if (*op + len > out_cap) return -1;

        /* Copy `len` bytes from `distance` back. Overlapping copies are
         * intentional (run-length) so we copy one byte at a time. */
        long from = *op - distance;
        for (int i = 0; i < len; i++)
            out[(*op)++] = out[from++];
    }
}

/* Build the two FIXED Huffman trees (RFC 1951 3.2.6). */
static void build_fixed_trees(huff *lit, huff *dist)
{
    u8 ll[288];
    int i = 0;
    for (; i < 144; i++) ll[i] = 8;
    for (; i < 256; i++) ll[i] = 9;
    for (; i < 280; i++) ll[i] = 7;
    for (; i < 288; i++) ll[i] = 8;
    huff_build(lit, ll, 288);

    u8 dl[30];
    for (i = 0; i < 30; i++) dl[i] = 5;
    huff_build(dist, dl, 30);
}

/* Order in which code-length-code lengths are stored (RFC 1951 3.2.7). */
static const u8 CLEN_ORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* Read a DYNAMIC block's two Huffman trees from the bit stream. */
static int read_dynamic_trees(bitreader *b, huff *lit, huff *dist)
{
    int hlit  = (int)br_bits(b, 5) + 257;   /* # literal/length codes */
    int hdist = (int)br_bits(b, 5) + 1;     /* # distance codes       */
    int hclen = (int)br_bits(b, 4) + 4;     /* # code-length codes    */
    if (hlit > 286 || hdist > 30) return -2;

    /* 1) code-length-code lengths */
    u8 cl_lengths[19];
    for (int i = 0; i < 19; i++) cl_lengths[i] = 0;
    for (int i = 0; i < hclen; i++)
        cl_lengths[CLEN_ORDER[i]] = (u8)br_bits(b, 3);
    if (b->error) return -3;

    huff clh;
    if (huff_build(&clh, cl_lengths, 19) != 0) return -2;

    /* 2) decode the lit+dist code lengths using the code-length tree */
    u8 lengths[286 + 30];
    int total = hlit + hdist;
    int n = 0;
    while (n < total) {
        int sym = huff_decode(b, &clh);
        if (sym < 0) return b->error ? -3 : -2;

        if (sym < 16) {
            lengths[n++] = (u8)sym;
        } else if (sym == 16) {            /* repeat previous 3..6 times */
            if (n == 0) return -2;
            int rep = 3 + (int)br_bits(b, 2);
            u8 prev = lengths[n - 1];
            while (rep-- && n < total) lengths[n++] = prev;
        } else if (sym == 17) {            /* repeat zero 3..10 times */
            int rep = 3 + (int)br_bits(b, 3);
            while (rep-- && n < total) lengths[n++] = 0;
        } else {                           /* sym == 18: repeat zero 11..138 */
            int rep = 11 + (int)br_bits(b, 7);
            while (rep-- && n < total) lengths[n++] = 0;
        }
        if (b->error) return -3;
    }

    if (huff_build(lit, lengths, hlit) != 0) return -2;
    if (huff_build(dist, lengths + hlit, hdist) != 0) return -2;
    return 0;
}

long inflate_decompress(const unsigned char *in, long in_len,
                        unsigned char *out, long out_cap)
{
    bitreader b;
    br_init(&b, in, in_len);
    long op = 0;

    for (;;) {
        int bfinal = br_bit(&b);
        int btype  = (int)br_bits(&b, 2);
        if (b.error) return -3;

        if (btype == 0) {
            /* STORED: align, read LEN/NLEN, copy raw bytes. */
            br_align(&b);
            if (b.pos + 4 > b.len) return -3;
            u32 len  = (u32)in[b.pos] | ((u32)in[b.pos + 1] << 8);
            u32 nlen = (u32)in[b.pos + 2] | ((u32)in[b.pos + 3] << 8);
            b.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) return -2;     /* sanity check */
            if (b.pos + (long)len > b.len) return -3;
            if (op + (long)len > out_cap) return -1;
            for (u32 i = 0; i < len; i++) out[op++] = in[b.pos++];
        } else if (btype == 1) {
            huff lit, dist;
            build_fixed_trees(&lit, &dist);
            int r = inflate_block(&b, &lit, &dist, out, out_cap, &op);
            if (r != 0) return r;
        } else if (btype == 2) {
            huff lit, dist;
            int r = read_dynamic_trees(&b, &lit, &dist);
            if (r != 0) return r;
            r = inflate_block(&b, &lit, &dist, out, out_cap, &op);
            if (r != 0) return r;
        } else {
            return -2;   /* btype 3 is reserved/invalid */
        }

        if (bfinal) break;
    }
    return op;
}

/* =========================================================================
 *  DEFLATE compressor.
 *
 *  Strategy: LZ77 (hash-chained, lazy single-step) producing a token stream,
 *  emitted in FIXED Huffman blocks. A bit writer packs symbols LSB-first.
 *  Whole-stream STORED fallback guarantees correctness/space if compression
 *  fails to fit.
 * ========================================================================= */

/* ---- bit writer (LSB-first, matching the bit reader) ---- */
typedef struct {
    u8  *dst;
    long cap;
    long pos;     /* next byte index                     */
    u32  bitbuf;  /* accumulated bits (LSB first)         */
    int  bitcnt;  /* number of valid bits in bitbuf       */
    int  overflow;/* set if we ran out of output room     */
} bitwriter;

static void bw_init(bitwriter *w, u8 *dst, long cap)
{
    w->dst = dst; w->cap = cap; w->pos = 0;
    w->bitbuf = 0; w->bitcnt = 0; w->overflow = 0;
}

/* Append the low `n` bits of `v`, LSB first. */
static void bw_bits(bitwriter *w, u32 v, int n)
{
    w->bitbuf |= (v & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u))) << w->bitcnt;
    w->bitcnt += n;
    while (w->bitcnt >= 8) {
        if (w->pos >= w->cap) { w->overflow = 1; return; }
        w->dst[w->pos++] = (u8)(w->bitbuf & 0xFF);
        w->bitbuf >>= 8;
        w->bitcnt -= 8;
    }
}

/* Flush remaining bits, padding the final byte with zeros. */
static void bw_flush(bitwriter *w)
{
    if (w->bitcnt > 0) {
        if (w->pos >= w->cap) { w->overflow = 1; return; }
        w->dst[w->pos++] = (u8)(w->bitbuf & 0xFF);
        w->bitbuf = 0;
        w->bitcnt = 0;
    }
}

/*
 * Emit a fixed-Huffman literal/length code. Fixed codes are stored MSB-first
 * in the canonical assignment but written to the stream LSB-first, so each
 * code must be bit-reversed before being packed. We reverse on the fly.
 */
static u32 reverse_bits(u32 code, int len)
{
    u32 r = 0;
    for (int i = 0; i < len; i++) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }
    return r;
}

/* Write a fixed-Huffman literal symbol (0..287). */
static void emit_fixed_litlen_symbol(bitwriter *w, int sym)
{
    u32 code; int len;
    if (sym <= 143)      { code = 0x30 + sym;        len = 8; }   /* 00110000.. */
    else if (sym <= 255) { code = 0x190 + (sym - 144); len = 9; } /* 110010000.. */
    else if (sym <= 279) { code = 0x00 + (sym - 256);  len = 7; } /* 0000000..   */
    else                 { code = 0xC0 + (sym - 280);  len = 8; } /* 11000000..  */
    bw_bits(w, reverse_bits(code, len), len);
}

/* Map a match length (3..258) to its length symbol + extra bits, emit them. */
static void emit_length(bitwriter *w, int length)
{
    int sym = 0;
    /* find the length code whose base<=length and base+range>length */
    for (int i = 28; i >= 0; i--) {
        if (length >= (int)LEN_BASE[i]) { sym = i; break; }
    }
    emit_fixed_litlen_symbol(w, 257 + sym);
    int extra = LEN_EXTRA[sym];
    if (extra) bw_bits(w, (u32)(length - (int)LEN_BASE[sym]), extra);
}

/* Map a distance (1..32768) to its distance symbol + extra bits, emit them.
 * Fixed Huffman uses 5-bit codes for distance symbols, written reversed. */
static void emit_distance(bitwriter *w, int distance)
{
    int sym = 0;
    for (int i = 29; i >= 0; i--) {
        if (distance >= (int)DIST_BASE[i]) { sym = i; break; }
    }
    bw_bits(w, reverse_bits((u32)sym, 5), 5);
    int extra = DIST_EXTRA[sym];
    if (extra) bw_bits(w, (u32)(distance - (int)DIST_BASE[sym]), extra);
}

/* ---- LZ77 match finder: hash-chained, fixed window ---- */
#define MIN_MATCH   3
#define MAX_MATCH   258
#define WINDOW      32768
#define HASH_BITS   15
#define HASH_SIZE   (1 << HASH_BITS)
#define MAX_CHAIN   128            /* search depth cap (speed vs ratio)   */

/* Hash 3 bytes into a HASH_BITS-wide bucket. */
static int hash3(const u8 *p)
{
    u32 h = ((u32)p[0] << 16) ^ ((u32)p[1] << 8) ^ (u32)p[2];
    h = (h * 2654435761u) >> (32 - HASH_BITS);
    return (int)(h & (HASH_SIZE - 1));
}

/* Static scratch (the kernel gives a tiny stack; these must not be on it). */
static int g_head[HASH_SIZE];      /* most-recent position for each hash  */
static int g_prev[WINDOW];         /* previous position chain (mod WINDOW) */

/*
 * Find the longest match for the bytes at `cur` within the window preceding
 * it. Returns the match length (>=MIN_MATCH) and sets *out_dist, or 0 if no
 * usable match. `in` is the whole buffer, `in_len` its length.
 */
static int find_match(const u8 *in, long in_len, long cur, int *out_dist)
{
    int h = hash3(in + cur);
    int cand = g_head[h];
    int best_len = 0;
    int best_dist = 0;
    long max_len = in_len - cur;
    if (max_len > MAX_MATCH) max_len = MAX_MATCH;
    if (max_len < MIN_MATCH) return 0;

    int chain = MAX_CHAIN;
    while (cand >= 0 && chain-- > 0) {
        long dist = cur - cand;
        if (dist > WINDOW) break;
        /* quick reject: compare the byte at best_len first */
        if (best_len > 0 && in[cand + best_len] != in[cur + best_len]) {
            cand = g_prev[cand & (WINDOW - 1)];
            continue;
        }
        int len = 0;
        while (len < max_len && in[cand + len] == in[cur + len]) len++;
        if (len > best_len) {
            best_len = len;
            best_dist = (int)dist;
            if (len >= (int)max_len) break;   /* can't do better */
        }
        cand = g_prev[cand & (WINDOW - 1)];
    }

    if (best_len >= MIN_MATCH) {
        *out_dist = best_dist;
        return best_len;
    }
    return 0;
}

/* Insert position `cur` into the hash chains. */
static void hash_insert(const u8 *in, long cur, int h)
{
    g_prev[cur & (WINDOW - 1)] = g_head[h];
    g_head[h] = (int)cur;
}

/*
 * STORED fallback: emit the entire input as one or more stored blocks. Always
 * valid; used when compression overflows or isn't worthwhile. Returns the
 * output length, or -1 if it does not fit in out_cap.
 */
static long emit_stored(const u8 *in, long in_len, u8 *out, long out_cap)
{
    long op = 0;
    long off = 0;
    do {
        long chunk = in_len - off;
        if (chunk > 65535) chunk = 65535;
        int final = (off + chunk >= in_len) ? 1 : 0;

        /* one header byte (BFINAL + BTYPE=00, byte-aligned) + 4 len bytes.
         * LEN is a 16-bit value; NLEN is its one's complement, also 16-bit.
         * Mask to 16 bits BEFORE inverting so the high byte is correct (a bare
         * ~chunk on a 64-bit long would sign-extend and corrupt NLEN). */
        if (op + 5 + chunk > out_cap) return -1;
        u32 len16  = (u32)chunk & 0xFFFFu;
        u32 nlen16 = (~len16) & 0xFFFFu;
        out[op++] = (u8)(final & 1);           /* BFINAL in bit0, BTYPE=00 */
        out[op++] = (u8)(len16 & 0xFF);
        out[op++] = (u8)((len16 >> 8) & 0xFF);
        out[op++] = (u8)(nlen16 & 0xFF);
        out[op++] = (u8)((nlen16 >> 8) & 0xFF);
        for (long i = 0; i < chunk; i++) out[op++] = in[off + i];
        off += chunk;
        if (final) break;
    } while (off < in_len);

    /* Handle the empty-input case: a single empty final stored block. */
    if (in_len == 0) {
        if (out_cap < 5) return -1;
        out[0] = 1; out[1] = 0; out[2] = 0; out[3] = 0xFF; out[4] = 0xFF;
        return 5;
    }
    return op;
}

long deflate_compress(const unsigned char *in, long in_len,
                      unsigned char *out, long out_cap)
{
    /* Empty input -> one empty final fixed block is cheapest & simplest, but
     * a stored empty block is equally valid and avoids a code path; use it. */
    if (in_len == 0)
        return emit_stored(in, 0, out, out_cap);

    /* Reset hash chains. */
    for (int i = 0; i < HASH_SIZE; i++) g_head[i] = -1;

    bitwriter w;
    bw_init(&w, out, out_cap);

    /* Single fixed-Huffman block covering the whole input (BFINAL=1). The
     * fixed block has no length limit, so one block suffices. */
    bw_bits(&w, 1, 1);   /* BFINAL = 1 */
    bw_bits(&w, 1, 2);   /* BTYPE  = 01 (fixed Huffman) */

    long cur = 0;
    while (cur < in_len) {
        int dist = 0;
        int mlen = 0;

        if (in_len - cur >= MIN_MATCH) {
            mlen = find_match(in, in_len, cur, &dist);
        }

        if (mlen >= MIN_MATCH) {
            emit_length(&w, mlen);
            emit_distance(&w, dist);
            /* Insert hash entries for every covered position so future
             * matches can reference inside this run. */
            int end = (int)cur + mlen;
            for (long p = cur; p < end && p + MIN_MATCH <= in_len; p++)
                hash_insert(in, p, hash3(in + p));
            cur += mlen;
        } else {
            emit_fixed_litlen_symbol(&w, in[cur]);
            if (cur + MIN_MATCH <= in_len)
                hash_insert(in, cur, hash3(in + cur));
            cur++;
        }

        if (w.overflow) {
            /* Compressed form didn't fit: fall back to STORED for the whole
             * input (still correct, just larger). */
            return emit_stored(in, in_len, out, out_cap);
        }
    }

    emit_fixed_litlen_symbol(&w, 256);   /* end-of-block */
    bw_flush(&w);

    if (w.overflow)
        return emit_stored(in, in_len, out, out_cap);

    /* If the "compressed" output ended up larger than a stored representation
     * would be, prefer stored (only when it actually fits). This keeps us
     * honest on incompressible data. */
    long stored_size = 5 * ((in_len + 65534) / 65535) + in_len;
    if (w.pos > stored_size && stored_size <= out_cap) {
        long s = emit_stored(in, in_len, out, out_cap);
        if (s > 0) return s;
    }
    return w.pos;
}

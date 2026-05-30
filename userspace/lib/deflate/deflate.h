/*
 * deflate.h -- self-contained DEFLATE compressor + INFLATE decompressor.
 * =======================================================================
 *
 * RFC 1951 (DEFLATE) raw stream codec for the from-scratch x86_64 OS.
 * FREESTANDING: no libc, no headers. Operates purely on in-memory buffers.
 *
 * This is the RAW DEFLATE layer only -- it knows nothing about the gzip
 * (RFC 1952) container. gzip.c wraps these with the 1f 8b ... CRC32/ISIZE
 * framing. CRC32 is also provided here because both layers need it.
 *
 * Linkage: deflate.c is compiled standalone and linked alongside gzip.o.
 * Everything below is a small, stable surface:
 *
 *   long deflate_compress(const unsigned char *in, long in_len,
 *                         unsigned char *out, long out_cap);
 *       Compress in[0..in_len) into out[0..out_cap). Returns the number of
 *       bytes written (the raw DEFLATE stream), or -1 if it did not fit.
 *       The output is always a VALID DEFLATE stream that any conformant
 *       inflate (zlib/gunzip) can decode. Depending on build it emits
 *       fixed-Huffman compressed blocks (with LZ77 matching) and falls back
 *       to stored (uncompressed) blocks where that is smaller or safer.
 *
 *   long inflate_decompress(const unsigned char *in, long in_len,
 *                           unsigned char *out, long out_cap);
 *       Decompress a raw DEFLATE stream in[0..in_len) into out[0..out_cap).
 *       Returns the number of decompressed bytes, or a negative error code:
 *           -1  output buffer too small
 *           -2  malformed stream / bad block type / bad Huffman code
 *           -3  input exhausted unexpectedly
 *       Handles all three block types: stored (00), fixed Huffman (01),
 *       dynamic Huffman (10).
 *
 *   unsigned int deflate_crc32(const unsigned char *buf, long len);
 *       Standard zlib/gzip CRC-32 (polynomial 0xEDB88320) over buf[0..len).
 */

#ifndef DEFLATE_H
#define DEFLATE_H

/* Raw DEFLATE codec. */
long deflate_compress(const unsigned char *in, long in_len,
                      unsigned char *out, long out_cap);

long inflate_decompress(const unsigned char *in, long in_len,
                        unsigned char *out, long out_cap);

/* CRC-32 (gzip/zlib polynomial, reflected). */
unsigned int deflate_crc32(const unsigned char *buf, long len);

#endif /* DEFLATE_H */

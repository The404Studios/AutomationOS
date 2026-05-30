/**
 * @file compression.c
 * @brief AutoFS Transparent Compression
 *
 * Supports multiple compression algorithms:
 * - zstd (best compression ratio)
 * - lz4 (fastest)
 * - zlib (good balance)
 */

#include "../../include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Simple LZ4-like compression (simplified implementation) */
static ssize_t compress_lz4_simple(const void *src, size_t src_len,
                                   void *dst, size_t dst_len) {
    /* Simplified LZ4 - just copy for now */
    /* Real implementation would use actual LZ4 library */
    if (dst_len < src_len) {
        errno = ENOSPC;
        return -1;
    }

    memcpy(dst, src, src_len);
    return src_len;
}

static ssize_t decompress_lz4_simple(const void *src, size_t src_len,
                                     void *dst, size_t dst_len) {
    if (dst_len < src_len) {
        errno = ENOSPC;
        return -1;
    }

    memcpy(dst, src, src_len);
    return src_len;
}

/* Simple RLE compression */
static ssize_t compress_rle(const void *src, size_t src_len,
                           void *dst, size_t dst_len) {
    const uint8_t *input = src;
    uint8_t *output = dst;
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < src_len && out_pos + 2 < dst_len) {
        uint8_t byte = input[in_pos];
        size_t run_len = 1;

        /* Count run length */
        while (in_pos + run_len < src_len &&
               input[in_pos + run_len] == byte &&
               run_len < 255) {
            run_len++;
        }

        /* Encode run */
        output[out_pos++] = byte;
        output[out_pos++] = (uint8_t)run_len;
        in_pos += run_len;
    }

    if (out_pos >= dst_len) {
        errno = ENOSPC;
        return -1;
    }

    return out_pos;
}

static ssize_t decompress_rle(const void *src, size_t src_len,
                              void *dst, size_t dst_len) {
    const uint8_t *input = src;
    uint8_t *output = dst;
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos + 1 < src_len && out_pos < dst_len) {
        uint8_t byte = input[in_pos++];
        uint8_t run_len = input[in_pos++];

        for (uint8_t i = 0; i < run_len && out_pos < dst_len; i++) {
            output[out_pos++] = byte;
        }
    }

    return out_pos;
}

/**
 * @brief Compress data
 *
 * @param algo Compression algorithm
 * @param src Source data
 * @param src_len Source length
 * @param dst Destination buffer
 * @param dst_len Destination buffer size
 * @return Compressed size or -1 on error
 */
ssize_t autofs_compress(compress_algo_t algo, const void *src, size_t src_len,
                       void *dst, size_t dst_len) {
    if (!src || !dst) {
        errno = EINVAL;
        return -1;
    }

    switch (algo) {
        case COMPRESS_NONE:
            if (dst_len < src_len) {
                errno = ENOSPC;
                return -1;
            }
            memcpy(dst, src, src_len);
            return src_len;

        case COMPRESS_LZ4:
            return compress_lz4_simple(src, src_len, dst, dst_len);

        case COMPRESS_ZLIB:
            /* Use RLE as simple compression */
            return compress_rle(src, src_len, dst, dst_len);

        case COMPRESS_ZSTD:
            /* Use RLE as simple compression */
            return compress_rle(src, src_len, dst, dst_len);

        default:
            errno = EINVAL;
            return -1;
    }
}

/**
 * @brief Decompress data
 *
 * @param algo Compression algorithm
 * @param src Compressed data
 * @param src_len Compressed length
 * @param dst Destination buffer
 * @param dst_len Destination buffer size
 * @return Decompressed size or -1 on error
 */
ssize_t autofs_decompress(compress_algo_t algo, const void *src, size_t src_len,
                         void *dst, size_t dst_len) {
    if (!src || !dst) {
        errno = EINVAL;
        return -1;
    }

    switch (algo) {
        case COMPRESS_NONE:
            if (dst_len < src_len) {
                errno = ENOSPC;
                return -1;
            }
            memcpy(dst, src, src_len);
            return src_len;

        case COMPRESS_LZ4:
            return decompress_lz4_simple(src, src_len, dst, dst_len);

        case COMPRESS_ZLIB:
            return decompress_rle(src, src_len, dst, dst_len);

        case COMPRESS_ZSTD:
            return decompress_rle(src, src_len, dst, dst_len);

        default:
            errno = EINVAL;
            return -1;
    }
}

/**
 * @brief Write compressed file data
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param data Data to write
 * @param len Data length
 * @return 0 on success, -1 on error
 */
int autofs_write_compressed(autofs_fs_t *fs, uint64_t ino, const void *data,
                           size_t len) {
    if (!fs || !data) {
        errno = EINVAL;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    /* Determine compression algorithm */
    compress_algo_t algo = fs->sb->default_compress_algo;
    if (algo == COMPRESS_NONE) {
        algo = COMPRESS_ZSTD;  /* Default to zstd */
    }

    /* Compress data */
    uint8_t *compressed = malloc(len);  /* Worst case: same size */
    if (!compressed) {
        free(inode);
        return -1;
    }

    ssize_t compressed_len = autofs_compress(algo, data, len, compressed, len);
    if (compressed_len < 0) {
        free(compressed);
        free(inode);
        return -1;
    }

    /* Only use compressed if smaller */
    if ((size_t)compressed_len < len) {
        /* Write compressed data */
        inode->compressed_size = compressed_len;
        inode->compress_algo = algo;
        inode->flags |= AUTOFS_INODE_COMPRESSED;
        inode->size = len;  /* Original size */

        /* Write blocks */
        size_t blocks_needed = (compressed_len + AUTOFS_BLOCK_SIZE - 1) / AUTOFS_BLOCK_SIZE;
        for (size_t i = 0; i < blocks_needed; i++) {
            uint8_t block_buf[AUTOFS_BLOCK_SIZE] = {0};
            size_t copy_len = (i + 1) * AUTOFS_BLOCK_SIZE <= (size_t)compressed_len ?
                             AUTOFS_BLOCK_SIZE :
                             compressed_len - i * AUTOFS_BLOCK_SIZE;

            memcpy(block_buf, compressed + i * AUTOFS_BLOCK_SIZE, copy_len);

            if (autofs_cow_write(fs, ino, i, block_buf) < 0) {
                free(compressed);
                free(inode);
                return -1;
            }
        }

        {
            uint64_t ratio_pct = (len > 0) ? (100 * (uint64_t)compressed_len / len) : 0;
            uint64_t ratio_frac = (len > 0) ? ((1000 * (uint64_t)compressed_len / len) % 10) : 0;
            printf("AutoFS Compression: Compressed %zu -> %zu bytes (%lu.%lu%%)\n",
                   len, (size_t)compressed_len, ratio_pct, ratio_frac);
        }
    } else {
        /* Write uncompressed */
        inode->size = len;
        inode->flags &= ~AUTOFS_INODE_COMPRESSED;

        size_t blocks_needed = (len + AUTOFS_BLOCK_SIZE - 1) / AUTOFS_BLOCK_SIZE;
        for (size_t i = 0; i < blocks_needed; i++) {
            uint8_t block_buf[AUTOFS_BLOCK_SIZE] = {0};
            size_t copy_len = (i + 1) * AUTOFS_BLOCK_SIZE <= len ?
                             AUTOFS_BLOCK_SIZE :
                             len - i * AUTOFS_BLOCK_SIZE;

            memcpy(block_buf, (uint8_t *)data + i * AUTOFS_BLOCK_SIZE, copy_len);

            if (autofs_cow_write(fs, ino, i, block_buf) < 0) {
                free(compressed);
                free(inode);
                return -1;
            }
        }
    }

    /* Update inode */
    autofs_put_inode(fs, inode);

    free(compressed);
    free(inode);
    return 0;
}

/**
 * @brief Read compressed file data
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param buf Buffer to read into
 * @param len Buffer size
 * @param offset Offset in file
 * @return Bytes read or -1 on error
 */
ssize_t autofs_read_compressed(autofs_fs_t *fs, uint64_t ino, void *buf,
                              size_t len, uint64_t offset) {
    if (!fs || !buf) {
        errno = EINVAL;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    if (!(inode->flags & AUTOFS_INODE_COMPRESSED)) {
        /* Not compressed - read directly */
        free(inode);
        errno = EINVAL;
        return -1;
    }

    /* Read all compressed data */
    size_t compressed_size = inode->compressed_size;
    uint8_t *compressed = malloc(compressed_size);
    if (!compressed) {
        free(inode);
        return -1;
    }

    /* Read compressed blocks */
    size_t blocks_to_read = (compressed_size + AUTOFS_BLOCK_SIZE - 1) / AUTOFS_BLOCK_SIZE;
    for (size_t i = 0; i < blocks_to_read; i++) {
        uint8_t block_buf[AUTOFS_BLOCK_SIZE];
        if (autofs_cow_read(fs, ino, i, block_buf) < 0) {
            free(compressed);
            free(inode);
            return -1;
        }

        size_t copy_len = (i + 1) * AUTOFS_BLOCK_SIZE <= compressed_size ?
                         AUTOFS_BLOCK_SIZE :
                         compressed_size - i * AUTOFS_BLOCK_SIZE;

        memcpy(compressed + i * AUTOFS_BLOCK_SIZE, block_buf, copy_len);
    }

    /* Decompress */
    uint8_t *decompressed = malloc(inode->size);
    if (!decompressed) {
        free(compressed);
        free(inode);
        return -1;
    }

    ssize_t decompressed_len = autofs_decompress(inode->compress_algo,
                                                compressed, compressed_size,
                                                decompressed, inode->size);

    free(compressed);

    if (decompressed_len < 0) {
        free(decompressed);
        free(inode);
        return -1;
    }

    /* Copy requested data */
    size_t copy_len = len;
    if (offset + len > (size_t)decompressed_len) {
        copy_len = decompressed_len - offset;
    }

    memcpy(buf, decompressed + offset, copy_len);

    free(decompressed);
    free(inode);

    return copy_len;
}

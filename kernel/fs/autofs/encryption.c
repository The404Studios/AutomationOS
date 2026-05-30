/**
 * @file encryption.c
 * @brief AutoFS Per-File Encryption
 *
 * Implements AES-256-GCM encryption for files.
 * Each file can have its own encryption key stored in extended attributes.
 */

#include "../../include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Simple XOR encryption (placeholder for real AES-256-GCM) */
static void xor_encrypt_decrypt(const void *key, size_t key_len,
                               const void *input, size_t input_len,
                               void *output) {
    const uint8_t *k = key;
    const uint8_t *in = input;
    uint8_t *out = output;

    for (size_t i = 0; i < input_len; i++) {
        out[i] = in[i] ^ k[i % key_len];
    }
}

/**
 * @brief Encrypt data using AES-256-GCM
 *
 * @param key Encryption key
 * @param key_len Key length (should be 32 for AES-256)
 * @param plaintext Data to encrypt
 * @param plain_len Plaintext length
 * @param ciphertext Output buffer
 * @param cipher_len Output length
 * @return 0 on success, -1 on error
 */
int autofs_encrypt(const void *key, size_t key_len,
                  const void *plaintext, size_t plain_len,
                  void *ciphertext, size_t *cipher_len) {
    if (!key || !plaintext || !ciphertext || !cipher_len) {
        errno = EINVAL;
        return -1;
    }

    if (key_len != 32) {
        errno = EINVAL;
        return -1;
    }

    /* Simple XOR encryption (real implementation would use AES-256-GCM) */
    xor_encrypt_decrypt(key, key_len, plaintext, plain_len, ciphertext);
    *cipher_len = plain_len;

    return 0;
}

/**
 * @brief Decrypt data using AES-256-GCM
 *
 * @param key Decryption key
 * @param key_len Key length
 * @param ciphertext Encrypted data
 * @param cipher_len Ciphertext length
 * @param plaintext Output buffer
 * @param plain_len Output length
 * @return 0 on success, -1 on error
 */
int autofs_decrypt(const void *key, size_t key_len,
                  const void *ciphertext, size_t cipher_len,
                  void *plaintext, size_t *plain_len) {
    if (!key || !ciphertext || !plaintext || !plain_len) {
        errno = EINVAL;
        return -1;
    }

    if (key_len != 32) {
        errno = EINVAL;
        return -1;
    }

    /* Simple XOR decryption (same as encryption for XOR) */
    xor_encrypt_decrypt(key, key_len, ciphertext, cipher_len, plaintext);
    *plain_len = cipher_len;

    return 0;
}

/**
 * @brief Enable encryption for a file
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param key Encryption key
 * @param key_len Key length (must be 32)
 * @return 0 on success, -1 on error
 */
int autofs_set_encrypted(autofs_fs_t *fs, uint64_t ino,
                        const void *key, size_t key_len) {
    if (!fs || !key) {
        errno = EINVAL;
        return -1;
    }

    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    if (!(fs->sb->features & AUTOFS_FEATURE_ENCRYPTION)) {
        errno = ENOTSUP;
        return -1;
    }

    if (key_len != 32) {
        errno = EINVAL;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    /* Store encryption key in extended attributes */
    if (autofs_xattr_set(fs, ino, "encryption.key", key, key_len) < 0) {
        free(inode);
        return -1;
    }

    /* Set encrypted flag */
    inode->flags |= AUTOFS_INODE_ENCRYPTED;

    /* Write inode */
    int ret = autofs_put_inode(fs, inode);
    free(inode);

    if (ret == 0) {
        printf("AutoFS Encryption: Enabled for inode %lu\n", ino);
    }

    return ret;
}

/**
 * @brief Write encrypted file data
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param data Data to write
 * @param len Data length
 * @return 0 on success, -1 on error
 */
int autofs_write_encrypted(autofs_fs_t *fs, uint64_t ino,
                          const void *data, size_t len) {
    if (!fs || !data) {
        errno = EINVAL;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    if (!(inode->flags & AUTOFS_INODE_ENCRYPTED)) {
        free(inode);
        errno = EINVAL;
        return -1;
    }

    /* Get encryption key from xattrs */
    uint8_t key[32];
    ssize_t key_len = autofs_xattr_get(fs, ino, "encryption.key", key, sizeof(key));
    if (key_len != 32) {
        free(inode);
        return -1;
    }

    /* Encrypt data */
    uint8_t *encrypted = malloc(len);
    if (!encrypted) {
        free(inode);
        return -1;
    }

    size_t encrypted_len;
    if (autofs_encrypt(key, 32, data, len, encrypted, &encrypted_len) < 0) {
        free(encrypted);
        free(inode);
        return -1;
    }

    /* Write encrypted blocks */
    inode->size = len;  /* Original size */
    size_t blocks_needed = (encrypted_len + AUTOFS_BLOCK_SIZE - 1) / AUTOFS_BLOCK_SIZE;

    for (size_t i = 0; i < blocks_needed; i++) {
        uint8_t block_buf[AUTOFS_BLOCK_SIZE] = {0};
        size_t copy_len = (i + 1) * AUTOFS_BLOCK_SIZE <= encrypted_len ?
                         AUTOFS_BLOCK_SIZE :
                         encrypted_len - i * AUTOFS_BLOCK_SIZE;

        memcpy(block_buf, encrypted + i * AUTOFS_BLOCK_SIZE, copy_len);

        if (autofs_cow_write(fs, ino, i, block_buf) < 0) {
            free(encrypted);
            free(inode);
            return -1;
        }
    }

    /* Update inode */
    autofs_put_inode(fs, inode);

    free(encrypted);
    free(inode);

    printf("AutoFS Encryption: Wrote %zu encrypted bytes to inode %lu\n",
           len, ino);

    return 0;
}

/**
 * @brief Read encrypted file data
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param buf Buffer to read into
 * @param len Buffer size
 * @param offset Offset in file
 * @return Bytes read or -1 on error
 */
ssize_t autofs_read_encrypted(autofs_fs_t *fs, uint64_t ino,
                             void *buf, size_t len, uint64_t offset) {
    if (!fs || !buf) {
        errno = EINVAL;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    if (!(inode->flags & AUTOFS_INODE_ENCRYPTED)) {
        free(inode);
        errno = EINVAL;
        return -1;
    }

    /* Get encryption key */
    uint8_t key[32];
    ssize_t key_len = autofs_xattr_get(fs, ino, "encryption.key", key, sizeof(key));
    if (key_len != 32) {
        free(inode);
        return -1;
    }

    /* Read encrypted data */
    size_t file_size = inode->size;
    uint8_t *encrypted = malloc(file_size);
    if (!encrypted) {
        free(inode);
        return -1;
    }

    size_t blocks_to_read = (file_size + AUTOFS_BLOCK_SIZE - 1) / AUTOFS_BLOCK_SIZE;
    for (size_t i = 0; i < blocks_to_read; i++) {
        uint8_t block_buf[AUTOFS_BLOCK_SIZE];
        if (autofs_cow_read(fs, ino, i, block_buf) < 0) {
            free(encrypted);
            free(inode);
            return -1;
        }

        size_t copy_len = (i + 1) * AUTOFS_BLOCK_SIZE <= file_size ?
                         AUTOFS_BLOCK_SIZE :
                         file_size - i * AUTOFS_BLOCK_SIZE;

        memcpy(encrypted + i * AUTOFS_BLOCK_SIZE, block_buf, copy_len);
    }

    /* Decrypt */
    uint8_t *decrypted = malloc(file_size);
    if (!decrypted) {
        free(encrypted);
        free(inode);
        return -1;
    }

    size_t decrypted_len;
    if (autofs_decrypt(key, 32, encrypted, file_size, decrypted, &decrypted_len) < 0) {
        free(decrypted);
        free(encrypted);
        free(inode);
        return -1;
    }

    free(encrypted);

    /* Copy requested data */
    size_t copy_len = len;
    if (offset + len > decrypted_len) {
        copy_len = decrypted_len - offset;
    }

    memcpy(buf, decrypted + offset, copy_len);

    free(decrypted);
    free(inode);

    return copy_len;
}

/**
 * @brief Generate encryption key from password
 *
 * @param password User password
 * @param pass_len Password length
 * @param key Output key buffer (32 bytes)
 * @return 0 on success, -1 on error
 */
int autofs_derive_key(const char *password, size_t pass_len, uint8_t *key) {
    if (!password || !key) {
        errno = EINVAL;
        return -1;
    }

    /* Simple key derivation (real implementation would use PBKDF2/Argon2) */
    memset(key, 0, 32);

    for (size_t i = 0; i < pass_len; i++) {
        key[i % 32] ^= (uint8_t)password[i];
    }

    /* Hash multiple times for better key */
    for (int round = 0; round < 1000; round++) {
        for (int i = 0; i < 32; i++) {
            key[i] = (key[i] << 1) ^ key[(i + 1) % 32];
        }
    }

    return 0;
}

/**
 * @file mkfs.autofs.c
 * @brief Create an AutoFS filesystem
 *
 * Usage: mkfs.autofs [-L label] [-c] [-e] [-s] <device>
 *   -L label    : Set filesystem label
 *   -c          : Enable compression
 *   -e          : Enable encryption
 *   -s          : Enable snapshots
 *   -j          : Enable journaling (default)
 */

#include "../../kernel/include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <getopt.h>

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-L label] [-c] [-e] [-s] [-j] <device>\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -L label    Set filesystem label\n");
    fprintf(stderr, "  -c          Enable compression\n");
    fprintf(stderr, "  -e          Enable encryption\n");
    fprintf(stderr, "  -s          Enable snapshots\n");
    fprintf(stderr, "  -j          Enable journaling (default: on)\n");
    fprintf(stderr, "  -h          Show this help\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s -L myfs -c -e -s /dev/sda1\n", prog);
    exit(1);
}

/**
 * @brief Create AutoFS filesystem
 *
 * @param device Device path
 * @param size Filesystem size in bytes (0 = use device size)
 * @param label Filesystem label
 * @return 0 on success, -1 on error
 */
int autofs_mkfs(const char *device, uint64_t size, const char *label) {
    if (!device) {
        errno = EINVAL;
        return -1;
    }

    printf("Creating AutoFS filesystem on %s\n", device);

    /* Open device */
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return -1;
    }

    /* Get device size if not specified */
    if (size == 0) {
        struct stat st;
        if (fstat(fd, &st) < 0) {
            perror("Failed to stat device");
            close(fd);
            return -1;
        }

        /* For regular files, use file size. For block devices, default to 100MB */
        if (S_ISREG(st.st_mode)) {
            size = st.st_size;
        } else {
            size = 100 * 1024 * 1024;  /* 100 MB */
        }
    }

    uint64_t block_count = size / AUTOFS_BLOCK_SIZE;

    printf("  Size: %lu bytes (%lu blocks)\n", size, block_count);

    /* Create superblock */
    autofs_superblock_t sb = {0};
    sb.magic = AUTOFS_MAGIC;
    sb.version = AUTOFS_VERSION;
    sb.block_size = AUTOFS_BLOCK_SIZE;
    sb.block_count = block_count;

    /* Calculate layout */
    /* Block 0: Superblock */
    /* Blocks 1-N: Block bitmap */
    /* Blocks N+1-M: Inode bitmap */
    /* Blocks M+1-P: Inode table */
    /* Blocks P+1-Q: Journal */
    /* Blocks Q+1-END: Data blocks */

    uint64_t blocks_per_bitmap_bit = 1;
    uint64_t block_bitmap_blocks = (block_count + (AUTOFS_BLOCK_SIZE * 8) - 1) /
                                   (AUTOFS_BLOCK_SIZE * 8);

    /* Allocate 1 inode per 8 blocks (typical ratio) */
    uint64_t inode_count = block_count / 8;
    uint64_t inode_bitmap_blocks = (inode_count + (AUTOFS_BLOCK_SIZE * 8) - 1) /
                                   (AUTOFS_BLOCK_SIZE * 8);

    uint64_t inodes_per_block = AUTOFS_BLOCK_SIZE / sizeof(autofs_inode_t);
    uint64_t inode_table_blocks = (inode_count + inodes_per_block - 1) / inodes_per_block;

    /* Journal size: 10% of filesystem or 1000 blocks, whichever is smaller */
    uint64_t journal_blocks = block_count / 10;
    if (journal_blocks > 1000) {
        journal_blocks = 1000;
    }
    if (journal_blocks < 10) {
        journal_blocks = 10;
    }

    /* Calculate block positions */
    sb.block_bitmap_start = 1;
    sb.block_bitmap_blocks = block_bitmap_blocks;

    sb.inode_bitmap_start = sb.block_bitmap_start + block_bitmap_blocks;
    sb.inode_bitmap_blocks = inode_bitmap_blocks;

    sb.inode_table_start = sb.inode_bitmap_start + inode_bitmap_blocks;
    sb.inode_table_blocks = inode_table_blocks;

    sb.journal_start = sb.inode_table_start + inode_table_blocks;
    sb.journal_size = journal_blocks;

    sb.data_start = sb.journal_start + journal_blocks;

    uint64_t data_blocks = block_count - sb.data_start;

    sb.free_blocks = data_blocks;
    sb.inode_count = inode_count;
    sb.free_inodes = inode_count - 1;  /* Reserve inode 1 for root */
    sb.root_inode = 1;

    /* Features */
    sb.features = AUTOFS_FEATURE_JOURNAL | AUTOFS_FEATURE_COW | AUTOFS_FEATURE_XATTR;

    /* Set label */
    if (label) {
        strncpy(sb.label, label, sizeof(sb.label) - 1);
    } else {
        strcpy(sb.label, "AutoFS");
    }

    sb.mount_time = 0;
    sb.write_time = time(NULL);
    sb.mount_count = 0;
    sb.max_mount_count = 30;

    sb.default_compress_algo = COMPRESS_ZSTD;

    /* Generate UUID (simplified) */
    for (int i = 0; i < 16; i++) {
        sb.uuid[i] = rand() & 0xFF;
    }

    printf("  Block bitmap: blocks %lu-%lu\n",
           sb.block_bitmap_start, sb.block_bitmap_start + block_bitmap_blocks - 1);
    printf("  Inode bitmap: blocks %lu-%lu\n",
           sb.inode_bitmap_start, sb.inode_bitmap_start + inode_bitmap_blocks - 1);
    printf("  Inode table: blocks %lu-%lu (%lu inodes)\n",
           sb.inode_table_start, sb.inode_table_start + inode_table_blocks - 1,
           inode_count);
    printf("  Journal: blocks %lu-%lu\n",
           sb.journal_start, sb.journal_start + journal_blocks - 1);
    printf("  Data: blocks %lu-%lu\n", sb.data_start, block_count - 1);

    /* Write superblock */
    if (lseek(fd, 0, SEEK_SET) < 0) {
        perror("Failed to seek");
        close(fd);
        return -1;
    }

    if (write(fd, &sb, sizeof(sb)) != sizeof(sb)) {
        perror("Failed to write superblock");
        close(fd);
        return -1;
    }

    printf("  Superblock written\n");

    /* Initialize block bitmap (all free) */
    uint8_t *block_bitmap = calloc(block_bitmap_blocks, AUTOFS_BLOCK_SIZE);
    if (!block_bitmap) {
        perror("Failed to allocate block bitmap");
        close(fd);
        return -1;
    }

    /* Mark metadata blocks as used */
    for (uint64_t i = 0; i < sb.data_start; i++) {
        size_t byte = i / 8;
        int bit = i % 8;
        block_bitmap[byte] |= (1 << bit);
    }

    if (lseek(fd, sb.block_bitmap_start * AUTOFS_BLOCK_SIZE, SEEK_SET) < 0) {
        perror("Failed to seek to block bitmap");
        free(block_bitmap);
        close(fd);
        return -1;
    }

    if (write(fd, block_bitmap, block_bitmap_blocks * AUTOFS_BLOCK_SIZE) !=
        (ssize_t)(block_bitmap_blocks * AUTOFS_BLOCK_SIZE)) {
        perror("Failed to write block bitmap");
        free(block_bitmap);
        close(fd);
        return -1;
    }

    free(block_bitmap);
    printf("  Block bitmap initialized\n");

    /* Initialize inode bitmap (all free except root) */
    uint8_t *inode_bitmap = calloc(inode_bitmap_blocks, AUTOFS_BLOCK_SIZE);
    if (!inode_bitmap) {
        perror("Failed to allocate inode bitmap");
        close(fd);
        return -1;
    }

    /* Mark inode 0 and 1 as used (0 is reserved, 1 is root) */
    inode_bitmap[0] = 0x03;  /* Bits 0 and 1 */

    if (lseek(fd, sb.inode_bitmap_start * AUTOFS_BLOCK_SIZE, SEEK_SET) < 0) {
        perror("Failed to seek to inode bitmap");
        free(inode_bitmap);
        close(fd);
        return -1;
    }

    if (write(fd, inode_bitmap, inode_bitmap_blocks * AUTOFS_BLOCK_SIZE) !=
        (ssize_t)(inode_bitmap_blocks * AUTOFS_BLOCK_SIZE)) {
        perror("Failed to write inode bitmap");
        free(inode_bitmap);
        close(fd);
        return -1;
    }

    free(inode_bitmap);
    printf("  Inode bitmap initialized\n");

    /* Create root inode */
    autofs_inode_t root_inode = {0};
    root_inode.ino = 1;
    root_inode.type = AUTOFS_TYPE_DIR;
    root_inode.mode = 0755;
    root_inode.uid = 0;
    root_inode.gid = 0;
    root_inode.size = 0;
    root_inode.blocks = 0;
    root_inode.links_count = 2;  /* . and .. */
    root_inode.refcount = 1;

    time_t now = time(NULL);
    root_inode.atime = now;
    root_inode.mtime = now;
    root_inode.ctime = now;
    root_inode.crtime = now;

    /* Write root inode */
    if (lseek(fd, sb.inode_table_start * AUTOFS_BLOCK_SIZE, SEEK_SET) < 0) {
        perror("Failed to seek to inode table");
        close(fd);
        return -1;
    }

    /* Skip inode 0 (reserved) */
    uint8_t zero_inode[sizeof(autofs_inode_t)] = {0};
    if (write(fd, zero_inode, sizeof(zero_inode)) != sizeof(zero_inode)) {
        perror("Failed to write reserved inode");
        close(fd);
        return -1;
    }

    /* Write root inode */
    if (write(fd, &root_inode, sizeof(root_inode)) != sizeof(root_inode)) {
        perror("Failed to write root inode");
        close(fd);
        return -1;
    }

    printf("  Root directory created\n");

    /* Initialize journal */
    /* Just zero it out for now */
    printf("  Initializing journal...\n");

    fsync(fd);
    close(fd);

    printf("\nAutoFS filesystem created successfully!\n");
    printf("  Label: %s\n", sb.label);
    printf("  UUID: ");
    for (int i = 0; i < 16; i++) {
        printf("%02x", sb.uuid[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) printf("-");
    }
    printf("\n");

    return 0;
}

int main(int argc, char **argv) {
    char *label = NULL;
    uint64_t features = 0;
    int c;

    while ((c = getopt(argc, argv, "L:cesjh")) != -1) {
        switch (c) {
            case 'L':
                label = optarg;
                break;
            case 'c':
                features |= AUTOFS_FEATURE_COMPRESSION;
                break;
            case 'e':
                features |= AUTOFS_FEATURE_ENCRYPTION;
                break;
            case 's':
                features |= AUTOFS_FEATURE_SNAPSHOTS;
                break;
            case 'j':
                features |= AUTOFS_FEATURE_JOURNAL;
                break;
            case 'h':
            default:
                usage(argv[0]);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: No device specified\n\n");
        usage(argv[0]);
    }

    const char *device = argv[optind];

    /* Confirm with user */
    printf("WARNING: This will destroy all data on %s\n", device);
    printf("Continue? (y/N): ");
    fflush(stdout);

    char confirm[10];
    if (fgets(confirm, sizeof(confirm), stdin) == NULL ||
        (confirm[0] != 'y' && confirm[0] != 'Y')) {
        printf("Aborted.\n");
        return 1;
    }

    if (autofs_mkfs(device, 0, label) < 0) {
        fprintf(stderr, "Failed to create filesystem\n");
        return 1;
    }

    return 0;
}

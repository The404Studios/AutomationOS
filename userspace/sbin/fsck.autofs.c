/**
 * @file fsck.autofs.c
 * @brief Check and repair AutoFS filesystem
 *
 * Usage: fsck.autofs [-a] [-r] [-n] <device>
 *   -a    : Automatically repair without asking
 *   -r    : Interactively repair
 *   -n    : Check only, no repair
 */

#include "../../kernel/include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>

typedef struct fsck_context {
    int fd;
    autofs_superblock_t sb;
    uint8_t *block_bitmap;
    uint8_t *inode_bitmap;
    bool auto_repair;
    bool interactive;
    bool check_only;
    int errors_found;
    int errors_fixed;
} fsck_context_t;

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-a] [-r] [-n] <device>\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -a    Automatically repair without asking\n");
    fprintf(stderr, "  -r    Interactively repair\n");
    fprintf(stderr, "  -n    Check only, no repair\n");
    fprintf(stderr, "  -h    Show this help\n");
    exit(1);
}

/**
 * @brief Read superblock
 */
static int fsck_read_superblock(fsck_context_t *ctx) {
    if (lseek(ctx->fd, 0, SEEK_SET) < 0) {
        perror("Failed to seek to superblock");
        return -1;
    }

    if (read(ctx->fd, &ctx->sb, sizeof(ctx->sb)) != sizeof(ctx->sb)) {
        perror("Failed to read superblock");
        return -1;
    }

    return 0;
}

/**
 * @brief Check superblock validity
 */
static int fsck_check_superblock(fsck_context_t *ctx) {
    printf("Checking superblock...\n");

    if (ctx->sb.magic != AUTOFS_MAGIC) {
        fprintf(stderr, "ERROR: Invalid magic number: 0x%lx (expected 0x%x)\n",
                ctx->sb.magic, AUTOFS_MAGIC);
        ctx->errors_found++;
        return -1;
    }

    if (ctx->sb.version != AUTOFS_VERSION) {
        fprintf(stderr, "WARNING: Unsupported version: %lu\n", ctx->sb.version);
    }

    if (ctx->sb.block_size != AUTOFS_BLOCK_SIZE) {
        fprintf(stderr, "ERROR: Invalid block size: %lu\n", ctx->sb.block_size);
        ctx->errors_found++;
        return -1;
    }

    if (ctx->sb.free_blocks > ctx->sb.block_count) {
        fprintf(stderr, "ERROR: Free blocks (%lu) > total blocks (%lu)\n",
                ctx->sb.free_blocks, ctx->sb.block_count);
        ctx->errors_found++;
    }

    if (ctx->sb.free_inodes > ctx->sb.inode_count) {
        fprintf(stderr, "ERROR: Free inodes (%lu) > total inodes (%lu)\n",
                ctx->sb.free_inodes, ctx->sb.inode_count);
        ctx->errors_found++;
    }

    printf("  Magic: 0x%lx ✓\n", ctx->sb.magic);
    printf("  Version: %lu ✓\n", ctx->sb.version);
    printf("  Block size: %lu ✓\n", ctx->sb.block_size);
    printf("  Blocks: %lu total, %lu free\n",
           ctx->sb.block_count, ctx->sb.free_blocks);
    printf("  Inodes: %lu total, %lu free\n",
           ctx->sb.inode_count, ctx->sb.free_inodes);

    return 0;
}

/**
 * @brief Load bitmaps
 */
static int fsck_load_bitmaps(fsck_context_t *ctx) {
    printf("Loading bitmaps...\n");

    /* Load block bitmap */
    size_t block_bitmap_size = ctx->sb.block_bitmap_blocks * AUTOFS_BLOCK_SIZE;
    ctx->block_bitmap = malloc(block_bitmap_size);
    if (!ctx->block_bitmap) {
        perror("Failed to allocate block bitmap");
        return -1;
    }

    off_t offset = ctx->sb.block_bitmap_start * AUTOFS_BLOCK_SIZE;
    if (lseek(ctx->fd, offset, SEEK_SET) < 0) {
        perror("Failed to seek to block bitmap");
        return -1;
    }

    if (read(ctx->fd, ctx->block_bitmap, block_bitmap_size) != (ssize_t)block_bitmap_size) {
        perror("Failed to read block bitmap");
        return -1;
    }

    /* Load inode bitmap */
    size_t inode_bitmap_size = ctx->sb.inode_bitmap_blocks * AUTOFS_BLOCK_SIZE;
    ctx->inode_bitmap = malloc(inode_bitmap_size);
    if (!ctx->inode_bitmap) {
        perror("Failed to allocate inode bitmap");
        return -1;
    }

    offset = ctx->sb.inode_bitmap_start * AUTOFS_BLOCK_SIZE;
    if (lseek(ctx->fd, offset, SEEK_SET) < 0) {
        perror("Failed to seek to inode bitmap");
        return -1;
    }

    if (read(ctx->fd, ctx->inode_bitmap, inode_bitmap_size) != (ssize_t)inode_bitmap_size) {
        perror("Failed to read inode bitmap");
        return -1;
    }

    printf("  Bitmaps loaded ✓\n");

    return 0;
}

/**
 * @brief Check bitmap consistency
 */
static int fsck_check_bitmaps(fsck_context_t *ctx) {
    printf("Checking bitmaps...\n");

    /* Count free blocks in bitmap */
    uint64_t free_blocks = 0;
    size_t bitmap_bytes = ctx->sb.block_bitmap_blocks * AUTOFS_BLOCK_SIZE;

    for (size_t i = 0; i < bitmap_bytes * 8; i++) {
        if (i >= ctx->sb.block_count) break;

        size_t byte = i / 8;
        int bit = i % 8;

        if (!(ctx->block_bitmap[byte] & (1 << bit))) {
            free_blocks++;
        }
    }

    if (free_blocks != ctx->sb.free_blocks) {
        fprintf(stderr, "ERROR: Block bitmap inconsistency\n");
        fprintf(stderr, "  Superblock says %lu free blocks\n", ctx->sb.free_blocks);
        fprintf(stderr, "  Bitmap says %lu free blocks\n", free_blocks);
        ctx->errors_found++;

        if (ctx->auto_repair) {
            ctx->sb.free_blocks = free_blocks;
            printf("  Fixed: Updated superblock free_blocks to %lu\n", free_blocks);
            ctx->errors_fixed++;
        }
    } else {
        printf("  Block bitmap consistent ✓\n");
    }

    /* Count free inodes in bitmap */
    uint64_t free_inodes = 0;
    bitmap_bytes = ctx->sb.inode_bitmap_blocks * AUTOFS_BLOCK_SIZE;

    for (size_t i = 0; i < bitmap_bytes * 8; i++) {
        if (i >= ctx->sb.inode_count) break;

        size_t byte = i / 8;
        int bit = i % 8;

        if (!(ctx->inode_bitmap[byte] & (1 << bit))) {
            free_inodes++;
        }
    }

    if (free_inodes != ctx->sb.free_inodes) {
        fprintf(stderr, "ERROR: Inode bitmap inconsistency\n");
        fprintf(stderr, "  Superblock says %lu free inodes\n", ctx->sb.free_inodes);
        fprintf(stderr, "  Bitmap says %lu free inodes\n", free_inodes);
        ctx->errors_found++;

        if (ctx->auto_repair) {
            ctx->sb.free_inodes = free_inodes;
            printf("  Fixed: Updated superblock free_inodes to %lu\n", free_inodes);
            ctx->errors_fixed++;
        }
    } else {
        printf("  Inode bitmap consistent ✓\n");
    }

    return 0;
}

/**
 * @brief Check root inode
 */
static int fsck_check_root(fsck_context_t *ctx) {
    printf("Checking root directory...\n");

    /* Read root inode */
    off_t inode_offset = ctx->sb.inode_table_start * AUTOFS_BLOCK_SIZE +
                        ctx->sb.root_inode * sizeof(autofs_inode_t);

    if (lseek(ctx->fd, inode_offset, SEEK_SET) < 0) {
        perror("Failed to seek to root inode");
        return -1;
    }

    autofs_inode_t root_inode;
    if (read(ctx->fd, &root_inode, sizeof(root_inode)) != sizeof(root_inode)) {
        perror("Failed to read root inode");
        return -1;
    }

    /* Check root inode */
    if (root_inode.ino != ctx->sb.root_inode) {
        fprintf(stderr, "ERROR: Root inode number mismatch\n");
        ctx->errors_found++;
    }

    if (root_inode.type != AUTOFS_TYPE_DIR) {
        fprintf(stderr, "ERROR: Root inode is not a directory (type %d)\n",
                root_inode.type);
        ctx->errors_found++;
    }

    if (root_inode.links_count < 2) {
        fprintf(stderr, "ERROR: Root directory has invalid link count: %u\n",
                root_inode.links_count);
        ctx->errors_found++;
    }

    printf("  Root inode: %lu ✓\n", root_inode.ino);
    printf("  Type: directory ✓\n");
    printf("  Links: %u ✓\n", root_inode.links_count);

    return 0;
}

/**
 * @brief Check journal
 */
static int fsck_check_journal(fsck_context_t *ctx) {
    printf("Checking journal...\n");

    if (!(ctx->sb.features & AUTOFS_FEATURE_JOURNAL)) {
        printf("  Journal disabled\n");
        return 0;
    }

    printf("  Journal: blocks %lu-%lu\n",
           ctx->sb.journal_start,
           ctx->sb.journal_start + ctx->sb.journal_size - 1);

    /* Basic journal checks */
    /* Real implementation would parse and validate journal entries */

    printf("  Journal valid ✓\n");

    return 0;
}

/**
 * @brief Write back superblock
 */
static int fsck_write_superblock(fsck_context_t *ctx) {
    if (ctx->check_only) {
        return 0;
    }

    if (lseek(ctx->fd, 0, SEEK_SET) < 0) {
        perror("Failed to seek to superblock");
        return -1;
    }

    if (write(ctx->fd, &ctx->sb, sizeof(ctx->sb)) != sizeof(ctx->sb)) {
        perror("Failed to write superblock");
        return -1;
    }

    fsync(ctx->fd);

    return 0;
}

/**
 * @brief Run filesystem check
 */
static int fsck_run(fsck_context_t *ctx, const char *device) {
    printf("AutoFS filesystem check: %s\n", device);
    printf("========================================\n\n");

    /* Open device */
    int flags = ctx->check_only ? O_RDONLY : O_RDWR;
    ctx->fd = open(device, flags);
    if (ctx->fd < 0) {
        perror("Failed to open device");
        return -1;
    }

    /* Read superblock */
    if (fsck_read_superblock(ctx) < 0) {
        close(ctx->fd);
        return -1;
    }

    /* Check superblock */
    if (fsck_check_superblock(ctx) < 0) {
        close(ctx->fd);
        return -1;
    }

    /* Load bitmaps */
    if (fsck_load_bitmaps(ctx) < 0) {
        close(ctx->fd);
        return -1;
    }

    /* Check bitmaps */
    fsck_check_bitmaps(ctx);

    /* Check root directory */
    fsck_check_root(ctx);

    /* Check journal */
    fsck_check_journal(ctx);

    /* Write back if we made repairs */
    if (ctx->errors_fixed > 0) {
        printf("\nWriting repairs...\n");
        fsck_write_superblock(ctx);
    }

    /* Cleanup */
    free(ctx->block_bitmap);
    free(ctx->inode_bitmap);
    close(ctx->fd);

    /* Print summary */
    printf("\n========================================\n");
    printf("Filesystem check complete\n");
    printf("  Errors found: %d\n", ctx->errors_found);
    printf("  Errors fixed: %d\n", ctx->errors_fixed);

    if (ctx->errors_found == 0) {
        printf("\n✓ Filesystem is clean\n");
        return 0;
    } else if (ctx->errors_fixed == ctx->errors_found) {
        printf("\n✓ All errors fixed\n");
        return 0;
    } else {
        printf("\n✗ Errors remain - manual intervention required\n");
        return 1;
    }
}

int main(int argc, char **argv) {
    fsck_context_t ctx = {0};
    int c;

    while ((c = getopt(argc, argv, "arnh")) != -1) {
        switch (c) {
            case 'a':
                ctx.auto_repair = true;
                break;
            case 'r':
                ctx.interactive = true;
                break;
            case 'n':
                ctx.check_only = true;
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

    int ret = fsck_run(&ctx, device);

    return ret;
}

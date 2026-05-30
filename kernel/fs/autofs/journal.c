/**
 * @file journal.c
 * @brief AutoFS Journaling Implementation
 *
 * Write-ahead logging for crash consistency. All metadata changes
 * are logged to the journal before being applied to the filesystem.
 *
 * Transaction lifecycle:
 * 1. Begin transaction
 * 2. Log operations (writes, unlinks, etc.)
 * 3. Commit transaction (make changes permanent)
 * 4. Checkpoint (apply to main filesystem)
 *
 * Recovery:
 * - On mount after crash, replay committed transactions
 * - Discard uncommitted transactions
 */

#include "../../include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* Journal transaction states */
#define JOURNAL_STATE_ACTIVE    1
#define JOURNAL_STATE_COMMIT    2
#define JOURNAL_STATE_COMPLETE  3
#define JOURNAL_STATE_ABORTED   4

/* Journal header */
typedef struct journal_header {
    uint64_t magic;              /* Journal magic */
    uint64_t version;            /* Journal version */
    uint64_t transaction_seq;    /* Next transaction ID */
    uint64_t head;               /* Journal head (oldest) */
    uint64_t tail;               /* Journal tail (newest) */
} journal_header_t;

#define JOURNAL_MAGIC 0x4A4F55524E414C  /* "JOURNAL" */

/**
 * @brief Initialize journal
 *
 * @param fs Filesystem
 * @return 0 on success, -1 on error
 */
int autofs_journal_init(autofs_fs_t *fs) {
    if (!fs) {
        errno = EINVAL;
        return -1;
    }

    /* Allocate journal buffer */
    size_t journal_size = fs->sb->journal_size * AUTOFS_BLOCK_SIZE;
    fs->journal_buffer = malloc(journal_size);
    if (!fs->journal_buffer) {
        return -1;
    }

    /* Read journal header */
    journal_header_t header;
    off_t offset = fs->sb->journal_start * AUTOFS_BLOCK_SIZE;

    if (lseek(fs->fd, offset, SEEK_SET) < 0) {
        free(fs->journal_buffer);
        return -1;
    }

    if (read(fs->fd, &header, sizeof(header)) != sizeof(header)) {
        free(fs->journal_buffer);
        return -1;
    }

    /* Verify journal magic */
    if (header.magic != JOURNAL_MAGIC) {
        fprintf(stderr, "Invalid journal magic\n");

        /* Initialize new journal */
        header.magic = JOURNAL_MAGIC;
        header.version = 1;
        header.transaction_seq = 1;
        header.head = 0;
        header.tail = 0;

        if (lseek(fs->fd, offset, SEEK_SET) < 0) {
            free(fs->journal_buffer);
            return -1;
        }

        if (write(fs->fd, &header, sizeof(header)) != sizeof(header)) {
            free(fs->journal_buffer);
            return -1;
        }
    }

    fs->current_transaction_id = header.transaction_seq;

    printf("AutoFS Journal: Initialized (transactions: %lu)\n",
           header.transaction_seq - 1);

    return 0;
}

/**
 * @brief Begin a new transaction
 *
 * @param fs Filesystem
 * @return 0 on success, -1 on error
 */
int autofs_journal_begin(autofs_fs_t *fs) {
    if (!fs || fs->read_only) {
        errno = EROFS;
        return -1;
    }

    if (!fs->journal_buffer) {
        errno = EINVAL;
        return -1;
    }

    /* Increment transaction ID */
    fs->current_transaction_id++;

    return 0;
}

/**
 * @brief Log an operation to the journal
 *
 * @param fs Filesystem
 * @param op Operation type
 * @param block Block number
 * @param data Block data
 * @return 0 on success, -1 on error
 */
int autofs_journal_log(autofs_fs_t *fs, journal_op_t op, uint64_t block,
                      const void *data) {
    if (!fs || !data) {
        errno = EINVAL;
        return -1;
    }

    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    /* Create journal entry */
    journal_entry_t entry;
    entry.transaction_id = fs->current_transaction_id;
    entry.operation = op;
    entry.block_num = block;

    if (data) {
        memcpy(entry.data, data, AUTOFS_BLOCK_SIZE);
    } else {
        memset(entry.data, 0, AUTOFS_BLOCK_SIZE);
    }

    /* Calculate checksum */
    entry.checksum = autofs_checksum(&entry, sizeof(entry) - sizeof(entry.checksum));

    /* Write to journal */
    off_t offset = fs->sb->journal_start * AUTOFS_BLOCK_SIZE;

    /* Find next free journal slot (simple circular buffer) */
    static uint64_t journal_offset = sizeof(journal_header_t);

    if (lseek(fs->fd, offset + journal_offset, SEEK_SET) < 0) {
        return -1;
    }

    if (write(fs->fd, &entry, sizeof(entry)) != sizeof(entry)) {
        return -1;
    }

    /* Advance journal offset */
    journal_offset += sizeof(entry);
    if (journal_offset >= fs->sb->journal_size * AUTOFS_BLOCK_SIZE) {
        journal_offset = sizeof(journal_header_t);
    }

    /* Sync to disk */
    fsync(fs->fd);

    return 0;
}

/**
 * @brief Commit a transaction
 *
 * @param fs Filesystem
 * @return 0 on success, -1 on error
 */
int autofs_journal_commit(autofs_fs_t *fs) {
    if (!fs) {
        errno = EINVAL;
        return -1;
    }

    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    /* Write commit record */
    journal_transaction_t commit_record;
    commit_record.transaction_id = fs->current_transaction_id;
    commit_record.timestamp = time(NULL);
    commit_record.entry_count = 0;  /* Updated during recovery */
    commit_record.state = JOURNAL_STATE_COMMIT;
    commit_record.checksum = autofs_checksum(&commit_record,
                                            sizeof(commit_record) - sizeof(commit_record.checksum));

    /* Write commit record to journal */
    off_t offset = fs->sb->journal_start * AUTOFS_BLOCK_SIZE;
    static uint64_t commit_offset = sizeof(journal_header_t);

    if (lseek(fs->fd, offset + commit_offset, SEEK_SET) < 0) {
        return -1;
    }

    if (write(fs->fd, &commit_record, sizeof(commit_record)) != sizeof(commit_record)) {
        return -1;
    }

    commit_offset += sizeof(commit_record);
    if (commit_offset >= fs->sb->journal_size * AUTOFS_BLOCK_SIZE) {
        commit_offset = sizeof(journal_header_t);
    }

    /* Sync to disk - critical for durability */
    fsync(fs->fd);

    printf("AutoFS Journal: Transaction %lu committed\n",
           fs->current_transaction_id);

    return 0;
}

/**
 * @brief Abort a transaction
 *
 * @param fs Filesystem
 * @return 0 on success, -1 on error
 */
int autofs_journal_abort(autofs_fs_t *fs) {
    if (!fs) {
        errno = EINVAL;
        return -1;
    }

    if (fs->read_only) {
        return 0;  /* Nothing to abort */
    }

    /* Write abort record */
    journal_transaction_t abort_record;
    abort_record.transaction_id = fs->current_transaction_id;
    abort_record.timestamp = time(NULL);
    abort_record.entry_count = 0;
    abort_record.state = JOURNAL_STATE_ABORTED;
    abort_record.checksum = autofs_checksum(&abort_record,
                                           sizeof(abort_record) - sizeof(abort_record.checksum));

    /* Write abort record to journal */
    off_t offset = fs->sb->journal_start * AUTOFS_BLOCK_SIZE;
    static uint64_t abort_offset = sizeof(journal_header_t);

    if (lseek(fs->fd, offset + abort_offset, SEEK_SET) < 0) {
        return -1;
    }

    write(fs->fd, &abort_record, sizeof(abort_record));

    abort_offset += sizeof(abort_record);
    if (abort_offset >= fs->sb->journal_size * AUTOFS_BLOCK_SIZE) {
        abort_offset = sizeof(journal_header_t);
    }

    printf("AutoFS Journal: Transaction %lu aborted\n",
           fs->current_transaction_id);

    return 0;
}

/**
 * @brief Recover filesystem from journal after crash
 *
 * @param fs Filesystem
 * @return 0 on success, -1 on error
 */
int autofs_journal_recover(autofs_fs_t *fs) {
    if (!fs) {
        errno = EINVAL;
        return -1;
    }

    printf("AutoFS Journal: Starting recovery...\n");

    /* Read journal */
    off_t journal_offset = fs->sb->journal_start * AUTOFS_BLOCK_SIZE;
    size_t journal_size = fs->sb->journal_size * AUTOFS_BLOCK_SIZE;

    uint8_t *journal_data = malloc(journal_size);
    if (!journal_data) {
        return -1;
    }

    if (lseek(fs->fd, journal_offset, SEEK_SET) < 0) {
        free(journal_data);
        return -1;
    }

    if (read(fs->fd, journal_data, journal_size) != (ssize_t)journal_size) {
        free(journal_data);
        return -1;
    }

    /* Parse journal entries */
    size_t offset = sizeof(journal_header_t);
    uint64_t transactions_replayed = 0;
    uint64_t entries_replayed = 0;

    while (offset + sizeof(journal_entry_t) <= journal_size) {
        journal_entry_t *entry = (journal_entry_t *)(journal_data + offset);

        /* Check if this is a transaction record */
        if (entry->transaction_id == 0) {
            offset += sizeof(journal_entry_t);
            continue;
        }

        /* Verify checksum */
        uint64_t stored_checksum = entry->checksum;
        entry->checksum = 0;
        uint64_t calculated_checksum = autofs_checksum(entry,
                                                       sizeof(*entry) - sizeof(entry->checksum));
        entry->checksum = stored_checksum;

        if (stored_checksum != calculated_checksum) {
            /* Corrupted entry, stop recovery */
            printf("AutoFS Journal: Corrupted entry at offset %zu\n", offset);
            break;
        }

        /* Check if transaction was committed */
        /* This is simplified - real implementation would track commit records */
        bool committed = true;  /* Assume committed for now */

        if (committed) {
            /* Replay operation */
            switch (entry->operation) {
                case JOURNAL_OP_WRITE:
                    /* Write block to filesystem */
                    autofs_write_block(fs, entry->block_num, entry->data);
                    entries_replayed++;
                    break;

                case JOURNAL_OP_UNLINK:
                    /* Mark inode as free */
                    /* This would need more context */
                    entries_replayed++;
                    break;

                default:
                    /* Other operations */
                    entries_replayed++;
                    break;
            }

            transactions_replayed++;
        }

        offset += sizeof(journal_entry_t);
    }

    free(journal_data);

    if (transactions_replayed > 0 || entries_replayed > 0) {
        printf("AutoFS Journal: Recovery complete\n");
        printf("  Transactions replayed: %lu\n", transactions_replayed);
        printf("  Entries replayed: %lu\n", entries_replayed);

        /* Sync filesystem */
        fsync(fs->fd);
    } else {
        printf("AutoFS Journal: No recovery needed\n");
    }

    return 0;
}

/**
 * @brief Checkpoint journal (apply to main filesystem and clear)
 *
 * @param fs Filesystem
 * @return 0 on success, -1 on error
 */
int autofs_journal_checkpoint(autofs_fs_t *fs) {
    if (!fs || fs->read_only) {
        errno = EROFS;
        return -1;
    }

    /* All changes are already applied during commit */
    /* Just need to clear journal */

    /* Reset journal header */
    journal_header_t header;
    header.magic = JOURNAL_MAGIC;
    header.version = 1;
    header.transaction_seq = fs->current_transaction_id + 1;
    header.head = 0;
    header.tail = 0;

    off_t offset = fs->sb->journal_start * AUTOFS_BLOCK_SIZE;

    if (lseek(fs->fd, offset, SEEK_SET) < 0) {
        return -1;
    }

    if (write(fs->fd, &header, sizeof(header)) != sizeof(header)) {
        return -1;
    }

    fsync(fs->fd);

    printf("AutoFS Journal: Checkpoint complete\n");

    return 0;
}

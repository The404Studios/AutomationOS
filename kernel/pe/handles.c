/**
 * Windows Handle Management
 *
 * Implements Windows-style handle table for managing kernel objects.
 * Maps handles to file descriptors, threads, mutexes, etc.
 */

#include <kernel/pe_win32.h>
#include <string.h>
#include <stdio.h>

/**
 * Initialize handle table
 */
void handle_table_init(win32_handle_table_t *table) {
    if (!table) return;

    memset(table, 0, sizeof(win32_handle_table_t));
    mutex_init(&table->lock);
    table->next_handle = 1; // Start at 1, reserve 0 for NULL

    printf("HandleTable: Initialized (%u max handles)\n", MAX_HANDLES);
}

/**
 * Create handle
 */
HANDLE handle_create(win32_handle_table_t *table, void *object, handle_type_t type) {
    if (!table || !object) {
        return INVALID_HANDLE_VALUE;
    }

    mutex_lock(&table->lock);

    // Find free slot
    uint32_t index = 0;
    bool found = false;

    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        uint32_t idx = (table->next_handle + i) % MAX_HANDLES;
        if (!table->entries[idx].in_use) {
            index = idx;
            found = true;
            table->next_handle = (idx + 1) % MAX_HANDLES;
            break;
        }
    }

    if (!found) {
        mutex_unlock(&table->lock);
        printf("HandleTable: No free handles\n");
        return INVALID_HANDLE_VALUE;
    }

    // Create handle entry
    handle_entry_t *entry = &table->entries[index];
    entry->object = object;
    entry->type = type;
    entry->access_mask = 0xFFFFFFFF;
    entry->ref_count = 1;
    entry->in_use = true;

    mutex_unlock(&table->lock);

    // Handle value is index + 1 (to avoid NULL)
    HANDLE handle = (HANDLE)(uint64_t)(index + 1);

    const char *type_names[] = {
        "FILE", "THREAD", "PROCESS", "MUTEX", "SEMAPHORE", "EVENT", "PIPE", "SOCKET"
    };
    const char *type_name = (type < 8) ? type_names[type] : "UNKNOWN";

    printf("HandleTable: Created handle %p (type %s, object %p)\n",
           handle, type_name, object);

    return handle;
}

/**
 * Get object from handle
 */
void* handle_get_object(win32_handle_table_t *table, HANDLE handle) {
    handle_entry_t *entry = handle_get_entry(table, handle);
    return entry ? entry->object : NULL;
}

/**
 * Get handle entry
 */
handle_entry_t* handle_get_entry(win32_handle_table_t *table, HANDLE handle) {
    if (!table || handle == INVALID_HANDLE_VALUE || handle == NULL) {
        return NULL;
    }

    // Convert handle to index
    uint64_t index = (uint64_t)handle - 1;

    if (index >= MAX_HANDLES) {
        printf("HandleTable: Invalid handle %p (index %lu out of range)\n",
               handle, index);
        return NULL;
    }

    mutex_lock(&table->lock);

    handle_entry_t *entry = &table->entries[index];

    if (!entry->in_use) {
        mutex_unlock(&table->lock);
        printf("HandleTable: Handle %p not in use\n", handle);
        return NULL;
    }

    mutex_unlock(&table->lock);

    return entry;
}

/**
 * Close handle
 */
void handle_close(win32_handle_table_t *table, HANDLE handle) {
    if (!table || handle == INVALID_HANDLE_VALUE || handle == NULL) {
        return;
    }

    uint64_t index = (uint64_t)handle - 1;

    if (index >= MAX_HANDLES) {
        return;
    }

    mutex_lock(&table->lock);

    handle_entry_t *entry = &table->entries[index];

    if (!entry->in_use) {
        mutex_unlock(&table->lock);
        return;
    }

    entry->ref_count--;

    if (entry->ref_count == 0) {
        // Free handle
        printf("HandleTable: Closed handle %p\n", handle);
        memset(entry, 0, sizeof(handle_entry_t));
    }

    mutex_unlock(&table->lock);
}

/**
 * Duplicate handle
 */
HANDLE handle_duplicate(win32_handle_table_t *table, HANDLE handle) {
    if (!table || handle == INVALID_HANDLE_VALUE || handle == NULL) {
        return INVALID_HANDLE_VALUE;
    }

    uint64_t index = (uint64_t)handle - 1;

    if (index >= MAX_HANDLES) {
        return INVALID_HANDLE_VALUE;
    }

    mutex_lock(&table->lock);

    handle_entry_t *entry = &table->entries[index];

    if (!entry->in_use) {
        mutex_unlock(&table->lock);
        return INVALID_HANDLE_VALUE;
    }

    entry->ref_count++;

    mutex_unlock(&table->lock);

    printf("HandleTable: Duplicated handle %p (ref_count=%u)\n",
           handle, entry->ref_count);

    return handle;
}

/**
 * Get handle count
 */
uint32_t handle_get_count(win32_handle_table_t *table) {
    if (!table) return 0;

    mutex_lock(&table->lock);

    uint32_t count = 0;
    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        if (table->entries[i].in_use) {
            count++;
        }
    }

    mutex_unlock(&table->lock);

    return count;
}

/**
 * Print handle table (debug)
 */
void handle_print_table(win32_handle_table_t *table) {
    if (!table) return;

    printf("=== Handle Table ===\n");

    mutex_lock(&table->lock);

    const char *type_names[] = {
        "FILE", "THREAD", "PROCESS", "MUTEX", "SEMAPHORE", "EVENT", "PIPE", "SOCKET"
    };

    uint32_t count = 0;
    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        handle_entry_t *entry = &table->entries[i];

        if (entry->in_use) {
            HANDLE handle = (HANDLE)(uint64_t)(i + 1);
            const char *type_name = (entry->type < 8) ? type_names[entry->type] : "UNKNOWN";

            printf("Handle %p: %s object=%p refs=%u\n",
                   handle, type_name, entry->object, entry->ref_count);

            count++;
        }
    }

    mutex_unlock(&table->lock);

    printf("Total: %u handles\n", count);
    printf("====================\n");
}

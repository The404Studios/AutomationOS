#include "../include/audit.h"
#include "../include/kernel.h"
#include "../include/mem.h"

extern void* kmalloc(size_t size);
extern void kfree(void* ptr);
extern void* memset(void* ptr, int value, size_t size);
extern void* memcpy(void* dest, const void* src, size_t n);

// Simple spinlock implementation
static inline void spin_lock(uint32_t* lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        // Spin
        __asm__ volatile("pause");
    }
}

static inline void spin_unlock(uint32_t* lock) {
    __sync_lock_release(lock);
}

audit_buffer_t* audit_buffer_create(void) {
    audit_buffer_t* buffer = (audit_buffer_t*)kmalloc(sizeof(audit_buffer_t));
    if (!buffer) {
        kprintf("[AUDIT] Failed to allocate buffer structure\n");
        return NULL;
    }

    buffer->events = (audit_event_t*)kmalloc(sizeof(audit_event_t) * AUDIT_BUFFER_SIZE);
    if (!buffer->events) {
        kprintf("[AUDIT] Failed to allocate buffer events\n");
        kfree(buffer);
        return NULL;
    }

    memset(buffer->events, 0, sizeof(audit_event_t) * AUDIT_BUFFER_SIZE);
    buffer->head = 0;
    buffer->tail = 0;
    buffer->count = 0;
    buffer->dropped = 0;
    buffer->sequence = 0;
    buffer->last_hash = 0x1337DEADBEEF0000ULL;  // Initial hash seed
    buffer->lock = 0;

    kprintf("[AUDIT] Ring buffer created (size: %d events, %llu bytes)\n",
            AUDIT_BUFFER_SIZE,
            (uint64_t)(sizeof(audit_event_t) * AUDIT_BUFFER_SIZE));

    return buffer;
}

void audit_buffer_destroy(audit_buffer_t* buffer) {
    if (!buffer) return;

    if (buffer->events) {
        kfree(buffer->events);
    }
    kfree(buffer);

    kprintf("[AUDIT] Ring buffer destroyed\n");
}

int audit_buffer_write(audit_buffer_t* buffer, audit_event_t* event) {
    if (!buffer || !event) {
        return -1;
    }

    spin_lock(&buffer->lock);

    // Check if buffer is full
    if (buffer->count >= AUDIT_BUFFER_SIZE) {
        // Overwrite oldest entry (circular buffer behavior)
        buffer->tail = (buffer->tail + 1) % AUDIT_BUFFER_SIZE;
        buffer->dropped++;
        audit_stats.events_dropped++;
    } else {
        buffer->count++;
    }

    // Assign sequence number
    event->sequence = buffer->sequence++;

    // Compute hash for tamper detection (hash chain)
    event->prev_hash = buffer->last_hash;
    event->hash = audit_hash_event(event);
    buffer->last_hash = event->hash;

    // Write event to ring buffer
    memcpy(&buffer->events[buffer->head], event, sizeof(audit_event_t));

    // Advance head
    buffer->head = (buffer->head + 1) % AUDIT_BUFFER_SIZE;

    spin_unlock(&buffer->lock);

    return 0;
}

int audit_buffer_read(audit_buffer_t* buffer, audit_event_t* event) {
    if (!buffer || !event) {
        return -1;
    }

    spin_lock(&buffer->lock);

    // Check if buffer is empty
    if (buffer->count == 0) {
        spin_unlock(&buffer->lock);
        return -1;  // No events available
    }

    // Read event from tail
    memcpy(event, &buffer->events[buffer->tail], sizeof(audit_event_t));

    // Advance tail
    buffer->tail = (buffer->tail + 1) % AUDIT_BUFFER_SIZE;
    buffer->count--;

    spin_unlock(&buffer->lock);

    return 0;
}

uint32_t audit_buffer_count(audit_buffer_t* buffer) {
    if (!buffer) return 0;

    spin_lock(&buffer->lock);
    uint32_t count = buffer->count;
    spin_unlock(&buffer->lock);

    return count;
}

bool audit_buffer_is_full(audit_buffer_t* buffer) {
    if (!buffer) return true;
    return buffer->count >= AUDIT_BUFFER_SIZE;
}

// Batch read for userspace tools (reads multiple events at once)
int audit_buffer_read_batch(audit_buffer_t* buffer, audit_event_t* events,
                           uint32_t max_count, uint32_t* actual_count) {
    if (!buffer || !events || !actual_count) {
        return -1;
    }

    spin_lock(&buffer->lock);

    uint32_t count = 0;
    while (count < max_count && buffer->count > 0) {
        memcpy(&events[count], &buffer->events[buffer->tail], sizeof(audit_event_t));
        buffer->tail = (buffer->tail + 1) % AUDIT_BUFFER_SIZE;
        buffer->count--;
        count++;
    }

    *actual_count = count;

    spin_unlock(&buffer->lock);

    return 0;
}

// Peek at events without removing them (for real-time monitoring)
int audit_buffer_peek(audit_buffer_t* buffer, audit_event_t* event, uint32_t offset) {
    if (!buffer || !event) {
        return -1;
    }

    spin_lock(&buffer->lock);

    if (offset >= buffer->count) {
        spin_unlock(&buffer->lock);
        return -1;  // Offset beyond available events
    }

    uint32_t pos = (buffer->tail + offset) % AUDIT_BUFFER_SIZE;
    memcpy(event, &buffer->events[pos], sizeof(audit_event_t));

    spin_unlock(&buffer->lock);

    return 0;
}

// Clear all events (privileged operation)
void audit_buffer_clear(audit_buffer_t* buffer) {
    if (!buffer) return;

    spin_lock(&buffer->lock);

    buffer->head = 0;
    buffer->tail = 0;
    buffer->count = 0;
    // Preserve dropped count and sequence for forensics

    spin_unlock(&buffer->lock);

    kprintf("[AUDIT] Ring buffer cleared\n");
}

// Get buffer statistics
void audit_buffer_get_stats(audit_buffer_t* buffer,
                           uint32_t* count, uint64_t* dropped,
                           uint64_t* sequence) {
    if (!buffer) return;

    spin_lock(&buffer->lock);

    if (count) *count = buffer->count;
    if (dropped) *dropped = buffer->dropped;
    if (sequence) *sequence = buffer->sequence;

    spin_unlock(&buffer->lock);
}

// Verify hash chain integrity
bool audit_buffer_verify_integrity(audit_buffer_t* buffer) {
    if (!buffer) return false;

    spin_lock(&buffer->lock);

    bool valid = true;
    uint64_t expected_hash = 0x1337DEADBEEF0000ULL;

    for (uint32_t i = 0; i < buffer->count; i++) {
        uint32_t pos = (buffer->tail + i) % AUDIT_BUFFER_SIZE;
        audit_event_t* event = &buffer->events[pos];

        // Check if previous hash matches
        if (event->prev_hash != expected_hash) {
            kprintf("[AUDIT] Hash chain broken at sequence %llu\n", event->sequence);
            valid = false;
            break;
        }

        // Recompute hash and verify
        uint64_t computed_hash = audit_hash_event(event);
        if (computed_hash != event->hash) {
            kprintf("[AUDIT] Event hash mismatch at sequence %llu\n", event->sequence);
            valid = false;
            break;
        }

        expected_hash = event->hash;
    }

    spin_unlock(&buffer->lock);

    return valid;
}

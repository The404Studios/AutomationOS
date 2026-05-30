#include "../../include/rlimit.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

extern uint64_t get_timer_ticks(void);
extern void memset(void* ptr, int value, size_t num);
extern void memcpy(void* dest, const void* src, size_t n);

// Initialize resource limit subsystem
void rlimit_init(void) {
    kprintf("[RLIMIT] Initializing resource limit subsystem...\n");
    kprintf("[RLIMIT] Default limits:\n");
    kprintf("  CPU: %llu ms\n", RLIMIT_DEFAULT_CPU);
    kprintf("  Memory: %llu MB\n", RLIMIT_DEFAULT_MEMORY / (1024*1024));
    kprintf("  File descriptors: %u\n", RLIMIT_DEFAULT_NOFILE);
    kprintf("  Network bandwidth: %llu MB/s\n", RLIMIT_DEFAULT_NET_BW / (1024*1024));
    kprintf("[RLIMIT] Resource limit subsystem initialized\n");
}

// Create new resource limit container with default limits
rlimit_container_t* rlimit_create_container(void) {
    rlimit_container_t* rl = (rlimit_container_t*)kmalloc(sizeof(rlimit_container_t));
    if (!rl) {
        kprintf("[RLIMIT] Failed to allocate resource limit container\n");
        return NULL;
    }

    // Initialize all limits to defaults
    for (int i = 0; i < RLIMIT_MAX; i++) {
        rl->limits[i].soft = RLIMIT_INFINITY;
        rl->limits[i].hard = RLIMIT_INFINITY;
        rl->soft_limit_signaled[i] = false;
    }

    // Set default limits
    rl->limits[RLIMIT_CPU].soft = RLIMIT_DEFAULT_CPU;
    rl->limits[RLIMIT_CPU].hard = RLIMIT_DEFAULT_CPU * 2;

    rl->limits[RLIMIT_MEMORY].soft = RLIMIT_DEFAULT_MEMORY;
    rl->limits[RLIMIT_MEMORY].hard = RLIMIT_DEFAULT_MEMORY * 2;

    rl->limits[RLIMIT_RSS].soft = RLIMIT_DEFAULT_MEMORY;
    rl->limits[RLIMIT_RSS].hard = RLIMIT_DEFAULT_MEMORY * 2;

    rl->limits[RLIMIT_VMEM].soft = RLIMIT_DEFAULT_MEMORY * 2;
    rl->limits[RLIMIT_VMEM].hard = RLIMIT_DEFAULT_MEMORY * 4;

    rl->limits[RLIMIT_NOFILE].soft = RLIMIT_DEFAULT_NOFILE;
    rl->limits[RLIMIT_NOFILE].hard = RLIMIT_DEFAULT_NOFILE * 2;

    rl->limits[RLIMIT_NPROC].soft = RLIMIT_DEFAULT_NPROC;
    rl->limits[RLIMIT_NPROC].hard = RLIMIT_DEFAULT_NPROC;

    rl->limits[RLIMIT_NET_RX].soft = RLIMIT_INFINITY;
    rl->limits[RLIMIT_NET_RX].hard = RLIMIT_INFINITY;

    rl->limits[RLIMIT_NET_TX].soft = RLIMIT_INFINITY;
    rl->limits[RLIMIT_NET_TX].hard = RLIMIT_INFINITY;

    // Initialize usage statistics
    memset(&rl->usage, 0, sizeof(rusage_t));

    // Initialize CPU quota with defaults
    rl->cpu_quota.shares = CPU_SHARES_DEFAULT;
    rl->cpu_quota.quota_us = 0;  // No quota by default
    rl->cpu_quota.period_us = CPU_PERIOD_DEFAULT;
    rl->cpu_quota.used_us = 0;
    rl->cpu_quota.period_start = get_timer_ticks();
    rl->cpu_quota.throttled = false;

    // Initialize token buckets
    token_bucket_init(&rl->net_rx_bucket, RLIMIT_DEFAULT_NET_BW, RLIMIT_DEFAULT_NET_BW);
    token_bucket_init(&rl->net_tx_bucket, RLIMIT_DEFAULT_NET_BW, RLIMIT_DEFAULT_NET_BW);
    token_bucket_init(&rl->disk_read_bucket, RLIMIT_DEFAULT_DISK_BW, RLIMIT_DEFAULT_DISK_BW);
    token_bucket_init(&rl->disk_write_bucket, RLIMIT_DEFAULT_DISK_BW, RLIMIT_DEFAULT_DISK_BW);

    return rl;
}

// Destroy resource limit container
void rlimit_destroy_container(rlimit_container_t* rl) {
    if (!rl) return;
    kfree(rl);
}

// Inherit resource limits from parent process
rlimit_container_t* rlimit_inherit_container(rlimit_container_t* parent) {
    if (!parent) {
        return rlimit_create_container();
    }

    rlimit_container_t* child = (rlimit_container_t*)kmalloc(sizeof(rlimit_container_t));
    if (!child) {
        kprintf("[RLIMIT] Failed to allocate child resource limit container\n");
        return NULL;
    }

    // Copy all limits from parent
    memcpy(child, parent, sizeof(rlimit_container_t));

    // Reset usage statistics (child starts fresh)
    memset(&child->usage, 0, sizeof(rusage_t));

    // Reset soft limit signals
    for (int i = 0; i < RLIMIT_MAX; i++) {
        child->soft_limit_signaled[i] = false;
    }

    // Reset CPU quota usage
    child->cpu_quota.used_us = 0;
    child->cpu_quota.period_start = get_timer_ticks();
    child->cpu_quota.throttled = false;

    // Reinitialize token buckets (start full)
    token_bucket_init(&child->net_rx_bucket,
                     parent->net_rx_bucket.rate,
                     parent->net_rx_bucket.capacity);
    token_bucket_init(&child->net_tx_bucket,
                     parent->net_tx_bucket.rate,
                     parent->net_tx_bucket.capacity);
    token_bucket_init(&child->disk_read_bucket,
                     parent->disk_read_bucket.rate,
                     parent->disk_read_bucket.capacity);
    token_bucket_init(&child->disk_write_bucket,
                     parent->disk_write_bucket.rate,
                     parent->disk_write_bucket.capacity);

    return child;
}

// Get resource limit
int rlimit_get(rlimit_container_t* rl, rlimit_type_t type, rlimit_t* out) {
    if (!rl || !out || type >= RLIMIT_MAX) {
        return -1;
    }

    memcpy(out, &rl->limits[type], sizeof(rlimit_t));
    return 0;
}

// Set resource limit
int rlimit_set(rlimit_container_t* rl, rlimit_type_t type, const rlimit_t* limit) {
    if (!rl || !limit || type >= RLIMIT_MAX) {
        return -1;
    }

    // Validate: soft <= hard
    if (limit->soft != RLIMIT_INFINITY &&
        limit->hard != RLIMIT_INFINITY &&
        limit->soft > limit->hard) {
        kprintf("[RLIMIT] Invalid limit: soft (%llu) > hard (%llu)\n",
                limit->soft, limit->hard);
        return -1;
    }

    memcpy(&rl->limits[type], limit, sizeof(rlimit_t));

    // Reset soft limit signal flag
    rl->soft_limit_signaled[type] = false;

    // Update token buckets for bandwidth limits
    uint64_t current_time = get_timer_ticks();
    if (type == RLIMIT_NET_RX && limit->hard != RLIMIT_INFINITY) {
        token_bucket_init(&rl->net_rx_bucket, limit->hard, limit->hard);
    }
    if (type == RLIMIT_NET_TX && limit->hard != RLIMIT_INFINITY) {
        token_bucket_init(&rl->net_tx_bucket, limit->hard, limit->hard);
    }

    kprintf("[RLIMIT] Set limit type %d: soft=%llu hard=%llu\n",
            type, limit->soft, limit->hard);

    return 0;
}

// Get current resource usage
void rlimit_get_usage(rlimit_container_t* rl, rusage_t* out) {
    if (!rl || !out) return;
    memcpy(out, &rl->usage, sizeof(rusage_t));
}

// Print resource usage statistics
void rlimit_print_usage(rlimit_container_t* rl) {
    if (!rl) return;

    kprintf("[RLIMIT] Resource Usage:\n");
    kprintf("  CPU time: %llu ms\n", rl->usage.cpu_time);
    kprintf("  Memory: %llu / %llu bytes (peak: %llu)\n",
            rl->usage.memory_current,
            rl->limits[RLIMIT_MEMORY].hard,
            rl->usage.memory_peak);
    kprintf("  RSS: %llu bytes\n", rl->usage.rss_current);
    kprintf("  Virtual memory: %llu bytes\n", rl->usage.vmem_current);
    kprintf("  File descriptors: %u / %llu\n",
            rl->usage.fd_count,
            rl->limits[RLIMIT_NOFILE].hard);
    kprintf("  Network RX: %llu bytes\n", rl->usage.net_rx_bytes);
    kprintf("  Network TX: %llu bytes\n", rl->usage.net_tx_bytes);
    kprintf("  Disk read: %llu bytes\n", rl->usage.disk_read_bytes);
    kprintf("  Disk write: %llu bytes\n", rl->usage.disk_write_bytes);
    kprintf("  Context switches: %llu\n", rl->usage.context_switches);
    kprintf("  Page faults: %llu\n", rl->usage.page_faults);
}

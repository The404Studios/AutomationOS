#include "../../include/rlimit.h"
#include "../../include/kernel.h"

extern uint64_t get_timer_ticks(void);

// Initialize token bucket
void token_bucket_init(token_bucket_t* bucket, uint64_t rate, uint64_t capacity) {
    if (!bucket) return;

    bucket->rate = rate;          // bytes per second
    bucket->capacity = capacity;  // burst size
    bucket->tokens = capacity;    // start full
    bucket->last_update = get_timer_ticks();
}

// Refill token bucket based on elapsed time
void token_bucket_refill(token_bucket_t* bucket, uint64_t current_time) {
    if (!bucket) return;

    uint64_t elapsed = current_time - bucket->last_update;
    if (elapsed == 0) return;

    // Calculate tokens to add (rate is per second, elapsed is in ticks)
    // Assume 1000 ticks = 1 second
    uint64_t tokens_to_add = (bucket->rate * elapsed) / 1000;

    bucket->tokens += tokens_to_add;
    if (bucket->tokens > bucket->capacity) {
        bucket->tokens = bucket->capacity;
    }

    bucket->last_update = current_time;
}

// Try to consume tokens from bucket
bool token_bucket_consume(token_bucket_t* bucket, uint64_t tokens, uint64_t current_time) {
    if (!bucket) return false;

    // Refill first
    token_bucket_refill(bucket, current_time);

    // Check if enough tokens available
    if (bucket->tokens >= tokens) {
        bucket->tokens -= tokens;
        return true;
    }

    return false;
}

// Check network receive limit
bool rlimit_check_network_rx(rlimit_container_t* rl, uint64_t bytes) {
    if (!rl) return false;

    uint64_t current_time = get_timer_ticks();

    // Try to consume tokens from RX bucket
    if (!token_bucket_consume(&rl->net_rx_bucket, bytes, current_time)) {
        kprintf("[RLIMIT] Network RX rate limit exceeded (requested %llu bytes)\n", bytes);
        return false;
    }

    // Also check total data limit
    rlimit_t* limit = &rl->limits[RLIMIT_NET_RX];
    if (limit->hard != RLIMIT_INFINITY &&
        rl->usage.net_rx_bytes + bytes > limit->hard) {
        kprintf("[RLIMIT] Network RX total limit exceeded\n");
        return false;
    }

    return true;
}

// Check network transmit limit
bool rlimit_check_network_tx(rlimit_container_t* rl, uint64_t bytes) {
    if (!rl) return false;

    uint64_t current_time = get_timer_ticks();

    // Try to consume tokens from TX bucket
    if (!token_bucket_consume(&rl->net_tx_bucket, bytes, current_time)) {
        kprintf("[RLIMIT] Network TX rate limit exceeded (requested %llu bytes)\n", bytes);
        return false;
    }

    // Also check total data limit
    rlimit_t* limit = &rl->limits[RLIMIT_NET_TX];
    if (limit->hard != RLIMIT_INFINITY &&
        rl->usage.net_tx_bytes + bytes > limit->hard) {
        kprintf("[RLIMIT] Network TX total limit exceeded\n");
        return false;
    }

    return true;
}

// Account network receive
void rlimit_account_network_rx(rlimit_container_t* rl, uint64_t bytes) {
    if (!rl) return;

    rl->usage.net_rx_bytes += bytes;

    // Check soft limit
    rlimit_t* limit = &rl->limits[RLIMIT_NET_RX];
    if (limit->soft != RLIMIT_INFINITY &&
        rl->usage.net_rx_bytes > limit->soft &&
        !rl->soft_limit_signaled[RLIMIT_NET_RX]) {
        kprintf("[RLIMIT] Network RX soft limit exceeded (%llu > %llu bytes)\n",
                rl->usage.net_rx_bytes, limit->soft);
        rl->soft_limit_signaled[RLIMIT_NET_RX] = true;
    }
}

// Account network transmit
void rlimit_account_network_tx(rlimit_container_t* rl, uint64_t bytes) {
    if (!rl) return;

    rl->usage.net_tx_bytes += bytes;

    // Check soft limit
    rlimit_t* limit = &rl->limits[RLIMIT_NET_TX];
    if (limit->soft != RLIMIT_INFINITY &&
        rl->usage.net_tx_bytes > limit->soft &&
        !rl->soft_limit_signaled[RLIMIT_NET_TX]) {
        kprintf("[RLIMIT] Network TX soft limit exceeded (%llu > %llu bytes)\n",
                rl->usage.net_tx_bytes, limit->soft);
        rl->soft_limit_signaled[RLIMIT_NET_TX] = true;
    }
}

// Check disk read limit
bool rlimit_check_disk_read(rlimit_container_t* rl, uint64_t bytes) {
    if (!rl) return false;

    uint64_t current_time = get_timer_ticks();

    // Try to consume tokens from disk read bucket
    if (!token_bucket_consume(&rl->disk_read_bucket, bytes, current_time)) {
        kprintf("[RLIMIT] Disk read rate limit exceeded (requested %llu bytes)\n", bytes);
        return false;
    }

    return true;
}

// Check disk write limit
bool rlimit_check_disk_write(rlimit_container_t* rl, uint64_t bytes) {
    if (!rl) return false;

    uint64_t current_time = get_timer_ticks();

    // Try to consume tokens from disk write bucket
    if (!token_bucket_consume(&rl->disk_write_bucket, bytes, current_time)) {
        kprintf("[RLIMIT] Disk write rate limit exceeded (requested %llu bytes)\n", bytes);
        return false;
    }

    return true;
}

// Account disk read
void rlimit_account_disk_read(rlimit_container_t* rl, uint64_t bytes) {
    if (!rl) return;
    rl->usage.disk_read_bytes += bytes;
}

// Account disk write
void rlimit_account_disk_write(rlimit_container_t* rl, uint64_t bytes) {
    if (!rl) return;
    rl->usage.disk_write_bytes += bytes;
}

// Get current network bandwidth usage (bytes/sec)
uint64_t rlimit_get_net_rx_bandwidth(rlimit_container_t* rl) {
    if (!rl) return 0;
    return rl->net_rx_bucket.rate - rl->net_rx_bucket.tokens;
}

uint64_t rlimit_get_net_tx_bandwidth(rlimit_container_t* rl) {
    if (!rl) return 0;
    return rl->net_tx_bucket.rate - rl->net_tx_bucket.tokens;
}

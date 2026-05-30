#include "../../include/rlimit.h"
#include "../../include/kernel.h"
#include "../../include/sched.h"

// Get current time in ticks (TODO: replace with actual timer)
extern uint64_t get_timer_ticks(void);

// Convert ticks to milliseconds (assuming 1000 ticks = 1 second)
static uint64_t ticks_to_ms(uint64_t ticks) {
    return ticks;  // Simplified for now
}

// Convert microseconds to ticks
static uint64_t us_to_ticks(uint64_t us) {
    return us / 1000;  // Simplified: 1 tick = 1ms
}

// Check if CPU time limit would be exceeded
bool rlimit_check_cpu(rlimit_container_t* rl, uint64_t additional_ms) {
    if (!rl) return false;

    uint64_t new_total = rl->usage.cpu_time + additional_ms;
    rlimit_t* limit = &rl->limits[RLIMIT_CPU];

    // Check hard limit first
    if (limit->hard != RLIMIT_INFINITY && new_total > limit->hard) {
        kprintf("[RLIMIT] CPU hard limit exceeded (%llu > %llu ms)\n",
                new_total, limit->hard);
        return false;
    }

    // Check soft limit and signal if exceeded (only once)
    if (limit->soft != RLIMIT_INFINITY &&
        new_total > limit->soft &&
        !rl->soft_limit_signaled[RLIMIT_CPU]) {
        kprintf("[RLIMIT] CPU soft limit exceeded (%llu > %llu ms), sending SIGXCPU\n",
                new_total, limit->soft);

        // TODO: Send SIGXCPU signal to process
        rl->soft_limit_signaled[RLIMIT_CPU] = true;
    }

    return true;
}

// Account CPU time used
void rlimit_account_cpu(rlimit_container_t* rl, uint64_t ms) {
    if (!rl) return;

    rl->usage.cpu_time += ms;

    // Update CPU quota usage if quota is set
    if (rl->cpu_quota.quota_us > 0) {
        rl->cpu_quota.used_us += (ms * 1000);  // Convert ms to us
    }
}

// Set CPU shares (for proportional scheduling)
void rlimit_set_cpu_shares(rlimit_container_t* rl, uint64_t shares) {
    if (!rl) return;

    rl->cpu_quota.shares = shares;
    kprintf("[RLIMIT] Set CPU shares to %llu\n", shares);
}

// Set CPU quota (% of CPU)
void rlimit_set_cpu_quota(rlimit_container_t* rl, uint64_t quota_us, uint64_t period_us) {
    if (!rl) return;

    rl->cpu_quota.quota_us = quota_us;
    rl->cpu_quota.period_us = period_us;
    rl->cpu_quota.used_us = 0;
    rl->cpu_quota.period_start = get_timer_ticks();
    rl->cpu_quota.throttled = false;

    uint64_t percent = (quota_us * 100) / period_us;
    kprintf("[RLIMIT] Set CPU quota: %llu us per %llu us period (%llu%%)\n",
            quota_us, period_us, percent);
}

// Check if CPU quota exceeded (called on every schedule)
bool rlimit_cpu_quota_exceeded(rlimit_container_t* rl) {
    if (!rl || rl->cpu_quota.quota_us == 0) {
        return false;  // No quota set
    }

    uint64_t current_ticks = get_timer_ticks();
    uint64_t period_elapsed = current_ticks - rl->cpu_quota.period_start;
    uint64_t period_ticks = us_to_ticks(rl->cpu_quota.period_us);

    // Check if period has expired - refill quota
    if (period_elapsed >= period_ticks) {
        rlimit_cpu_quota_refill(rl);
        return false;
    }

    // Check if quota exceeded within current period
    if (rl->cpu_quota.used_us >= rl->cpu_quota.quota_us) {
        if (!rl->cpu_quota.throttled) {
            kprintf("[RLIMIT] CPU quota exceeded, throttling process\n");
            rl->cpu_quota.throttled = true;
        }
        return true;
    }

    return false;
}

// Refill CPU quota (start new period)
void rlimit_cpu_quota_refill(rlimit_container_t* rl) {
    if (!rl) return;

    rl->cpu_quota.used_us = 0;
    rl->cpu_quota.period_start = get_timer_ticks();

    if (rl->cpu_quota.throttled) {
        kprintf("[RLIMIT] CPU quota refilled, unthrottling process\n");
        rl->cpu_quota.throttled = false;
    }
}

// Calculate effective priority based on CPU shares
// Higher shares = higher priority in scheduler
// Returns priority value (higher is better)
uint64_t rlimit_calculate_cpu_priority(rlimit_container_t* rl) {
    if (!rl) return CPU_SHARES_DEFAULT;

    // Simple proportional priority
    // Could be extended with fairness algorithms (CFS-style)
    return rl->cpu_quota.shares;
}

// Update CPU usage from scheduler tick
// Called from schedule() on every timer interrupt
void rlimit_cpu_tick(rlimit_container_t* rl, uint64_t ticks) {
    if (!rl) return;

    // Convert ticks to milliseconds
    uint64_t ms = ticks_to_ms(ticks);

    // Check if this would exceed limits
    if (!rlimit_check_cpu(rl, ms)) {
        // Hard limit exceeded - kill process
        kprintf("[RLIMIT] CPU hard limit exceeded, terminating process\n");
        // TODO: Terminate process (set state to TERMINATED)
        return;
    }

    // Account the CPU time
    rlimit_account_cpu(rl, ms);

    // Check if CPU quota exceeded
    if (rlimit_cpu_quota_exceeded(rl)) {
        // Process should be throttled - scheduler will skip it
        process_t* current = process_get_current();
        if (current) {
            current->state = PROCESS_BLOCKED;  // Temporarily block
        }
    }
}

// Check if process should be scheduled based on CPU limits
bool rlimit_can_schedule(rlimit_container_t* rl) {
    if (!rl) return true;

    // Check if CPU quota exceeded
    if (rlimit_cpu_quota_exceeded(rl)) {
        return false;
    }

    // Check if hard limit exceeded
    rlimit_t* limit = &rl->limits[RLIMIT_CPU];
    if (limit->hard != RLIMIT_INFINITY &&
        rl->usage.cpu_time >= limit->hard) {
        return false;
    }

    return true;
}

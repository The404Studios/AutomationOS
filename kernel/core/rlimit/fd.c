#include "../../include/rlimit.h"
#include "../../include/kernel.h"

// System-wide file descriptor count (all processes)
static uint32_t system_fd_count = 0;
static uint32_t system_fd_limit = 65536;  // System-wide limit

// Check if opening new FD would exceed limit
bool rlimit_check_fd(rlimit_container_t* rl) {
    if (!rl) return false;

    // Check per-process limit
    rlimit_t* limit = &rl->limits[RLIMIT_NOFILE];
    if (limit->hard != RLIMIT_INFINITY &&
        rl->usage.fd_count >= limit->hard) {
        kprintf("[RLIMIT] File descriptor hard limit exceeded (%u >= %llu)\n",
                rl->usage.fd_count, limit->hard);
        return false;
    }

    // Check system-wide limit
    if (system_fd_count >= system_fd_limit) {
        kprintf("[RLIMIT] System-wide FD limit exceeded (%u >= %u)\n",
                system_fd_count, system_fd_limit);
        return false;
    }

    // Check soft limit and warn
    if (limit->soft != RLIMIT_INFINITY &&
        rl->usage.fd_count >= limit->soft &&
        !rl->soft_limit_signaled[RLIMIT_NOFILE]) {
        kprintf("[RLIMIT] File descriptor soft limit exceeded (%u >= %llu)\n",
                rl->usage.fd_count, limit->soft);
        rl->soft_limit_signaled[RLIMIT_NOFILE] = true;
    }

    return true;
}

// Account file descriptor open
void rlimit_account_fd_open(rlimit_container_t* rl) {
    if (!rl) return;

    rl->usage.fd_count++;
    system_fd_count++;

    // Check if approaching limit
    rlimit_t* limit = &rl->limits[RLIMIT_NOFILE];
    if (limit->hard != RLIMIT_INFINITY) {
        uint64_t usage_percent = (rl->usage.fd_count * 100) / limit->hard;
        if (usage_percent >= 90) {
            kprintf("[RLIMIT] Warning: FD usage at %llu%% (%u / %llu)\n",
                    usage_percent, rl->usage.fd_count, limit->hard);
        }
    }
}

// Account file descriptor close
void rlimit_account_fd_close(rlimit_container_t* rl) {
    if (!rl) return;

    if (rl->usage.fd_count > 0) {
        rl->usage.fd_count--;
    }

    if (system_fd_count > 0) {
        system_fd_count--;
    }
}

// Get system-wide FD usage
uint32_t rlimit_get_system_fd_count(void) {
    return system_fd_count;
}

// Set system-wide FD limit
void rlimit_set_system_fd_limit(uint32_t limit) {
    system_fd_limit = limit;
    kprintf("[RLIMIT] Set system-wide FD limit to %u\n", limit);
}

// Calculate FD pressure (0-100%)
uint32_t rlimit_get_fd_pressure(rlimit_container_t* rl) {
    if (!rl) return 0;

    rlimit_t* limit = &rl->limits[RLIMIT_NOFILE];
    if (limit->hard == RLIMIT_INFINITY) {
        return 0;
    }

    return (rl->usage.fd_count * 100) / limit->hard;
}

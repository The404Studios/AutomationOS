#include "../../include/rlimit.h"
#include "../../include/kernel.h"
#include "../../include/sched.h"
#include "../../include/mem.h"

// Check if memory allocation would exceed limit
bool rlimit_check_memory(rlimit_container_t* rl, uint64_t additional_bytes) {
    if (!rl) return false;

    uint64_t new_total = rl->usage.memory_current + additional_bytes;
    rlimit_t* limit = &rl->limits[RLIMIT_MEMORY];

    // Check hard limit
    if (limit->hard != RLIMIT_INFINITY && new_total > limit->hard) {
        kprintf("[RLIMIT] Memory hard limit exceeded (%llu > %llu bytes)\n",
                new_total, limit->hard);
        return false;
    }

    // Check soft limit and warn
    if (limit->soft != RLIMIT_INFINITY &&
        new_total > limit->soft &&
        !rl->soft_limit_signaled[RLIMIT_MEMORY]) {
        kprintf("[RLIMIT] Memory soft limit exceeded (%llu > %llu bytes)\n",
                new_total, limit->soft);
        rl->soft_limit_signaled[RLIMIT_MEMORY] = true;
    }

    return true;
}

// Check RSS (physical memory) limit
bool rlimit_check_rss(rlimit_container_t* rl, uint64_t additional_bytes) {
    if (!rl) return false;

    uint64_t new_rss = rl->usage.rss_current + additional_bytes;
    rlimit_t* limit = &rl->limits[RLIMIT_RSS];

    // Check hard limit
    if (limit->hard != RLIMIT_INFINITY && new_rss > limit->hard) {
        kprintf("[RLIMIT] RSS hard limit exceeded (%llu > %llu bytes)\n",
                new_rss, limit->hard);
        return false;
    }

    return true;
}

// Check virtual memory limit
bool rlimit_check_vmem(rlimit_container_t* rl, uint64_t additional_bytes) {
    if (!rl) return false;

    uint64_t new_vmem = rl->usage.vmem_current + additional_bytes;
    rlimit_t* limit = &rl->limits[RLIMIT_VMEM];

    // Check hard limit
    if (limit->hard != RLIMIT_INFINITY && new_vmem > limit->hard) {
        kprintf("[RLIMIT] Virtual memory hard limit exceeded (%llu > %llu bytes)\n",
                new_vmem, limit->hard);
        return false;
    }

    return true;
}

// Account memory allocation
void rlimit_account_memory_alloc(rlimit_container_t* rl, uint64_t bytes) {
    if (!rl) return;

    rl->usage.memory_current += bytes;

    // Update peak
    if (rl->usage.memory_current > rl->usage.memory_peak) {
        rl->usage.memory_peak = rl->usage.memory_current;
    }

    // TODO: Distinguish between RSS and virtual memory
    // For now, assume all allocated memory is physical
    rl->usage.rss_current += bytes;
    rl->usage.vmem_current += bytes;
}

// Account memory deallocation
void rlimit_account_memory_free(rlimit_container_t* rl, uint64_t bytes) {
    if (!rl) return;

    if (rl->usage.memory_current >= bytes) {
        rl->usage.memory_current -= bytes;
        rl->usage.rss_current -= bytes;
        rl->usage.vmem_current -= bytes;
    } else {
        kprintf("[RLIMIT] Warning: Freeing more memory than allocated\n");
        rl->usage.memory_current = 0;
        rl->usage.rss_current = 0;
        rl->usage.vmem_current = 0;
    }
}

// OOM killer - kill process when memory limit exceeded
void rlimit_oom_kill(process_t* proc) {
    if (!proc) return;

    kprintf("[RLIMIT] OOM: Killing process '%s' (PID %d) due to memory limit\n",
            proc->name, proc->pid);

    // Set process state to terminated
    proc->state = PROCESS_TERMINATED;
    proc->exit_status = 137;            // 128 + SIGKILL: OOM-killed
    process_on_terminate(proc);         // wake a parent blocked in waitpid()

    // TODO: Free process resources
}

// Memory pressure notification (warning before OOM)
void rlimit_memory_pressure(rlimit_container_t* rl, int level) {
    if (!rl) return;

    const char* level_str[] = {"LOW", "MEDIUM", "HIGH", "CRITICAL"};
    if (level < 0 || level > 3) level = 0;

    kprintf("[RLIMIT] Memory pressure: %s (%llu / %llu bytes used)\n",
            level_str[level],
            rl->usage.memory_current,
            rl->limits[RLIMIT_MEMORY].hard);

    // TODO: Send pressure notification to process
    // Process can respond by freeing caches, etc.
}

// Calculate memory pressure level (0-3)
int rlimit_get_memory_pressure(rlimit_container_t* rl) {
    if (!rl) return 0;

    rlimit_t* limit = &rl->limits[RLIMIT_MEMORY];
    if (limit->hard == RLIMIT_INFINITY) {
        return 0;  // No limit
    }

    uint64_t usage_percent = (rl->usage.memory_current * 100) / limit->hard;

    if (usage_percent >= 95) return 3;  // CRITICAL
    if (usage_percent >= 85) return 2;  // HIGH
    if (usage_percent >= 70) return 1;  // MEDIUM
    return 0;  // LOW
}

// Check if process should be OOM killed
bool rlimit_should_oom_kill(rlimit_container_t* rl) {
    if (!rl) return false;

    rlimit_t* limit = &rl->limits[RLIMIT_MEMORY];
    if (limit->hard == RLIMIT_INFINITY) {
        return false;
    }

    // Kill if over hard limit
    return rl->usage.memory_current > limit->hard;
}

// Account page fault
void rlimit_account_page_fault(rlimit_container_t* rl) {
    if (!rl) return;
    rl->usage.page_faults++;
}

// Account context switch
void rlimit_account_context_switch(rlimit_container_t* rl) {
    if (!rl) return;
    rl->usage.context_switches++;
}

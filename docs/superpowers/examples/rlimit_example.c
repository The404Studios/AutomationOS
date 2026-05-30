/**
 * Resource Limits Example Program
 *
 * Demonstrates how to use the resource limit API in AutomationOS
 */

#include "../../kernel/include/rlimit.h"
#include "../../kernel/include/syscall.h"
#include "../../kernel/include/kernel.h"

// Example 1: Setting CPU time limits
void example_cpu_limits(void) {
    kprintf("\n=== Example 1: CPU Time Limits ===\n");

    // Get current limits
    rlimit_t old_limit;
    sys_getrlimit(RLIMIT_CPU, (uint64_t)&old_limit, 0, 0, 0, 0);
    kprintf("Current CPU limit: soft=%llu ms, hard=%llu ms\n",
            old_limit.soft, old_limit.hard);

    // Set new limits: 5 seconds soft, 10 seconds hard
    rlimit_t new_limit = {
        .soft = 5000,   // 5 seconds
        .hard = 10000   // 10 seconds
    };
    sys_setrlimit(RLIMIT_CPU, (uint64_t)&new_limit, 0, 0, 0, 0);
    kprintf("Set CPU limit: soft=5s, hard=10s\n");

    // Simulate CPU-intensive work
    kprintf("Running CPU-intensive loop...\n");
    // When CPU time exceeds 5s: SIGXCPU signal sent (soft limit)
    // When CPU time exceeds 10s: Process terminated (hard limit)
}

// Example 2: Memory limits with OOM handling
void example_memory_limits(void) {
    kprintf("\n=== Example 2: Memory Limits ===\n");

    // Set memory limit: 32 MB soft, 64 MB hard
    rlimit_t mem_limit = {
        .soft = 32 * 1024 * 1024,  // 32 MB
        .hard = 64 * 1024 * 1024   // 64 MB
    };
    sys_setrlimit(RLIMIT_MEMORY, (uint64_t)&mem_limit, 0, 0, 0, 0);
    kprintf("Set memory limit: 32MB soft, 64MB hard\n");

    // Check resource usage
    rusage_t usage;
    sys_getrusage(RUSAGE_SELF, (uint64_t)&usage, 0, 0, 0, 0);
    kprintf("Current memory usage: %llu bytes\n", usage.memory_current);
    kprintf("Peak memory usage: %llu bytes\n", usage.memory_peak);

    // Try to allocate memory
    void* buffer = kmalloc(10 * 1024 * 1024);  // 10 MB
    if (buffer) {
        kprintf("Successfully allocated 10 MB\n");
        kfree(buffer);
    } else {
        kprintf("Allocation failed - limit exceeded\n");
    }
}

// Example 3: File descriptor limits
void example_fd_limits(void) {
    kprintf("\n=== Example 3: File Descriptor Limits ===\n");

    // Set FD limit: 100 soft, 200 hard
    rlimit_t fd_limit = {
        .soft = 100,
        .hard = 200
    };
    sys_setrlimit(RLIMIT_NOFILE, (uint64_t)&fd_limit, 0, 0, 0, 0);
    kprintf("Set FD limit: 100 soft, 200 hard\n");

    // Try to open many files
    int fds[300];
    int opened = 0;

    for (int i = 0; i < 300; i++) {
        int fd = sys_open("/tmp/testfile", O_RDONLY, 0, 0, 0, 0);
        if (fd < 0) {
            kprintf("Failed to open file %d: limit exceeded\n", i);
            break;
        }
        fds[opened++] = fd;
    }

    kprintf("Successfully opened %d files\n", opened);

    // Close files
    for (int i = 0; i < opened; i++) {
        sys_close(fds[i], 0, 0, 0, 0, 0);
    }
}

// Example 4: CPU quota (percentage limit)
void example_cpu_quota(void) {
    kprintf("\n=== Example 4: CPU Quota ===\n");

    process_t* current = process_get_current();
    if (!current || !current->rlimits) {
        kprintf("Error: No resource limit container\n");
        return;
    }

    // Set 50% CPU quota (50ms per 100ms period)
    rlimit_set_cpu_quota(current->rlimits, 50000, 100000);
    kprintf("Set CPU quota: 50%% (50ms per 100ms)\n");

    // This process can now use max 50% CPU
    // If it tries to use more, it will be throttled
    kprintf("Process will be throttled if CPU usage exceeds 50%%\n");

    // Check if throttled
    if (rlimit_cpu_quota_exceeded(current->rlimits)) {
        kprintf("Process is currently throttled\n");
    } else {
        kprintf("Process is running normally\n");
    }
}

// Example 5: CPU shares (proportional scheduling)
void example_cpu_shares(void) {
    kprintf("\n=== Example 5: CPU Shares ===\n");

    process_t* current = process_get_current();
    if (!current || !current->rlimits) {
        kprintf("Error: No resource limit container\n");
        return;
    }

    // Set CPU shares to 2048 (2x default priority)
    rlimit_set_cpu_shares(current->rlimits, 2048);
    kprintf("Set CPU shares: 2048 (2x default priority)\n");

    // This process will get 2x CPU time compared to
    // processes with default shares (1024)
    kprintf("Process has 2x CPU priority\n");
}

// Example 6: Network bandwidth limiting
void example_network_limits(void) {
    kprintf("\n=== Example 6: Network Bandwidth Limits ===\n");

    process_t* current = process_get_current();
    if (!current || !current->rlimits) {
        kprintf("Error: No resource limit container\n");
        return;
    }

    // Set network TX bandwidth: 1 MB/s with 2 MB burst
    token_bucket_init(&current->rlimits->net_tx_bucket,
                     1 * 1024 * 1024,   // 1 MB/s rate
                     2 * 1024 * 1024);  // 2 MB burst
    kprintf("Set network TX limit: 1 MB/s (2 MB burst)\n");

    // Try to send data
    char buffer[1024];
    int sent = socket_send(sockfd, buffer, sizeof(buffer));
    if (sent < 0) {
        kprintf("Send failed: rate limited (-EAGAIN)\n");
    } else {
        kprintf("Successfully sent %d bytes\n", sent);
    }
}

// Example 7: Hierarchical limits (parent/child)
void example_hierarchical_limits(void) {
    kprintf("\n=== Example 7: Hierarchical Limits ===\n");

    process_t* parent = process_get_current();
    if (!parent || !parent->rlimits) {
        kprintf("Error: No resource limit container\n");
        return;
    }

    // Set parent limits
    rlimit_t parent_limit = {
        .soft = 10000,  // 10 seconds
        .hard = 20000   // 20 seconds
    };
    rlimit_set(parent->rlimits, RLIMIT_CPU, &parent_limit);
    kprintf("Parent CPU limit: 10s soft, 20s hard\n");

    // Fork child process
    int pid = fork();
    if (pid == 0) {
        // Child process
        process_t* child = process_get_current();

        // Child inherits parent's limits
        rlimit_t child_limit;
        rlimit_get(child->rlimits, RLIMIT_CPU, &child_limit);
        kprintf("Child inherited limits: %llu soft, %llu hard\n",
                child_limit.soft, child_limit.hard);

        // Child cannot raise limits above parent's
        rlimit_t new_limit = {
            .soft = 30000,  // Would exceed parent's hard limit
            .hard = 30000
        };
        int result = rlimit_set(child->rlimits, RLIMIT_CPU, &new_limit);
        if (result != 0) {
            kprintf("Child cannot exceed parent's limits\n");
        }

        exit(0);
    }
}

// Example 8: Memory pressure monitoring
void example_memory_pressure(void) {
    kprintf("\n=== Example 8: Memory Pressure Monitoring ===\n");

    process_t* current = process_get_current();
    if (!current || !current->rlimits) {
        kprintf("Error: No resource limit container\n");
        return;
    }

    // Get memory pressure level
    int pressure = rlimit_get_memory_pressure(current->rlimits);
    const char* levels[] = {"LOW", "MEDIUM", "HIGH", "CRITICAL"};
    kprintf("Memory pressure: %s\n", levels[pressure]);

    // Get usage statistics
    rusage_t usage;
    rlimit_get_usage(current->rlimits, &usage);

    rlimit_t limit;
    rlimit_get(current->rlimits, RLIMIT_MEMORY, &limit);

    uint64_t usage_pct = (usage.memory_current * 100) / limit.hard;
    kprintf("Memory usage: %llu%% (%llu / %llu bytes)\n",
            usage_pct, usage.memory_current, limit.hard);

    // Take action based on pressure
    if (pressure >= 2) {  // HIGH or CRITICAL
        kprintf("High memory pressure - freeing caches\n");
        // Free non-essential memory
    }
}

// Example 9: Soft limit warning handling
void example_soft_limit_warning(void) {
    kprintf("\n=== Example 9: Soft Limit Warnings ===\n");

    // Set CPU limit with soft warning
    rlimit_t limit = {
        .soft = 1000,   // 1 second (warning)
        .hard = 5000    // 5 seconds (termination)
    };
    sys_setrlimit(RLIMIT_CPU, (uint64_t)&limit, 0, 0, 0, 0);

    kprintf("Set CPU limit: 1s soft (warning), 5s hard (kill)\n");
    kprintf("When soft limit exceeded, SIGXCPU signal will be sent\n");
    kprintf("Process can catch signal and cleanup gracefully\n");

    // In real code, setup signal handler:
    // signal(SIGXCPU, sigxcpu_handler);
}

// Example 10: Resource usage statistics
void example_resource_stats(void) {
    kprintf("\n=== Example 10: Resource Usage Statistics ===\n");

    // Get comprehensive usage stats
    rusage_t usage;
    sys_getrusage(RUSAGE_SELF, (uint64_t)&usage, 0, 0, 0, 0);

    kprintf("Resource Usage:\n");
    kprintf("  CPU time:        %llu ms\n", usage.cpu_time);
    kprintf("  Memory current:  %llu bytes\n", usage.memory_current);
    kprintf("  Memory peak:     %llu bytes\n", usage.memory_peak);
    kprintf("  RSS:             %llu bytes\n", usage.rss_current);
    kprintf("  Virtual memory:  %llu bytes\n", usage.vmem_current);
    kprintf("  File descriptors: %u\n", usage.fd_count);
    kprintf("  Network RX:      %llu bytes\n", usage.net_rx_bytes);
    kprintf("  Network TX:      %llu bytes\n", usage.net_tx_bytes);
    kprintf("  Disk read:       %llu bytes\n", usage.disk_read_bytes);
    kprintf("  Disk write:      %llu bytes\n", usage.disk_write_bytes);
    kprintf("  Context switches: %llu\n", usage.context_switches);
    kprintf("  Page faults:     %llu\n", usage.page_faults);
}

// Main example runner
int main(void) {
    kprintf("\n");
    kprintf("╔════════════════════════════════════════════════════════╗\n");
    kprintf("║     AutomationOS Resource Limits Examples             ║\n");
    kprintf("╚════════════════════════════════════════════════════════╝\n");

    example_cpu_limits();
    example_memory_limits();
    example_fd_limits();
    example_cpu_quota();
    example_cpu_shares();
    example_network_limits();
    example_hierarchical_limits();
    example_memory_pressure();
    example_soft_limit_warning();
    example_resource_stats();

    kprintf("\n");
    kprintf("╔════════════════════════════════════════════════════════╗\n");
    kprintf("║                Examples Complete                       ║\n");
    kprintf("╚════════════════════════════════════════════════════════╝\n");

    return 0;
}

// Expected Output:
//
// === Example 1: CPU Time Limits ===
// Current CPU limit: soft=10000 ms, hard=20000 ms
// Set CPU limit: soft=5s, hard=10s
// Running CPU-intensive loop...
//
// === Example 2: Memory Limits ===
// Set memory limit: 32MB soft, 64MB hard
// Current memory usage: 0 bytes
// Peak memory usage: 0 bytes
// Successfully allocated 10 MB
//
// === Example 3: File Descriptor Limits ===
// Set FD limit: 100 soft, 200 hard
// Failed to open file 200: limit exceeded
// Successfully opened 200 files
//
// === Example 4: CPU Quota ===
// Set CPU quota: 50% (50ms per 100ms)
// Process will be throttled if CPU usage exceeds 50%
// Process is running normally
//
// === Example 5: CPU Shares ===
// Set CPU shares: 2048 (2x default priority)
// Process has 2x CPU priority
//
// === Example 6: Network Bandwidth Limits ===
// Set network TX limit: 1 MB/s (2 MB burst)
// Successfully sent 1024 bytes
//
// === Example 7: Hierarchical Limits ===
// Parent CPU limit: 10s soft, 20s hard
// Child inherited limits: 10000 soft, 20000 hard
// Child cannot exceed parent's limits
//
// === Example 8: Memory Pressure Monitoring ===
// Memory pressure: LOW
// Memory usage: 15% (20971520 / 134217728 bytes)
//
// === Example 9: Soft Limit Warnings ===
// Set CPU limit: 1s soft (warning), 5s hard (kill)
// When soft limit exceeded, SIGXCPU signal will be sent
// Process can catch signal and cleanup gracefully
//
// === Example 10: Resource Usage Statistics ===
// Resource Usage:
//   CPU time:        1234 ms
//   Memory current:  20971520 bytes
//   Memory peak:     31457280 bytes
//   RSS:             20971520 bytes
//   Virtual memory:  20971520 bytes
//   File descriptors: 42
//   Network RX:      1048576 bytes
//   Network TX:      2097152 bytes
//   Disk read:       5242880 bytes
//   Disk write:      1048576 bytes
//   Context switches: 123
//   Page faults:     456

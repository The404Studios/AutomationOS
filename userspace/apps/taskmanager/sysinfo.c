// userspace/apps/taskmanager/sysinfo.c - System information collection

#include "taskmanager.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"
#include "../../libc/syscall.h"

// Get system-wide statistics
int get_system_stats(system_stats_t* stats) {
    // TODO: Implement actual syscall to get system stats
    // For now, generate mock data

    static uint64_t tick = 0;
    tick++;

    // Process counts
    stats->total_processes = 8;
    stats->running_processes = 4;
    stats->blocked_processes = 2;

    // CPU statistics (simulate 4 cores)
    stats->cpu_count = 4;
    stats->cpu_usage[0] = 45 + (tick % 20);
    stats->cpu_usage[1] = 32 + (tick % 15);
    stats->cpu_usage[2] = 28 + (tick % 10);
    stats->cpu_usage[3] = 15 + (tick % 8);

    // Calculate total CPU usage
    uint64_t sum = 0;
    for (int i = 0; i < stats->cpu_count; i++) {
        sum += stats->cpu_usage[i];
    }
    stats->cpu_total_usage = sum / stats->cpu_count;

    // Memory statistics (simulate 4 GB total)
    stats->memory_total = 4ULL * 1024 * 1024 * 1024;
    stats->memory_used = (2ULL * 1024 * 1024 * 1024) + (tick * 1024 * 1024);
    stats->memory_free = stats->memory_total - stats->memory_used;
    stats->memory_cached = 512ULL * 1024 * 1024;
    stats->memory_buffers = 256ULL * 1024 * 1024;

    // Ensure memory_used doesn't exceed total
    if (stats->memory_used > stats->memory_total) {
        stats->memory_used = stats->memory_total * 60 / 100;  // 60%
        stats->memory_free = stats->memory_total - stats->memory_used;
    }

    // Disk I/O statistics (simulate activity)
    stats->disk_read_rate = (8ULL * 1024 * 1024) + ((tick * 512 * 1024) % (10 * 1024 * 1024));
    stats->disk_write_rate = (4ULL * 1024 * 1024) + ((tick * 256 * 1024) % (5 * 1024 * 1024));
    stats->disk_read_total = tick * 10ULL * 1024 * 1024;
    stats->disk_write_total = tick * 5ULL * 1024 * 1024;

    // Network statistics (simulate activity)
    stats->net_recv_rate = (1ULL * 1024 * 1024) + ((tick * 128 * 1024) % (2 * 1024 * 1024));
    stats->net_send_rate = (512ULL * 1024) + ((tick * 64 * 1024) % (1024 * 1024));
    stats->net_recv_total = tick * 1ULL * 1024 * 1024;
    stats->net_send_total = tick * 512ULL * 1024;

    // System uptime (simulate)
    stats->uptime_seconds = tick;

    return 0;
}

// Update performance history
void update_perf_history(perf_history_t* history, const system_stats_t* stats) {
    uint32_t idx = history->current_sample;

    history->cpu_history[idx] = stats->cpu_total_usage;
    history->memory_history[idx] = (stats->memory_used * 100) / stats->memory_total;
    history->disk_history[idx] = (stats->disk_read_rate + stats->disk_write_rate) / (1024 * 1024);  // MB/s
    history->network_history[idx] = (stats->net_recv_rate + stats->net_send_rate) / (1024 * 1024);  // MB/s

    history->current_sample = (idx + 1) % HISTORY_SAMPLES;
}

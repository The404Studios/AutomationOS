// userspace/apps/taskmanager/procinfo.c - Process information collection

#include "taskmanager.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"
#include "../../libc/syscall.h"

// System call to get process list (TODO: implement in kernel)
// For now, we'll simulate with mock data
#define SYS_GET_PROCESS_LIST 100
#define SYS_GET_SYSTEM_INFO 101

// Mock system call wrappers
static long sys_get_process_list(process_info_t* procs, int max_count) {
    // TODO: Replace with actual syscall
    // For now, return mock data
    (void)procs;
    (void)max_count;
    return -1;  // Not implemented
}

static long sys_get_system_info(system_stats_t* stats) {
    // TODO: Replace with actual syscall
    (void)stats;
    return -1;  // Not implemented
}

// Collect process information from kernel
int collect_process_info(process_info_t* procs, int max_count) {
    // Try kernel syscall first
    long result = sys_get_process_list(procs, max_count);
    if (result >= 0) {
        return (int)result;
    }

    // Fallback: Parse /proc filesystem (when available)
    // For now, generate mock data for testing
    return collect_mock_process_info(procs, max_count);
}

// Mock data generator for testing UI
int collect_mock_process_info(process_info_t* procs, int max_count) {
    if (max_count < 10) {
        return 0;
    }

    // Mock process 1: init
    procs[0].pid = 1;
    procs[0].parent_pid = 0;
    procs[0].state = PROC_RUNNING;
    strcpy(procs[0].name, "init");
    strcpy(procs[0].username, "root");
    procs[0].cpu_percent = 1;
    procs[0].memory_rss = 2048 * 1024;  // 2 MB
    procs[0].memory_shared = 512 * 1024;
    procs[0].memory_virtual = 4096 * 1024;
    procs[0].disk_read = 0;
    procs[0].disk_write = 0;
    procs[0].net_recv = 0;
    procs[0].net_send = 0;
    procs[0].priority = 0;
    procs[0].cpu_affinity = 0xFFFF;
    procs[0].threads = 1;
    procs[0].total_cpu_time = 5000;
    procs[0].start_time = 0;

    // Mock process 2: shell
    procs[1].pid = 2;
    procs[1].parent_pid = 1;
    procs[1].state = PROC_RUNNING;
    strcpy(procs[1].name, "shell");
    strcpy(procs[1].username, "user");
    procs[1].cpu_percent = 5;
    procs[1].memory_rss = 4096 * 1024;  // 4 MB
    procs[1].memory_shared = 1024 * 1024;
    procs[1].memory_virtual = 8192 * 1024;
    procs[1].disk_read = 1024;
    procs[1].disk_write = 512;
    procs[1].net_recv = 0;
    procs[1].net_send = 0;
    procs[1].priority = 0;
    procs[1].cpu_affinity = 0xFFFF;
    procs[1].threads = 1;
    procs[1].total_cpu_time = 12000;
    procs[1].start_time = 100;

    // Mock process 3: taskmanager (self)
    procs[2].pid = 3;
    procs[2].parent_pid = 2;
    procs[2].state = PROC_RUNNING;
    strcpy(procs[2].name, "taskmanager");
    strcpy(procs[2].username, "user");
    procs[2].cpu_percent = 15;
    procs[2].memory_rss = 3072 * 1024;  // 3 MB
    procs[2].memory_shared = 512 * 1024;
    procs[2].memory_virtual = 6144 * 1024;
    procs[2].disk_read = 2048;
    procs[2].disk_write = 1024;
    procs[2].net_recv = 0;
    procs[2].net_send = 0;
    procs[2].priority = 0;
    procs[2].cpu_affinity = 0xFFFF;
    procs[2].threads = 1;
    procs[2].total_cpu_time = 8000;
    procs[2].start_time = 500;

    // Mock process 4: filemanager
    procs[3].pid = 4;
    procs[3].parent_pid = 2;
    procs[3].state = PROC_READY;
    strcpy(procs[3].name, "filemanager");
    strcpy(procs[3].username, "user");
    procs[3].cpu_percent = 8;
    procs[3].memory_rss = 5120 * 1024;  // 5 MB
    procs[3].memory_shared = 2048 * 1024;
    procs[3].memory_virtual = 10240 * 1024;
    procs[3].disk_read = 4096;
    procs[3].disk_write = 2048;
    procs[3].net_recv = 0;
    procs[3].net_send = 0;
    procs[3].priority = 0;
    procs[3].cpu_affinity = 0xFFFF;
    procs[3].threads = 2;
    procs[3].total_cpu_time = 6000;
    procs[3].start_time = 600;

    // Mock process 5: compositor
    procs[4].pid = 5;
    procs[4].parent_pid = 1;
    procs[4].state = PROC_RUNNING;
    strcpy(procs[4].name, "compositor");
    strcpy(procs[4].username, "user");
    procs[4].cpu_percent = 25;
    procs[4].memory_rss = 16384 * 1024;  // 16 MB
    procs[4].memory_shared = 8192 * 1024;
    procs[4].memory_virtual = 32768 * 1024;
    procs[4].disk_read = 512;
    procs[4].disk_write = 256;
    procs[4].net_recv = 0;
    procs[4].net_send = 0;
    procs[4].priority = -5;
    procs[4].cpu_affinity = 0xFFFF;
    procs[4].threads = 4;
    procs[4].total_cpu_time = 45000;
    procs[4].start_time = 200;

    // Mock process 6: network-daemon
    procs[5].pid = 6;
    procs[5].parent_pid = 1;
    procs[5].state = PROC_BLOCKED;
    strcpy(procs[5].name, "network-daemon");
    strcpy(procs[5].username, "system");
    procs[5].cpu_percent = 2;
    procs[5].memory_rss = 1024 * 1024;  // 1 MB
    procs[5].memory_shared = 256 * 1024;
    procs[5].memory_virtual = 2048 * 1024;
    procs[5].disk_read = 0;
    procs[5].disk_write = 0;
    procs[5].net_recv = 1024 * 1024;  // 1 MB/s
    procs[5].net_send = 512 * 1024;   // 512 KB/s
    procs[5].priority = 0;
    procs[5].cpu_affinity = 0xFFFF;
    procs[5].threads = 3;
    procs[5].total_cpu_time = 3000;
    procs[5].start_time = 150;

    // Mock process 7: disk-cache
    procs[6].pid = 7;
    procs[6].parent_pid = 1;
    procs[6].state = PROC_BLOCKED;
    strcpy(procs[6].name, "disk-cache");
    strcpy(procs[6].username, "system");
    procs[6].cpu_percent = 3;
    procs[6].memory_rss = 32768 * 1024;  // 32 MB
    procs[6].memory_shared = 16384 * 1024;
    procs[6].memory_virtual = 65536 * 1024;
    procs[6].disk_read = 8192 * 1024;   // 8 MB/s
    procs[6].disk_write = 4096 * 1024;  // 4 MB/s
    procs[6].net_recv = 0;
    procs[6].net_send = 0;
    procs[6].priority = 5;
    procs[6].cpu_affinity = 0xFFFF;
    procs[6].threads = 2;
    procs[6].total_cpu_time = 7000;
    procs[6].start_time = 180;

    // Mock process 8: scheduler
    procs[7].pid = 0;  // Kernel process
    procs[7].parent_pid = 0;
    procs[7].state = PROC_RUNNING;
    strcpy(procs[7].name, "[kernel]");
    strcpy(procs[7].username, "kernel");
    procs[7].cpu_percent = 5;
    procs[7].memory_rss = 8192 * 1024;  // 8 MB
    procs[7].memory_shared = 0;
    procs[7].memory_virtual = 8192 * 1024;
    procs[7].disk_read = 0;
    procs[7].disk_write = 0;
    procs[7].net_recv = 0;
    procs[7].net_send = 0;
    procs[7].priority = -20;
    procs[7].cpu_affinity = 0xFFFF;
    procs[7].threads = 8;
    procs[7].total_cpu_time = 100000;
    procs[7].start_time = 0;

    return 8;  // Return number of processes
}

// Filter processes by name
int filter_processes(process_info_t* procs, int count, const char* filter) {
    if (!filter || filter[0] == '\0') {
        return count;
    }

    int filtered = 0;
    for (int i = 0; i < count; i++) {
        // Simple substring match (case-insensitive)
        const char* haystack = procs[i].name;
        const char* needle = filter;
        bool match = false;

        for (size_t j = 0; haystack[j]; j++) {
            bool submatch = true;
            for (size_t k = 0; needle[k]; k++) {
                char c1 = haystack[j + k];
                char c2 = needle[k];
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) {
                    submatch = false;
                    break;
                }
            }
            if (submatch) {
                match = true;
                break;
            }
        }

        if (match) {
            if (filtered != i) {
                procs[filtered] = procs[i];
            }
            filtered++;
        }
    }

    return filtered;
}

// Sort processes
void sort_processes(process_info_t* procs, int count, sort_mode_t mode, bool ascending) {
    // Simple bubble sort (sufficient for < 256 processes)
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            bool swap = false;

            switch (mode) {
                case SORT_PID:
                    swap = ascending ? (procs[j].pid > procs[j+1].pid)
                                    : (procs[j].pid < procs[j+1].pid);
                    break;
                case SORT_NAME:
                    swap = ascending ? (strcmp(procs[j].name, procs[j+1].name) > 0)
                                    : (strcmp(procs[j].name, procs[j+1].name) < 0);
                    break;
                case SORT_CPU:
                    swap = ascending ? (procs[j].cpu_percent > procs[j+1].cpu_percent)
                                    : (procs[j].cpu_percent < procs[j+1].cpu_percent);
                    break;
                case SORT_MEMORY:
                    swap = ascending ? (procs[j].memory_rss > procs[j+1].memory_rss)
                                    : (procs[j].memory_rss < procs[j+1].memory_rss);
                    break;
                case SORT_DISK:
                    {
                        uint64_t disk_j = procs[j].disk_read + procs[j].disk_write;
                        uint64_t disk_j1 = procs[j+1].disk_read + procs[j+1].disk_write;
                        swap = ascending ? (disk_j > disk_j1) : (disk_j < disk_j1);
                    }
                    break;
                case SORT_NETWORK:
                    {
                        uint64_t net_j = procs[j].net_recv + procs[j].net_send;
                        uint64_t net_j1 = procs[j+1].net_recv + procs[j+1].net_send;
                        swap = ascending ? (net_j > net_j1) : (net_j < net_j1);
                    }
                    break;
            }

            if (swap) {
                process_info_t temp = procs[j];
                procs[j] = procs[j+1];
                procs[j+1] = temp;
            }
        }
    }
}

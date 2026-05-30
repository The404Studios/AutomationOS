// userspace/apps/taskmanager/utils.c - Utility functions

#include "taskmanager.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"

// Format bytes in human-readable form (B, KB, MB, GB, TB)
const char* format_bytes(uint64_t bytes, char* buffer, int size) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    uint64_t value = bytes;

    while (value >= 1024 && unit < 4) {
        value /= 1024;
        unit++;
    }

    // Simple integer division (no floating point)
    if (unit == 0) {
        snprintf(buffer, size, "%llu B", value);
    } else {
        // Calculate decimal part
        uint64_t temp = bytes;
        for (int i = 0; i < unit - 1; i++) {
            temp /= 1024;
        }
        uint64_t decimal = ((temp % 1024) * 10) / 1024;
        snprintf(buffer, size, "%llu.%llu %s", value, decimal, units[unit]);
    }

    return buffer;
}

// Format rate in human-readable form (B/s, KB/s, MB/s, GB/s)
const char* format_rate(uint64_t bytes_per_sec, char* buffer, int size) {
    const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int unit = 0;
    uint64_t value = bytes_per_sec;

    while (value >= 1024 && unit < 3) {
        value /= 1024;
        unit++;
    }

    if (unit == 0) {
        snprintf(buffer, size, "%llu B/s", value);
    } else {
        uint64_t temp = bytes_per_sec;
        for (int i = 0; i < unit - 1; i++) {
            temp /= 1024;
        }
        uint64_t decimal = ((temp % 1024) * 10) / 1024;
        snprintf(buffer, size, "%llu.%llu %s", value, decimal, units[unit]);
    }

    return buffer;
}

// Format time in human-readable form (HH:MM:SS or days)
const char* format_time(uint64_t seconds, char* buffer, int size) {
    if (seconds < 60) {
        snprintf(buffer, size, "%llus", seconds);
    } else if (seconds < 3600) {
        uint64_t mins = seconds / 60;
        uint64_t secs = seconds % 60;
        snprintf(buffer, size, "%llum%02llus", mins, secs);
    } else if (seconds < 86400) {
        uint64_t hours = seconds / 3600;
        uint64_t mins = (seconds % 3600) / 60;
        uint64_t secs = seconds % 60;
        snprintf(buffer, size, "%lluh%02llum%02llus", hours, mins, secs);
    } else {
        uint64_t days = seconds / 86400;
        uint64_t hours = (seconds % 86400) / 3600;
        uint64_t mins = (seconds % 3600) / 60;
        snprintf(buffer, size, "%llud%02lluh%02llum", days, hours, mins);
    }

    return buffer;
}

// Convert process state to string
const char* state_to_string(proc_state_t state) {
    switch (state) {
        case PROC_CREATED:
            return "Created";
        case PROC_READY:
            return "Ready";
        case PROC_RUNNING:
            return "Running";
        case PROC_BLOCKED:
            return "Blocked";
        case PROC_TERMINATED:
            return "Dead";
        default:
            return "Unknown";
    }
}

// ANSI color helpers
void set_color(int fg, int bg) {
    printf("\033[%d;%dm", fg + 30, bg + 40);
}

void reset_color(void) {
    printf("\033[0m");
}

// Forward declaration for show_process_details
void show_process_details(const process_info_t* proc);

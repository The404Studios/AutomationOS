// userspace/apps/taskmanager/taskmanager.h - Task Manager declarations

#ifndef TASKMANAGER_H
#define TASKMANAGER_H

#include <stdint.h>
#include <stdbool.h>

// Configuration
#define MAX_PROCESSES 256
#define MAX_PROC_NAME 64
#define MAX_USERNAME 32
#define HISTORY_SAMPLES 60  // 60 seconds of history at 1Hz
#define REFRESH_RATE_MS 1000
#define UI_WIDTH 80
#define UI_HEIGHT 24

// Process states (must match kernel)
typedef enum {
    PROC_CREATED,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_TERMINATED
} proc_state_t;

// Process information
typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    proc_state_t state;
    char name[MAX_PROC_NAME];
    char username[MAX_USERNAME];

    // Resource usage
    uint64_t cpu_percent;       // CPU usage percentage (0-100)
    uint64_t memory_rss;        // Resident Set Size (bytes)
    uint64_t memory_shared;     // Shared memory (bytes)
    uint64_t memory_virtual;    // Virtual memory (bytes)
    uint64_t disk_read;         // Disk read bytes/sec
    uint64_t disk_write;        // Disk write bytes/sec
    uint64_t net_recv;          // Network receive bytes/sec
    uint64_t net_send;          // Network send bytes/sec
    uint64_t total_cpu_time;    // Total CPU time (ticks)

    // Process properties
    int32_t priority;           // Process priority
    uint32_t cpu_affinity;      // CPU affinity mask
    uint32_t threads;           // Number of threads
    uint64_t start_time;        // Process start time
} process_info_t;

// System-wide statistics
typedef struct {
    uint32_t total_processes;
    uint32_t running_processes;
    uint32_t blocked_processes;

    // CPU statistics (per-core and total)
    uint8_t cpu_count;
    uint64_t cpu_usage[16];     // Per-core usage (0-100)
    uint64_t cpu_total_usage;   // Total CPU usage

    // Memory statistics
    uint64_t memory_total;      // Total system memory
    uint64_t memory_used;       // Used memory
    uint64_t memory_free;       // Free memory
    uint64_t memory_cached;     // Cached memory
    uint64_t memory_buffers;    // Buffer memory

    // Disk I/O statistics
    uint64_t disk_read_rate;    // Bytes/sec
    uint64_t disk_write_rate;   // Bytes/sec
    uint64_t disk_read_total;   // Total bytes read
    uint64_t disk_write_total;  // Total bytes written

    // Network statistics
    uint64_t net_recv_rate;     // Bytes/sec
    uint64_t net_send_rate;     // Bytes/sec
    uint64_t net_recv_total;    // Total bytes received
    uint64_t net_send_total;    // Total bytes sent

    // System uptime
    uint64_t uptime_seconds;
} system_stats_t;

// Performance history for graphing
typedef struct {
    uint64_t cpu_history[HISTORY_SAMPLES];
    uint64_t memory_history[HISTORY_SAMPLES];
    uint64_t disk_history[HISTORY_SAMPLES];
    uint64_t network_history[HISTORY_SAMPLES];
    uint32_t current_sample;
} perf_history_t;

// UI tabs
typedef enum {
    TAB_PROCESSES,
    TAB_PERFORMANCE,
    TAB_SERVICES,
    TAB_COUNT
} ui_tab_t;

// Sort modes for process list
typedef enum {
    SORT_PID,
    SORT_NAME,
    SORT_CPU,
    SORT_MEMORY,
    SORT_DISK,
    SORT_NETWORK
} sort_mode_t;

// UI state
typedef struct {
    ui_tab_t current_tab;
    sort_mode_t sort_mode;
    bool sort_ascending;
    uint32_t selected_process;
    uint32_t scroll_offset;
    bool show_kernel_processes;
    char search_filter[64];
} ui_state_t;

// Process collection and filtering
int collect_process_info(process_info_t* procs, int max_count);
int filter_processes(process_info_t* procs, int count, const char* filter);
void sort_processes(process_info_t* procs, int count, sort_mode_t mode, bool ascending);

// System statistics
int get_system_stats(system_stats_t* stats);
void update_perf_history(perf_history_t* history, const system_stats_t* stats);

// Process control
int kill_process(uint32_t pid, int signal);
int suspend_process(uint32_t pid);
int resume_process(uint32_t pid);
int set_process_priority(uint32_t pid, int priority);
int set_cpu_affinity(uint32_t pid, uint32_t affinity_mask);

// UI rendering
void render_ui(const ui_state_t* ui, const process_info_t* procs, int proc_count,
               const system_stats_t* stats, const perf_history_t* history);
void render_processes_tab(const ui_state_t* ui, const process_info_t* procs, int count);
void render_performance_tab(const system_stats_t* stats, const perf_history_t* history);
void render_services_tab(void);
void render_header(ui_tab_t current_tab);
void render_footer(const system_stats_t* stats);
void render_graph(const char* title, const uint64_t* data, int samples, uint64_t max_val);

// Input handling
void handle_input(ui_state_t* ui, process_info_t* procs, int proc_count);

// Utility functions
const char* format_bytes(uint64_t bytes, char* buffer, int size);
const char* format_rate(uint64_t bytes_per_sec, char* buffer, int size);
const char* format_time(uint64_t seconds, char* buffer, int size);
const char* state_to_string(proc_state_t state);
void clear_screen(void);
void move_cursor(int row, int col);
void set_color(int fg, int bg);
void reset_color(void);

#endif // TASKMANAGER_H

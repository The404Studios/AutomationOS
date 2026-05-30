#ifndef RLIMIT_H
#define RLIMIT_H

#include "types.h"

// Resource limit types
typedef enum {
    RLIMIT_CPU = 0,          // CPU time in milliseconds
    RLIMIT_MEMORY = 1,       // Memory in bytes (RSS + virtual)
    RLIMIT_RSS = 2,          // Physical memory (resident set size)
    RLIMIT_VMEM = 3,         // Virtual memory
    RLIMIT_NOFILE = 4,       // Number of open file descriptors
    RLIMIT_NPROC = 5,        // Number of processes (per user)
    RLIMIT_NET_RX = 6,       // Network receive bandwidth (bytes/sec)
    RLIMIT_NET_TX = 7,       // Network transmit bandwidth (bytes/sec)
    RLIMIT_DISK_READ = 8,    // Disk read bandwidth (bytes/sec)
    RLIMIT_DISK_WRITE = 9,   // Disk write bandwidth (bytes/sec)
    RLIMIT_MAX = 10
} rlimit_type_t;

// Signal sent when soft limit exceeded
#define SIGXCPU 24

// Resource limit structure (soft and hard limits)
typedef struct {
    uint64_t soft;  // Soft limit (warning threshold)
    uint64_t hard;  // Hard limit (enforcement threshold)
} rlimit_t;

// Special limit values
#define RLIMIT_INFINITY ((uint64_t)-1)
#define RLIMIT_DEFAULT_CPU 10000          // 10 seconds in milliseconds
#define RLIMIT_DEFAULT_MEMORY (128*1024*1024)  // 128MB
#define RLIMIT_DEFAULT_NOFILE 1024
#define RLIMIT_DEFAULT_NPROC 512
#define RLIMIT_DEFAULT_NET_BW (10*1024*1024)  // 10MB/s
#define RLIMIT_DEFAULT_DISK_BW (50*1024*1024) // 50MB/s

// CPU quota structure (proportional scheduling)
typedef struct {
    uint64_t shares;        // CPU shares (relative weight)
    uint64_t quota_us;      // CPU quota in microseconds per period
    uint64_t period_us;     // Period length in microseconds
    uint64_t used_us;       // CPU time used in current period
    uint64_t period_start;  // Start of current period (ticks)
    bool throttled;         // Currently throttled?
} cpu_quota_t;

#define CPU_SHARES_DEFAULT 1024
#define CPU_PERIOD_DEFAULT 100000  // 100ms in microseconds

// Resource usage statistics
typedef struct {
    uint64_t cpu_time;      // Total CPU time used (milliseconds)
    uint64_t memory_peak;   // Peak memory usage (bytes)
    uint64_t memory_current; // Current memory usage (bytes)
    uint64_t rss_current;   // Current RSS (bytes)
    uint64_t vmem_current;  // Current virtual memory (bytes)
    uint32_t fd_count;      // Number of open file descriptors
    uint64_t net_rx_bytes;  // Total bytes received
    uint64_t net_tx_bytes;  // Total bytes transmitted
    uint64_t disk_read_bytes;  // Total bytes read from disk
    uint64_t disk_write_bytes; // Total bytes written to disk
    uint64_t context_switches;  // Number of context switches
    uint64_t page_faults;   // Number of page faults
} rusage_t;

// Token bucket for rate limiting (network, disk I/O)
typedef struct {
    uint64_t capacity;      // Maximum tokens (burst size)
    uint64_t tokens;        // Current tokens available
    uint64_t rate;          // Refill rate (tokens per second)
    uint64_t last_update;   // Last refill time (ticks)
} token_bucket_t;

// Per-process resource limit container
typedef struct rlimit_container {
    rlimit_t limits[RLIMIT_MAX];     // All resource limits
    rusage_t usage;                   // Current usage statistics
    cpu_quota_t cpu_quota;            // CPU quota/shares
    token_bucket_t net_rx_bucket;     // Network RX rate limiter
    token_bucket_t net_tx_bucket;     // Network TX rate limiter
    token_bucket_t disk_read_bucket;  // Disk read rate limiter
    token_bucket_t disk_write_bucket; // Disk write rate limiter
    bool soft_limit_signaled[RLIMIT_MAX]; // Track soft limit signals
} rlimit_container_t;

// Resource limit management
void rlimit_init(void);
rlimit_container_t* rlimit_create_container(void);
void rlimit_destroy_container(rlimit_container_t* rl);
rlimit_container_t* rlimit_inherit_container(rlimit_container_t* parent);

// Limit getters/setters
int rlimit_get(rlimit_container_t* rl, rlimit_type_t type, rlimit_t* out);
int rlimit_set(rlimit_container_t* rl, rlimit_type_t type, const rlimit_t* limit);

// Enforcement - check before allocation
bool rlimit_check_cpu(rlimit_container_t* rl, uint64_t additional_ms);
bool rlimit_check_memory(rlimit_container_t* rl, uint64_t additional_bytes);
bool rlimit_check_rss(rlimit_container_t* rl, uint64_t additional_bytes);
bool rlimit_check_vmem(rlimit_container_t* rl, uint64_t additional_bytes);
bool rlimit_check_fd(rlimit_container_t* rl);
bool rlimit_check_network_rx(rlimit_container_t* rl, uint64_t bytes);
bool rlimit_check_network_tx(rlimit_container_t* rl, uint64_t bytes);
bool rlimit_check_disk_read(rlimit_container_t* rl, uint64_t bytes);
bool rlimit_check_disk_write(rlimit_container_t* rl, uint64_t bytes);

// Usage accounting - update after allocation/deallocation
void rlimit_account_cpu(rlimit_container_t* rl, uint64_t ms);
void rlimit_account_memory_alloc(rlimit_container_t* rl, uint64_t bytes);
void rlimit_account_memory_free(rlimit_container_t* rl, uint64_t bytes);
void rlimit_account_fd_open(rlimit_container_t* rl);
void rlimit_account_fd_close(rlimit_container_t* rl);
void rlimit_account_network_rx(rlimit_container_t* rl, uint64_t bytes);
void rlimit_account_network_tx(rlimit_container_t* rl, uint64_t bytes);
void rlimit_account_disk_read(rlimit_container_t* rl, uint64_t bytes);
void rlimit_account_disk_write(rlimit_container_t* rl, uint64_t bytes);

// CPU quota management
void rlimit_set_cpu_shares(rlimit_container_t* rl, uint64_t shares);
void rlimit_set_cpu_quota(rlimit_container_t* rl, uint64_t quota_us, uint64_t period_us);
bool rlimit_cpu_quota_exceeded(rlimit_container_t* rl);
void rlimit_cpu_quota_refill(rlimit_container_t* rl);

// Token bucket operations
void token_bucket_init(token_bucket_t* bucket, uint64_t rate, uint64_t capacity);
bool token_bucket_consume(token_bucket_t* bucket, uint64_t tokens, uint64_t current_time);
void token_bucket_refill(token_bucket_t* bucket, uint64_t current_time);

// Get current resource usage
void rlimit_get_usage(rlimit_container_t* rl, rusage_t* out);

// OOM killer (called when memory limit exceeded)
void rlimit_oom_kill(struct process* proc);

#endif

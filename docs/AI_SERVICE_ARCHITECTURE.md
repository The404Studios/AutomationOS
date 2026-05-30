# AutomationOS AI Service Architecture

**Version:** 1.0  
**Date:** 2026-05-26  
**Status:** Phase 3 Design Specification

---

## Executive Summary

The AI Service is the defining innovation of AutomationOS - a comprehensive artificial intelligence subsystem running as privileged kernel threads with direct access to all OS internals. Unlike traditional operating systems where AI exists solely in userspace, AutomationOS embeds AI deep within the kernel, enabling real-time system optimization, predictive resource management, security monitoring, and adaptive behavior learning.

**Core Principles:**
1. **Zero-Copy Observability** - Lock-free ring buffers expose all kernel events without performance overhead
2. **Safe Bounded Control** - AI can tune system parameters within carefully designed safety limits
3. **Extensible Plugin Architecture** - Modular AI agents for specialized tasks
4. **Multi-Language Support** - C/C++ for performance, Python for rapid development
5. **Fail-Safe Operation** - AI failures never compromise system stability

---

## 1. AI Thread Manager Architecture

### 1.1 Overview

The AI Thread Manager is the core orchestrator of all AI operations within the kernel. It manages multiple AI worker threads, coordinates communication between AI and kernel subsystems, handles plugin lifecycle, and ensures safe operation under all conditions.

### 1.2 Thread Model

**Architecture:**
```c
// Per-CPU AI worker threads
struct ai_worker_thread {
    uint32_t cpu_id;                    // CPU affinity
    thread_t* kernel_thread;            // Kernel thread handle
    ai_state_t state;                   // RUNNING, PAUSED, CRASHED, STOPPED
    ring_buffer_t* telemetry_queue;     // Local telemetry queue
    plugin_list_t* active_plugins;      // Plugins running on this thread
    uint64_t events_processed;          // Statistics
    uint64_t decisions_made;            // Statistics
    cpu_time_t cpu_time_used;           // Resource tracking
    ai_crash_context_t* crash_info;     // Debug info if crashed
};

struct ai_thread_manager {
    ai_worker_thread_t* workers[MAX_CPUS];
    uint32_t num_workers;
    
    // Global state
    atomic_bool global_enable;          // Master on/off switch
    atomic_bool emergency_stop;         // Emergency shutdown
    
    // Plugin registry
    plugin_registry_t* plugins;
    
    // IPC infrastructure
    shared_memory_t* telemetry_shm;     // Shared telemetry data
    message_queue_t* control_queue;     // AI → Kernel control messages
    message_queue_t* event_queue;       // Kernel → AI event notifications
    
    // Statistics
    atomic_uint64_t total_events;
    atomic_uint64_t total_decisions;
    uint64_t uptime_ns;
    
    // Configuration
    ai_config_t* config;
};
```

**Thread Creation:**
```c
int ai_service_init(void) {
    ai_thread_manager_t* mgr = kmalloc(sizeof(ai_thread_manager_t));
    
    // Initialize global state
    mgr->global_enable = true;
    mgr->emergency_stop = false;
    mgr->num_workers = num_cpus();
    
    // Create telemetry infrastructure
    mgr->telemetry_shm = create_shared_memory(TELEMETRY_SHM_SIZE);
    mgr->control_queue = create_message_queue(1024);
    mgr->event_queue = create_message_queue(4096);
    
    // Initialize plugin registry
    mgr->plugins = plugin_registry_init();
    
    // Create per-CPU worker threads
    for (int i = 0; i < mgr->num_workers; i++) {
        ai_worker_thread_t* worker = kmalloc(sizeof(ai_worker_thread_t));
        worker->cpu_id = i;
        worker->state = AI_STATE_STOPPED;
        worker->telemetry_queue = ring_buffer_create(RING_BUFFER_SIZE);
        worker->active_plugins = list_create();
        
        // Create kernel thread with high priority
        thread_attr_t attr = {
            .priority = PRIORITY_HIGH,
            .cpu_affinity = i,
            .stack_size = 128 * 1024,
            .name = "ai_worker_<cpu_id>"
        };
        worker->kernel_thread = thread_create(ai_worker_main, worker, &attr);
        
        mgr->workers[i] = worker;
    }
    
    // Load built-in plugins
    load_builtin_plugins(mgr);
    
    // Start all workers
    for (int i = 0; i < mgr->num_workers; i++) {
        thread_start(mgr->workers[i]->kernel_thread);
        mgr->workers[i]->state = AI_STATE_RUNNING;
    }
    
    klog(LOG_INFO, "AI Service initialized with %d workers\n", mgr->num_workers);
    return 0;
}
```

**Worker Thread Main Loop:**
```c
void ai_worker_main(void* arg) {
    ai_worker_thread_t* worker = (ai_worker_thread_t*)arg;
    
    klog(LOG_INFO, "AI worker %d starting\n", worker->cpu_id);
    
    while (!ai_should_stop(worker)) {
        // Check for emergency stop
        if (atomic_load(&ai_mgr->emergency_stop)) {
            ai_emergency_shutdown(worker);
            break;
        }
        
        // Process telemetry events
        telemetry_event_t event;
        while (ring_buffer_read(worker->telemetry_queue, &event)) {
            // Dispatch to plugins
            for_each_plugin(worker->active_plugins, plugin) {
                if (plugin_handles_event(plugin, event.type)) {
                    plugin_process_event(plugin, &event, worker);
                }
            }
            worker->events_processed++;
        }
        
        // Process async notifications
        ai_message_t msg;
        if (message_queue_try_recv(ai_mgr->event_queue, &msg, 0)) {
            handle_async_notification(worker, &msg);
        }
        
        // Run periodic plugin tasks
        uint64_t now = get_monotonic_time_ns();
        for_each_plugin(worker->active_plugins, plugin) {
            if (plugin_should_run_periodic(plugin, now)) {
                plugin_periodic_task(plugin, worker);
            }
        }
        
        // Yield CPU (we're not spinning)
        thread_yield();
    }
    
    klog(LOG_INFO, "AI worker %d stopping\n", worker->cpu_id);
    worker->state = AI_STATE_STOPPED;
}
```

### 1.3 Thread Lifecycle Management

**States:**
- `AI_STATE_STOPPED` - Thread not running
- `AI_STATE_RUNNING` - Normal operation
- `AI_STATE_PAUSED` - Temporarily suspended (debugging, updates)
- `AI_STATE_CRASHED` - Fault detected, awaiting restart
- `AI_STATE_RECOVERY` - Restarting after crash

**Pause/Resume:**
```c
// Pause AI operations (for debugging, updates)
int ai_pause_all(void) {
    for (int i = 0; i < ai_mgr->num_workers; i++) {
        atomic_store(&ai_mgr->workers[i]->state, AI_STATE_PAUSED);
    }
    // Wait for all workers to acknowledge pause
    wait_for_all_paused(ai_mgr);
    return 0;
}

int ai_resume_all(void) {
    for (int i = 0; i < ai_mgr->num_workers; i++) {
        atomic_store(&ai_mgr->workers[i]->state, AI_STATE_RUNNING);
    }
    return 0;
}
```

**Crash Recovery:**
```c
void ai_crash_handler(ai_worker_thread_t* worker, exception_t* ex) {
    klog(LOG_ERROR, "AI worker %d crashed: %s\n", worker->cpu_id, ex->message);
    
    // Save crash context
    worker->crash_info = save_crash_context(worker, ex);
    worker->state = AI_STATE_CRASHED;
    
    // Notify system admin
    notify_admin(AI_CRASH_EVENT, worker->crash_info);
    
    // Attempt automatic restart
    if (should_auto_restart(worker)) {
        schedule_delayed_work(ai_restart_worker, worker, 5000); // 5 second delay
    }
}

void ai_restart_worker(void* arg) {
    ai_worker_thread_t* worker = (ai_worker_thread_t*)arg;
    
    worker->state = AI_STATE_RECOVERY;
    
    // Clear telemetry queue
    ring_buffer_clear(worker->telemetry_queue);
    
    // Reload plugins
    reload_plugins(worker);
    
    // Restart thread
    thread_restart(worker->kernel_thread);
    worker->state = AI_STATE_RUNNING;
    
    klog(LOG_INFO, "AI worker %d restarted\n", worker->cpu_id);
}
```

### 1.4 Resource Limits

AI threads are constrained to prevent resource exhaustion:

```c
struct ai_resource_limits {
    uint64_t max_cpu_time_ns;      // 10% of total CPU per second
    uint64_t max_memory_bytes;     // 256 MB per worker
    uint32_t max_plugins;          // 32 plugins per worker
    uint32_t max_decisions_per_sec;// 1000 decisions/sec
    uint64_t max_telemetry_age_ms; // 100ms telemetry latency threshold
};
```

Enforcement happens in the worker loop and during plugin execution.

---

## 2. Telemetry Collection System

### 2.1 Overview

The telemetry system captures every significant kernel operation and exposes it to AI threads with minimal performance overhead. It uses lock-free per-CPU ring buffers for zero-contention writes.

### 2.2 Ring Buffer Architecture

**Design:**
```c
// Lock-free single-producer single-consumer ring buffer
struct ring_buffer {
    telemetry_event_t* events;    // Array of events
    uint32_t capacity;            // Power of 2 size
    atomic_uint32_t write_pos;    // Producer position
    atomic_uint32_t read_pos;     // Consumer position
    uint64_t dropped_events;      // Overflow counter
    uint32_t cpu_id;              // CPU affinity
};

// Create per-CPU ring buffers during boot
ring_buffer_t* telemetry_buffers[MAX_CPUS];

void telemetry_init(void) {
    for (int i = 0; i < num_cpus(); i++) {
        telemetry_buffers[i] = ring_buffer_create(RING_BUFFER_SIZE);
        telemetry_buffers[i]->cpu_id = i;
    }
}
```

**Write Operation (lock-free):**
```c
static inline bool ring_buffer_write(ring_buffer_t* rb, telemetry_event_t* event) {
    uint32_t write = atomic_load_relaxed(&rb->write_pos);
    uint32_t read = atomic_load_acquire(&rb->read_pos);
    
    // Check if buffer is full
    if (write - read >= rb->capacity) {
        atomic_inc(&rb->dropped_events);
        return false;
    }
    
    // Write event
    uint32_t index = write & (rb->capacity - 1);
    rb->events[index] = *event;
    
    // Advance write pointer with release semantics
    atomic_store_release(&rb->write_pos, write + 1);
    return true;
}
```

**Read Operation:**
```c
static inline bool ring_buffer_read(ring_buffer_t* rb, telemetry_event_t* event) {
    uint32_t read = atomic_load_relaxed(&rb->read_pos);
    uint32_t write = atomic_load_acquire(&rb->write_pos);
    
    // Check if buffer is empty
    if (read == write) {
        return false;
    }
    
    // Read event
    uint32_t index = read & (rb->capacity - 1);
    *event = rb->events[index];
    
    // Advance read pointer
    atomic_store_release(&rb->read_pos, read + 1);
    return true;
}
```

### 2.3 Event Types

**Core Event Categories:**

```c
enum telemetry_event_type {
    // Memory events
    TEV_MEM_ALLOC = 0x1000,        // Physical/virtual allocation
    TEV_MEM_FREE,                  // Memory freed
    TEV_MEM_PAGE_FAULT,            // Page fault
    TEV_MEM_OOM,                   // Out of memory
    TEV_MEM_PRESSURE,              // Memory pressure threshold
    
    // Process events
    TEV_PROC_CREATE = 0x2000,      // fork/exec
    TEV_PROC_EXIT,                 // Process exit
    TEV_PROC_SCHED,                // Context switch
    TEV_PROC_WAIT,                 // Wait event
    TEV_PROC_PRIORITY_CHANGE,      // Priority adjustment
    
    // Syscall events
    TEV_SYSCALL_ENTER = 0x3000,    // Syscall entry
    TEV_SYSCALL_EXIT,              // Syscall exit
    TEV_SYSCALL_SLOW,              // Slow syscall (>1ms)
    
    // I/O events
    TEV_IO_READ = 0x4000,          // File/device read
    TEV_IO_WRITE,                  // File/device write
    TEV_IO_OPEN,                   // File open
    TEV_IO_CLOSE,                  // File close
    TEV_IO_BLOCK,                  // Blocked on I/O
    
    // Network events
    TEV_NET_CONNECT = 0x5000,      // Socket connect
    TEV_NET_ACCEPT,                // Socket accept
    TEV_NET_SEND,                  // Data sent
    TEV_NET_RECV,                  // Data received
    TEV_NET_ERROR,                 // Network error
    
    // Interrupt events
    TEV_IRQ_ENTER = 0x6000,        // Interrupt handler entry
    TEV_IRQ_EXIT,                  // Interrupt handler exit
    
    // Filesystem events
    TEV_FS_CREATE = 0x7000,        // File/dir creation
    TEV_FS_DELETE,                 // File/dir deletion
    TEV_FS_RENAME,                 // Rename
    TEV_FS_CHMOD,                  // Permission change
    
    // Security events
    TEV_SEC_CAP_DENY = 0x8000,     // Capability violation
    TEV_SEC_SANDBOX_ESCAPE,        // Sandbox escape attempt
    TEV_SEC_PRIVILEGE_ESC,         // Privilege escalation
    TEV_SEC_ANOMALY,               // Unusual behavior
    
    // Device events
    TEV_DEV_ATTACH = 0x9000,       // Device hot-plug
    TEV_DEV_DETACH,                // Device removal
    TEV_DEV_ERROR,                 // Device error
    
    // AI events
    TEV_AI_DECISION = 0xA000,      // AI made decision
    TEV_AI_TUNING,                 // Parameter tuned
    TEV_AI_ALERT,                  // AI detected issue
};
```

**Event Structure:**
```c
struct telemetry_event {
    uint64_t timestamp_ns;         // Monotonic timestamp
    uint32_t cpu_id;               // CPU where event occurred
    uint32_t type;                 // Event type (from enum)
    
    // Context
    pid_t pid;                     // Process ID (0 for kernel)
    tid_t tid;                     // Thread ID
    
    // Event-specific data (union for different event types)
    union {
        struct {                   // Memory event
            uint64_t address;
            uint64_t size;
            uint32_t flags;
        } mem;
        
        struct {                   // Process event
            pid_t target_pid;
            uint32_t exit_code;
            uint64_t cpu_time_ns;
        } proc;
        
        struct {                   // Syscall event
            uint32_t syscall_num;
            uint64_t args[6];
            int64_t result;
            uint64_t latency_ns;
        } syscall;
        
        struct {                   // I/O event
            uint32_t fd;
            uint64_t offset;
            uint64_t size;
            uint64_t latency_ns;
        } io;
        
        struct {                   // Network event
            uint32_t socket_fd;
            uint32_t remote_ip;
            uint16_t remote_port;
            uint64_t bytes;
        } net;
        
        struct {                   // Security event
            uint32_t capability;
            char resource[64];
            uint32_t severity;
        } sec;
        
        // ... additional event types
        
        uint8_t raw[128];          // Raw data fallback
    } data;
};
```

### 2.4 Instrumentation Points

**Example: Memory Allocation Instrumentation**
```c
void* kmalloc(size_t size) {
    uint64_t start = rdtsc();
    
    void* ptr = buddy_allocator_alloc(size);
    
    uint64_t end = rdtsc();
    
    // Emit telemetry event
    telemetry_event_t event = {
        .timestamp_ns = get_monotonic_time_ns(),
        .cpu_id = get_cpu_id(),
        .type = TEV_MEM_ALLOC,
        .pid = current_process ? current_process->pid : 0,
        .tid = current_thread ? current_thread->tid : 0,
        .data.mem = {
            .address = (uint64_t)ptr,
            .size = size,
            .flags = 0
        }
    };
    telemetry_emit(&event);
    
    return ptr;
}

static inline void telemetry_emit(telemetry_event_t* event) {
    uint32_t cpu = event->cpu_id;
    ring_buffer_write(telemetry_buffers[cpu], event);
}
```

**Critical Path Optimization:**
For hot paths (scheduler, fast syscalls), use sampling:
```c
// Sample 1% of scheduler events
if (fast_rand() % 100 == 0) {
    telemetry_emit(&event);
}
```

### 2.5 Aggregation & Statistics

AI threads consume raw events and compute derived metrics:

```c
struct telemetry_aggregator {
    // Memory stats
    uint64_t total_mem_alloc_bytes;
    uint64_t total_mem_free_bytes;
    uint64_t current_mem_usage;
    double mem_alloc_rate_per_sec;
    
    // CPU stats
    uint64_t total_context_switches;
    double context_switch_rate_per_sec;
    uint64_t total_cpu_time_ns[MAX_CPUS];
    
    // I/O stats
    uint64_t total_read_bytes;
    uint64_t total_write_bytes;
    double io_read_bps;
    double io_write_bps;
    
    // Network stats
    uint64_t total_net_sent_bytes;
    uint64_t total_net_recv_bytes;
    uint32_t active_connections;
    
    // Moving averages (exponential)
    double avg_syscall_latency_ns;
    double avg_page_fault_latency_ns;
    double avg_io_latency_ns;
    
    // Anomaly detection
    uint64_t anomaly_score;
    bool high_memory_pressure;
    bool high_cpu_contention;
};
```

AI plugins update aggregators in real-time and use them for decision-making.

---

## 3. Control Interface

### 3.1 Overview

The control interface allows AI threads to safely tune kernel parameters and trigger actions. Every control has well-defined bounds and rollback capability.

### 3.2 Tunable Parameters

**Parameter Registry:**
```c
enum tunable_param {
    // Scheduler tunables
    TUNE_SCHED_PRIORITY = 1000,    // Process priority adjustment
    TUNE_SCHED_TIMESLICE,          // Timeslice duration
    TUNE_SCHED_BOOST,              // Temporary priority boost
    TUNE_SCHED_AFFINITY,           // CPU affinity hint
    
    // Memory tunables
    TUNE_MEM_SWAPPINESS,           // Swap aggressiveness (0-100)
    TUNE_MEM_CACHE_PRESSURE,       // Page cache pressure
    TUNE_MEM_COMPACTION,           // Memory compaction trigger
    TUNE_MEM_RECLAIM_THRESHOLD,    // OOM threshold
    
    // I/O tunables
    TUNE_IO_SCHEDULER_MODE,        // CFQ, Deadline, NOOP
    TUNE_IO_READ_AHEAD_KB,         // Read-ahead size
    TUNE_IO_QUEUE_DEPTH,           // Device queue depth
    TUNE_IO_MERGE_REQUESTS,        // Request merging on/off
    
    // Filesystem tunables
    TUNE_FS_CACHE_SIZE,            // Page cache size limit
    TUNE_FS_DIRTY_RATIO,           // Dirty page writeback threshold
    TUNE_FS_SYNC_INTERVAL,         // Periodic sync interval
    
    // Network tunables
    TUNE_NET_BUFFER_SIZE,          // Socket buffer sizes
    TUNE_NET_CONGESTION_ALGO,      // TCP congestion control
    TUNE_NET_KEEPALIVE,            // TCP keepalive settings
    TUNE_NET_RMEM_MAX,             // Max receive buffer
    TUNE_NET_WMEM_MAX,             // Max send buffer
};

struct tunable_def {
    uint32_t param_id;
    char name[64];
    enum { INT, UINT, BOOL, STRING } type;
    
    // Bounds
    int64_t min_value;
    int64_t max_value;
    int64_t default_value;
    
    // Validation
    bool (*validate)(int64_t value);
    
    // Apply function
    int (*apply)(int64_t value);
    
    // Rollback function
    int (*rollback)(void);
    
    // Metadata
    char description[256];
    uint32_t risk_level;           // 0=safe, 10=dangerous
};
```

**Tunable Application:**
```c
int ai_tune_parameter(ai_worker_thread_t* worker, uint32_t param_id, int64_t value) {
    tunable_def_t* tunable = get_tunable_def(param_id);
    if (!tunable) {
        return -EINVAL;
    }
    
    // Bounds check
    if (value < tunable->min_value || value > tunable->max_value) {
        klog(LOG_WARN, "AI tuning out of bounds: %s = %ld (min=%ld, max=%ld)\n",
             tunable->name, value, tunable->min_value, tunable->max_value);
        return -ERANGE;
    }
    
    // Custom validation
    if (tunable->validate && !tunable->validate(value)) {
        return -EINVAL;
    }
    
    // Save current value for rollback
    int64_t old_value = tunable_get_current(param_id);
    tunable_save_state(param_id, old_value);
    
    // Apply tuning
    int ret = tunable->apply(value);
    if (ret < 0) {
        klog(LOG_ERROR, "AI tuning failed: %s = %ld\n", tunable->name, value);
        return ret;
    }
    
    // Record decision
    telemetry_event_t event = {
        .timestamp_ns = get_monotonic_time_ns(),
        .cpu_id = worker->cpu_id,
        .type = TEV_AI_TUNING,
        .data.raw = {param_id, old_value, value}
    };
    telemetry_emit(&event);
    
    worker->decisions_made++;
    
    klog(LOG_INFO, "AI tuned %s: %ld → %ld\n", tunable->name, old_value, value);
    return 0;
}
```

### 3.3 Safe Bounds

**Example: Scheduler Priority Tuning**
```c
static tunable_def_t tune_sched_priority = {
    .param_id = TUNE_SCHED_PRIORITY,
    .name = "sched.priority",
    .type = INT,
    .min_value = PRIORITY_MIN,         // e.g., 1
    .max_value = PRIORITY_MAX - 10,    // Reserve top 10 for critical tasks
    .default_value = PRIORITY_NORMAL,
    .validate = validate_priority,
    .apply = apply_priority_change,
    .rollback = rollback_priority,
    .description = "Process scheduling priority",
    .risk_level = 2  // Low risk
};

static bool validate_priority(int64_t value) {
    // Don't allow AI to boost init or kernel threads
    process_t* proc = current_process;
    if (proc->pid == 1 || proc->flags & PROC_FLAG_KERNEL) {
        return false;
    }
    return true;
}

static int apply_priority_change(int64_t new_priority) {
    process_t* proc = current_process;
    proc->priority = (uint8_t)new_priority;
    scheduler_update_priority(proc);
    return 0;
}
```

**Example: Memory Swappiness (High Risk)**
```c
static tunable_def_t tune_mem_swappiness = {
    .param_id = TUNE_MEM_SWAPPINESS,
    .name = "mem.swappiness",
    .type = UINT,
    .min_value = 10,               // Never too low (thrashing)
    .max_value = 90,               // Never too high (excess swapping)
    .default_value = 60,
    .validate = NULL,
    .apply = apply_swappiness,
    .rollback = rollback_swappiness,
    .description = "Memory swap aggressiveness (10-90)",
    .risk_level = 7  // High risk - can cause thrashing
};
```

### 3.4 Rollback Mechanism

AI changes are tracked and can be reverted:

```c
struct tunable_history_entry {
    uint64_t timestamp_ns;
    uint32_t param_id;
    int64_t old_value;
    int64_t new_value;
    ai_worker_thread_t* worker;
};

// Global history (ring buffer)
tunable_history_entry_t tunable_history[HISTORY_SIZE];
atomic_uint32_t history_pos;

void tunable_save_state(uint32_t param_id, int64_t old_value) {
    uint32_t pos = atomic_fetch_add(&history_pos, 1) % HISTORY_SIZE;
    tunable_history[pos] = (tunable_history_entry_t){
        .timestamp_ns = get_monotonic_time_ns(),
        .param_id = param_id,
        .old_value = old_value,
        .new_value = tunable_get_current(param_id),
        .worker = current_ai_worker
    };
}

// Rollback last N changes
int ai_rollback_recent_changes(uint32_t count) {
    for (int i = 0; i < count; i++) {
        uint32_t pos = (history_pos - 1 - i) % HISTORY_SIZE;
        tunable_history_entry_t* entry = &tunable_history[pos];
        
        tunable_def_t* tunable = get_tunable_def(entry->param_id);
        if (tunable->rollback) {
            tunable->rollback();
        } else {
            tunable->apply(entry->old_value);
        }
    }
    return 0;
}
```

### 3.5 Control Actions

Beyond parameter tuning, AI can trigger actions:

```c
enum ai_action {
    AI_ACTION_KILL_PROCESS,        // Terminate runaway process
    AI_ACTION_PAUSE_PROCESS,       // Pause process (debugging)
    AI_ACTION_RESUME_PROCESS,      // Resume paused process
    AI_ACTION_RECLAIM_MEMORY,      // Trigger memory reclamation
    AI_ACTION_COMPACT_MEMORY,      // Defragment memory
    AI_ACTION_FLUSH_CACHE,         // Flush filesystem cache
    AI_ACTION_RELOAD_DRIVER,       // Restart driver
    AI_ACTION_ALERT_ADMIN,         // Notify system admin
    AI_ACTION_THROTTLE_IO,         // Throttle I/O for process
    AI_ACTION_THROTTLE_NET,        // Throttle network for process
};

int ai_execute_action(ai_worker_thread_t* worker, enum ai_action action, void* args) {
    // Validate permissions
    if (!ai_can_execute_action(worker, action)) {
        return -EPERM;
    }
    
    // Execute action with audit log
    int ret = 0;
    switch (action) {
        case AI_ACTION_KILL_PROCESS: {
            pid_t pid = *(pid_t*)args;
            ret = process_kill(pid, SIGKILL);
            klog(LOG_WARN, "AI killed process %d\n", pid);
            break;
        }
        
        case AI_ACTION_RECLAIM_MEMORY:
            ret = memory_reclaim(MEM_RECLAIM_SOFT);
            klog(LOG_INFO, "AI triggered memory reclaim\n");
            break;
        
        case AI_ACTION_ALERT_ADMIN: {
            char* message = (char*)args;
            send_admin_notification(message);
            break;
        }
        
        // ... other actions
    }
    
    // Audit log
    audit_log_ai_action(worker, action, args, ret);
    
    return ret;
}
```

---

## 4. Plugin System Architecture

### 4.1 Overview

The plugin system allows modular AI agents to be loaded dynamically, either at boot or runtime. Plugins subscribe to event types and implement specific AI behaviors.

### 4.2 Plugin Interface

**Plugin Structure:**
```c
struct ai_plugin {
    // Metadata
    char name[64];
    char description[256];
    uint32_t version;
    char author[64];
    
    // Lifecycle callbacks
    int (*init)(ai_plugin_t* plugin, ai_worker_thread_t* worker);
    void (*shutdown)(ai_plugin_t* plugin);
    
    // Event handling
    uint32_t* event_types;         // Array of event types to subscribe to
    uint32_t num_event_types;
    void (*process_event)(ai_plugin_t* plugin, telemetry_event_t* event, ai_worker_thread_t* worker);
    
    // Periodic tasks
    uint64_t periodic_interval_ns; // 0 = no periodic task
    void (*periodic_task)(ai_plugin_t* plugin, ai_worker_thread_t* worker);
    
    // Configuration
    void* config;                  // Plugin-specific config
    
    // State
    void* private_data;            // Plugin-specific state
    
    // Capabilities
    uint64_t required_capabilities; // What plugin can do
    
    // Statistics
    uint64_t events_handled;
    uint64_t decisions_made;
    uint64_t errors;
};
```

**Plugin Registration:**
```c
int ai_register_plugin(ai_plugin_t* plugin) {
    // Validate plugin
    if (!plugin->name || !plugin->process_event) {
        return -EINVAL;
    }
    
    // Check capabilities
    if (!validate_plugin_capabilities(plugin)) {
        klog(LOG_ERROR, "Plugin %s has invalid capabilities\n", plugin->name);
        return -EPERM;
    }
    
    // Initialize plugin
    if (plugin->init && plugin->init(plugin, NULL) < 0) {
        klog(LOG_ERROR, "Plugin %s init failed\n", plugin->name);
        return -EFAULT;
    }
    
    // Add to registry
    list_add(ai_mgr->plugins, plugin);
    
    // Load into all workers
    for (int i = 0; i < ai_mgr->num_workers; i++) {
        plugin_load_into_worker(plugin, ai_mgr->workers[i]);
    }
    
    klog(LOG_INFO, "AI plugin registered: %s v%d\n", plugin->name, plugin->version);
    return 0;
}
```

### 4.3 Built-in Plugins

#### 4.3.1 Performance Optimizer Plugin

**Purpose:** Dynamically tune scheduler, memory, and I/O based on workload patterns.

**Implementation:**
```c
struct perf_optimizer_state {
    // Workload classification
    enum { WORKLOAD_IDLE, WORKLOAD_IO, WORKLOAD_CPU, WORKLOAD_MIXED } workload_type;
    
    // Metrics
    double cpu_util_avg;
    double io_wait_avg;
    uint64_t mem_pressure;
    
    // Tuning history
    uint64_t last_tune_time_ns;
    uint32_t tune_count;
};

static void perf_optimizer_process_event(ai_plugin_t* plugin, telemetry_event_t* event, ai_worker_thread_t* worker) {
    perf_optimizer_state_t* state = (perf_optimizer_state_t*)plugin->private_data;
    
    switch (event->type) {
        case TEV_PROC_SCHED:
            // Track context switches
            state->cpu_util_avg = update_moving_avg(state->cpu_util_avg, calculate_cpu_util());
            break;
        
        case TEV_IO_BLOCK:
            // Track I/O wait time
            state->io_wait_avg = update_moving_avg(state->io_wait_avg, event->data.io.latency_ns);
            break;
        
        case TEV_MEM_PRESSURE:
            // Track memory pressure
            state->mem_pressure = event->data.mem.size;
            break;
    }
}

static void perf_optimizer_periodic_task(ai_plugin_t* plugin, ai_worker_thread_t* worker) {
    perf_optimizer_state_t* state = (perf_optimizer_state_t*)plugin->private_data;
    
    // Classify workload
    if (state->cpu_util_avg > 0.8 && state->io_wait_avg < 0.1) {
        state->workload_type = WORKLOAD_CPU;
    } else if (state->io_wait_avg > 0.5) {
        state->workload_type = WORKLOAD_IO;
    } else if (state->cpu_util_avg < 0.2) {
        state->workload_type = WORKLOAD_IDLE;
    } else {
        state->workload_type = WORKLOAD_MIXED;
    }
    
    // Apply optimizations
    switch (state->workload_type) {
        case WORKLOAD_CPU:
            // Reduce timeslice for better responsiveness
            ai_tune_parameter(worker, TUNE_SCHED_TIMESLICE, 5);  // 5ms
            break;
        
        case WORKLOAD_IO:
            // Increase read-ahead
            ai_tune_parameter(worker, TUNE_IO_READ_AHEAD_KB, 512);
            // Lower swappiness (keep working set in RAM)
            ai_tune_parameter(worker, TUNE_MEM_SWAPPINESS, 20);
            break;
        
        case WORKLOAD_IDLE:
            // Trigger memory reclaim
            ai_execute_action(worker, AI_ACTION_RECLAIM_MEMORY, NULL);
            break;
    }
    
    state->tune_count++;
}

static int perf_optimizer_init(ai_plugin_t* plugin, ai_worker_thread_t* worker) {
    perf_optimizer_state_t* state = kmalloc(sizeof(perf_optimizer_state_t));
    memset(state, 0, sizeof(*state));
    plugin->private_data = state;
    return 0;
}

static ai_plugin_t perf_optimizer_plugin = {
    .name = "performance_optimizer",
    .description = "Dynamically tunes system for workload patterns",
    .version = 1,
    .author = "AutomationOS",
    .init = perf_optimizer_init,
    .shutdown = NULL,
    .event_types = (uint32_t[]){TEV_PROC_SCHED, TEV_IO_BLOCK, TEV_MEM_PRESSURE},
    .num_event_types = 3,
    .process_event = perf_optimizer_process_event,
    .periodic_interval_ns = 1000000000,  // Run every 1 second
    .periodic_task = perf_optimizer_periodic_task,
    .required_capabilities = CAP_AI_TUNE_SCHEDULER | CAP_AI_TUNE_MEMORY | CAP_AI_TUNE_IO
};
```

#### 4.3.2 Health Monitor Plugin

**Purpose:** Detect resource leaks, runaway processes, and system health issues.

**Implementation:**
```c
struct health_monitor_state {
    // Per-process tracking
    hash_map_t* process_stats;  // pid → process_health_t
    
    // Thresholds
    uint64_t mem_leak_threshold;
    double cpu_runaway_threshold;
    uint32_t fd_leak_threshold;
};

struct process_health {
    pid_t pid;
    uint64_t baseline_mem;
    uint64_t current_mem;
    uint64_t mem_growth_rate;
    double cpu_usage;
    uint32_t open_fds;
    uint64_t last_check_time;
    bool flagged;
};

static void health_monitor_process_event(ai_plugin_t* plugin, telemetry_event_t* event, ai_worker_thread_t* worker) {
    health_monitor_state_t* state = (health_monitor_state_t*)plugin->private_data;
    
    if (event->type == TEV_MEM_ALLOC) {
        process_health_t* health = hash_map_get(state->process_stats, event->pid);
        if (!health) {
            health = create_process_health(event->pid);
            hash_map_put(state->process_stats, event->pid, health);
        }
        
        health->current_mem += event->data.mem.size;
        
        // Check for memory leak
        uint64_t growth = health->current_mem - health->baseline_mem;
        if (growth > state->mem_leak_threshold) {
            klog(LOG_WARN, "AI detected potential memory leak in PID %d: %lu bytes\n",
                 event->pid, growth);
            
            // Alert admin
            char msg[256];
            snprintf(msg, sizeof(msg), "Memory leak detected: PID %d, %lu MB leaked",
                     event->pid, growth / (1024*1024));
            ai_execute_action(worker, AI_ACTION_ALERT_ADMIN, msg);
            
            health->flagged = true;
        }
    }
}

static void health_monitor_periodic_task(ai_plugin_t* plugin, ai_worker_thread_t* worker) {
    health_monitor_state_t* state = (health_monitor_state_t*)plugin->private_data;
    
    // Scan all processes
    for_each_process(proc) {
        process_health_t* health = hash_map_get(state->process_stats, proc->pid);
        if (!health) continue;
        
        // Check CPU usage
        double cpu_usage = calculate_cpu_usage(proc);
        if (cpu_usage > state->cpu_runaway_threshold) {
            klog(LOG_WARN, "AI detected runaway process: PID %d, %.1f%% CPU\n",
                 proc->pid, cpu_usage * 100);
            
            // Option 1: Throttle
            // Option 2: Kill (extreme)
            if (cpu_usage > 0.95) {  // 95%+ CPU
                ai_execute_action(worker, AI_ACTION_KILL_PROCESS, &proc->pid);
            }
        }
        
        // Check file descriptor leaks
        if (proc->num_open_fds > state->fd_leak_threshold) {
            klog(LOG_WARN, "AI detected FD leak: PID %d, %u open FDs\n",
                 proc->pid, proc->num_open_fds);
        }
    }
}

static ai_plugin_t health_monitor_plugin = {
    .name = "health_monitor",
    .description = "Detects resource leaks and runaway processes",
    .version = 1,
    .author = "AutomationOS",
    .init = health_monitor_init,
    .shutdown = health_monitor_shutdown,
    .event_types = (uint32_t[]){TEV_MEM_ALLOC, TEV_MEM_FREE, TEV_PROC_CREATE, TEV_PROC_EXIT},
    .num_event_types = 4,
    .process_event = health_monitor_process_event,
    .periodic_interval_ns = 5000000000,  // Every 5 seconds
    .periodic_task = health_monitor_periodic_task,
    .required_capabilities = CAP_AI_KILL_PROCESS | CAP_AI_ALERT
};
```

#### 4.3.3 Security Auditor Plugin

**Purpose:** Monitor for sandbox escapes, privilege escalation, and anomalous behavior.

**Implementation:**
```c
struct security_auditor_state {
    // Anomaly detection
    hash_map_t* baseline_behavior;  // pid → behavior_profile_t
    
    // Alert counters
    uint64_t sandbox_violations;
    uint64_t privilege_esc_attempts;
    uint64_t anomalies;
};

struct behavior_profile {
    pid_t pid;
    
    // Normal behavior
    uint32_t typical_syscalls[NUM_SYSCALLS];  // Frequency histogram
    uint32_t typical_file_access_patterns;
    uint32_t typical_network_patterns;
    
    // Deviations
    uint32_t anomaly_score;
};

static void security_auditor_process_event(ai_plugin_t* plugin, telemetry_event_t* event, ai_worker_thread_t* worker) {
    security_auditor_state_t* state = (security_auditor_state_t*)plugin->private_data;
    
    switch (event->type) {
        case TEV_SEC_CAP_DENY:
            // Capability violation - possible sandbox escape attempt
            state->sandbox_violations++;
            klog(LOG_ALERT, "AI detected capability violation: PID %d, capability %s\n",
                 event->pid, capability_name(event->data.sec.capability));
            
            // High severity - kill process
            if (event->data.sec.severity >= 8) {
                ai_execute_action(worker, AI_ACTION_KILL_PROCESS, &event->pid);
            }
            break;
        
        case TEV_SEC_PRIVILEGE_ESC:
            // Privilege escalation attempt
            state->privilege_esc_attempts++;
            klog(LOG_ALERT, "AI detected privilege escalation: PID %d\n", event->pid);
            ai_execute_action(worker, AI_ACTION_KILL_PROCESS, &event->pid);
            break;
        
        case TEV_SYSCALL_ENTER:
            // Check for unusual syscall patterns
            behavior_profile_t* profile = get_behavior_profile(state, event->pid);
            if (is_anomalous_syscall(profile, event->data.syscall.syscall_num)) {
                profile->anomaly_score++;
                
                if (profile->anomaly_score > ANOMALY_THRESHOLD) {
                    klog(LOG_WARN, "AI detected anomalous behavior: PID %d\n", event->pid);
                    state->anomalies++;
                    
                    // Alert admin
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Anomalous process behavior: PID %d", event->pid);
                    ai_execute_action(worker, AI_ACTION_ALERT_ADMIN, msg);
                }
            }
            break;
    }
}

static ai_plugin_t security_auditor_plugin = {
    .name = "security_auditor",
    .description = "Monitors for security violations and anomalies",
    .version = 1,
    .author = "AutomationOS",
    .init = security_auditor_init,
    .shutdown = security_auditor_shutdown,
    .event_types = (uint32_t[]){TEV_SEC_CAP_DENY, TEV_SEC_SANDBOX_ESCAPE, 
                                TEV_SEC_PRIVILEGE_ESC, TEV_SYSCALL_ENTER},
    .num_event_types = 4,
    .process_event = security_auditor_process_event,
    .periodic_interval_ns = 0,  // Event-driven only
    .periodic_task = NULL,
    .required_capabilities = CAP_AI_KILL_PROCESS | CAP_AI_ALERT | CAP_AI_AUDIT
};
```

### 4.4 Custom Plugin Development

**Example: Custom Prefetcher Plugin**
```c
// user_prefetcher_plugin.c
#include <automation/ai_plugin.h>

struct prefetcher_state {
    hash_map_t* access_patterns;  // file → access_pattern_t
};

struct access_pattern {
    char filepath[256];
    uint64_t last_access_time;
    uint32_t access_count;
    uint32_t predicted_next_access_offset;
};

static void prefetcher_process_event(ai_plugin_t* plugin, telemetry_event_t* event, ai_worker_thread_t* worker) {
    if (event->type != TEV_IO_READ) return;
    
    prefetcher_state_t* state = plugin->private_data;
    
    // Learn access pattern
    // ... ML model to predict next read offset
    
    // Issue prefetch
    if (should_prefetch(...)) {
        filesystem_prefetch(fd, predicted_offset, predicted_size);
    }
}

static ai_plugin_t my_prefetcher = {
    .name = "ml_prefetcher",
    .description = "Machine learning-based prefetcher",
    .version = 1,
    .author = "User",
    .process_event = prefetcher_process_event,
    .event_types = (uint32_t[]){TEV_IO_READ},
    .num_event_types = 1,
    .periodic_interval_ns = 0,
    .required_capabilities = CAP_AI_PREFETCH
};

// Load plugin at runtime
int load_custom_plugin(void) {
    return ai_register_plugin(&my_prefetcher);
}
```

---

## 5. Python Integration

### 5.1 Overview

Python is embedded in kernel space to enable rapid prototyping of AI behaviors. A custom memory allocator ensures Python doesn't exhaust kernel memory.

### 5.2 Embedded CPython

**Initialization:**
```c
int python_init_kernel_space(void) {
    // Set custom memory allocator
    PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &kernel_pymem_allocator);
    PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &kernel_pymem_allocator);
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &kernel_pymem_allocator);
    
    // Initialize Python interpreter
    Py_Initialize();
    
    // Add kernel module to sys.modules
    PyObject* kernel_module = PyModule_Create(&kernel_module_def);
    PyModule_AddObject(PyImport_GetModuleDict(), "kernel", kernel_module);
    
    klog(LOG_INFO, "Python initialized in kernel space\n");
    return 0;
}
```

**Custom Allocator:**
```c
static void* kernel_pymem_malloc(void* ctx, size_t size) {
    // Use kernel heap with limits
    if (current_python_mem_usage + size > PYTHON_MEM_LIMIT) {
        return NULL;  // Allocation denied
    }
    void* ptr = kmalloc(size);
    if (ptr) {
        atomic_add(&current_python_mem_usage, size);
    }
    return ptr;
}

static void kernel_pymem_free(void* ctx, void* ptr) {
    if (!ptr) return;
    size_t size = ksize(ptr);  // Get allocation size
    kfree(ptr);
    atomic_sub(&current_python_mem_usage, size);
}
```

### 5.3 Kernel Module Bindings

**Expose kernel functions to Python:**
```c
// kernel module (C side)
static PyObject* py_tune_parameter(PyObject* self, PyObject* args) {
    uint32_t param_id;
    int64_t value;
    
    if (!PyArg_ParseTuple(args, "Il", &param_id, &value)) {
        return NULL;
    }
    
    int ret = ai_tune_parameter(current_ai_worker, param_id, value);
    
    return PyLong_FromLong(ret);
}

static PyObject* py_get_telemetry(PyObject* self, PyObject* args) {
    // Return aggregated telemetry as Python dict
    PyObject* dict = PyDict_New();
    
    telemetry_aggregator_t* agg = get_current_aggregator();
    
    PyDict_SetItemString(dict, "mem_usage", PyLong_FromUnsignedLongLong(agg->current_mem_usage));
    PyDict_SetItemString(dict, "cpu_util", PyFloat_FromDouble(agg->total_cpu_time_ns));
    PyDict_SetItemString(dict, "io_read_bps", PyFloat_FromDouble(agg->io_read_bps));
    // ... more fields
    
    return dict;
}

static PyMethodDef kernel_methods[] = {
    {"tune_parameter", py_tune_parameter, METH_VARARGS, "Tune kernel parameter"},
    {"get_telemetry", py_get_telemetry, METH_NOARGS, "Get telemetry snapshot"},
    {"execute_action", py_execute_action, METH_VARARGS, "Execute AI action"},
    {NULL, NULL, 0, NULL}
};
```

### 5.4 Python AI Plugin Example

**Python-based plugin:**
```python
# performance_tuner.py
import kernel

class PerformanceTuner:
    def __init__(self):
        self.baseline_cpu = 0.5
        self.baseline_mem = 1024 * 1024 * 1024  # 1 GB
        
    def process_event(self, event):
        if event['type'] == 'TEV_MEM_PRESSURE':
            # Memory pressure detected
            telemetry = kernel.get_telemetry()
            
            if telemetry['mem_usage'] > self.baseline_mem * 1.5:
                # Reduce page cache
                kernel.tune_parameter(kernel.TUNE_MEM_CACHE_PRESSURE, 200)
                
                # Trigger reclaim
                kernel.execute_action(kernel.AI_ACTION_RECLAIM_MEMORY)
    
    def periodic_task(self):
        telemetry = kernel.get_telemetry()
        
        # Simple heuristic: if CPU high and I/O low, reduce timeslice
        if telemetry['cpu_util'] > 0.8 and telemetry['io_wait'] < 0.1:
            kernel.tune_parameter(kernel.TUNE_SCHED_TIMESLICE, 5)  # 5ms

# Register plugin
plugin = PerformanceTuner()
kernel.register_python_plugin(plugin)
```

**Loading Python plugins:**
```c
int load_python_plugin(const char* script_path) {
    FILE* fp = fopen(script_path, "r");
    if (!fp) {
        return -ENOENT;
    }
    
    // Execute Python script
    PyRun_SimpleFile(fp, script_path);
    
    fclose(fp);
    
    klog(LOG_INFO, "Loaded Python AI plugin: %s\n", script_path);
    return 0;
}
```

---

## 6. Performance Optimizer Design

### 6.1 Architecture

The Performance Optimizer is a sophisticated built-in plugin that learns workload patterns and applies multi-dimensional tuning.

### 6.2 Workload Classification

**ML-Based Classification:**
```c
enum workload_class {
    WC_IDLE,           // <20% CPU, <10% I/O
    WC_CPU_BOUND,      // >80% CPU, <20% I/O wait
    WC_IO_BOUND,       // >50% I/O wait, any CPU
    WC_BALANCED,       // Moderate CPU and I/O
    WC_MEMORY_HEAVY,   // High memory churn
    WC_NETWORK_HEAVY,  // High network I/O
    WC_INTERACTIVE,    // Frequent context switches, low latency needs
    WC_BATCH,          // Long-running, throughput-focused
};

struct workload_features {
    double cpu_utilization;
    double io_wait_ratio;
    double mem_churn_rate;
    double net_throughput;
    double context_switch_rate;
    double syscall_rate;
    double avg_latency_ms;
};

workload_class classify_workload(workload_features_t* features) {
    // Simple decision tree (could be replaced with ML model)
    if (features->cpu_utilization < 0.2) {
        return WC_IDLE;
    } else if (features->io_wait_ratio > 0.5) {
        return WC_IO_BOUND;
    } else if (features->cpu_utilization > 0.8 && features->io_wait_ratio < 0.2) {
        return WC_CPU_BOUND;
    } else if (features->mem_churn_rate > MEM_CHURN_THRESHOLD) {
        return WC_MEMORY_HEAVY;
    } else if (features->net_throughput > NET_THROUGHPUT_THRESHOLD) {
        return WC_NETWORK_HEAVY;
    } else if (features->context_switch_rate > 1000) {
        return WC_INTERACTIVE;
    } else {
        return WC_BALANCED;
    }
}
```

### 6.3 Tuning Strategies

**Per-Workload Tuning Profiles:**
```c
struct tuning_profile {
    // Scheduler
    uint32_t timeslice_ms;
    uint32_t priority_boost;
    
    // Memory
    uint32_t swappiness;
    uint32_t cache_pressure;
    
    // I/O
    uint32_t read_ahead_kb;
    enum io_scheduler_mode io_sched;
    
    // Network
    uint32_t tcp_rmem_max;
    uint32_t tcp_wmem_max;
};

static tuning_profile_t tuning_profiles[] = {
    [WC_CPU_BOUND] = {
        .timeslice_ms = 5,           // Short timeslice for responsiveness
        .priority_boost = 0,
        .swappiness = 10,            // Keep everything in RAM
        .cache_pressure = 50,
        .read_ahead_kb = 128,
        .io_sched = IO_SCHED_NOOP,   // Minimize I/O overhead
    },
    [WC_IO_BOUND] = {
        .timeslice_ms = 20,          // Longer timeslice, fewer context switches
        .priority_boost = 5,
        .swappiness = 60,
        .cache_pressure = 100,
        .read_ahead_kb = 1024,       // Aggressive read-ahead
        .io_sched = IO_SCHED_CFQ,    // Fair queueing
    },
    [WC_INTERACTIVE] = {
        .timeslice_ms = 3,           // Very short timeslice
        .priority_boost = 10,        // Boost foreground apps
        .swappiness = 20,
        .cache_pressure = 50,
        .read_ahead_kb = 256,
        .io_sched = IO_SCHED_DEADLINE,
    },
    // ... other profiles
};

void apply_tuning_profile(workload_class wc) {
    tuning_profile_t* profile = &tuning_profiles[wc];
    
    ai_tune_parameter(NULL, TUNE_SCHED_TIMESLICE, profile->timeslice_ms);
    ai_tune_parameter(NULL, TUNE_MEM_SWAPPINESS, profile->swappiness);
    ai_tune_parameter(NULL, TUNE_MEM_CACHE_PRESSURE, profile->cache_pressure);
    ai_tune_parameter(NULL, TUNE_IO_READ_AHEAD_KB, profile->read_ahead_kb);
    ai_tune_parameter(NULL, TUNE_IO_SCHEDULER_MODE, profile->io_sched);
}
```

### 6.4 Adaptive Learning

**Feedback Loop:**
```c
struct tuning_experiment {
    uint64_t start_time;
    workload_class wc;
    tuning_profile_t applied_profile;
    
    // Metrics before
    double baseline_throughput;
    double baseline_latency;
    
    // Metrics after
    double achieved_throughput;
    double achieved_latency;
    
    // Outcome
    bool successful;  // Improvement or not
};

void evaluate_tuning_experiment(tuning_experiment_t* exp) {
    double throughput_improvement = 
        (exp->achieved_throughput - exp->baseline_throughput) / exp->baseline_throughput;
    double latency_improvement = 
        (exp->baseline_latency - exp->achieved_latency) / exp->baseline_latency;
    
    exp->successful = (throughput_improvement > 0.05 || latency_improvement > 0.05);
    
    if (exp->successful) {
        // Update tuning profile
        save_tuning_profile(exp->wc, &exp->applied_profile);
    } else {
        // Revert
        ai_rollback_recent_changes(5);
    }
}
```

---

## 7. Health Monitor Design

### 7.1 Resource Leak Detection

**Memory Leak Detection:**
```c
struct leak_detector {
    hash_map_t* process_memory_tracking;  // pid → mem_timeline_t
    uint64_t leak_detection_threshold;    // e.g., 100 MB growth in 10 minutes
};

struct mem_timeline {
    pid_t pid;
    uint64_t samples[SAMPLE_SIZE];  // Ring buffer of memory usage samples
    uint32_t sample_index;
    uint64_t baseline;
    uint64_t current;
    double growth_rate;  // bytes per second
};

void detect_memory_leak(leak_detector_t* detector, pid_t pid) {
    mem_timeline_t* timeline = hash_map_get(detector->process_memory_tracking, pid);
    if (!timeline) return;
    
    // Calculate linear regression on samples
    double growth_rate = calculate_growth_rate(timeline->samples, SAMPLE_SIZE);
    
    // If sustained growth over threshold, flag as leak
    if (growth_rate > detector->leak_detection_threshold) {
        klog(LOG_WARN, "Memory leak detected: PID %d, %.2f MB/min\n",
             pid, growth_rate * 60.0 / (1024*1024));
        
        // Alert admin
        ai_execute_action(current_ai_worker, AI_ACTION_ALERT_ADMIN, 
                         format_leak_message(pid, growth_rate));
    }
}
```

**File Descriptor Leak Detection:**
```c
void detect_fd_leak(pid_t pid) {
    process_t* proc = process_get(pid);
    if (!proc) return;
    
    if (proc->num_open_fds > FD_LEAK_THRESHOLD) {
        klog(LOG_WARN, "FD leak detected: PID %d, %u open FDs\n", 
             pid, proc->num_open_fds);
        
        // List top FDs
        for (int i = 0; i < proc->num_open_fds && i < 10; i++) {
            klog(LOG_INFO, "  FD %d: %s\n", i, proc->fds[i]->path);
        }
    }
}
```

### 7.2 Runaway Process Detection

**CPU Runaway:**
```c
void detect_cpu_runaway(void) {
    for_each_process(proc) {
        double cpu_usage = calculate_cpu_usage(proc);
        
        // If process consuming >90% CPU for >30 seconds, flag
        if (cpu_usage > 0.9) {
            uint64_t duration = get_monotonic_time_ns() - proc->high_cpu_start_time;
            if (duration > 30 * NSEC_PER_SEC) {
                klog(LOG_WARN, "CPU runaway detected: PID %d (%s), %.1f%% CPU\n",
                     proc->pid, proc->name, cpu_usage * 100);
                
                // Option 1: Throttle
                ai_tune_parameter(NULL, TUNE_SCHED_PRIORITY, PRIORITY_LOW);
                
                // Option 2: Kill if critical
                if (cpu_usage > 0.98) {
                    ai_execute_action(current_ai_worker, AI_ACTION_KILL_PROCESS, &proc->pid);
                }
            }
        } else {
            proc->high_cpu_start_time = get_monotonic_time_ns();
        }
    }
}
```

### 7.3 Disk Space Monitoring

```c
void monitor_disk_space(void) {
    for_each_filesystem(fs) {
        uint64_t total = fs->total_blocks * fs->block_size;
        uint64_t free = fs->free_blocks * fs->block_size;
        double usage_pct = 100.0 * (1.0 - (double)free / total);
        
        if (usage_pct > 90.0) {
            klog(LOG_WARN, "Disk space critical: %s at %.1f%% full\n", 
                 fs->mount_point, usage_pct);
            
            // Trigger cleanup
            ai_execute_action(current_ai_worker, AI_ACTION_FLUSH_CACHE, NULL);
            
            // Suggest files to delete
            // ...
        }
    }
}
```

---

## 8. Security Auditor Design

### 8.1 Anomaly Detection

**Behavioral Profiling:**
```c
struct behavior_profile {
    pid_t pid;
    
    // Syscall frequency distribution
    uint32_t syscall_histogram[NUM_SYSCALLS];
    uint32_t total_syscalls;
    
    // File access patterns
    hash_set_t* accessed_files;
    
    // Network patterns
    hash_set_t* contacted_hosts;
    
    // Timestamps
    uint64_t profile_start_time;
    uint64_t last_update_time;
};

void update_behavior_profile(behavior_profile_t* profile, telemetry_event_t* event) {
    if (event->type == TEV_SYSCALL_ENTER) {
        uint32_t syscall_num = event->data.syscall.syscall_num;
        profile->syscall_histogram[syscall_num]++;
        profile->total_syscalls++;
    }
    // ... other event types
}

bool is_anomalous(behavior_profile_t* profile, telemetry_event_t* event) {
    if (event->type != TEV_SYSCALL_ENTER) return false;
    
    uint32_t syscall_num = event->data.syscall.syscall_num;
    double expected_freq = (double)profile->syscall_histogram[syscall_num] / profile->total_syscalls;
    
    // If this syscall is rare (<1% of total), flag as anomaly
    if (expected_freq < 0.01 && profile->total_syscalls > 1000) {
        return true;
    }
    
    return false;
}
```

### 8.2 Sandbox Violation Tracking

```c
void handle_sandbox_violation(pid_t pid, uint32_t capability, const char* resource) {
    // Increment violation counter
    process_t* proc = process_get(pid);
    proc->sandbox_violations++;
    
    // Log to audit trail
    audit_log(AUDIT_SANDBOX_VIOLATION, pid, capability, resource);
    
    // Progressive response
    if (proc->sandbox_violations == 1) {
        // First offense: warning
        klog(LOG_WARN, "Sandbox violation: PID %d tried to access %s\n", pid, resource);
    } else if (proc->sandbox_violations < 5) {
        // Multiple offenses: throttle
        ai_execute_action(current_ai_worker, AI_ACTION_PAUSE_PROCESS, &pid);
    } else {
        // Persistent violations: kill
        klog(LOG_ALERT, "Repeated sandbox violations: killing PID %d\n", pid);
        ai_execute_action(current_ai_worker, AI_ACTION_KILL_PROCESS, &pid);
    }
}
```

### 8.3 Privilege Escalation Detection

```c
void detect_privilege_escalation(pid_t pid) {
    process_t* proc = process_get(pid);
    
    // Check for suspicious capability gains
    uint64_t new_caps = proc->capabilities & ~proc->initial_capabilities;
    
    if (new_caps != 0) {
        klog(LOG_ALERT, "Privilege escalation detected: PID %d gained capabilities 0x%lx\n",
             pid, new_caps);
        
        // Immediate termination
        ai_execute_action(current_ai_worker, AI_ACTION_KILL_PROCESS, &pid);
        
        // Alert admin
        char msg[256];
        snprintf(msg, sizeof(msg), "Privilege escalation: PID %d", pid);
        ai_execute_action(current_ai_worker, AI_ACTION_ALERT_ADMIN, msg);
    }
}
```

---

## 9. System Integration

### 9.1 Boot Sequence

```c
// kernel_main.c
void kernel_main(boot_info_t* boot_info) {
    // ... basic initialization
    
    mem_init(boot_info->memory_map);
    interrupts_init();
    scheduler_init();
    
    // Start AI service early (Phase 3)
    ai_service_init();
    
    vfs_init();
    drivers_init();
    network_init();
    
    // Load AI plugins
    ai_load_builtin_plugins();
    ai_load_config_plugins("/etc/ai/plugins/");
    
    // ... rest of boot
}
```

### 9.2 Syscall Integration

**New AI-specific syscalls:**
```c
// AI query syscall
long sys_ai_query(int query_type, void __user* data, size_t len) {
    if (!capable(CAP_AI_QUERY)) {
        return -EPERM;
    }
    
    switch (query_type) {
        case AI_QUERY_TELEMETRY:
            return copy_telemetry_to_user(data, len);
        case AI_QUERY_STATUS:
            return copy_ai_status_to_user(data, len);
        case AI_QUERY_DECISIONS:
            return copy_recent_decisions_to_user(data, len);
        default:
            return -EINVAL;
    }
}

// AI tuning syscall (privileged)
long sys_ai_tune(int param_id, int64_t value) {
    if (!capable(CAP_SYS_ADMIN)) {
        return -EPERM;
    }
    
    return ai_tune_parameter(current_ai_worker, param_id, value);
}

// AI control syscall
long sys_ai_control(int action, void __user* args) {
    if (!capable(CAP_SYS_ADMIN)) {
        return -EPERM;
    }
    
    switch (action) {
        case AI_CTRL_PAUSE:
            return ai_pause_all();
        case AI_CTRL_RESUME:
            return ai_resume_all();
        case AI_CTRL_RELOAD_PLUGINS:
            return ai_reload_plugins();
        case AI_CTRL_EMERGENCY_STOP:
            atomic_store(&ai_mgr->emergency_stop, true);
            return 0;
        default:
            return -EINVAL;
    }
}
```

### 9.3 Procfs Integration

**Expose AI status via /proc/ai/**
```
/proc/ai/
├── status              # Overall AI service status
├── workers/
│   ├── 0/              # Per-CPU worker
│   │   ├── state       # RUNNING, PAUSED, CRASHED
│   │   ├── stats       # Events processed, decisions made
│   │   ├── plugins     # Loaded plugins
│   │   └── stack       # Current stack trace
│   ├── 1/
│   └── ...
├── telemetry/
│   ├── aggregate       # Aggregated telemetry
│   └── raw             # Recent raw events
├── plugins/
│   ├── performance_optimizer
│   ├── health_monitor
│   └── security_auditor
├── decisions/          # Recent AI decisions
└── config              # Current configuration
```

---

## 10. Configuration & Tuning

### 10.1 Configuration File

**/etc/ai/config.toml:**
```toml
[ai_service]
enabled = true
num_workers = -1  # -1 = auto (one per CPU)
emergency_stop = false

[telemetry]
ring_buffer_size = 65536
sample_rate = 1.0  # 1.0 = 100%, 0.01 = 1% sampling
max_event_age_ms = 100

[resource_limits]
max_cpu_time_percent = 10  # 10% of total CPU
max_memory_mb = 256
max_plugins_per_worker = 32
max_decisions_per_sec = 1000

[plugins]
builtin = ["performance_optimizer", "health_monitor", "security_auditor"]
custom = ["/etc/ai/plugins/custom_prefetcher.so"]
python_scripts = ["/etc/ai/plugins/ml_tuner.py"]

[performance_optimizer]
enabled = true
workload_classification_interval_ms = 1000
auto_tune = true

[health_monitor]
enabled = true
mem_leak_threshold_mb = 100
cpu_runaway_threshold = 0.9
fd_leak_threshold = 10000
check_interval_ms = 5000

[security_auditor]
enabled = true
anomaly_detection = true
auto_kill_on_privilege_esc = true
sandbox_violation_tolerance = 5
```

### 10.2 Runtime Configuration

**Via syscall:**
```c
ai_config_t config;
sys_ai_query(AI_QUERY_CONFIG, &config, sizeof(config));

config.telemetry.sample_rate = 0.1;  // Reduce to 10% sampling
sys_ai_control(AI_CTRL_UPDATE_CONFIG, &config);
```

**Via procfs:**
```bash
echo 0.1 > /proc/ai/config/telemetry/sample_rate
```

---

## 11. Performance Considerations

### 11.1 Zero-Copy Telemetry

- Lock-free ring buffers eliminate contention
- AI threads read directly from shared memory
- No syscalls required for telemetry access

**Overhead:** <1% CPU, <10 MB memory per worker

### 11.2 Sampling

For hot paths (>100k events/sec), use sampling:
- Default: 100% for rare events, 1-10% for frequent events
- Adaptive: AI adjusts sample rate based on load

### 11.3 Memory Footprint

**Per-Worker Memory:**
- Ring buffer: 64 KB × 2 (telemetry + control) = 128 KB
- Plugin state: ~1 MB per plugin × 3 plugins = 3 MB
- Python interpreter: ~50 MB (shared across workers)
- Total: ~4 MB per worker

**8-core system:** 8 workers × 4 MB + 50 MB Python = ~82 MB

### 11.4 Latency

- Telemetry write: <50 ns (lock-free)
- Event dispatch to plugin: <1 µs
- Parameter tuning: <100 µs (includes validation)

---

## 12. Security & Safety

### 12.1 Fail-Safe Operation

**Watchdog:**
```c
void ai_watchdog_thread(void) {
    while (true) {
        for (int i = 0; i < ai_mgr->num_workers; i++) {
            ai_worker_thread_t* worker = ai_mgr->workers[i];
            
            // Check if worker is responsive
            if (!ai_worker_responsive(worker, 5000)) {  // 5 second timeout
                klog(LOG_ERROR, "AI worker %d unresponsive, restarting\n", i);
                ai_restart_worker(worker);
            }
            
            // Check resource usage
            if (worker->cpu_time_used > CPU_TIME_LIMIT) {
                klog(LOG_WARN, "AI worker %d exceeded CPU limit\n", i);
                ai_pause_worker(worker);
            }
        }
        
        thread_sleep(1000);  // Check every second
    }
}
```

### 12.2 Emergency Stop

**User-triggered emergency stop:**
```bash
# Immediately halt all AI operations
echo 1 > /proc/ai/emergency_stop
```

**Automatic triggers:**
- AI causes system instability (crash loop detected)
- AI exceeds resource limits repeatedly
- Manual override by admin

### 12.3 Audit Trail

All AI decisions logged:
```c
void audit_log_ai_action(ai_worker_thread_t* worker, enum ai_action action, void* args, int result) {
    audit_entry_t entry = {
        .timestamp = get_monotonic_time_ns(),
        .worker_id = worker->cpu_id,
        .action = action,
        .result = result,
        .args_hash = hash(args),
    };
    
    append_to_audit_log(&entry);
}
```

Audit log stored in `/var/log/ai_audit.log`.

---

## 13. Testing Strategy

### 13.1 Unit Tests

**Test ring buffer:**
```c
void test_ring_buffer(void) {
    ring_buffer_t* rb = ring_buffer_create(16);
    
    // Write 10 events
    for (int i = 0; i < 10; i++) {
        telemetry_event_t event = {.timestamp_ns = i};
        assert(ring_buffer_write(rb, &event));
    }
    
    // Read 10 events
    for (int i = 0; i < 10; i++) {
        telemetry_event_t event;
        assert(ring_buffer_read(rb, &event));
        assert(event.timestamp_ns == i);
    }
    
    // Buffer should be empty
    telemetry_event_t event;
    assert(!ring_buffer_read(rb, &event));
}
```

### 13.2 Integration Tests

**Test AI plugin loading:**
```c
void test_ai_plugin_loading(void) {
    // Load plugin
    ai_plugin_t* plugin = &perf_optimizer_plugin;
    assert(ai_register_plugin(plugin) == 0);
    
    // Verify loaded on all workers
    for (int i = 0; i < ai_mgr->num_workers; i++) {
        assert(worker_has_plugin(ai_mgr->workers[i], plugin));
    }
    
    // Emit test event
    telemetry_event_t event = {.type = TEV_MEM_PRESSURE};
    telemetry_emit(&event);
    
    // Wait for processing
    thread_sleep(100);
    
    // Verify plugin handled event
    assert(plugin->events_handled > 0);
}
```

### 13.3 Stress Tests

**Telemetry flood:**
```c
void stress_test_telemetry(void) {
    // Emit 1M events
    for (int i = 0; i < 1000000; i++) {
        telemetry_event_t event = {
            .timestamp_ns = get_monotonic_time_ns(),
            .cpu_id = get_cpu_id(),
            .type = TEV_MEM_ALLOC,
        };
        telemetry_emit(&event);
    }
    
    // Check for dropped events
    for (int i = 0; i < num_cpus(); i++) {
        ring_buffer_t* rb = telemetry_buffers[i];
        klog(LOG_INFO, "CPU %d: %lu dropped events\n", i, rb->dropped_events);
    }
}
```

### 13.4 AI Behavior Tests

**Test performance optimizer:**
```c
void test_performance_optimizer(void) {
    // Simulate CPU-bound workload
    simulate_workload(WORKLOAD_CPU, 10000);  // 10 seconds
    
    // Check if AI tuned timeslice
    int64_t timeslice = tunable_get_current(TUNE_SCHED_TIMESLICE);
    assert(timeslice <= 10);  // Should reduce timeslice for CPU-bound
    
    // Simulate I/O-bound workload
    simulate_workload(WORKLOAD_IO, 10000);
    
    // Check if AI increased read-ahead
    int64_t read_ahead = tunable_get_current(TUNE_IO_READ_AHEAD_KB);
    assert(read_ahead >= 512);  // Should increase for I/O-bound
}
```

---

## 14. Future Enhancements

### 14.1 Machine Learning Models

Replace simple heuristics with trained models:
- **Workload classification:** Neural network trained on telemetry features
- **Anomaly detection:** Autoencoder for behavioral profiling
- **Prefetching:** LSTM for access pattern prediction
- **Tuning optimization:** Reinforcement learning for parameter selection

### 14.2 Distributed AI

For multi-machine setups:
- Share telemetry across cluster
- Coordinated tuning decisions
- Centralized anomaly detection

### 14.3 User-Facing AI

Expose AI insights to users:
- Desktop widget showing AI decisions
- Recommendations (e.g., "AI suggests closing unused apps")
- Performance reports

### 14.4 Self-Improving AI

- A/B testing of tuning strategies
- Automatic hyperparameter optimization
- Evolutionary algorithms for plugin development

---

## 15. Conclusion

The AI Service is the beating heart of AutomationOS, transforming a traditional kernel into an intelligent, self-optimizing system. By running AI as privileged kernel threads with comprehensive observability and bounded control, AutomationOS achieves unprecedented system automation while maintaining safety and stability.

**Key Innovations:**
1. **Lock-Free Telemetry** - Zero-overhead observability at kernel scale
2. **Safe Bounded Control** - AI can optimize without destabilizing
3. **Extensible Plugins** - Modular AI agents for diverse tasks
4. **Multi-Language Support** - C for performance, Python for flexibility
5. **Fail-Safe Design** - AI failures never compromise system stability

**Phase 3 Deliverables:**
- AI thread manager with per-CPU workers
- Ring buffer telemetry collection
- Tunable control interface with safe bounds
- Plugin system with 3 built-in plugins
- Python integration with kernel bindings
- Comprehensive testing suite

This architecture positions AutomationOS as the world's first truly AI-native operating system.

---

**End of AI Service Architecture**

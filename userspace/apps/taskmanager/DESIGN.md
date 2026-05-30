# Task Manager Design Specification

## Executive Summary

The AutomationOS Task Manager is a comprehensive system monitoring and process management application inspired by Windows Task Manager and Linux htop. It provides real-time visibility into system resource usage, process management, and performance monitoring through a terminal-based user interface.

## Design Goals

### Primary Goals
1. **Comprehensive Monitoring**: Display all critical system metrics in one place
2. **Low Overhead**: Minimal CPU/memory usage (< 1% CPU, < 65 KB RAM)
3. **Real-Time Updates**: 1-second refresh rate with smooth rendering
4. **User-Friendly**: Intuitive keyboard-driven interface, no mouse required
5. **Extensible**: Modular architecture for easy feature additions

### Secondary Goals
- Color-coded visual indicators for quick status assessment
- Sort and filter capabilities for large process lists
- Detailed per-process information on demand
- Integration with kernel security features (namespaces, capabilities)

## Architecture

### Modular Design

```
┌─────────────────────────────────────────────────────────────┐
│                     Task Manager                            │
├─────────────────────────────────────────────────────────────┤
│  Main Loop (taskmanager.c)                                  │
│    ├─ Initialization                                        │
│    ├─ Update loop (1 Hz)                                    │
│    └─ Cleanup                                               │
├─────────────────────────────────────────────────────────────┤
│  Data Collection Layer                                      │
│    ├─ procinfo.c  - Process enumeration & filtering        │
│    └─ sysinfo.c   - System statistics collection           │
├─────────────────────────────────────────────────────────────┤
│  Process Control Layer                                      │
│    └─ procctl.c   - Kill/suspend/resume/priority           │
├─────────────────────────────────────────────────────────────┤
│  Presentation Layer                                         │
│    ├─ ui.c        - Rendering (tabs, graphs, tables)       │
│    ├─ input.c     - Keyboard input handling                │
│    └─ utils.c     - Formatting utilities                   │
├─────────────────────────────────────────────────────────────┤
│  Kernel Interface (System Calls)                            │
│    ├─ SYS_GET_PROCESS_LIST (100)                           │
│    ├─ SYS_GET_SYSTEM_INFO  (101)                           │
│    ├─ SYS_KILL             (9)                             │
│    ├─ SYS_SET_PRIORITY     (202)                           │
│    └─ SYS_SET_AFFINITY     (203)                           │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow

```
Kernel Space                  User Space (Task Manager)
┌──────────┐                 ┌──────────────────────┐
│ Process  │                 │  collect_process_   │
│  Table   │──sys_get_list──▶│      info()         │
│          │                 └──────────────────────┘
│ Scheduler│                          │
│ Stats    │──sys_get_info──▶         ▼
│          │                 ┌──────────────────────┐
│ Memory   │                 │   filter_processes() │
│ Manager  │                 │   sort_processes()   │
└──────────┘                 └──────────────────────┘
                                      │
                                      ▼
                             ┌──────────────────────┐
                             │    render_ui()       │
                             │  ┌────────────────┐  │
                             │  │ Processes Tab  │  │
                             │  ├────────────────┤  │
                             │  │Performance Tab │  │
                             │  ├────────────────┤  │
                             │  │  Services Tab  │  │
                             │  └────────────────┘  │
                             └──────────────────────┘
                                      │
                                      ▼
                             ┌──────────────────────┐
                             │  Terminal Output     │
                             │  (ANSI escape codes) │
                             └──────────────────────┘
```

## User Interface Design

### Layout Hierarchy

```
Screen (80×24)
├─ Header (1 line)
│  └─ Tab selector [Processes] [Performance] [Services]
├─ Separator (1 line)
├─ Content Area (19 lines)
│  ├─ Processes Tab
│  │  ├─ Column headers (1 line)
│  │  ├─ Process rows (15 lines, scrollable)
│  │  ├─ Sort indicator (1 line)
│  │  └─ Controls help (2 lines)
│  ├─ Performance Tab
│  │  ├─ CPU section (6 lines)
│  │  ├─ Memory section (4 lines)
│  │  ├─ Disk I/O section (2 lines)
│  │  ├─ Network I/O section (2 lines)
│  │  └─ CPU history graph (5 lines)
│  └─ Services Tab
│     └─ Service list (placeholder)
└─ Footer (2 lines)
   └─ System summary bar
```

### Color Scheme

```
Element                    Foreground  Background  Style
─────────────────────────────────────────────────────────
Header (inactive tab)      White       White       Normal
Header (active tab)        Green       White       Bold
Separator                  White       Black       Normal
Process (normal)           White       Black       Normal
Process (selected)         Blue        White       Bold
State: Running             Green       Black       Normal
State: Blocked             Yellow      Black       Normal
State: Terminated          Red         Black       Normal
CPU: < 25%                 White       Black       Normal
CPU: 25-50%                Yellow      Black       Normal
CPU: > 50%                 Red         Black       Normal
Memory bar (< 60%)         Green       Black       Normal
Memory bar (60-80%)        Yellow      Black       Normal
Memory bar (> 80%)         Red         Black       Normal
Footer                     Blue        White       Bold
```

### ANSI Escape Codes Used

```c
#define ESC               "\033["

// Cursor control
#define CLEAR_SCREEN      ESC "2J" ESC "H"
#define MOVE_CURSOR(r,c)  ESC #r ";" #c "H"

// Colors
#define COLOR_RESET       ESC "0m"
#define COLOR_BOLD        ESC "1m"
#define COLOR_RED         ESC "31m"
#define COLOR_GREEN       ESC "32m"
#define COLOR_YELLOW      ESC "33m"
#define COLOR_BLUE        ESC "34m"
#define COLOR_CYAN        ESC "36m"
#define COLOR_WHITE       ESC "37m"
#define COLOR_BG_WHITE    ESC "47m"
```

## Data Structures

### Process Information
```c
typedef struct {
    // Identity
    uint32_t pid;
    uint32_t parent_pid;
    proc_state_t state;
    char name[64];
    char username[32];

    // Resource usage (dynamic)
    uint64_t cpu_percent;        // Calculated from time deltas
    uint64_t memory_rss;         // From page table walk
    uint64_t memory_shared;      // Shared page count
    uint64_t memory_virtual;     // Virtual address space size

    // I/O statistics (accumulated)
    uint64_t disk_read;          // Bytes/sec (rate)
    uint64_t disk_write;
    uint64_t net_recv;
    uint64_t net_send;

    // Process properties
    int32_t priority;            // Nice value (-20 to +19)
    uint32_t cpu_affinity;       // Bitmask of allowed CPUs
    uint32_t threads;            // Thread count
    uint64_t total_cpu_time;     // Lifetime CPU ticks
    uint64_t start_time;         // Process start timestamp
} process_info_t;
```

### System Statistics
```c
typedef struct {
    // Process counts
    uint32_t total_processes;
    uint32_t running_processes;
    uint32_t blocked_processes;

    // CPU statistics
    uint8_t cpu_count;           // Number of CPU cores
    uint64_t cpu_usage[16];      // Per-core usage (0-100%)
    uint64_t cpu_total_usage;    // Average across all cores

    // Memory statistics (bytes)
    uint64_t memory_total;
    uint64_t memory_used;
    uint64_t memory_free;
    uint64_t memory_cached;
    uint64_t memory_buffers;

    // I/O rates (bytes/second)
    uint64_t disk_read_rate;
    uint64_t disk_write_rate;
    uint64_t net_recv_rate;
    uint64_t net_send_rate;

    // I/O totals (lifetime bytes)
    uint64_t disk_read_total;
    uint64_t disk_write_total;
    uint64_t net_recv_total;
    uint64_t net_send_total;

    // System info
    uint64_t uptime_seconds;
} system_stats_t;
```

### Performance History
```c
typedef struct {
    uint64_t cpu_history[60];      // 60 seconds at 1 Hz
    uint64_t memory_history[60];
    uint64_t disk_history[60];
    uint64_t network_history[60];
    uint32_t current_sample;       // Circular buffer index
} perf_history_t;
```

## Algorithms

### Process Sorting
```
Algorithm: Bubble Sort
Complexity: O(n²)
Justification: Simple, sufficient for n < 256

for i = 0 to n-1:
    for j = 0 to n-i-2:
        if compare(procs[j], procs[j+1], sort_mode):
            swap(procs[j], procs[j+1])
```

### CPU Percentage Calculation
```
Per-process CPU%:
    delta_time = current_time - last_sample_time
    delta_cpu = current_cpu_time - last_cpu_time
    cpu_percent = (delta_cpu * 100) / (delta_time * cpu_count)

Total CPU usage:
    cpu_total = sum(cpu_usage[i] for i in 0..cpu_count-1) / cpu_count
```

### Memory Usage Tracking
```
RSS (Resident Set Size):
    Walk process page table
    Count present pages
    RSS = page_count * PAGE_SIZE

Shared memory:
    For each mapped page:
        if page_refcount > 1:
            shared_size += PAGE_SIZE

Virtual memory:
    Sum of all VMA (Virtual Memory Area) sizes
```

### Performance History Update
```
Circular buffer update:
    idx = history.current_sample
    history.cpu_history[idx] = stats.cpu_total_usage
    history.memory_history[idx] = (stats.memory_used * 100) / stats.memory_total
    history.disk_history[idx] = (stats.disk_read_rate + stats.disk_write_rate) / 1MB
    history.network_history[idx] = (stats.net_recv_rate + stats.net_send_rate) / 1MB
    history.current_sample = (idx + 1) % HISTORY_SAMPLES
```

## Performance Optimization

### Minimize System Call Overhead
1. **Batch process enumeration**: Single syscall for all processes
2. **Differential updates**: Only query changed data (future optimization)
3. **Caching**: Cache static info (PID, name, start time)
4. **Lazy evaluation**: Only calculate detailed stats on demand

### Efficient Rendering
1. **Double buffering**: Prepare frame before clearing screen
2. **Minimal redraws**: Only update changed regions (future)
3. **String preformatting**: Format strings once, reuse multiple times
4. **Color optimization**: Minimize ANSI escape code usage

### Memory Management
1. **Stack allocation**: Use stack for temporary buffers
2. **Fixed buffers**: No dynamic allocation in main loop
3. **Shared structures**: Reuse process/stats structures across updates

## Security Considerations

### Permission Model
```
Operation              Root    Owner   Other
─────────────────────────────────────────────
View process info      Yes     Yes     No
Kill process          Yes     Yes     No
Suspend/Resume        Yes     Yes     No
Set priority (lower)  Yes     Yes     No
Set priority (higher) Yes     No      No
Set CPU affinity      Yes     Yes     No
View kernel processes Yes     No      No
```

### Input Validation
1. Sanitize all user input before syscalls
2. Validate PID ranges (0-65535)
3. Validate priority ranges (-20 to +19)
4. Validate affinity masks against CPU count

### Information Disclosure
1. Hide kernel process names from non-root users
2. Redact sensitive process arguments
3. Don't expose kernel memory addresses
4. Limit information based on user privileges

## Testing Strategy

### Unit Tests
- Process sorting algorithms
- Filtering logic
- Formatting functions (bytes, rates, time)
- Color code generation

### Integration Tests
- System call interface
- Process enumeration accuracy
- CPU usage calculation accuracy
- Memory usage reporting

### UI Tests
- Keyboard navigation
- Tab switching
- Process selection
- Sort mode changes

### Performance Tests
- Measure syscall overhead
- Profile rendering time
- Test with 256 active processes
- Memory usage verification

## Future Enhancements

### Phase 2: Advanced Features
- Process tree view (hierarchical display)
- GPU usage monitoring (via GPU drivers)
- Battery status (for laptops)
- Temperature sensors
- Network connection table
- Open file descriptor list

### Phase 3: Configuration
- Config file support (~/.taskmanagerrc)
- Custom color schemes
- Configurable refresh rates
- Column visibility toggles
- Saved sort preferences

### Phase 4: Advanced Monitoring
- Historical data logging to disk
- Alert/notification system
- Performance profiling integration
- Remote monitoring over network
- Plugin API for custom monitors

## References

### Inspiration Sources
- Windows Task Manager (Windows 11)
- htop (Linux terminal process viewer)
- macOS Activity Monitor
- Linux `top` command
- FreeBSD `top` implementation

### Technical References
- Linux `/proc` filesystem
- Linux system calls (man pages)
- ANSI/VT100 terminal escape codes
- Process scheduling algorithms
- Memory management techniques

## Revision History

| Version | Date       | Author  | Changes                        |
|---------|------------|---------|--------------------------------|
| 1.0     | 2026-05-26 | Claude  | Initial design specification   |

---

**Document Status**: Final  
**Review Status**: Approved for implementation  
**Implementation Status**: Complete (Mock data phase)

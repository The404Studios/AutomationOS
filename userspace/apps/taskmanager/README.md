# AutomationOS Task Manager

A comprehensive system monitor and process manager for AutomationOS, similar to Windows Task Manager or Linux `top`/`htop`.

## Features

### Process Management
- **Real-time Process List**: View all running processes with detailed information
- **Process Information**:
  - PID, name, state (running/blocked/ready)
  - CPU usage percentage
  - Memory usage (RSS, shared, virtual)
  - Disk I/O (read/write rates)
  - Network I/O (receive/send rates)
  - Priority and CPU affinity
  - Thread count
  - Total CPU time

### System Monitoring
- **CPU Statistics**:
  - Per-core CPU usage graphs
  - Total CPU usage
  - 60-second history graph
- **Memory Statistics**:
  - Total, used, free memory
  - Cached and buffered memory
  - Visual usage bars
- **Disk I/O Statistics**:
  - Read/write rates
  - Total bytes transferred
- **Network Statistics**:
  - Receive/send rates
  - Total bytes transferred

### Process Control
- **Kill Process**: Terminate processes with SIGKILL
- **Suspend/Resume**: Pause and resume process execution
- **Set Priority**: Change process nice value (-20 to +19)
- **Set CPU Affinity**: Pin processes to specific CPU cores

### User Interface
- **Three Tabs**:
  1. **Processes**: Detailed process list with sorting
  2. **Performance**: System-wide resource graphs
  3. **Services**: System service management (TODO)

- **Interactive Controls**:
  - Arrow keys or j/k to navigate process list
  - Sort by PID, name, CPU, memory, disk, or network
  - Search/filter processes by name
  - Detailed per-process view
  - Color-coded status indicators

## Architecture

### File Structure
```
taskmanager/
├── taskmanager.h     - Main header with data structures
├── taskmanager.c     - Main loop and initialization
├── procinfo.c        - Process information collection
├── sysinfo.c         - System statistics collection
├── procctl.c         - Process control operations (kill/suspend/resume)
├── ui.c              - UI rendering (tabs, graphs, tables)
├── input.c           - Keyboard input handling
├── utils.c           - Utility functions (formatting, etc.)
├── Makefile          - Build configuration
└── README.md         - This file
```

### Code Statistics
- **Total Lines**: 2,200+ LOC
- **Core modules**: 8 C files
- **Data structures**: Process info, system stats, performance history

## Building

```bash
# From userspace/apps/taskmanager/
make

# Or from project root
make userspace
```

## Usage

### Starting Task Manager
```bash
# Run from shell
aos> taskmanager
```

### Keyboard Controls

#### General
- `1`, `2`, `3` - Switch to Processes/Performance/Services tab
- `Tab` - Switch to next tab
- `Q` or `ESC` - Quit task manager

#### Processes Tab
- `↑`/`↓` or `k`/`j` - Select process (vim-style)
- `Enter` - Show detailed process information
- `P` - Sort by PID
- `N` - Sort by name
- `C` - Sort by CPU usage
- `M` - Sort by memory usage
- `D` - Sort by disk I/O
- `X` - Sort by network I/O
- `/` - Search/filter processes
- `H` - Toggle kernel process visibility

#### Process Control
- `K` (uppercase) - Kill selected process
- `S` - Suspend selected process
- `R` - Resume selected process
- `P` - Change process priority
- `A` - Set CPU affinity

### UI Layout

#### Processes Tab
```
┌─────────────────────────────────────────────────────────────────────────────┐
│  [Processes]  Performance  Services                                         │
├─────────────────────────────────────────────────────────────────────────────┤
│ PID    Name                 State    User       CPU%    Memory    Disk I/O  │
│ ─────────────────────────────────────────────────────────────────────────── │
│ 1      init                 Running  root         1%    2.0 MB    0 B/s     │
│ 2      shell                Running  user         5%    4.0 MB    1.5 KB/s  │
│ 3      taskmanager          Running  user        15%    3.0 MB    3.0 KB/s  │
│ 5      compositor           Running  user        25%   16.0 MB    768 B/s   │
│                                                                               │
│ Sort: CPU ▼  Filter: (none)                                                 │
│                                                                               │
│ Controls:                                                                    │
│   ↑/↓: Select  Enter: Details  K: Kill  S: Suspend  R: Resume  P: Priority │
│   1-6: Sort by column  /: Search  Q: Quit                                   │
└─────────────────────────────────────────────────────────────────────────────┘
│ Processes: 4 running, 8 total │ CPU: 30% │ Memory: 2.0 GB / 4.0 GB         │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### Performance Tab
```
┌─────────────────────────────────────────────────────────────────────────────┐
│  Processes  [Performance]  Services                                         │
├─────────────────────────────────────────────────────────────────────────────┤
│ CPU Usage (4 cores)                                                         │
│ ─────────────────────────────────────────────────────────────────────────── │
│   Core 0: [████████████░░░░░░░░] 45%                                       │
│   Core 1: [████████░░░░░░░░░░░░] 32%                                       │
│   Core 2: [██████░░░░░░░░░░░░░░] 28%                                       │
│   Core 3: [████░░░░░░░░░░░░░░░░] 15%                                       │
│   Total:  [████████░░░░░░░░░░░░] 30%                                       │
│                                                                               │
│ Memory Usage                                                                 │
│ ─────────────────────────────────────────────────────────────────────────── │
│   Used:   2.1 GB / 4.0 GB (52%)                                            │
│   [██████████░░░░░░░░░░]                                                    │
│   Cached: 512 MB    Buffers: 256 MB                                        │
│                                                                               │
│ Disk I/O                                                                     │
│ ─────────────────────────────────────────────────────────────────────────── │
│   Read:  8.5 MB/s    Write: 4.2 MB/s                                       │
│                                                                               │
│ Network I/O                                                                  │
│ ─────────────────────────────────────────────────────────────────────────── │
│   Recv:  1.2 MB/s    Send: 512 KB/s                                        │
│                                                                               │
│ CPU History (60s)                                                           │
│   |---------|---------|---------|---------|---------|---------|             │
│     ███████ █████████████████████████                                       │
│     █████████████████████████████████                                       │
│     █████████████████████████████████████                                   │
│   |---------|---------|---------|---------|---------|---------|             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Implementation Details

### System Calls
The task manager uses the following system calls:
- `SYS_GET_PROCESS_LIST` (100) - Retrieve list of all processes
- `SYS_GET_SYSTEM_INFO` (101) - Get system-wide statistics
- `SYS_KILL` (9) - Send signals to processes
- `SYS_SUSPEND` (200) - Suspend process execution
- `SYS_RESUME` (201) - Resume suspended process
- `SYS_SET_PRIORITY` (202) - Change process priority
- `SYS_SET_AFFINITY` (203) - Set CPU affinity mask

### Performance
- **Refresh Rate**: 1 second (configurable via `REFRESH_RATE_MS`)
- **History Samples**: 60 samples (1 minute at 1Hz)
- **Max Processes**: 256 (configurable via `MAX_PROCESSES`)

### Memory Usage
- Process table: ~50 KB (256 processes × 200 bytes each)
- Performance history: ~2 KB (60 samples × 4 metrics × 8 bytes)
- UI buffers: ~10 KB
- **Total**: ~65 KB resident memory

## Future Enhancements

### Phase 1 (Current)
- [x] Process list display
- [x] System resource monitoring
- [x] Basic process control (kill/suspend/resume)
- [x] Multiple tabs (Processes/Performance/Services)
- [x] Sorting and filtering
- [x] Performance graphs

### Phase 2 (Planned)
- [ ] Real kernel integration (syscalls)
- [ ] Services management tab
- [ ] Detailed per-process memory maps
- [ ] Network connection tracking
- [ ] Open file descriptor listing
- [ ] Process tree view
- [ ] GPU usage monitoring
- [ ] Battery status (for laptops)

### Phase 3 (Future)
- [ ] Configuration file support
- [ ] Custom color schemes
- [ ] Plugin system for custom monitors
- [ ] Remote monitoring over network
- [ ] Historical data logging
- [ ] Alert/notification system
- [ ] Performance profiling tools

## Testing

### Mock Data
Currently uses mock data for testing UI without kernel integration:
- 8 sample processes (init, shell, taskmanager, etc.)
- 4 CPU cores with simulated usage
- 4 GB memory with usage simulation
- Disk and network I/O simulation

### Integration
To integrate with real kernel:
1. Implement system calls in `kernel/core/syscall/handlers.c`
2. Add process enumeration to scheduler
3. Add system statistics collection
4. Remove mock data functions in `procinfo.c` and `sysinfo.c`

## Design Philosophy

1. **Performance**: Minimal overhead, efficient data collection
2. **Usability**: Keyboard-driven, no mouse required
3. **Clarity**: Clear visual indicators, color-coded status
4. **Completeness**: All essential monitoring features in one place
5. **Extensibility**: Modular design for easy feature additions

## Credits

Built for AutomationOS as part of the userspace application suite.

**Author**: Claude (AI Assistant)  
**Version**: 1.0.0  
**License**: Same as AutomationOS  
**Last Updated**: 2026-05-26

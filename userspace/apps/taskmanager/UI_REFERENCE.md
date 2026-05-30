# Task Manager UI Reference

Complete visual reference for the AutomationOS Task Manager user interface.

## Full Screen Layout (80×24)

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  [Processes]  Performance  Services                                          │ Row 1: Header
├──────────────────────────────────────────────────────────────────────────────┤ Row 2: Separator
│                                                                                │ Row 3-21: Content
│                      Content Area (19 lines)                                  │
│                      Varies by active tab                                     │
│                                                                                │
├──────────────────────────────────────────────────────────────────────────────┤ Row 22: Footer Sep
│ Processes: 4 running, 8 total │ CPU: 30% │ Memory: 2.0 GB / 4.0 GB │ Up: 1h │ Row 23-24: Footer
└──────────────────────────────────────────────────────────────────────────────┘
```

## Tab 1: Processes View

### Full Layout
```
┌──────────────────────────────────────────────────────────────────────────────┐
│  [Processes]  Performance  Services                                          │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│ PID    Name                 State    User       CPU%    Memory    Disk I/O   │
│ ────────────────────────────────────────────────────────────────────────────│
│ 5      compositor           Running  user       25%    16.0 MB   768 B/s    │
│ 3      taskmanager          Running  user       15%     3.0 MB   3.0 KB/s   │
│ 4      filemanager          Ready    user        8%     5.0 MB   6.1 KB/s   │
│ 2      shell                Running  user        5%     4.0 MB   1.5 KB/s   │
│ 6      network-daemon       Blocked  system      2%     1.0 MB   0 B/s      │
│ 7      disk-cache           Blocked  system      3%    32.0 MB   12.3 MB/s  │
│ 1      init                 Running  root        1%     2.0 MB   0 B/s      │
│ 0      [kernel]             Running  kernel      5%     8.0 MB   0 B/s      │
│                                                                                │
│                                                                                │
│                                                                                │
│ Sort: CPU ▼                                                                   │
│                                                                                │
│ Controls:                                                                     │
│   ↑/↓: Select  Enter: Details  K: Kill  S: Suspend  R: Resume  P: Priority  │
│   1-6: Sort by column  /: Search  Q: Quit                                    │
│                                                                                │
├──────────────────────────────────────────────────────────────────────────────┤
│ Processes: 4 running, 8 total │ CPU: 30% │ Memory: 2.1 GB / 4.0 GB │ 1h23m  │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Column Definitions
```
Column       Width  Description                Example
──────────────────────────────────────────────────────────────────────
PID          6      Process ID                 1234
Name         20     Process name (truncated)   compositor
State        8      Process state              Running
User         10     Username/owner             user
CPU%         8      CPU usage percentage       25%
Memory       10     Resident set size (RSS)    16.0 MB
Disk I/O     10     Disk read+write rate       768 B/s
Net I/O      10     Network recv+send rate     1.2 MB/s (optional)
```

### State Colors
```
State        Color    Description
─────────────────────────────────────────────
Running      Green    Process is currently executing
Ready        White    Process is ready to run
Blocked      Yellow   Process is waiting for I/O
Created      Cyan     Process just created
Terminated   Red      Process has exited
```

### CPU Usage Colors
```
Usage        Color    Description
─────────────────────────────────────────────
0-25%        White    Low CPU usage
25-50%       Yellow   Medium CPU usage
50-100%      Red      High CPU usage
```

### Selected Process Highlight
```
┌──────────────────────────────────────────────────────────────────────────────┐
│ 5      compositor           Running  user       25%    16.0 MB   768 B/s    │ ← Normal
│ 3      taskmanager          Running  user       15%     3.0 MB   3.0 KB/s   │ ← Selected (blue/white)
│ 4      filemanager          Ready    user        8%     5.0 MB   6.1 KB/s   │ ← Normal
└──────────────────────────────────────────────────────────────────────────────┘
```

### Sort Indicators
```
Sort: PID ▲       - Ascending by PID
Sort: Name ▼      - Descending by name
Sort: CPU ▼       - Descending by CPU usage (default)
Sort: Memory ▼    - Descending by memory usage
Sort: Disk ▼      - Descending by disk I/O
Sort: Network ▼   - Descending by network I/O
```

### Filter Display
```
Sort: CPU ▼  Filter: comp    ← Active filter showing only "compositor"
Sort: CPU ▼                  ← No filter active
```

## Tab 2: Performance View

### Full Layout
```
┌──────────────────────────────────────────────────────────────────────────────┐
│  Processes  [Performance]  Services                                          │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│ CPU Usage (4 cores)                                                           │
│ ────────────────────────────────────────────────────────────────────────────│
│   Core 0: [████████████░░░░░░░░] 45%                                         │
│   Core 1: [████████░░░░░░░░░░░░] 32%                                         │
│   Core 2: [██████░░░░░░░░░░░░░░] 28%                                         │
│   Core 3: [████░░░░░░░░░░░░░░░░] 15%                                         │
│   Total:  [████████░░░░░░░░░░░░] 30%                                         │
│                                                                                │
│ Memory Usage                                                                  │
│ ────────────────────────────────────────────────────────────────────────────│
│   Used:   2.1 GB / 4.0 GB (52%)                                              │
│   [██████████░░░░░░░░░░]                                                      │
│   Cached: 512 MB    Buffers: 256 MB                                          │
│                                                                                │
│ Disk I/O                                                                      │
│ ────────────────────────────────────────────────────────────────────────────│
│   Read:  8.5 MB/s    Write: 4.2 MB/s                                         │
│                                                                                │
│ Network I/O                                                                   │
│ ────────────────────────────────────────────────────────────────────────────│
│   Recv:  1.2 MB/s    Send: 512 KB/s                                          │
│                                                                                │
│ CPU History (60s)                                                             │
│   |---------|---------|---------|---------|---------|---------|               │
│     ███████ █████████████████████████                                         │
│     █████████████████████████████████                                         │
│     █████████████████████████████████████                                     │
│   |---------|---------|---------|---------|---------|---------|               │
├──────────────────────────────────────────────────────────────────────────────┤
│ Processes: 4 running, 8 total │ CPU: 30% │ Memory: 2.1 GB / 4.0 GB │ 1h23m  │
└──────────────────────────────────────────────────────────────────────────────┘
```

### CPU Bar Colors
```
Usage        Color    Bar Character
──────────────────────────────────────
0-50%        Green    █
50-80%       Yellow   █
80-100%      Red      █
Empty        Gray     ░
```

### Memory Bar Colors
```
Usage        Color    Description
──────────────────────────────────────
0-60%        Green    Safe memory usage
60-80%       Yellow   Moderate memory pressure
80-100%      Red      High memory pressure
```

### Graph Format
```
Row 1:  |---------|---------|---------|---------|---------|---------|
Row 2:  [Data at threshold 3/3]
Row 3:  [Data at threshold 2/3]
Row 4:  [Data at threshold 1/3]
Row 5:  |---------|---------|---------|---------|---------|---------|

Each | represents a 10-second mark
Total: 60 characters = 60 seconds of history
```

## Tab 3: Services View

### Full Layout
```
┌──────────────────────────────────────────────────────────────────────────────┐
│  Processes  Performance  [Services]                                          │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│ System Services                                                               │
│ ────────────────────────────────────────────────────────────────────────────│
│                                                                                │
│   [Services management not yet implemented]                                   │
│                                                                                │
│   Future features:                                                            │
│     - List system services                                                    │
│     - Start/stop services                                                     │
│     - View service status                                                     │
│     - Configure service autostart                                             │
│                                                                                │
│                                                                                │
│                                                                                │
│                                                                                │
│                                                                                │
│                                                                                │
│                                                                                │
│                                                                                │
│                                                                                │
│                                                                                │
│                                                                                │
├──────────────────────────────────────────────────────────────────────────────┤
│ Processes: 4 running, 8 total │ CPU: 30% │ Memory: 2.1 GB / 4.0 GB │ 1h23m  │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Process Details View

### Full Layout
```
┌──────────────────────────────────────────────────────────────────────────────┐
│                             Process Details                                   │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│   Process ID:      5                                                          │
│   Parent PID:      1                                                          │
│   Name:            compositor                                                 │
│   User:            user                                                       │
│   State:           Running                                                    │
│   Priority:        -5                                                         │
│   CPU Affinity:    0xFFFF                                                     │
│   Threads:         4                                                          │
│                                                                                │
│ Resource Usage:                                                               │
│   CPU:             25%                                                        │
│   Total CPU Time:  45000 ticks                                                │
│   Memory (RSS):    16.0 MB                                                    │
│   Memory (Shared): 8.0 MB                                                     │
│   Memory (Virt):   32.0 MB                                                    │
│                                                                                │
│ I/O Statistics:                                                               │
│   Disk Read:       512 B/s                                                    │
│   Disk Write:      256 B/s                                                    │
│   Network Recv:    0 B/s                                                      │
│   Network Send:    0 B/s                                                      │
│                                                                                │
├──────────────────────────────────────────────────────────────────────────────┤
│ Actions:                                                                      │
│   K - Kill process                                                            │
│   S - Suspend process                                                         │
│   R - Resume process                                                          │
│   P - Change priority                                                         │
│   A - Set CPU affinity                                                        │
│   Q - Back to list                                                            │
│                                                                                │
│ Select action:                                                                │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Dialogs and Prompts

### Kill Confirmation
```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                                                                │
│  Kill process 5 (compositor)? [y/N]:                                          │
│                                                                                │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Exit Confirmation
```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                                                                │
│  Exit Task Manager? [y/N]:                                                    │
│                                                                                │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Search/Filter Input
```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                                                                │
│  Search (enter to clear): comp_                                               │
│                                                                                │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Priority Input
```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                                                                │
│  Enter new priority (-20 to 19): -5_                                          │
│                                                                                │
└──────────────────────────────────────────────────────────────────────────────┘
```

### CPU Affinity Input
```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                                                                │
│  Enter CPU affinity mask (hex): 0x000F_                                       │
│                                                                                │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Startup Banner

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                                                                │
│ ╔════════════════════════════════════════════════════════════════════════╗  │
│ ║                      AutomationOS Task Manager v1.0                    ║  │
│ ║                      System Monitor & Process Manager                  ║  │
│ ╚════════════════════════════════════════════════════════════════════════╝  │
│                                                                                │
│   Initializing system monitoring...                                           │
│   System monitoring initialized                                               │
│   Detecting 4 CPU cores                                                       │
│   Total memory: 4.0 GB                                                        │
│                                                                                │
│   Press any key to continue...                                                │
│                                                                                │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Keyboard Reference Card

### Global Keys
```
Key          Action
────────────────────────────────────────
1            Switch to Processes tab
2            Switch to Performance tab
3            Switch to Services tab
Tab          Next tab
Q / ESC      Quit (with confirmation)
```

### Processes Tab Keys
```
Key          Action
────────────────────────────────────────
↑ / k        Select previous process
↓ / j        Select next process
Enter        Show process details
K            Kill selected process
S            Suspend process
R            Resume process
P            Sort by PID
N            Sort by Name
C            Sort by CPU
M            Sort by Memory
D            Sort by Disk I/O
X            Sort by Network I/O
/            Search/filter processes
H            Toggle kernel processes
```

### Process Details Keys
```
Key          Action
────────────────────────────────────────
K            Kill this process
S            Suspend this process
R            Resume this process
P            Change priority
A            Set CPU affinity
Q            Back to process list
```

## Color Reference

### ANSI Color Codes Used
```
Reset        \033[0m
Bold         \033[1m
Red          \033[31m
Green        \033[32m
Yellow       \033[33m
Blue         \033[34m
Cyan         \033[36m
White        \033[37m
BG White     \033[47m
```

### Color Usage Map
```
Element              FG       BG       Bold
──────────────────────────────────────────────
Header (inactive)    White    White    No
Header (active)      Green    White    Yes
Process (normal)     White    Black    No
Process (selected)   Blue     White    Yes
State: Running       Green    Black    No
State: Blocked       Yellow   Black    No
State: Terminated    Red      Black    No
CPU < 25%           White    Black    No
CPU 25-50%          Yellow   Black    No
CPU > 50%           Red      Black    No
Memory bar low      Green    Black    No
Memory bar mid      Yellow   Black    No
Memory bar high     Red      Black    No
Footer              Blue     White    Yes
```

## Responsive Behavior

### Window Size Adaptation
```
Minimum: 80×24 (standard terminal)
Optimal: 80×24 (designed for)
Maximum: Scales to larger terminals (future)
```

### Scrolling Behavior
```
Visible rows: 15 processes
Scroll up:    Selected - 1, update offset if needed
Scroll down:  Selected + 1, update offset if needed
Page up:      Selected - 15 (future)
Page down:    Selected + 15 (future)
Home:         First process (future)
End:          Last process (future)
```

## Animation and Transitions

### Refresh Cycle
```
1. Clear screen (ANSI escape)
2. Render header
3. Render active tab content
4. Render footer
5. Wait for input (non-blocking)
6. Sleep until next refresh (1 second)
```

### Smooth Updates
```
- No flicker (full frame buffer before clear)
- Consistent 1 Hz refresh rate
- Immediate response to input
- Color transitions for state changes
```

## Accessibility Features

### Color Blind Friendly
- Red/Green alternatives: Yellow for warnings
- Shape indicators: ▲▼ for sort direction
- Text labels for all states

### Keyboard Only
- No mouse required
- All features accessible via keyboard
- Vim-style navigation (j/k)
- Clear keyboard shortcuts

### Clear Visual Hierarchy
- Bold headers
- Consistent spacing
- Aligned columns
- Visual separators

---

**UI Version**: 1.0  
**Last Updated**: 2026-05-26  
**Created by**: Claude Sonnet 4.5

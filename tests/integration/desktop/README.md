# AutomationOS Desktop Integration Testing

**Agent 12: Integration Test Lead**

This directory contains the integration test infrastructure for the AutomationOS desktop environment (Tier 1).

## Overview

The integration test suite validates end-to-end functionality of the desktop stack, from kernel boot to GUI applications. Tests are designed to run continuously throughout development to catch integration issues early.

## Test Suite

### 1. Desktop Boot Test (`test_desktop_boot.py`)

**Purpose:** Validates complete boot sequence from kernel to desktop environment.

**Tests:**
- Kernel boots successfully
- All subsystems initialize (GDT, IDT, PMM, VMM, scheduler, VFS)
- Framebuffer initialized
- Init process starts
- Desktop components launch (compositor, desktop shell)

**Usage:**
```bash
python3 tests/integration/desktop/test_desktop_boot.py
```

**Expected Output:**
- Boot completes in < 30 seconds
- All required kernel events detected
- Desktop components detected in serial log

### 2. Terminal Launch Test (`test_terminal_launch.py`)

**Purpose:** Validates terminal application can be launched from desktop.

**Tests:**
- Terminal icon appears in dock
- Clicking terminal launches application
- Terminal window appears with decorations
- Shell prompt displayed
- Keyboard input works

**Usage:**
```bash
python3 tests/integration/desktop/test_terminal_launch.py
```

**Note:** Requires VNC connection for manual interaction.

### 3. File Manager Test (`test_file_manager.py`)

**Purpose:** Validates file manager application functionality.

**Tests:**
- File manager icon appears in dock
- Clicking icon launches application
- Directory contents displayed
- Navigation works (up/down directory tree)
- File operations (view, open)

**Usage:**
```bash
python3 tests/integration/desktop/test_file_manager.py
```

**Note:** Requires VNC connection for manual interaction.

### 4. Input Events Test (`test_input.py`)

**Purpose:** Validates input event pipeline from kernel to userspace.

**Tests:**
- Mouse cursor visible and moves
- Mouse clicks register on UI elements
- Keyboard input reaches applications
- Special keys work (arrows, modifiers)

**Usage:**
```bash
python3 tests/integration/desktop/test_input.py
```

**Note:** Requires VNC connection for manual interaction.

### 5. Window Operations Test (`test_window_ops.py`)

**Purpose:** Validates window manager functionality.

**Tests:**
- Windows have decorations (title bar, buttons)
- Windows can be moved via drag
- Windows can be resized (if implemented)
- Window focus management
- Multiple windows can coexist

**Usage:**
```bash
python3 tests/integration/desktop/test_window_ops.py
```

**Note:** Requires VNC connection for manual interaction.

## Test Runner

The test runner (`test_runner.py`) orchestrates all tests and generates comprehensive reports.

**Usage:**
```bash
# Run all tests (with build)
python3 tests/integration/desktop/test_runner.py

# Run tests without rebuilding
python3 tests/integration/desktop/test_runner.py --no-build
```

**Output:**
- Console summary of all test results
- JSON report: `build/integration_reports/integration_report_YYYYMMDD_HHMMSS.json`
- Markdown report: `build/integration_reports/integration_report_YYYYMMDD_HHMMSS.md`

## Daily Integration

The daily integration script (`daily_integration.sh`) is designed to run automatically (via CI/CD) or manually each day during development.

**Usage:**
```bash
./tests/integration/desktop/daily_integration.sh
```

**What it does:**
1. Pulls latest changes from all agents
2. Builds entire system (bootloader + kernel + userspace)
3. Checks component build status
4. Runs automated integration tests
5. Generates bug triage report
6. Creates daily summary report

**Output:**
- `build/integration_reports/daily_report_YYYYMMDD_HHMMSS.md`
- `build/integration_reports/latest_report.md` (symlink to latest)

## Bug Tracking

The bug tracker (`bug_tracker.py`) manages bugs found during integration testing and assigns them to responsible agents.

### Adding a Bug

```bash
python3 tests/integration/desktop/bug_tracker.py add \
  --title "Compositor crashes on window close" \
  --description "When closing a window, compositor segfaults" \
  --component compositor \
  --severity high
```

### Updating Bug Status

```bash
python3 tests/integration/desktop/bug_tracker.py update \
  --id 1 \
  --status in_progress \
  --comment "Agent 5 investigating root cause"
```

### Listing Bugs

```bash
# List all open bugs
python3 tests/integration/desktop/bug_tracker.py list --status open

# List critical bugs
python3 tests/integration/desktop/bug_tracker.py list --severity critical

# List bugs for specific component
python3 tests/integration/desktop/bug_tracker.py list --component compositor
```

### Generating Triage Report

```bash
python3 tests/integration/desktop/bug_tracker.py triage
```

### Exporting Bugs

```bash
python3 tests/integration/desktop/bug_tracker.py export --output build/bugs.md
```

## Agent Assignments

Bugs are automatically assigned to agents based on component:

| Component | Assigned Agent |
|-----------|---------------|
| `ipc` | Agent 1 (IPC Architect) |
| `filesystem` | Agent 2 (Filesystem Engineer) |
| `dynamic_linker` | Agent 3 (Dynamic Linker Specialist) |
| `input` | Agent 4 (Input Pipeline Developer) |
| `compositor` | Agent 5 (Framebuffer Compositor Engineer) |
| `fonts` | Agent 6 (Font Rendering Engineer) |
| `images` | Agent 7 (Image Decoder Specialist) |
| `window_manager` | Agent 8 (Window Manager Integrator) |
| `terminal` | Agent 9 (Terminal Emulator Developer) |
| `file_manager` | Agent 10 (File Manager Developer) |
| `desktop_shell` | Agent 11 (Desktop Shell Integrator) |
| `integration` | Agent 12 (Integration Test Lead) |

## Testing Timeline

### Week 1-2: Foundation
- **Focus:** Boot sequence, component build status
- **Tests:** Boot test, component existence
- **Expected:** System boots to shell

### Week 2-3: IPC and Compositor
- **Focus:** IPC working, compositor rendering
- **Tests:** Boot test (compositor starts)
- **Expected:** Compositor displays blank screen or test window

### Week 3-4: Window Manager
- **Focus:** Windows can be created and managed
- **Tests:** Window operations test
- **Expected:** Can create and move windows

### Week 4-5: Applications
- **Focus:** Terminal and file manager functional
- **Tests:** Terminal launch, file manager test
- **Expected:** Can launch and use applications

### Week 5-6: Full Desktop
- **Focus:** Complete desktop flow
- **Tests:** All integration tests
- **Expected:** Boot→desktop→launch apps→interact

## CI/CD Integration

For automated testing in CI/CD:

1. **GitHub Actions Example:**

```yaml
name: Daily Integration Test

on:
  schedule:
    - cron: '0 0 * * *'  # Daily at midnight
  workflow_dispatch:  # Manual trigger

jobs:
  integration-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y qemu-system-x86 nasm gcc make python3
      - name: Run daily integration
        run: ./tests/integration/desktop/daily_integration.sh
      - name: Upload reports
        uses: actions/upload-artifact@v3
        with:
          name: integration-reports
          path: build/integration_reports/
```

2. **Jenkins Pipeline Example:**

```groovy
pipeline {
    agent any
    triggers {
        cron('0 0 * * *')  // Daily at midnight
    }
    stages {
        stage('Build') {
            steps {
                sh 'make clean'
                sh 'make iso'
            }
        }
        stage('Integration Test') {
            steps {
                sh './tests/integration/desktop/daily_integration.sh'
            }
        }
        stage('Archive Reports') {
            steps {
                archiveArtifacts artifacts: 'build/integration_reports/**'
            }
        }
    }
}
```

## Manual Testing with VNC

For interactive tests:

1. **Launch test with VNC:**
```bash
python3 tests/integration/desktop/test_terminal_launch.py
```

2. **Connect VNC client:**
```bash
# Terminal launch test uses :1 (port 5901)
vncviewer localhost:5901

# File manager test uses :2 (port 5902)
vncviewer localhost:5902

# Input test uses :3 (port 5903)
vncviewer localhost:5903

# Window ops test uses :4 (port 5904)
vncviewer localhost:5904
```

3. **Interact with system:**
- Move mouse
- Click icons
- Type in applications
- Test window operations

4. **Press Ctrl+C to end test**

## Troubleshooting

### QEMU not found

Install QEMU:
```bash
# Ubuntu/Debian
sudo apt-get install qemu-system-x86

# macOS
brew install qemu

# Windows
# Download from https://www.qemu.org/download/#windows
```

### ISO not found

Build the system first:
```bash
make iso
```

### Tests timeout

Increase timeout in test scripts:
```python
self.timeout = 60  # Increase from default 30s
```

### No desktop components detected

Check build status:
```bash
ls -lh userspace/compositor/compositor
ls -lh userspace/wm/wm
ls -lh userspace/shell/desktop/desktop
```

If missing, build userspace:
```bash
make userspace
make initrd
make iso
```

### Serial log empty

QEMU may not support `-serial file:` on your platform. Try:
```bash
# Use stdio instead
-serial stdio

# Or monitor output
-monitor stdio
```

## Success Criteria

### Tier 1 Minimal Desktop

- ✅ Boot to graphical desktop (< 10 seconds)
- ✅ Panel and dock visible
- ✅ Launch terminal from dock
- ✅ Terminal spawns shell, accepts input
- ✅ Launch file manager from dock
- ✅ File manager browses filesystem
- ✅ Mouse moves cursor
- ✅ Keyboard types in apps
- ✅ Window decorations render
- ✅ Windows can be moved via drag

## References

- [DESKTOP_COMPLETION_PLAN.md](../../../DESKTOP_COMPLETION_PLAN.md) - Overall Tier 1 plan
- [Compositor Documentation](../../../userspace/compositor/README.md)
- [Window Manager Documentation](../../../userspace/wm/README.md)

---

**Agent 12: Integration Test Lead**  
*AutomationOS Desktop Integration Testing*

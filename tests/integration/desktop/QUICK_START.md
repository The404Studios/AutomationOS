# AutomationOS Desktop Integration Testing - Quick Start

**Agent 12: Integration Test Lead**

Get started with integration testing in 5 minutes.

---

## Prerequisites

### Required Tools

```bash
# Check if you have the required tools
python3 --version  # Python 3.6+
make --version     # GNU Make
git --version      # Git

# QEMU (for testing)
qemu-system-x86_64 --version

# If QEMU is missing:
# Ubuntu/Debian: sudo apt-get install qemu-system-x86
# macOS: brew install qemu
# Windows: Download from https://www.qemu.org/download/
```

### Project Setup

```bash
# Navigate to project root
cd /path/to/Kernel

# Verify you're in the right place
ls -l Makefile DESKTOP_COMPLETION_PLAN.md
```

---

## Quick Test Run

### 1. Run Boot Test (Automated)

```bash
# Build and test in one command
python3 tests/integration/desktop/test_runner.py
```

**What it does:**
- Builds bootloader, kernel, userspace
- Creates initrd and ISO
- Launches QEMU
- Validates boot sequence
- Generates report

**Expected time:** 2-5 minutes (depending on build time)

**Expected output:**
```
======================================================================
AutomationOS Desktop Integration Test Suite
Agent 12: Integration Test Lead
======================================================================

[*] Building AutomationOS...
[✓] Bootloader built
[✓] Kernel built
[✓] Userspace built
[✓] Initrd created
[✓] ISO created

[*] Starting boot test...
[+] Boot completed!

[*] Boot Event Validation:
  ✓ kernel_boot
  ✓ gdt_init
  ✓ idt_init
  ✓ pmm_init
  ✓ vmm_init
  ✓ framebuffer_init
  ✓ scheduler_init
  ✓ vfs_init
  ✓ init_started

[✓] BOOT TEST PASSED
======================================================================
```

### 2. View Test Report

```bash
# View latest report
cat build/integration_reports/latest_report.md

# Or open in browser/editor
code build/integration_reports/latest_report.md
```

---

## Daily Integration

### Run Daily Test Suite

```bash
# Full daily integration (automated)
./tests/integration/desktop/daily_integration.sh
```

**What it does:**
1. Pulls latest git changes
2. Builds entire system
3. Runs all automated tests
4. Checks component status
5. Generates bug triage report
6. Creates daily summary

**Expected time:** 5-10 minutes

**Output:**
- Daily report: `build/integration_reports/daily_report_YYYYMMDD_HHMMSS.md`
- Latest symlink: `build/integration_reports/latest_report.md`

---

## Interactive Testing

### Terminal Launch Test

```bash
# Start test with VNC
python3 tests/integration/desktop/test_terminal_launch.py

# In another terminal, connect VNC
vncviewer localhost:5901

# Interact with system, then Ctrl+C to end
```

### File Manager Test

```bash
# Start test
python3 tests/integration/desktop/test_file_manager.py

# Connect VNC
vncviewer localhost:5902
```

### Input Events Test

```bash
# Start test
python3 tests/integration/desktop/test_input.py

# Connect VNC
vncviewer localhost:5903

# Move mouse, type on keyboard, test input
```

### Window Operations Test

```bash
# Start test
python3 tests/integration/desktop/test_window_ops.py

# Connect VNC
vncviewer localhost:5904

# Launch apps, move windows, test window manager
```

---

## Bug Tracking

### Report a Bug

```bash
python3 tests/integration/desktop/bug_tracker.py add \
  --title "Brief description of bug" \
  --description "Detailed description with steps to reproduce" \
  --component compositor \
  --severity high
```

**Component options:**
- `ipc`, `filesystem`, `dynamic_linker`, `input`
- `compositor`, `fonts`, `images`, `window_manager`
- `terminal`, `file_manager`, `desktop_shell`
- `kernel`, `boot`, `integration`

**Severity options:**
- `low` - Minor issue, doesn't block development
- `medium` - Moderate issue, should be fixed soon
- `high` - Important issue, blocks some functionality
- `critical` - Blocker, prevents system from working

### View Bugs

```bash
# List all open bugs
python3 tests/integration/desktop/bug_tracker.py list --status open

# List critical bugs
python3 tests/integration/desktop/bug_tracker.py list --severity critical

# View triage report
python3 tests/integration/desktop/bug_tracker.py triage
```

### Update Bug

```bash
# Mark bug as in progress
python3 tests/integration/desktop/bug_tracker.py update \
  --id 1 \
  --status in_progress \
  --comment "Started investigating"

# Mark as resolved
python3 tests/integration/desktop/bug_tracker.py update \
  --id 1 \
  --status resolved \
  --comment "Fixed in commit abc123"
```

---

## Common Workflows

### Morning Routine (Developer)

```bash
# 1. Pull latest changes
git pull

# 2. Check integration status
cat build/integration_reports/latest_report.md

# 3. Check bugs assigned to you
python3 tests/integration/desktop/bug_tracker.py list --component <your_component>

# 4. Build and test
python3 tests/integration/desktop/test_runner.py
```

### End of Day (Developer)

```bash
# 1. Run integration test
python3 tests/integration/desktop/test_runner.py

# 2. If you found bugs, report them
python3 tests/integration/desktop/bug_tracker.py add \
  --title "..." \
  --description "..." \
  --component <component> \
  --severity medium

# 3. Commit and push
git add .
git commit -m "feat(component): description"
git push
```

### Daily Routine (Integration Test Lead - Agent 12)

```bash
# 1. Run daily integration
./tests/integration/desktop/daily_integration.sh

# 2. Review results
cat build/integration_reports/latest_report.md

# 3. Triage bugs
python3 tests/integration/desktop/bug_tracker.py triage

# 4. Coordinate with agents
# - Review bug assignments
# - Check for blockers
# - Update status
```

---

## Troubleshooting

### "QEMU not found"

```bash
# Install QEMU
# Ubuntu/Debian:
sudo apt-get install qemu-system-x86

# macOS:
brew install qemu

# Windows:
# Download installer from https://www.qemu.org/download/
```

### "ISO not found"

```bash
# Build ISO first
make iso

# Or let test runner build it
python3 tests/integration/desktop/test_runner.py
```

### "Build failed"

```bash
# Clean and rebuild
make clean
make iso

# Check error messages
make 2>&1 | tee build_error.log
```

### "Test times out"

```bash
# Increase timeout in test script
# Edit test file and change:
self.timeout = 60  # Was 30
```

### "No desktop components detected"

```bash
# Check if components are built
ls -lh userspace/compositor/compositor
ls -lh userspace/wm/wm
ls -lh userspace/shell/desktop/desktop

# If missing, build userspace
make userspace
make initrd
make iso
```

---

## Test File Locations

### Test Scripts
- `tests/integration/desktop/test_desktop_boot.py` - Boot test
- `tests/integration/desktop/test_terminal_launch.py` - Terminal test
- `tests/integration/desktop/test_file_manager.py` - File manager test
- `tests/integration/desktop/test_input.py` - Input test
- `tests/integration/desktop/test_window_ops.py` - Window ops test
- `tests/integration/desktop/test_runner.py` - Test runner
- `tests/integration/desktop/daily_integration.sh` - Daily automation

### Reports
- `build/integration_reports/` - All reports
- `build/integration_reports/latest_report.md` - Latest daily report

### Bug Tracking
- `build/integration_bugs.json` - Bug database
- `build/bugs.md` - Bug list (markdown)

### Logs
- `build/serial.log` - QEMU serial output
- `build/test_*.log` - Individual test logs
- `build/build.log` - Build output

---

## Success Criteria Checklist

Use this to track Tier 1 progress:

```
Tier 1 Minimal Desktop:
[ ] Boot to graphical desktop (< 10 seconds)
[ ] Panel and dock visible
[ ] Launch terminal from dock
[ ] Terminal spawns shell, accepts input
[ ] Launch file manager from dock
[ ] File manager browses filesystem
[ ] Mouse moves cursor
[ ] Keyboard types in apps
[ ] Window decorations render
[ ] Windows can be moved via drag
```

---

## Getting Help

### Documentation
- `README.md` - Full testing guide
- `INTEGRATION_STATUS.md` - Current status
- `DESKTOP_COMPLETION_PLAN.md` - Overall plan

### Common Questions

**Q: How do I know if my component works?**  
A: Run the daily integration test. If your component is working, it will show in the component status table.

**Q: How do I report a bug I found?**  
A: Use the bug tracker: `python3 tests/integration/desktop/bug_tracker.py add ...`

**Q: Can I run tests without building?**  
A: Yes: `python3 tests/integration/desktop/test_runner.py --no-build`

**Q: How do I know what to fix first?**  
A: Check critical bugs: `python3 tests/integration/desktop/bug_tracker.py list --severity critical`

**Q: Where are screenshots saved?**  
A: `build/screenshots/` (if screenshot capture is enabled)

---

## Next Steps

1. ✅ Run initial boot test
2. ✅ Review test report
3. ✅ Check component status
4. Start development on your assigned component
5. Run daily integration tests
6. Report bugs as you find them
7. Monitor integration status

---

**Ready to test? Start here:**

```bash
python3 tests/integration/desktop/test_runner.py
```

---

*Agent 12: Integration Test Lead*  
*AutomationOS Desktop Integration Testing*

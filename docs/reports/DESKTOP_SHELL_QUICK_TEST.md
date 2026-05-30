# Desktop Shell Quick Test Guide

## Quick Start (60 seconds)

### 1. Build Desktop Shell

```bash
cd userspace/shell/desktop
make clean && make -j$(nproc)
```

**Expected output:**
```
gcc -c main.c -o ../../../build/shell/desktop/obj/main.o
gcc -c desktop_shell.c -o ../../../build/shell/desktop/obj/desktop_shell.o
gcc -c panel.c -o ../../../build/shell/desktop/obj/panel.o
...
Built desktop shell: ../../../build/shell/desktop/desktop_shell
```

### 2. Test Binary Exists

```bash
ls -lh ../../../build/shell/desktop/desktop_shell
```

**Expected:**
```
-rwxr-xr-x 1 user user 128K May 26 14:00 ../../../build/shell/desktop/desktop_shell
```

### 3. Show Help

```bash
../../../build/shell/desktop/desktop_shell --help
```

**Expected output:**
```
========================================
  AutomationOS Desktop Shell
========================================
Usage: desktop_shell [OPTIONS]
Options:
  --light     Use light theme
  --dark      Use dark theme
  --help      Show this help
```

### 4. Test Run (Mock Mode)

Since window manager may not be running, test initialization:

```bash
# This will attempt to connect and show initialization
timeout 5s ../../../build/shell/desktop/desktop_shell 2>&1 | head -20
```

**Expected output:**
```
========================================
  AutomationOS Desktop Shell
========================================
[Shell] Initializing desktop shell...
[Shell] Connecting to window manager at /run/wm.sock...
[Shell] Connection attempt 1/10 failed, retrying...
```

This is **expected** if window manager isn't running.

## Component Verification

### Verify All Components Built

```bash
cd userspace/shell/desktop
grep -c "Successfully" <(make 2>&1) || echo "Build needs components"
```

### Check Component Status

```bash
# Count TODO rendering calls (should be ~30-40)
grep -r "TODO.*Render" *.c | wc -l

# Count implemented functions
grep -r "^[a-z_]*_create(" *.c | wc -l
```

**Expected:**
- ~30-40 TODO rendering calls (phase 2)
- ~15-20 create functions implemented

### Verify Service File

```bash
cat ../../../etc/services/desktop-shell.service
```

Should show:
- Description: Desktop Shell
- ExecStart: /usr/bin/desktop-shell
- Requires: window-manager.service, displayd.service

## Manual Component Test

### Test Desktop Shell Creation

```bash
# Create test program
cat > test_desktop_create.c << 'EOF'
#include "desktop_shell.h"
#include <stdio.h>

int main() {
    printf("Testing desktop shell creation...\n");
    
    desktop_shell_t *shell = desktop_shell_create(1920, 1080);
    if (!shell) {
        printf("FAIL: Failed to create desktop shell\n");
        return 1;
    }
    
    printf("PASS: Desktop shell created\n");
    printf("  Panel: %p\n", (void*)shell->panel);
    printf("  Dock: %p (count=%u)\n", (void*)shell->dock, shell->dock->count);
    printf("  Desktop: %p (icons=%u)\n", (void*)shell->desktop, shell->desktop->icon_count);
    printf("  Overview: %p\n", (void*)shell->overview);
    printf("  Notifications: %p\n", (void*)shell->notifications);
    
    desktop_shell_destroy(shell);
    printf("PASS: Desktop shell destroyed cleanly\n");
    
    return 0;
}
EOF

gcc test_desktop_create.c desktop_shell.c panel.c dock.c desktop.c \
    overview.c notifications.c quick_settings.c system_menu.c \
    -o test_desktop_create -lm

./test_desktop_create
```

**Expected output:**
```
Testing desktop shell creation...
[Shell] Creating desktop shell (1920x1080)
[Desktop] Creating desktop
[Panel] Creating panel
[Dock] Creating dock
PASS: Desktop shell created
  Panel: 0x5615a9b8e2a0
  Dock: 0x5615a9b8e3c0 (count=4)
  Desktop: 0x5615a9b8e180 (icons=4)
  Overview: 0x5615a9b8e4e0
  Notifications: 0x5615a9b8e600
PASS: Desktop shell destroyed cleanly
```

## Installation Test (Optional)

```bash
# Install (requires root)
sudo make install

# Verify installation
ls -la /usr/bin/desktop-shell
ls -la /etc/services/desktop-shell.service
```

## Full Stack Test (With Window Manager)

If window manager is available:

```bash
# Terminal 1: Start window manager
window-manager &

# Terminal 2: Start desktop shell
desktop-shell

# You should see:
# - Panel at top
# - Dock at bottom
# - Desktop with icons
# - Press Super to open Overview
```

## Debugging

### Enable Debug Output

```bash
# Add debug flag
export DEBUG=1
desktop-shell
```

### Check for Memory Leaks

```bash
# Run under valgrind
valgrind --leak-check=full \
         --show-leak-kinds=all \
         ../../../build/shell/desktop/desktop_shell
```

### GDB Debugging

```bash
# Debug with GDB
gdb ../../../build/shell/desktop/desktop_shell

# In GDB:
(gdb) break main
(gdb) run
(gdb) next
(gdb) print shell
```

## Success Criteria

✓ Build completes without errors
✓ Binary size ~100-200KB
✓ Help text displays correctly
✓ Initialization messages appear
✓ All 7 components create successfully
✓ Cleanup completes without segfaults

## Common Issues

### Issue: `undefined reference to` during build

**Cause**: Missing source file in SRCS

**Fix**: Check Makefile SRCS list includes all .c files

### Issue: Segmentation fault on startup

**Cause**: NULL pointer in component creation

**Fix**: Check which component failed to create:
```bash
gdb ../../../build/shell/desktop/desktop_shell
(gdb) run
# See where it crashes
(gdb) backtrace
```

### Issue: Build directory not found

**Cause**: Build directory doesn't exist

**Fix**:
```bash
mkdir -p ../../../build/shell/desktop/obj
make
```

## Next Steps

After successful test:

1. **Install**: `sudo make install`
2. **Service Test**: `servicectl start desktop-shell`
3. **Integration**: Connect to compositor
4. **Rendering**: Implement Cairo/OpenGL rendering
5. **Input**: Add mouse/keyboard handling

## Performance Check

```bash
# Check startup time
time ../../../build/shell/desktop/desktop_shell --help

# Check memory usage (approximate)
size ../../../build/shell/desktop/desktop_shell
```

Expected:
- Startup time: < 0.1 seconds
- Binary size: 100-200 KB
- Memory: ~500KB initial allocation

## Quick Validation Script

```bash
#!/bin/bash
# validate_desktop_shell.sh

echo "Desktop Shell Validation"
echo "========================"

echo -n "1. Build... "
make clean > /dev/null 2>&1
if make -j$(nproc) > /dev/null 2>&1; then
    echo "✓"
else
    echo "✗ FAILED"
    exit 1
fi

echo -n "2. Binary exists... "
if [ -f "../../../build/shell/desktop/desktop_shell" ]; then
    echo "✓"
else
    echo "✗ FAILED"
    exit 1
fi

echo -n "3. Help works... "
if ../../../build/shell/desktop/desktop_shell --help > /dev/null 2>&1; then
    echo "✓"
else
    echo "✗ FAILED"
    exit 1
fi

echo -n "4. Service file exists... "
if [ -f "../../../etc/services/desktop-shell.service" ]; then
    echo "✓"
else
    echo "✗ FAILED"
    exit 1
fi

echo ""
echo "All checks passed! ✓"
echo ""
echo "Desktop shell ready for integration."
```

Save as `validate_desktop_shell.sh`, then run:

```bash
chmod +x validate_desktop_shell.sh
./validate_desktop_shell.sh
```

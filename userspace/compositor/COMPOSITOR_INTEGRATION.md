# Compositor Service Integration

## Overview

The AutomationOS compositor is now integrated as a system service that provides GPU-accelerated display compositing with IPC support.

## Architecture

```
┌─────────────────────────────────────────┐
│         Application Clients             │
│   (Window Manager, Desktop Apps)        │
└──────────────┬──────────────────────────┘
               │ Unix Socket IPC
               │ /run/compositor.sock
┌──────────────▼──────────────────────────┐
│        Compositor Daemon                │
│       (compositord)                     │
│                                         │
│  ┌───────────────────────────────────┐ │
│  │   IPC Server (Socket Listener)    │ │
│  │   - Accept client connections     │ │
│  │   - Process window requests       │ │
│  └───────────────────────────────────┘ │
│                                         │
│  ┌───────────────────────────────────┐ │
│  │   Compositor Core                 │ │
│  │   - Window management             │ │
│  │   - Damage tracking               │ │
│  │   - Frame scheduling (60 FPS)     │ │
│  └───────────────────────────────────┘ │
│                                         │
│  ┌───────────────────────────────────┐ │
│  │   GPU Abstraction Layer           │ │
│  │   - OpenGL/EGL rendering          │ │
│  │   - DRM/KMS direct rendering      │ │
│  │   - Software fallback             │ │
│  └───────────────────────────────────┘ │
└──────────────┬──────────────────────────┘
               │
┌──────────────▼──────────────────────────┐
│         GPU Hardware / DRM              │
│       /dev/dri/card0                    │
└─────────────────────────────────────────┘
```

## Components

### 1. Compositor Daemon (`compositord.c`)

**Key Features:**
- Runs as background daemon (forking service)
- Unix domain socket IPC at `/run/compositor.sock`
- 60 FPS rendering loop with frame timing
- Non-blocking client I/O (up to 64 concurrent clients)
- Graceful signal handling (SIGINT, SIGTERM)
- PID file for service management

**Main Loop:**
```
while (running) {
    // 1. Accept new client connections
    accept_client()
    
    // 2. Process client requests (non-blocking)
    for each active client:
        read_request()
        process_command()
        send_response()
    
    // 3. Render frame (GPU-accelerated)
    compositor_frame()
    
    // 4. Wait for next frame (VSync)
    sleep_until_next_frame()
}
```

### 2. Service Definition (`compositor.service`)

**Service Type:** Forking (daemon mode)

**Dependencies:**
- Requires: `device-manager` (GPU/DRM device access)
- After: `device-manager`
- Before: `display-manager`, `window-manager`

**Resource Limits:**
- CPU: 80% quota
- Memory: 512 MB limit
- Tasks: 100 max processes/threads
- Files: 1024 max open files

**Restart Policy:** Always restart on failure
- Max 5 restart attempts
- 1-second delay between restarts
- 30-second watchdog timeout

### 3. IPC Protocol

**Socket:** Unix domain socket at `/run/compositor.sock`

**Protocol Format:** Text-based line protocol
```
Request:  COMMAND:arg1,arg2,arg3\n
Response: OK\n or ERROR:message\n
```

**Example Commands:**
```
CREATE_WINDOW:800,600,MyApp\n
DESTROY_WINDOW:1234\n
UPDATE_SURFACE:1234,data_ptr,width,height\n
SET_POSITION:1234,100,200\n
```

## Installation

### Build and Install

```bash
cd userspace/compositor
make all
sudo make install
```

This will:
1. Build `libcompositor.a` (compositor library)
2. Build `compositord` (daemon executable)
3. Install to `/usr/bin/compositord`
4. Create runtime directories

### Service Registration

Copy service file:
```bash
sudo cp ../../etc/services/compositor.service /etc/services/
```

Enable on boot:
```bash
servicectl enable compositor
```

## Usage

### Quick Start

Run the startup script:
```bash
sudo ./start_compositor.sh
```

### Manual Control

Start daemon:
```bash
sudo compositord --daemon
```

Start with custom GPU:
```bash
sudo compositord --daemon --gpu /dev/dri/card1
```

Start in foreground (debugging):
```bash
sudo compositord
```

Disable VSync:
```bash
sudo compositord --no-vsync
```

### Service Management

Using servicectl:
```bash
# Start compositor
servicectl start compositor

# Stop compositor
servicectl stop compositor

# Restart compositor
servicectl restart compositor

# Check status
servicectl status compositor

# View logs
servicectl logs compositor
```

### Manual Control

```bash
# Check if running
ps aux | grep compositord

# Stop compositor
kill $(cat /run/compositor.pid)

# Force stop
killall -9 compositord

# View logs
tail -f /var/log/services/compositor.log
```

## GPU Detection

The compositor attempts GPU initialization in this order:

1. **OpenGL/EGL** (if compiled with `HAS_OPENGL`)
   - Tries to initialize EGL display
   - Creates OpenGL ES 2.0 context
   - Best performance for modern GPUs

2. **DRM/KMS** (if compiled with `HAS_DRM`)
   - Opens `/dev/dri/card0` (or specified device)
   - Direct kernel mode-setting
   - Lower latency, better for embedded

3. **Software Fallback**
   - CPU-based rendering
   - Functional but slower
   - Always available

### Checking GPU Support

```bash
# List DRM devices
ls -l /dev/dri/

# Check GPU info
lspci | grep -i vga

# Check DRM driver
lsmod | grep drm

# Test GPU device access
sudo cat /dev/dri/card0 > /dev/null
```

## Performance

### Frame Rate

- **Target:** 60 FPS (16.67ms per frame)
- **VSync:** Enabled by default (tear-free)
- **Actual:** Depends on GPU and window count

### Metrics

The compositor tracks:
- FPS (updated every second)
- Frame time (microseconds)
- Damage region count
- Window count
- Memory usage

### Optimization Features

1. **Damage Tracking**
   - Only redraws changed regions
   - Reduces GPU load

2. **Triple Buffering**
   - Smooth frame pacing
   - Eliminates input lag

3. **Texture Caching**
   - Window contents cached on GPU
   - Only upload when surface is dirty

## Troubleshooting

### Compositor Won't Start

**Check GPU device:**
```bash
ls -l /dev/dri/card0
```

**Check permissions:**
```bash
# Add user to video group
sudo usermod -a -G video $USER
```

**Check logs:**
```bash
tail -f /var/log/services/compositor.log
```

### No Display Output

**Verify display detection:**
- Compositor logs show "Display added: 1920x1080 @ 60Hz"

**Check framebuffer:**
```bash
ls -l /dev/fb0
```

**Try software fallback:**
```bash
sudo compositord --daemon --gpu /dev/null
```

### Low Frame Rate

**Check CPU usage:**
```bash
top -p $(cat /run/compositor.pid)
```

**Disable VSync for testing:**
```bash
sudo compositord --no-vsync
```

**Reduce window effects:**
- Set `effects_enabled = false` in config

### IPC Socket Issues

**Check socket exists:**
```bash
ls -l /run/compositor.sock
```

**Check socket permissions:**
```bash
# Should be world-writable (0777)
chmod 777 /run/compositor.sock
```

**Test socket connection:**
```bash
echo "PING" | nc -U /run/compositor.sock
```

## Integration with Window Manager

The window manager communicates with the compositor via IPC:

```c
// Connect to compositor
int sock = socket(AF_UNIX, SOCK_STREAM, 0);
struct sockaddr_un addr = {0};
addr.sun_family = AF_UNIX;
strcpy(addr.sun_path, "/run/compositor.sock");
connect(sock, (struct sockaddr *)&addr, sizeof(addr));

// Send command
char cmd[] = "CREATE_WINDOW:800,600,MyApp\n";
write(sock, cmd, strlen(cmd));

// Read response
char response[256];
read(sock, response, sizeof(response));

// Close connection
close(sock);
```

## Next Steps

1. **Protocol Implementation**
   - Expand IPC command set
   - Add binary protocol for performance
   - Implement shared memory for surfaces

2. **Display Manager Integration**
   - Start compositor before login
   - Hand off display to user session
   - Multi-user support

3. **Window Manager Integration**
   - Full window lifecycle management
   - Focus handling
   - Input event routing

4. **Performance Monitoring**
   - Expose metrics via IPC
   - Performance profiling tools
   - Frame time graphs

5. **Advanced Features**
   - Multi-monitor hot-plug
   - Hardware cursor support
   - Screen recording/capture

## Code Statistics

- **compositord.c:** 350 LOC
- **compositor.service:** 50 lines
- **Integration scripts:** 100 LOC
- **Total added:** ~500 LOC

## Testing

### Smoke Test

```bash
# 1. Build and install
cd userspace/compositor
make clean
make all
sudo make install

# 2. Start daemon
sudo ./start_compositor.sh

# 3. Verify running
ps aux | grep compositord
ls -l /run/compositor.sock

# 4. Test IPC
echo "PING" | nc -U /run/compositor.sock

# 5. Stop daemon
sudo kill $(cat /run/compositor.pid)
```

### Integration Test

```bash
# Run full test suite
make test

# Run specific demos
./demo_simple_window
./demo_animations
./desktop_stack_validator
```

## References

- Main compositor code: `compositor.c` (11.7 KB)
- GPU abstraction: `gpu.c` (12.8 KB)
- Compositor header: `compositor.h` (4.3 KB)
- Service manager: `../system/services/servicemanager.c`

## Authors

- Desktop Compositor Integrator Agent (May 2026)
- Based on AutomationOS Compositor by Desktop Stack Validator

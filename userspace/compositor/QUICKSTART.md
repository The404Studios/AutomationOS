# Compositor Service - Quick Start Guide

## 🚀 5-Minute Setup

### 1. Build
```bash
cd userspace/compositor
make compositord
```

### 2. Install
```bash
sudo make install
```

### 3. Start
```bash
sudo compositord --daemon
```

### 4. Test
```bash
make test_ipc_client
./test_ipc_client
```

### 5. Stop
```bash
sudo kill $(cat /run/compositor.pid)
```

---

## 📋 One-Command Startup

```bash
sudo ./start_compositor.sh
```

This does everything: builds, installs, starts, and verifies.

---

## 🔍 Check Status

### Is it running?
```bash
ps aux | grep compositord
```

### Check PID
```bash
cat /run/compositor.pid
```

### Check socket
```bash
ls -l /run/compositor.sock
```

### View logs
```bash
tail -f /var/log/services/compositor.log
```

---

## 🎮 Command-Line Options

```bash
# Run as daemon (background)
compositord --daemon

# Use specific GPU
compositord --daemon --gpu /dev/dri/card1

# Disable VSync (for testing)
compositord --no-vsync

# Show help
compositord --help
```

---

## 🔧 Troubleshooting

### Compositor won't start

**Check GPU device:**
```bash
ls -l /dev/dri/card0
```

**Try software fallback:**
```bash
sudo compositord --daemon --gpu /dev/null
```

### No IPC response

**Check socket exists:**
```bash
ls -l /run/compositor.sock
```

**Test connection:**
```bash
echo "PING" | nc -U /run/compositor.sock
```

### Still having issues?

**Read the logs:**
```bash
tail -30 /var/log/services/compositor.log
```

**Check process status:**
```bash
ps aux | grep compositord
```

---

## 📚 More Info

- Full documentation: `COMPOSITOR_INTEGRATION.md`
- Integration status: `INTEGRATION_STATUS.md`
- Compositor code: `compositor.c`, `gpu.c`
- Daemon code: `compositord.c`

---

## ✅ Quick Validation

Run this to verify everything works:

```bash
# Build and start
cd userspace/compositor
make compositord
sudo ./start_compositor.sh

# Wait 2 seconds
sleep 2

# Test IPC
make test_ipc_client
./test_ipc_client

# Check status
ps aux | grep compositord
ls -l /run/compositor.*

# Stop
sudo kill $(cat /run/compositor.pid)
```

If all commands succeed, **graphics are working!** 🎉

---

## 🏁 Expected Output

```
=== AutomationOS Compositor Service ===

[1/5] Creating runtime directories...
[2/5] Checking GPU device...
  ✓ GPU found: /dev/dri/card0
[3/5] Building compositor daemon...
  ✓ Compositor daemon built successfully
[4/5] Installing compositor daemon...
  ✓ Installed to /usr/bin/compositord
[5/5] Starting compositor daemon...

✓ Compositor started successfully!
  PID: 1234
  Socket: /run/compositor.sock

Compositor Status:
root  1234  0.5  0.1  /usr/bin/compositord --daemon

IPC Socket:
srwxrwxrwx 1 root root 0 May 26 14:00 /run/compositor.sock

To stop: kill 1234
To view logs: tail -f /var/log/services/compositor.log
```

---

**Graphics infrastructure operational!**

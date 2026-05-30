# AutomationOS Service Manager

## Overview

The AutomationOS Service Manager is a systemd-like service management system that provides reliable, automated control of system services and daemons. It handles service lifecycle, dependencies, monitoring, and recovery.

## Architecture

### Core Components

1. **Service Manager (`servicemanager.c`)** - 3000+ LOC
   - Service lifecycle management (start, stop, restart)
   - Dependency resolution and ordering
   - Resource management and limits
   - Automatic restart on failure
   - Watchdog monitoring

2. **Service Control (`servicectl.c`)** - 1500+ LOC
   - Command-line interface for service management
   - Service status queries
   - Log viewing and following

3. **Monitor (`monitor.c`)** - 800+ LOC
   - CPU and memory usage tracking
   - Health checks and alerting
   - Performance metrics collection
   - Restart loop detection

4. **Logging (`logging.c`)** - 600+ LOC
   - Centralized log collection
   - Log rotation and compression
   - Query and search capabilities
   - Log retention management

## Service Lifecycle

```
           start
STOPPED ---------> STARTING
   ^                   |
   |                   | (async)
   |                   v
   |              RUNNING
   |                   |
   |                   | stop
   |                   v
   +------------- STOPPING
```

### Service States

- **STOPPED** - Service is not running
- **STARTING** - Service is in the process of starting
- **RUNNING** - Service is running normally
- **STOPPING** - Service is being stopped
- **FAILED** - Service failed to start or crashed
- **RESTARTING** - Service is restarting after crash

## Service Configuration

Services are configured using `.service` files in `/etc/services/`.

### Configuration Format

```ini
[Service]
Description=Network Management Service
Type=simple

ExecStart=/usr/sbin/networkd
ExecStop=/bin/kill -TERM $MAINPID
WorkingDirectory=/var/lib/network

User=network
Group=network

Requires=dbus.service
After=dbus.service

Restart=always
RestartDelay=5s
RestartMaxAttempts=5

CPUQuota=50%
MemoryLimit=100M
TaskLimit=100
FileLimit=1024

TimeoutStartSec=30s
TimeoutStopSec=30s

WatchdogSec=60s
```

### Service Types

- **simple** - Process runs directly (default)
- **forking** - Process forks and parent exits
- **oneshot** - One-time execution
- **notify** - Service notifies when ready
- **dbus** - D-Bus activated service
- **idle** - Run only when system is idle

### Restart Policies

- **no** - Never restart
- **always** - Always restart
- **on-failure** - Restart only on failure
- **on-abnormal** - Restart on abnormal exit
- **on-abort** - Restart on core dump
- **on-watchdog** - Restart on watchdog timeout

## Command-Line Interface

### Starting and Stopping Services

```bash
# Start a service
servicectl start networking

# Stop a service
servicectl stop networking

# Restart a service
servicectl restart networking

# Reload service configuration
servicectl reload networking
```

### Enabling and Disabling Services

```bash
# Enable service (start on boot)
servicectl enable networking

# Disable service
servicectl disable networking
```

### Viewing Status

```bash
# Show service status
servicectl status networking

# List all services
servicectl list
```

### Viewing Logs

```bash
# Show last 100 lines of logs
servicectl logs networking

# Follow logs in real-time
servicectl logs -f networking

# Show last 50 lines
servicectl logs -n 50 networking
```

## System Services

### 1. Network Manager (networkd)

**Purpose:** Manages network interfaces, DHCP, DNS

**Features:**
- Automatic interface discovery
- DHCP client
- Static IP configuration
- DNS resolution
- Link monitoring
- Network statistics

**Dependencies:** dbus.service

**Configuration:** `/etc/services/networking.service`

### 2. Display Manager (displayd)

**Purpose:** Manages desktop environment and user sessions

**Features:**
- Desktop environment startup
- User session management
- Screen locking
- Fast user switching

**Dependencies:** dbus.service, audiod.service, networkd.service

### 3. Audio Server (audiod)

**Purpose:** Audio mixing and routing

**Features:**
- Multi-source audio mixing
- Output routing
- Volume control
- Bluetooth audio support

**Dependencies:** dbus.service

### 4. Notification Daemon (notificationd)

**Purpose:** Application notification handling

**Features:**
- Notification queue
- Display management
- Notification history
- Do Not Disturb mode

**Dependencies:** dbus.service, displayd.service

### 5. Update Service (updated)

**Purpose:** System update management

**Features:**
- Automatic update checks
- Background downloads
- Update application on shutdown
- Rollback support

**Dependencies:** networkd.service

### 6. Backup Service (backupd)

**Purpose:** Automated backup management

**Features:**
- Scheduled backups
- Incremental backups
- Cloud sync integration
- Backup verification

**Dependencies:** networkd.service

### 7. Logging Service (logd)

**Purpose:** Centralized log collection

**Features:**
- Log aggregation
- Log rotation
- Log compression
- Query interface

**Dependencies:** None (starts early)

### 8. Cron Service (crond)

**Purpose:** Scheduled task execution

**Features:**
- User crontabs
- System crontabs
- Job monitoring
- Missed job execution

**Dependencies:** None

## Dependency Management

### Dependency Types

- **Requires** - Hard dependency (must start first)
- **Wants** - Soft dependency (prefer to start first)
- **Before** - Start before these services
- **After** - Start after these services
- **Conflicts** - Cannot run with these services

### Boot Sequence

```
1. logd.service                 (logging)
2. dbus.service                 (message bus)
3. networkd.service             (requires dbus)
4. audiod.service               (requires dbus)
5. notificationd.service        (requires dbus)
6. displayd.service             (requires network, audio, dbus)
7. updated.service              (requires network)
8. backupd.service              (requires network)
```

## Monitoring and Alerting

### Health Checks

The monitor component performs the following checks:

- CPU usage per service
- Memory usage per service
- Restart count
- Crash detection
- Hang detection (watchdog)

### Alerts

Alerts are triggered for:

- High CPU usage (> 80%)
- High memory usage (> 500 MB)
- Restart loops (5+ restarts in 5 minutes)
- Service hangs (no activity for 60 seconds)

Alerts are:
- Printed to console
- Logged to `/var/log/services/alerts.log`
- Sent to notification daemon

## Resource Management

### CPU Limits

Services can have CPU quotas:

```ini
CPUQuota=50%    # Limit to 50% CPU
```

### Memory Limits

Services can have memory limits:

```ini
MemoryLimit=100M    # Limit to 100 MB
```

### Task Limits

Limit number of processes/threads:

```ini
TaskLimit=100    # Max 100 tasks
```

### File Descriptor Limits

Limit open files:

```ini
FileLimit=1024    # Max 1024 open files
```

## Logging

### Log Format

```
[timestamp] [service] [level] message
```

Example:
```
[2026-05-26 10:23:45] [networking.service] [INFO] Started network interface eth0
```

### Log Levels

- **DEBUG** - Detailed debugging information
- **INFO** - Normal informational messages
- **WARNING** - Warning conditions
- **ERROR** - Error conditions
- **CRITICAL** - Critical conditions requiring immediate attention

### Log Storage

- Current logs: `/var/log/services/<service>.log`
- Rotated logs: `/var/log/services/<service>.log.<timestamp>`
- Compressed logs: `/var/log/services/<service>.log.<timestamp>.gz`

### Log Rotation

Logs are automatically rotated when:
- File size exceeds 10 MB
- Daily rotation (if configured)

### Log Compression

Logs older than 7 days are automatically compressed using gzip.

### Log Retention

Logs are kept for 30 days, then automatically deleted.

## Watchdog

Services can enable watchdog monitoring:

```ini
WatchdogSec=60s
```

The service must send watchdog pings within the timeout period, or it will be killed and restarted.

Send watchdog ping:
```bash
servicectl watchdog-ping networking
```

## Performance

### Startup Performance

- Parallel service startup where possible
- Service manager overhead: < 10 MB RAM
- Average service start time: < 2 seconds

### Monitoring Overhead

- Monitor thread runs every 5 seconds
- Minimal CPU usage (< 1%)
- Memory tracking via /proc filesystem

### Reliability

- Automatic restart on crash
- Dependency-aware startup/shutdown
- Watchdog for hang detection
- Health monitoring and alerting

## Troubleshooting

### Service Won't Start

1. Check service status:
   ```bash
   servicectl status <service>
   ```

2. Check logs:
   ```bash
   servicectl logs <service>
   ```

3. Check dependencies:
   ```bash
   # View service configuration
   cat /etc/services/<service>.service
   ```

4. Check resource limits:
   ```bash
   # View current resource usage
   servicectl status <service>
   ```

### Service Keeps Restarting

1. Check for restart loops:
   ```bash
   servicectl status <service>
   # Look at restart count
   ```

2. Check logs for errors:
   ```bash
   servicectl logs <service>
   ```

3. Disable automatic restart:
   ```bash
   # Edit service configuration
   # Set: Restart=no
   ```

### High CPU/Memory Usage

1. Check resource usage:
   ```bash
   servicectl status <service>
   ```

2. View monitoring alerts:
   ```bash
   cat /var/log/services/alerts.log
   ```

3. Adjust resource limits:
   ```bash
   # Edit service configuration
   # Set: CPUQuota=50%
   # Set: MemoryLimit=200M
   ```

## API Reference

### Service Manager API

```c
// Initialize service manager
int service_manager_init(void);

// Start service
int service_start(const char *name);

// Stop service
int service_stop(const char *name);

// Restart service
int service_restart(const char *name);

// Reload service configuration
int service_reload(const char *name);

// Enable service (start on boot)
int service_enable(const char *name);

// Disable service
int service_disable(const char *name);

// Get service status
int service_status(const char *name, char *buffer, size_t size);

// List all services
int service_list(void (*callback)(const char *name, const char *state, void *userdata), void *userdata);

// Shutdown service manager
int service_manager_shutdown(void);
```

### Monitor API

```c
// Initialize monitor
int monitor_init(void);

// Register service for monitoring
int monitor_register_service(const char *name, pid_t pid);

// Unregister service
int monitor_unregister_service(const char *name);

// Update service PID (after restart)
int monitor_update_service_pid(const char *name, pid_t new_pid);

// Get service metrics
int monitor_get_metrics(const char *name, service_metrics_t *out_metrics);

// Update all metrics
void monitor_update_all(void);

// Print monitoring summary
void monitor_print_summary(void);

// Shutdown monitor
void monitor_shutdown(void);
```

### Logging API

```c
// Initialize logging
int log_init(void);

// Write log entry
int log_write(const char *service, log_level_t level, const char *format, ...);

// Rotate logs
int log_rotate_all(void);

// Compress old logs
int log_compress_old(void);

// Delete old logs
int log_delete_old(void);

// Query logs
int log_query_service(const char *service, log_entry_t *entries, int max_entries, log_level_t min_level);

// Get log statistics
int log_get_stats(const char *service, log_stats_t *stats);

// Run maintenance
int log_maintenance(void);
```

## Future Enhancements

- Socket activation
- Timer units
- Mount units
- Device units
- Target units (runlevels)
- Slice units (cgroups)
- Scope units
- Snapshot units
- Swap units
- Automount units
- Path units

## References

- systemd documentation: https://systemd.io/
- Linux Service Management: https://www.freedesktop.org/wiki/Software/systemd/
- AutomationOS Architecture Guide

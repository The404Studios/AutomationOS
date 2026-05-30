# Service Manager Integration

## Overview

This document describes the service manager integration into AutomationOS, which enables automatic service startup during boot.

## Architecture

```
Init Process (PID 1)
    |
    ├─> Service Manager (PID 2)
    |   |
    |   ├─> syslogd (PID 3)
    |   ├─> dbus (PID 4)
    |   ├─> devmgr (PID 5)
    |   ├─> audiod (PID 6)
    |   ├─> networking (PID 7)
    |   └─> displayd (PID 8)
    |
    └─> Shell (PID 9)
```

## Components

### 1. Service Manager (`userspace/system/services/`)

- **servicemanager.c** (1250+ LOC): Core service management implementation
  - Service loading from configuration files
  - Dependency resolution
  - Process lifecycle management
  - Restart policies
  - Resource limits
  - Watchdog monitoring
  - Logging

- **servicemanager.h**: Public API declarations
  - `service_manager_init()`: Initialize and load services
  - `service_manager_start_boot_services()`: Start enabled services
  - `service_start()`, `service_stop()`, `service_restart()`: Control services
  - `service_enable()`, `service_disable()`: Configure boot behavior

- **servicemanager_main.c**: Main entry point for service manager executable
  - Parses command line arguments
  - Handles boot mode (`--boot`)
  - Signal handling for graceful shutdown

### 2. Init Process (`userspace/init/init.c`)

Updated to spawn service manager during boot:

```c
// Start service manager
printf("[INIT] Starting service manager...\n");

int svcmgr_pid = fork();
if (svcmgr_pid == 0) {
    char* argv[] = { "/sbin/servicemanager", "--boot", NULL };
    execve("/sbin/servicemanager", argv, envp);
    exit(1);
}

printf("[INIT] Service manager started with PID %d\n", svcmgr_pid);
```

### 3. Service Definitions (`etc/services/`)

Service configuration files using systemd-like syntax:

- **syslogd.service**: System logging (no dependencies)
- **dbus.service**: D-Bus message bus (requires syslogd)
- **devmgr.service**: Device manager (requires syslogd)
- **audiod.service**: Audio server (requires dbus)
- **networking.service**: Network manager (requires dbus)
- **displayd.service**: Display manager (requires dbus, devmgr)
- **updated.service**: Update service (requires networking)

### 4. Boot Configuration (`etc/services/boot.conf`)

Lists services to start automatically:

```
syslogd
dbus
devmgr
audiod
networking
displayd
```

## Service File Format

```ini
[Service]
Description=Display Manager Service
Type=simple

ExecStart=/usr/sbin/displayd
WorkingDirectory=/var/lib/display

User=display
Group=display

Requires=dbus.service devmgr.service
After=dbus.service devmgr.service audiod.service
Wants=audiod.service

Restart=always
RestartDelay=3s

CPUQuota=80%
MemoryLimit=500M

TimeoutStartSec=60s
TimeoutStopSec=30s
```

## Dependency Resolution

The service manager implements sophisticated dependency resolution:

1. **Requires**: Hard dependencies - service won't start if these fail
2. **Wants**: Soft dependencies - service starts even if these fail
3. **After**: Ordering - wait for these services to start first
4. **Before**: Ordering - start before these services
5. **Conflicts**: Mutual exclusion - stop if these services start

## Boot Sequence

1. Kernel starts init process (PID 1)
2. Init prints banner and verifies PID
3. Init spawns service manager with `--boot` flag
4. Service manager:
   - Loads all .service files from `/etc/services/`
   - Reads `boot.conf` to determine enabled services
   - Creates monitor and watchdog threads
   - Starts enabled services in dependency order
   - Prints status for each service
5. Init spawns shell for user interaction
6. Services run in background, monitored by service manager

## Expected Output

```
[INIT] Starting service manager...
[INIT] Service manager started with PID 2
[SERVICE MANAGER] Initializing...
[SERVICE MANAGER] Loaded 7 services from boot.conf
[SERVICE MANAGER] Loaded service: syslogd (enabled)
[SERVICE MANAGER] Loaded service: dbus (enabled)
[SERVICE MANAGER] Loaded service: devmgr (enabled)
[SERVICE MANAGER] Loaded service: audiod (enabled)
[SERVICE MANAGER] Loaded service: networking (enabled)
[SERVICE MANAGER] Loaded service: displayd (enabled)
[SERVICE MANAGER] Loaded service: updated (disabled)
[SERVICE MANAGER] Initialized (7 services loaded)
[SERVICE MANAGER] Starting boot services...
[SERVICE] Starting boot service 1/6: syslogd
[SERVICE] Starting syslogd...
[SERVICE] syslogd started (PID 3)
[SERVICE] Starting boot service 2/6: dbus
[SERVICE] Starting dbus...
[SERVICE] dbus started (PID 4)
[SERVICE] Starting boot service 3/6: devmgr
[SERVICE] Starting devmgr...
[SERVICE] devmgr started (PID 5)
[SERVICE] Starting boot service 4/6: audiod
[SERVICE] Starting audiod...
[SERVICE] audiod started (PID 6)
[SERVICE] Starting boot service 5/6: networking
[SERVICE] Starting networking...
[SERVICE] networking started (PID 7)
[SERVICE] Starting boot service 6/6: displayd
[SERVICE] Starting displayd...
[SERVICE] displayd started (PID 8)
[SERVICE] Boot sequence complete (6/6 services started)
[INIT] Services should now be starting...
[INIT] Spawning shell...
[INIT] Shell started with PID 9
```

## Features Implemented

### Service Types
- Simple: Process runs directly
- Forking: Process forks and parent exits
- Oneshot: One-time execution
- Notify: Service notifies when ready

### Restart Policies
- No: Never restart
- Always: Always restart
- On-failure: Restart only on failure
- On-abnormal: Restart on signal/timeout
- On-abort: Restart on core dump

### Resource Limits
- CPU quota (percentage)
- Memory limit (bytes)
- Task limit (max processes)
- File limit (max open files)

### Monitoring
- Monitor thread: Checks process health
- Watchdog thread: Monitors watchdog pings
- Automatic restart on failure
- Exit code tracking
- Signal tracking

### Logging
- Per-service log files in `/var/log/services/`
- Timestamps and log levels
- Stdout/stderr redirection to logs

## Testing

Run the integration test:

```bash
bash test_service_integration.sh
```

## Mock Mode

For testing without actual service binaries:

```bash
export SERVICE_MOCK_MODE=1
/sbin/servicemanager --boot
```

This simulates service starts without executing real binaries.

## Future Enhancements

1. Socket activation (like systemd)
2. D-Bus activation
3. Service templates
4. Slice/scope support for cgroups
5. Timer-based activation
6. Path-based activation
7. Inter-service communication
8. Service status API for GUI
9. journald-style structured logging
10. Service rollback on failure

## Integration Points

### Desktop Integration
- Display manager service starts compositor
- Service status visible in system settings
- Quick settings shows running services

### Security Integration
- Services run with least privilege
- User/group isolation
- Resource limits enforced

### Monitoring Integration
- Task Manager shows service status
- Resource usage per service
- Service start/stop/restart controls

## Files Changed

- `userspace/init/init.c`: Added service manager spawning (~30 LOC)
- `userspace/system/services/servicemanager.c`: Service manager implementation (1250+ LOC)
- `userspace/system/services/servicemanager.h`: Public API (20 LOC)
- `userspace/system/services/servicemanager_main.c`: Main entry point (70 LOC)
- `userspace/system/services/Makefile`: Build configuration
- `etc/services/*.service`: Service definitions (7 files)
- `etc/services/boot.conf`: Boot configuration

**Total: ~1400 LOC of integration code**

## Summary

The service manager is now fully integrated into the init system. Services will start automatically during boot in the correct order based on dependencies. The system is resilient to service failures and provides comprehensive monitoring and logging.

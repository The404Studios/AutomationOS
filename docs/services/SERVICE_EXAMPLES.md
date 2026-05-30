// Service Manager Examples and Usage Guide

## Creating a Custom Service

### Step 1: Write Your Service Daemon

Example: Simple monitoring service

```c
// /usr/local/bin/mymonitord.c
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>

static bool running = true;

void signal_handler(int signo) {
    if (signo == SIGTERM) {
        running = false;
    }
}

int main(void) {
    signal(SIGTERM, signal_handler);
    
    printf("[mymonitord] Starting monitoring service\n");
    
    while (running) {
        // Do monitoring work
        printf("[mymonitord] System check...\n");
        sleep(60);
    }
    
    printf("[mymonitord] Shutting down\n");
    return 0;
}
```

### Step 2: Create Service Configuration

Create `/etc/services/mymonitor.service`:

```ini
[Service]
Description=Custom System Monitor
Type=simple

ExecStart=/usr/local/bin/mymonitord
WorkingDirectory=/var/lib/mymonitor

User=monitor
Group=monitor

Requires=networkd.service
After=networkd.service

Restart=always
RestartDelay=10s

CPUQuota=30%
MemoryLimit=50M

TimeoutStartSec=30s
TimeoutStopSec=30s
```

### Step 3: Enable and Start Service

```bash
# Enable service to start on boot
servicectl enable mymonitor

# Start service now
servicectl start mymonitor

# Check status
servicectl status mymonitor

# View logs
servicectl logs mymonitor
```

## Service Dependency Examples

### Web Application Stack

#### Database Service

`/etc/services/database.service`:
```ini
[Service]
Description=Application Database
Type=simple

ExecStart=/usr/bin/postgres -D /var/lib/postgres/data
ExecStop=/bin/kill -TERM $MAINPID

User=postgres
Group=postgres

Restart=always
RestartDelay=5s

MemoryLimit=1G
```

#### Application Service

`/etc/services/webapp.service`:
```ini
[Service]
Description=Web Application
Type=simple

ExecStart=/usr/bin/python3 /opt/webapp/app.py
WorkingDirectory=/opt/webapp

User=webapp
Group=webapp

# Depends on database and network
Requires=database.service networkd.service
After=database.service networkd.service

Restart=always
RestartDelay=5s

CPUQuota=80%
MemoryLimit=500M
```

#### Web Server Service

`/etc/services/webserver.service`:
```ini
[Service]
Description=Web Server (nginx)
Type=simple

ExecStart=/usr/sbin/nginx -g "daemon off;"
ExecReload=/bin/kill -HUP $MAINPID
ExecStop=/bin/kill -TERM $MAINPID

# Depends on application
Requires=webapp.service
After=webapp.service

Restart=always

CPUQuota=50%
MemoryLimit=200M
```

## Advanced Configuration Examples

### High-Availability Service

```ini
[Service]
Description=HA Service with Watchdog
Type=notify

ExecStart=/usr/bin/ha-service

# Aggressive restart policy
Restart=always
RestartDelay=1s
RestartMaxAttempts=999

# Watchdog monitoring
WatchdogSec=30s

# Resource limits
CPUQuota=90%
MemoryLimit=2G

# Fast timeouts
TimeoutStartSec=10s
TimeoutStopSec=10s
TimeoutAbortSec=5s
```

### One-Shot Service

```ini
[Service]
Description=Database Migration
Type=oneshot

ExecStart=/usr/bin/migrate-database.sh

User=dbadmin
Group=dbadmin

# No restart for one-shot
Restart=no

TimeoutStartSec=300s
```

### Forking Service

```ini
[Service]
Description=Legacy Daemon
Type=forking

ExecStart=/usr/sbin/legacy-daemon
PIDFile=/var/run/legacy.pid

Restart=on-failure
```

## Common Service Patterns

### Development Server

```ini
[Service]
Description=Development Server
Type=simple

ExecStart=/usr/bin/dev-server --port 3000
WorkingDirectory=/home/developer/project

User=developer
Group=developer

# Don't restart in development
Restart=no

# Generous resource limits
CPUQuota=100%
MemoryLimit=4G
```

### Background Worker

```ini
[Service]
Description=Background Job Worker
Type=simple

ExecStart=/usr/bin/worker --queue default

# Multiple workers can run
Requires=database.service redis.service
After=database.service redis.service

Restart=always

# Limit resources
CPUQuota=50%
MemoryLimit=256M
TaskLimit=50
```

### API Gateway

```ini
[Service]
Description=API Gateway
Type=simple

ExecStart=/usr/bin/api-gateway

# Gateway needs all backend services
Requires=auth.service users.service products.service
After=auth.service users.service products.service

# High availability
Restart=always
RestartDelay=2s

# High resource limits
CPUQuota=100%
MemoryLimit=2G

# Watchdog for health checks
WatchdogSec=60s
```

## Service Monitoring Examples

### Checking Service Status

```bash
# Get detailed status
servicectl status webapp

# Output:
# Service: webapp
# Description: Web Application
# Type: simple
# State: running
# Enabled: yes
# PID: 12345
# Memory: 256 KB
# CPU: 5%
# Restart Count: 0
# Uptime: 3600 seconds
```

### Monitoring Resource Usage

```bash
# List all services with resource usage
servicectl list

# Output:
# Name                 Status      CPU    Memory
# ────────────────────────────────────────────
# ● networkd           Running     2%     15 MB
# ● audiod             Running     1%     8 MB
# ● displayd           Running     5%     45 MB
# ○ backupd            Stopped     -      -
# ● updated            Running     0%     5 MB
```

### Following Logs

```bash
# Follow service logs in real-time
servicectl logs -f webapp

# Filter logs by level
servicectl logs webapp | grep ERROR

# Show last 50 lines
servicectl logs -n 50 webapp
```

## Troubleshooting Examples

### Service Fails to Start

```bash
# Check status
servicectl status myservice

# Check logs for errors
servicectl logs myservice | tail -20

# Try starting manually for debugging
/usr/bin/myservice

# Check dependencies
cat /etc/services/myservice.service | grep Requires
```

### Service Consumes Too Much Memory

```bash
# Check current usage
servicectl status myservice

# Add memory limit
# Edit /etc/services/myservice.service:
# MemoryLimit=100M

# Reload and restart
servicectl reload myservice
servicectl restart myservice
```

### Service Keeps Restarting

```bash
# Check restart count
servicectl status myservice

# View restart history in logs
servicectl logs myservice | grep "Starting service"

# Temporarily disable auto-restart
# Edit /etc/services/myservice.service:
# Restart=no

# Restart to apply
servicectl restart myservice

# Debug the crash
servicectl logs myservice | grep ERROR
```

## Integration Examples

### Integration with Init System

The service manager can be started by the init process:

```c
// userspace/init/init.c
int main(void) {
    // Start service manager
    pid_t pid = fork();
    if (pid == 0) {
        execve("/usr/sbin/servicemanagerd", NULL, NULL);
    }
    
    // Wait for service manager to initialize
    sleep(2);
    
    // Start boot services
    system("servicectl start logd");
    system("servicectl start networkd");
    system("servicectl start displayd");
}
```

### Integration with GUI

Services UI can be launched from desktop:

```bash
# Start services UI
/usr/bin/services-ui
```

### Integration with Monitoring

Custom monitoring scripts:

```bash
#!/bin/bash
# monitor-services.sh

# Check all critical services
for service in networkd displayd audiod; do
    status=$(servicectl status $service | grep State)
    if [[ $status != *"running"* ]]; then
        echo "ALERT: $service is not running!"
        # Send notification
        notify-send "Service Alert" "$service is down"
    fi
done
```

### Integration with Backup

Backup service can trigger backups:

```bash
#!/bin/bash
# /usr/local/bin/backup-services.sh

# Stop services before backup
servicectl stop webapp
servicectl stop database

# Perform backup
tar czf /backup/services-$(date +%Y%m%d).tar.gz /var/lib/*

# Restart services
servicectl start database
servicectl start webapp
```

## Testing Examples

### Test Service Configuration

```bash
# Validate service configuration syntax
servicectl validate myservice

# Test service start/stop
servicectl start myservice
sleep 5
servicectl status myservice
servicectl stop myservice
```

### Test Dependency Resolution

```bash
# Start service with dependencies
servicectl start webapp

# Should automatically start:
# 1. database.service
# 2. networkd.service
# 3. webapp.service

# Verify order
servicectl list | grep -E "database|network|webapp"
```

### Test Resource Limits

```bash
# Monitor resource usage under load
servicectl logs -f myservice &
stress-ng --cpu 4 --vm 2 --vm-bytes 1G --timeout 60s

# Check if service was killed for exceeding limits
servicectl status myservice
```

### Test Restart Policy

```bash
# Kill service to test restart
PID=$(servicectl status myservice | grep PID | awk '{print $2}')
kill -9 $PID

# Service should automatically restart
sleep 2
servicectl status myservice

# Check restart count
servicectl status myservice | grep "Restart Count"
```

## Best Practices

### 1. Use Descriptive Service Names

Good:
```ini
Description=Web Application HTTP Server
```

Bad:
```ini
Description=Server
```

### 2. Set Appropriate Timeouts

```ini
# Quick services
TimeoutStartSec=10s

# Services with initialization
TimeoutStartSec=60s

# Database migrations
TimeoutStartSec=300s
```

### 3. Configure Resource Limits

Always set limits to prevent runaway processes:

```ini
CPUQuota=50%
MemoryLimit=500M
TaskLimit=100
FileLimit=1024
```

### 4. Use Proper Restart Policies

```ini
# Critical services
Restart=always

# One-time tasks
Restart=no

# Most services
Restart=on-failure
```

### 5. Implement Watchdog for Critical Services

```ini
WatchdogSec=60s
```

Service must send watchdog pings:
```c
// In service code
system("servicectl watchdog-ping myservice");
```

### 6. Log Appropriately

Use appropriate log levels:
- DEBUG: Detailed diagnostic information
- INFO: Normal operations
- WARNING: Unexpected but recoverable conditions
- ERROR: Errors that affect functionality
- CRITICAL: Severe errors requiring immediate attention

### 7. Handle Signals Properly

```c
void signal_handler(int signo) {
    if (signo == SIGTERM) {
        // Clean shutdown
        cleanup();
        exit(0);
    } else if (signo == SIGHUP) {
        // Reload configuration
        reload_config();
    }
}
```

### 8. Test Failure Scenarios

- Service crash
- Dependency failure
- Resource exhaustion
- Network failure
- Configuration errors

## Performance Optimization

### Parallel Service Startup

Services with no dependencies start in parallel:

```ini
# These start simultaneously
logd.service    (no dependencies)
dbus.service    (no dependencies)
```

### Lazy Service Loading

Use `Wants` instead of `Requires` for optional dependencies:

```ini
# Will start even if optional service fails
Wants=cache.service
```

### Resource Pooling

Limit concurrent service starts:

```bash
# Service manager automatically staggers starts
# to avoid resource contention
```

## Security Considerations

### User Isolation

Always run services as dedicated users:

```ini
User=myservice
Group=myservice
```

### Resource Limits

Prevent DoS via resource exhaustion:

```ini
CPUQuota=50%
MemoryLimit=500M
TaskLimit=100
FileLimit=1024
```

### Working Directory

Set safe working directory:

```ini
WorkingDirectory=/var/lib/myservice
```

### Environment Variables

Be careful with environment variables:

```ini
# Avoid passing secrets via environment
# Use configuration files with proper permissions instead
```

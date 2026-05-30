# Capability Administration Guide

**Version:** 1.0  
**Audience:** System Administrators  
**Date:** 2026-05-26

---

## Table of Contents

1. [Introduction](#introduction)
2. [Capability Basics](#capability-basics)
3. [Managing Process Capabilities](#managing-process-capabilities)
4. [File Path Patterns](#file-path-patterns)
5. [Network Capabilities](#network-capabilities)
6. [Device Capabilities](#device-capabilities)
7. [IPC Capabilities](#ipc-capabilities)
8. [System Capabilities](#system-capabilities)
9. [Capability Inheritance](#capability-inheritance)
10. [Troubleshooting](#troubleshooting)
11. [Best Practices](#best-practices)
12. [Examples](#examples)

---

## Introduction

AutomationOS uses a capability-based security model where processes must explicitly hold capabilities to perform operations. Unlike traditional UNIX permissions, capabilities provide fine-grained control over what each process can do.

### Key Concepts

- **Zero Ambient Authority**: Processes start with no capabilities
- **Explicit Grants**: Administrators must explicitly grant capabilities
- **Fine-Grained**: Separate capabilities for read/write/execute
- **Revocable**: Capabilities can be removed at any time

### Advantages

- **Least Privilege**: Processes only get necessary permissions
- **Containment**: Compromised process has limited capabilities
- **Flexibility**: Fine-grained control over resources
- **Auditability**: All capability checks logged

---

## Capability Basics

### Viewing Process Capabilities

```bash
# List capabilities of current process
caplist

# List capabilities of specific process
caplist -p <pid>

# Example output:
# Process 42 capabilities:
#   CAP_FILE_READ: /home/user/*
#   CAP_FILE_WRITE: /tmp/*
#   CAP_NET_CONNECT: api.example.com:443
```

### Checking if Process Has Capability

```bash
# Check if process can read file
capcheck -p <pid> -t file_read -f /etc/passwd

# Check if process can connect to host
capcheck -p <pid> -t net_connect -h example.com:80
```

### Capability Types

| Category | Capabilities |
|----------|-------------|
| **File** | read, write, execute, create, delete, chown, chmod |
| **Network** | bind, connect, raw, listen |
| **Device** | access (general), gpu, audio, usb, serial |
| **IPC** | ipc (targeted), ipc_broadcast, ipc_receive, shared_mem |
| **System** | admin, module, time, boot, ptrace |
| **Process** | kill, trace, setuid, setgid, nice |

---

## Managing Process Capabilities

### Granting Capabilities

```bash
# Grant file read capability to process
capctl grant -p <pid> -t file_read -path "/home/user/*"

# Grant network connect capability
capctl grant -p <pid> -t net_connect -host "api.example.com:443"

# Grant device access
capctl grant -p <pid> -t gpu

# Grant system capability (requires admin privileges)
capctl grant -p <pid> -t sys_admin
```

### Revoking Capabilities

```bash
# Revoke specific capability
capctl revoke -p <pid> -t file_write

# Revoke all capabilities (process will likely fail)
capctl revoke-all -p <pid>
```

### Setting Capabilities for New Processes

**Option 1: Capability Manifest**

Create `/etc/capabilities/<program-name>.caps`:

```json
{
  "program": "/usr/bin/myapp",
  "capabilities": [
    {
      "type": "file_read",
      "pattern": "/var/lib/myapp/*"
    },
    {
      "type": "file_write",
      "pattern": "/var/lib/myapp/*"
    },
    {
      "type": "net_connect",
      "host": "*.example.com:443"
    }
  ],
  "inherit": ["file_read", "file_write"]
}
```

**Option 2: Wrapper Script**

```bash
#!/bin/bash
# Launch myapp with specific capabilities

capctl exec -t file_read -path "/var/lib/myapp/*" \
            -t file_write -path "/var/lib/myapp/*" \
            -t net_connect -host "*.example.com:443" \
            -- /usr/bin/myapp "$@"
```

---

## File Path Patterns

### Pattern Syntax

File capabilities use glob patterns:

| Pattern | Matches |
|---------|---------|
| `/home/user/*` | All files in /home/user and subdirectories |
| `/tmp/*.txt` | Only .txt files in /tmp (not subdirs) |
| `/var/log/app.log` | Specific file only |
| `*` | All files (dangerous, avoid) |

### Pattern Matching Examples

```bash
# Allow reading user's home directory
capctl grant -p $PID -t file_read -path "/home/alice/*"

# Allow writing to application data directory
capctl grant -p $PID -t file_write -path "/var/lib/myapp/*"

# Allow reading specific config file
capctl grant -p $PID -t file_read -path "/etc/myapp/config.ini"

# Allow creating files in tmp (but not reading existing ones)
capctl grant -p $PID -t file_create -path "/tmp/*"
```

### Pattern Restrictions

**Best Practices:**

- Be as specific as possible
- Avoid wildcard-only patterns (`*`)
- Use absolute paths (not relative)
- Consider subdirectories (use `/*` for recursive)

**Common Mistakes:**

```bash
# TOO BROAD: Grants access to everything
capctl grant -p $PID -t file_read -path "*"

# WRONG: Relative path
capctl grant -p $PID -t file_read -path "data/*"

# BETTER: Absolute, specific path
capctl grant -p $PID -t file_read -path "/opt/myapp/data/*"
```

---

## Network Capabilities

### Connection Capabilities

```bash
# Allow connecting to specific host and port
capctl grant -p $PID -t net_connect -host "api.example.com:443"

# Allow connecting to any host on port 80
capctl grant -p $PID -t net_connect -host "*:80"

# Allow connecting to any subdomain
capctl grant -p $PID -t net_connect -host "*.example.com:443"

# Allow connecting to IP range (CIDR notation)
capctl grant -p $PID -t net_connect -host "10.0.0.0/8:*"
```

### Binding to Ports

```bash
# Allow binding to specific port
capctl grant -p $PID -t net_bind -port 8080

# Allow binding to port range (requires privileged capability for <1024)
capctl grant -p $PID -t net_bind -port 8000-9000

# Allow binding to privileged port (requires sys_admin)
capctl grant -p $PID -t net_bind -port 80
```

### Raw Socket Access

```bash
# Grant raw socket capability (for ping, traceroute, etc.)
capctl grant -p $PID -t net_raw

# WARNING: Raw sockets can sniff network traffic, grant carefully
```

### Network Capability Examples

**Web Server:**

```bash
capctl grant -p $PID -t net_bind -port 80
capctl grant -p $PID -t net_bind -port 443
capctl grant -p $PID -t file_read -path "/var/www/*"
```

**API Client:**

```bash
capctl grant -p $PID -t net_connect -host "api.example.com:443"
capctl grant -p $PID -t file_read -path "/etc/myapp/api-key"
```

**Database Server:**

```bash
capctl grant -p $PID -t net_bind -port 5432
capctl grant -p $PID -t file_read -path "/var/lib/postgresql/*"
capctl grant -p $PID -t file_write -path "/var/lib/postgresql/*"
```

---

## Device Capabilities

### Granting Device Access

```bash
# Grant GPU access
capctl grant -p $PID -t gpu

# Grant audio device access
capctl grant -p $PID -t audio

# Grant USB device access
capctl grant -p $PID -t usb

# Grant serial port access
capctl grant -p $PID -t serial

# Grant access to specific device by ID
capctl grant -p $PID -t device_access -dev 0x1234
```

### Device Capability Use Cases

**Graphics Application:**

```bash
capctl grant -p $PID -t gpu
capctl grant -p $PID -t file_read -path "/home/user/images/*"
```

**Media Player:**

```bash
capctl grant -p $PID -t audio
capctl grant -p $PID -t file_read -path "/home/user/music/*"
```

**USB Manager:**

```bash
capctl grant -p $PID -t usb
capctl grant -p $PID -t file_write -path "/var/log/usb.log"
```

### Device Security Considerations

- **GPU**: Can access framebuffer, see screen contents
- **Audio**: Can record microphone input
- **USB**: Can access USB devices (including storage, keyboards)
- **Serial**: Can communicate with serial devices

Grant device capabilities only to trusted applications.

---

## IPC Capabilities

### Targeted IPC

```bash
# Allow sending IPC to specific process
capctl grant -p $PID -t ipc -target $TARGET_PID

# Allow receiving IPC messages
capctl grant -p $PID -t ipc_receive
```

### Broadcast IPC

```bash
# Allow sending IPC to any process (dangerous)
capctl grant -p $PID -t ipc_broadcast

# WARNING: Use sparingly, allows communication with all processes
```

### Shared Memory

```bash
# Allow creating and accessing shared memory
capctl grant -p $PID -t shared_mem
```

### IPC Security Model

**Parent-Child Communication:**

```bash
# Parent process grants IPC capability to child
capctl grant -p $CHILD_PID -t ipc -target $PARENT_PID
capctl grant -p $PARENT_PID -t ipc -target $CHILD_PID
```

**Service-Client Communication:**

```bash
# Service allows any client to connect
capctl grant -p $SERVICE_PID -t ipc_receive

# Clients get capability to talk to service
capctl grant -p $CLIENT_PID -t ipc -target $SERVICE_PID
```

---

## System Capabilities

### Administrative Capabilities

```bash
# Grant system administration capability
# WARNING: Extremely powerful, grants ability to modify system
capctl grant -p $PID -t sys_admin

# Grant module loading capability
capctl grant -p $PID -t sys_module

# Grant time setting capability
capctl grant -p $PID -t sys_time

# Grant reboot capability
capctl grant -p $PID -t sys_boot

# Grant ptrace capability (debugging)
capctl grant -p $PID -t sys_ptrace
```

### System Capability Risks

| Capability | Risk | Use Case |
|-----------|------|----------|
| `sys_admin` | Can modify system config, mount filesystems | Init process, system management tools |
| `sys_module` | Can load kernel modules, compromise kernel | Module loader, driver installer |
| `sys_time` | Can change system time, affect logs | NTP daemon, time sync tools |
| `sys_boot` | Can reboot/shutdown system | Init, power management |
| `sys_ptrace` | Can debug any process, read memory | Debuggers, profilers |

**Best Practice:** Grant system capabilities only to essential system services.

---

## Capability Inheritance

### How Inheritance Works

When a process calls `fork()`:

1. Child receives capabilities marked as inheritable
2. Default: File and IPC capabilities inherited, system capabilities not
3. Parent can customize inheritance mask

When a process calls `exec()`:

1. Capabilities reset based on executable's capability manifest
2. Only capabilities declared in manifest are granted
3. System capabilities require explicit approval

### Setting Inheritable Capabilities

```bash
# Mark capability as inheritable
capctl grant -p $PID -t file_read -path "/data/*" --inheritable

# Mark capability as non-inheritable
capctl modify -p $PID -t file_read --no-inherit
```

### Capability Manifest for Executables

Create `/etc/capabilities/<program>.caps`:

```json
{
  "program": "/usr/bin/child-process",
  "inherit_from_parent": ["file_read", "file_write"],
  "required_capabilities": [
    {
      "type": "net_connect",
      "host": "api.example.com:443"
    }
  ]
}
```

### Inheritance Examples

**Case 1: Shell Spawning Editor**

```bash
# Shell has: CAP_FILE_READ(/home/user/*), CAP_FILE_WRITE(/home/user/*)
# Shell spawns vim

# Vim inherits: CAP_FILE_READ(/home/user/*), CAP_FILE_WRITE(/home/user/*)
# Vim cannot: Access network, other directories
```

**Case 2: Service Spawning Worker**

```bash
# Service has: CAP_NET_BIND(8080), CAP_FILE_READ(/data/*)
# Service spawns worker

# Worker inherits: CAP_FILE_READ(/data/*)
# Worker does NOT inherit: CAP_NET_BIND (network capabilities not inherited by default)
```

---

## Troubleshooting

### Permission Denied Errors

**Symptom:** Process fails with "Permission denied" (EPERM/EACCES)

**Diagnosis:**

```bash
# Check process capabilities
caplist -p $PID

# Check audit log for denied capability
tail -f /var/log/audit.log | grep "CAP_DENIED"

# Example output:
# [CAP_DENIED] pid=42 cap=file_write path=/etc/passwd
```

**Solution:**

```bash
# Grant missing capability
capctl grant -p $PID -t file_write -path "/etc/passwd"
```

### Capability Not Taking Effect

**Symptom:** Granted capability, but process still denied

**Possible Causes:**

1. **Typo in path pattern**: Check exact path
2. **MAC policy also denying**: Check MAC logs
3. **Process already cached decision**: Restart process
4. **Capability revoked by parent**: Check parent process

**Diagnosis:**

```bash
# Verify capability was granted
caplist -p $PID | grep file_write

# Check MAC policy
macctl check -p $PID -path "/etc/passwd" -op write

# Check audit log
tail -n 50 /var/log/audit.log | grep "pid=$PID"
```

### Process Crashing After Capability Revocation

**Symptom:** Revoked capability, process crashed

**Cause:** Process tried to use revoked capability

**Prevention:**

```bash
# Instead of immediate revoke, mark as expiring
capctl grant -p $PID -t file_read -path "/data/*" --expire 3600

# Process has 1 hour to finish before capability expires
```

### Inherited Capabilities Not Working

**Symptom:** Child process should inherit capability, but doesn't

**Diagnosis:**

```bash
# Check if parent capability is inheritable
caplist -p $PARENT_PID --show-flags

# Example output:
# CAP_FILE_READ /data/* [inheritable]
# CAP_NET_CONNECT api.example.com:443 [non-inheritable]
```

**Solution:**

```bash
# Mark parent capability as inheritable
capctl modify -p $PARENT_PID -t file_read --inheritable
```

---

## Best Practices

### Principle of Least Privilege

**DO:**

- Grant minimum necessary capabilities
- Use specific path patterns, not wildcards
- Use specific hosts/ports for network capabilities
- Revoke capabilities when no longer needed

**DON'T:**

- Grant `sys_admin` unless absolutely necessary
- Use wildcard-only patterns (`*`)
- Grant `ipc_broadcast` to untrusted processes
- Grant device capabilities to network-facing services

### Security Checklist

Before granting capabilities:

- [ ] Is this capability necessary for the program's function?
- [ ] Can I use a more specific pattern/restriction?
- [ ] Should this capability be inheritable?
- [ ] What is the risk if this process is compromised?
- [ ] Are there MAC policies also protecting this resource?
- [ ] Is audit logging enabled for this capability?

### Capability Manifest Best Practices

**Well-Designed Manifest:**

```json
{
  "program": "/usr/bin/webapp",
  "capabilities": [
    {
      "type": "file_read",
      "pattern": "/var/www/*",
      "comment": "Read web content"
    },
    {
      "type": "net_bind",
      "port": 8080,
      "comment": "HTTP server"
    },
    {
      "type": "net_connect",
      "host": "db.internal:5432",
      "comment": "Database connection"
    }
  ],
  "inherit": [],
  "audit": true
}
```

**Poor Manifest:**

```json
{
  "program": "/usr/bin/webapp",
  "capabilities": [
    {
      "type": "file_read",
      "pattern": "*"
    },
    {
      "type": "sys_admin"
    }
  ]
}
```

### Regular Audits

```bash
# List all processes with sys_admin capability
caplist --all | grep sys_admin

# List all processes with broad file access
caplist --all | grep "file_.*\s\*$"

# Review audit log for denied capabilities
grep CAP_DENIED /var/log/audit.log | sort | uniq -c
```

---

## Examples

### Example 1: Web Server

```bash
# Create web server with minimal capabilities
PID=$(capctl exec \
  -t file_read -path "/var/www/*" \
  -t net_bind -port 80 \
  -t net_bind -port 443 \
  -- /usr/sbin/nginx)

# Verify capabilities
caplist -p $PID

# Output:
# Process 123 capabilities:
#   CAP_FILE_READ: /var/www/*
#   CAP_NET_BIND: port 80
#   CAP_NET_BIND: port 443
```

### Example 2: Backup Script

```bash
# Backup script needs read access to multiple directories
capctl exec \
  -t file_read -path "/home/*" \
  -t file_read -path "/etc/*" \
  -t file_write -path "/backup/*" \
  -- /usr/local/bin/backup.sh
```

### Example 3: Container Runtime

```bash
# Container needs isolated filesystem and network
capctl exec \
  -t file_read -path "/containers/myapp/*" \
  -t file_write -path "/containers/myapp/*" \
  -t file_execute -path "/containers/myapp/*" \
  -t net_connect -host "*:80" \
  -t net_connect -host "*:443" \
  -- /usr/bin/container-runtime /containers/myapp
```

### Example 4: Database Server

```bash
# Database server capabilities
PID=$(pgrep postgres)

# Grant data directory access
capctl grant -p $PID -t file_read -path "/var/lib/postgresql/*"
capctl grant -p $PID -t file_write -path "/var/lib/postgresql/*"

# Grant network binding
capctl grant -p $PID -t net_bind -port 5432

# Grant IPC for client connections
capctl grant -p $PID -t ipc_receive
```

### Example 5: Media Player

```bash
# Media player needs audio and file access
capctl exec \
  -t audio \
  -t file_read -path "/home/user/music/*" \
  -t file_read -path "/home/user/videos/*" \
  -- /usr/bin/mediaplayer
```

### Example 6: System Monitor

```bash
# System monitor needs broad read access, no write
capctl exec \
  -t file_read -path "/proc/*" \
  -t file_read -path "/sys/*" \
  -t sys_ptrace \
  -- /usr/bin/system-monitor
```

### Example 7: Sandboxed Browser

```bash
# Browser in strict sandbox
capctl exec \
  -t file_read -path "/home/user/downloads/*" \
  -t file_write -path "/home/user/downloads/*" \
  -t net_connect -host "*:80" \
  -t net_connect -host "*:443" \
  -t gpu \
  -t audio \
  -- /opt/browser/browser --sandboxed
```

---

## Additional Resources

- **API Reference**: `docs/phase2/CAPABILITY_API.md`
- **Security Architecture**: `docs/phase2/SECURITY_ARCHITECTURE.md`
- **Audit Guide**: `docs/phase2/AUDIT_GUIDE.md`
- **Man Pages**: `man 1 capctl`, `man 1 caplist`, `man 1 capcheck`

---

**Document Version:** 1.0  
**Last Updated:** 2026-05-26  
**Maintained By:** AutomationOS Security Team

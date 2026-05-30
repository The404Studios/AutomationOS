# Audit Logging Quick Start Guide

**5-minute guide to using the AutomationOS audit logging subsystem**

## For Kernel Developers

### 1. Initialize Audit Subsystem

Add to kernel initialization (`kernel/main.c`):

```c
#include "audit.h"

void kernel_main(void) {
    // ... other initialization ...
    
    audit_init();  // Initialize audit subsystem
    
    // ... rest of kernel initialization ...
}
```

### 2. Log Events in Syscalls

Add audit logging to security-sensitive operations:

```c
#include "audit.h"

int sys_open(const char* path, int flags) {
    // Check permissions
    if (!has_permission(path)) {
        // Log denial BEFORE returning error
        audit_log(AUDIT_FILE_OPEN, AUDIT_DENIED,
                 current->pid, current->uid, path, 0, -EACCES);
        return -EACCES;
    }
    
    // Perform operation
    int fd = vfs_open(path, flags);
    
    // Log success/failure
    audit_log(AUDIT_FILE_OPEN,
             fd >= 0 ? AUDIT_SUCCESS : AUDIT_FAILURE,
             current->pid, current->uid, path, 0, errno);
    
    return fd;
}
```

### 3. Use Convenience Macros

For common scenarios, use helper macros:

```c
// File access
AUDIT_LOG_FILE_ACCESS("/etc/passwd", AUDIT_SUCCESS);

// Capability denied
AUDIT_LOG_CAPABILITY_DENIED(CAP_NET_BIND_SERVICE);

// MAC denial
AUDIT_LOG_MAC_DENIED("/secret/file", "confidential");

// Syscall with result
AUDIT_LOG_SYSCALL(SYS_KILL, AUDIT_DENIED, -EPERM);
```

### 4. Log Custom Events

For specialized events:

```c
audit_event_t event;
memset(&event, 0, sizeof(event));

event.timestamp = audit_get_timestamp();
event.type = AUDIT_KERNEL_MODULE_LOAD;
event.result = AUDIT_SUCCESS;
event.pid = current->pid;
event.uid = current->uid;
strncpy(event.path, module_name, AUDIT_PATH_MAX - 1);
strncpy(event.comm, current->name, AUDIT_COMM_MAX - 1);

audit_log_event(&event);
```

## For Userspace Developers

### 1. Enable Auditing

```bash
# Enable audit logging
auditctl enable

# Check status
auditctl status
```

### 2. Add Audit Rules

```bash
# Alert on security violations
auditctl add -t 5000 -A alert     # Capability denied
auditctl add -t 5001 -A alert     # MAC denied

# Log specific paths
auditctl add -f /etc/* -A log     # Monitor /etc
auditctl add -f /var/log/* -A log # Monitor logs

# Ignore noisy processes
auditctl add -u 0 -A ignore       # Ignore root (UID 0)
auditctl add -p 1234 -A ignore    # Ignore PID 1234
```

### 3. Search Audit Logs

```bash
# Search by event type
ausearch -t 5000                   # Security violations
ausearch -t 2000                   # File open events

# Search by user
ausearch -u 1000                   # Events from UID 1000
ausearch -u 1000 -r failure        # Failed ops by UID 1000

# Search by path
ausearch -f /etc/passwd            # Access to /etc/passwd
ausearch -f /etc/*                 # All /etc access

# Search by command
ausearch -c sshd                   # All sshd events
ausearch -c bash -v                # Bash events (verbose)

# Limit results
ausearch -n 20                     # Show last 20 events
```

### 4. Generate Reports

```bash
# Summary report
aureport --summary

# Security violations
aureport --security

# Failed operations
aureport --failed

# Authentication events
aureport --auth

# File access
aureport --file
```

## Common Use Cases

### Monitor Sensitive Files

```bash
# Add rule to log all access to /etc/shadow
auditctl add -f /etc/shadow -A alert

# Check logs
ausearch -f /etc/shadow -v
```

### Track Security Violations

```bash
# Add rules for all security events
auditctl add -t 5000 -A alert     # Cap denied
auditctl add -t 5001 -A alert     # MAC denied
auditctl add -t 5002 -A alert     # Sandbox violation

# Generate security report
aureport --security
```

### Debug Failed Operations

```bash
# Search for all failures
ausearch -r failure

# Or use report
aureport --failed
```

### Monitor Specific User

```bash
# Log all events from UID 1000
auditctl add -u 1000 -A log

# View their activity
ausearch -u 1000 -v
```

### Compliance Reporting

```bash
# Generate authentication report (for auditors)
aureport --auth > auth_report.txt

# Generate security violations report
aureport --security > security_report.txt

# Generate summary for period
aureport --summary > audit_summary.txt
```

## Programmatic Access (C)

### Enable Auditing

```c
#include <sys/syscall.h>
#include "audit.h"

int enable_audit(void) {
    return syscall(SYS_AUDIT_ENABLE);
}
```

### Read Events

```c
audit_event_t events[100];
int count = syscall(SYS_AUDIT_READ, events, 100);

for (int i = 0; i < count; i++) {
    printf("Event %d: type=%d, pid=%u, path=%s\n",
           i, events[i].type, events[i].pid, events[i].path);
}
```

### Add Rule

```c
audit_rule_t rule;
memset(&rule, 0, sizeof(rule));

rule.filter_type = AUDIT_FILTER_TYPE;
rule.criteria.event_type = AUDIT_SECURITY_CAP_DENIED;
rule.action = AUDIT_ACTION_ALERT;
rule.enabled = true;

int rule_id = syscall(SYS_AUDIT_RULE_ADD, &rule);
printf("Rule added with ID: %d\n", rule_id);
```

### Get Statistics

```c
uint64_t total, dropped;
syscall(SYS_AUDIT_GET_STATS, &total, &dropped);

printf("Total: %llu, Dropped: %llu\n", total, dropped);
```

## Performance Tips

### Reduce Overhead

1. **Filter noisy events**: Use rules to ignore high-frequency events
   ```bash
   auditctl add -t 2001 -A ignore   # Ignore file reads
   ```

2. **Don't log successful ops**: Disable in config (default)
   ```c
   config.log_successful = false;
   ```

3. **Increase rate limit**: For high-traffic systems
   ```c
   config.rate_limit = 100000;  // 100k events/sec
   ```

### Monitor Buffer Health

```bash
# Check for dropped events
auditctl stats

# If drops are high:
# 1. Add more filters
# 2. Increase buffer size (kernel config)
# 3. Enable disk logging
```

## Troubleshooting

### No events showing up?

```bash
# Check if auditing is enabled
auditctl status

# Enable it
auditctl enable

# Generate test event
touch /tmp/test_file
ausearch -f /tmp/test_file
```

### Events being dropped?

```bash
# Check statistics
auditctl stats

# Add filters to reduce noise
auditctl add -u 0 -A ignore      # Ignore root
auditctl add -t 2001 -A ignore   # Ignore reads
```

### Want more detail?

```bash
# Use verbose mode
ausearch -v

# Check config
cat /proc/sys/kernel/audit/config  # (future: when procfs available)
```

## Best Practices

1. **Enable auditing early**: Call `audit_init()` in kernel boot
2. **Log before enforcement**: Log denials before returning errors
3. **Use structured events**: Fill all relevant fields in audit_event_t
4. **Add context**: Include paths, PIDs, UIDs in events
5. **Monitor statistics**: Check for dropped events regularly
6. **Filter wisely**: Balance security and performance
7. **Regular reports**: Generate compliance reports weekly
8. **Review failures**: Investigate failed operations promptly

## Security Checklist

- [ ] Auditing enabled at boot
- [ ] Default rules installed (cap denied, MAC denied, auth failure)
- [ ] Sensitive paths monitored (/etc, /var/log, /root)
- [ ] Security violation alerts configured
- [ ] Regular reports scheduled
- [ ] Log retention policy defined
- [ ] Access restricted (CAP_AUDIT_READ, CAP_AUDIT_CONTROL)
- [ ] Hash chain integrity checked regularly

## Next Steps

1. **Read full docs**: `kernel/audit/README.md`
2. **Run demo**: `userspace/examples/audit_demo`
3. **Run tests**: `make test-audit`
4. **Integrate with security**: Hook into capability/MAC systems
5. **Enable disk logging**: When VFS is available

## Quick Reference

### Event Type Ranges
- 1000-1099: Authentication
- 2000-2099: File access
- 3000-3099: Process events
- 4000-4099: Network events
- 5000-5099: Security violations
- 6000-6099: Configuration changes
- 7000-7099: Kernel events
- 8000-8099: System events

### Result Codes
- `AUDIT_SUCCESS` (0): Operation succeeded
- `AUDIT_FAILURE` (1): Operation failed
- `AUDIT_DENIED` (2): Permission denied
- `AUDIT_ERROR` (3): Error occurred

### Syscall Numbers
- `SYS_AUDIT_ENABLE` (200)
- `SYS_AUDIT_DISABLE` (201)
- `SYS_AUDIT_READ` (202)
- `SYS_AUDIT_RULE_ADD` (203)
- `SYS_AUDIT_RULE_DEL` (204)
- `SYS_AUDIT_GET_STATS` (205)

---

**Need help?** See `kernel/audit/README.md` for full documentation.

# Capability System Quick Reference

**For:** Kernel Developers and Security Auditors  
**Version:** 1.0  
**Date:** 2026-05-26

---

## Quick Start

### Include Required Headers
```c
#include "capability.h"
#include "sched.h"
```

### Check if Process Has Capability
```c
process_t* proc = process_get_current();
if (capability_has(proc->capabilities, CAP_FILE_READ)) {
    // Process can read files
}
```

### Check File Access
```c
if (capability_check_file(proc->capabilities, "/etc/passwd", CAP_FILE_READ)) {
    // Process can read /etc/passwd
}
```

### Check Network Access
```c
if (capability_check_net(proc->capabilities, "www.example.com", 80, CAP_NET_CONNECT)) {
    // Process can connect to www.example.com:80
}
```

---

## Common Capability Types

### File System
```c
CAP_FILE_READ      // Read files
CAP_FILE_WRITE     // Write files
CAP_FILE_EXECUTE   // Execute programs
CAP_FILE_CREATE    // Create files/directories
CAP_FILE_DELETE    // Delete files/directories
```

### Network
```c
CAP_NET_BIND       // Bind to ports
CAP_NET_CONNECT    // Connect to remote hosts
CAP_NET_RAW        // Raw sockets
CAP_NET_LISTEN     // Listen for connections
```

### System
```c
CAP_SYS_ADMIN      // System administration
CAP_SYS_MODULE     // Load kernel modules
CAP_SYS_TIME       // Set system time
```

### Process
```c
CAP_PROCESS_KILL   // Send signals
CAP_PROCESS_TRACE  // Debug processes
CAP_PROCESS_SETUID // Change user ID
```

---

## Creating Capabilities

### Simple Capability (No Constraints)
```c
capability_t* cap = capability_create_simple(
    CAP_SYS_ADMIN,
    CAP_FLAG_INHERITABLE | CAP_FLAG_DELEGATABLE
);
```

### File Capability (With Path Pattern)
```c
capability_t* cap = capability_create_file(
    CAP_FILE_READ,
    "/home/user/*",        // Path pattern
    CAP_FLAG_INHERITABLE
);
```

### Network Capability (With Host/Port)
```c
capability_t* cap = capability_create_net(
    CAP_NET_CONNECT,
    "*.example.com",       // Host pattern
    80,                    // Port min
    443,                   // Port max
    CAP_FLAG_INHERITABLE
);
```

### Device Capability
```c
capability_t* cap = capability_create_device(
    0x1234,                // Device ID (or 0xFFFFFFFF for all)
    "gpu",                 // Device class
    CAP_FLAG_INHERITABLE
);
```

### IPC Capability
```c
capability_t* cap = capability_create_ipc(
    42,                    // Target PID (or 0 for any)
    CAP_FLAG_INHERITABLE
);
```

---

## Managing Capability Sets

### Create Capability Set
```c
capability_set_t* caps = capability_set_create();
```

### Add Capability to Set
```c
int result = capability_add(caps, cap);
if (result != CAP_SUCCESS) {
    // Handle error
}
```

### Remove Capability from Set
```c
int result = capability_remove(caps, CAP_FILE_READ);
```

### Clone Capability Set
```c
capability_set_t* clone = capability_set_clone(original);
```

### Destroy Capability Set
```c
capability_set_destroy(caps);
```

---

## Process Capability Management

### Initialize Process with Capabilities
```c
process_t* proc = process_create("myapp", entry_point);
proc->capabilities = capability_create_user_set("myapp");
```

### Inherit Capabilities on Fork
```c
// Automatically done in process_fork:
child->capabilities = capability_inherit_from_parent(parent);
```

### Reset Capabilities on Exec
```c
// Automatically done in process_exec:
capability_set_destroy(proc->capabilities);
proc->capabilities = capability_reset_for_exec(proc, "/bin/program");
```

---

## Delegation and Revocation

### Delegate Capability to Another Process
```c
process_t* granter = process_get_current();
process_t* grantee = process_get_by_pid(target_pid);
capability_t* cap = ...; // Capability to delegate

int result = capability_grant(granter, grantee, cap);
if (result == CAP_SUCCESS) {
    // Delegation successful
}
```

### Revoke Capability from Process
```c
int result = capability_revoke(proc, CAP_FILE_WRITE);
if (result == CAP_SUCCESS) {
    // Revocation successful, global generation incremented
}
```

### Revoke All Capabilities
```c
int result = capability_revoke_all(proc);
```

---

## Syscall Integration

### Check Capability Before Syscall
```c
int64_t sys_myfunction(uint64_t arg1, ...) {
    process_t* proc = process_get_current();
    
    // Check if process has required capability
    if (!capability_has(proc->capabilities, CAP_MY_OPERATION)) {
        audit_log_capability(proc, CAP_MY_OPERATION, AUDIT_CAP_DENIED,
                           "missing capability for sys_myfunction");
        return -EPERM;
    }
    
    // Proceed with operation
    // ...
}
```

### Use syscall_check_capability Helper
```c
int64_t syscall_dispatch(uint64_t syscall_num, ...) {
    process_t* proc = process_get_current();
    
    if (!syscall_check_capability(syscall_num, proc->capabilities, args)) {
        return -EPERM;
    }
    
    return syscall_handlers[syscall_num](...);
}
```

---

## Capability Flags

```c
CAP_FLAG_INHERITABLE   0x01  // Child processes inherit
CAP_FLAG_DELEGATABLE   0x02  // Can be granted to others
CAP_FLAG_PERMANENT     0x04  // Cannot be revoked
CAP_FLAG_AUDIT         0x08  // Log all uses
CAP_FLAG_TIME_LIMITED  0x10  // Expires after timeout
```

### Example: Create Inheritable and Delegatable Capability
```c
capability_t* cap = capability_create_file(
    CAP_FILE_READ,
    "/tmp/*",
    CAP_FLAG_INHERITABLE | CAP_FLAG_DELEGATABLE
);
```

---

## Error Codes

```c
CAP_SUCCESS      0   // Success
CAP_ERROR       -1   // Generic error
CAP_ENOMEM      -2   // Out of memory
CAP_EINVAL      -3   // Invalid argument
CAP_EPERM       -4   // Permission denied
CAP_ENOTFOUND   -5   // Capability not found
CAP_EDUP        -6   // Duplicate capability
```

---

## Pattern Matching

### File Path Patterns
- `/home/user/*` - Matches all files in directory (recursive)
- `/tmp/*.txt` - Matches only .txt files in /tmp
- `/etc/passwd` - Matches specific file only

**Wildcard Rules:**
- `*` matches any sequence of characters
- No other wildcards supported (yet)
- Patterns are matched from start to end

### Network Host Patterns
- `*.example.com` - Matches any subdomain
- `www.example.com` - Matches exact hostname only
- `*` - Matches any host

**Port Ranges:**
- Single port: `port_min = 80, port_max = 0`
- Port range: `port_min = 80, port_max = 443`
- Any port: `port_min = 0, port_max = 0`

---

## Best Practices

### 1. Principle of Least Privilege
Grant only the minimum capabilities required:
```c
// Good: Specific path pattern
capability_create_file(CAP_FILE_READ, "/home/user/docs/*", ...)

// Bad: Overly broad pattern
capability_create_file(CAP_FILE_READ, "/*", ...)
```

### 2. Use Inheritance Wisely
Mark capabilities inheritable only if child processes need them:
```c
// Inheritable: Child processes need this
capability_create_file(CAP_FILE_READ, "/tmp/*", CAP_FLAG_INHERITABLE)

// Non-inheritable: Privileged operation, don't pass to children
capability_create_simple(CAP_SYS_ADMIN, 0)
```

### 3. Delegation Control
Only mark capabilities as delegatable if necessary:
```c
// Delegatable: Process may grant to services it spawns
capability_create_net(CAP_NET_CONNECT, "*", 80, 443,
                     CAP_FLAG_DELEGATABLE)

// Non-delegatable: Sensitive capability
capability_create_simple(CAP_SYS_MODULE, 0)
```

### 4. Check Capabilities Early
Check capabilities at syscall entry, before any work:
```c
int64_t sys_open(const char* path, int flags, ...) {
    // Check capability FIRST
    if (!capability_check_file(current->capabilities, path, 
        flags & O_WRONLY ? CAP_FILE_WRITE : CAP_FILE_READ)) {
        return -EPERM;
    }
    
    // Now proceed with open operation
    // ...
}
```

### 5. Audit Security-Sensitive Operations
Log capability denials for security monitoring:
```c
if (!capability_has(proc->capabilities, CAP_SYS_ADMIN)) {
    audit_log_capability(proc, CAP_SYS_ADMIN, AUDIT_CAP_DENIED,
                        "attempted unauthorized system administration");
    return -EPERM;
}
```

---

## Common Pitfalls

### 1. Forgetting to Check Capabilities
```c
// WRONG: No capability check
int64_t sys_dangerous_operation() {
    do_dangerous_thing();  // Anyone can call this!
}

// CORRECT: Check capability first
int64_t sys_dangerous_operation() {
    if (!capability_has(current->capabilities, CAP_SYS_ADMIN)) {
        return -EPERM;
    }
    do_dangerous_thing();
}
```

### 2. Using Wrong Capability Type
```c
// WRONG: Using CAP_FILE_READ for write operation
if (capability_has(caps, CAP_FILE_READ)) {
    write_file();  // Should check CAP_FILE_WRITE!
}

// CORRECT:
if (capability_has(caps, CAP_FILE_WRITE)) {
    write_file();
}
```

### 3. Not Freeing Capabilities
```c
// WRONG: Leaks memory
capability_t* cap = capability_create_simple(CAP_FILE_READ, 0);
// ... use capability ...
// (never freed)

// CORRECT: Free when done
capability_t* cap = capability_create_simple(CAP_FILE_READ, 0);
// ... use capability ...
capability_destroy(cap);
```

### 4. Overly Permissive Patterns
```c
// WRONG: Grants access to everything
capability_create_file(CAP_FILE_READ, "/*", ...)

// CORRECT: Limit to necessary directories
capability_create_file(CAP_FILE_READ, "/home/user/*", ...)
```

---

## Debugging

### Enable Capability Debug Logging
Capability system logs all operations with `[CAP]` prefix:
```
[CAP] Added capability type 1 (count: 1)
[CAP] File access granted: /home/user/file.txt matches pattern /home/user/*
[CAP] File access denied: /etc/passwd (capability type 1 not found)
```

### Check Process Capabilities
```c
void debug_print_capabilities(process_t* proc) {
    kprintf("Process %u (%s) capabilities:\n", proc->pid, proc->name);
    kprintf("  Count: %u\n", proc->capabilities->count);
    kprintf("  Bitmask: 0x%llx\n", proc->capabilities->bitmask);
    kprintf("  Generation: %u\n", proc->capabilities->generation);
    
    capability_t* cap = proc->capabilities->head;
    while (cap) {
        kprintf("  - Type %d, flags 0x%llx\n", cap->type, cap->flags);
        cap = cap->next;
    }
}
```

### Common Error Messages
- `"Failed to create capability set"` - Out of memory
- `"Should reject duplicate capability"` - Attempting to add same capability twice
- `"File access denied"` - Process lacks required file capability
- `"Process does not have capability to grant"` - Delegation attempted without delegatable flag

---

## Performance Tips

### 1. Use Bitmask for Simple Capabilities
Simple capabilities (types 0-63) use O(1) bitmask checking:
```c
// Fast: O(1) bitmask check
if (capability_has(caps, CAP_SYS_ADMIN)) { ... }
```

### 2. Minimize Pattern Complexity
Simple patterns are faster than complex wildcards:
```c
// Fast: Direct comparison
"/etc/passwd"

// Slower: Wildcard matching
"/home/*/docs/*.txt"
```

### 3. Cache Capability Checks
For frequently-checked paths, cache the result:
```c
// Cache capability check result in file descriptor
fd->can_read = capability_check_file(caps, path, CAP_FILE_READ);
fd->can_write = capability_check_file(caps, path, CAP_FILE_WRITE);

// Later: Use cached result
if (fd->can_read) {
    read_file(fd);
}
```

---

## Further Reading

- **Design Document**: `docs/phase2/CAPABILITY_SYSTEM_DESIGN.md`
- **Implementation Summary**: `docs/phase2/CAPABILITY_IMPLEMENTATION_SUMMARY.md`
- **Phase 2 Plan**: `docs/superpowers/plans/2026-05-26-phase2-security-isolation.md`
- **Unit Tests**: `tests/unit/test_capabilities.c`
- **Integration Tests**: `tests/integration/test_cap_enforcement.py`

---

**Last Updated:** 2026-05-26  
**Maintainer:** Security Systems Architect

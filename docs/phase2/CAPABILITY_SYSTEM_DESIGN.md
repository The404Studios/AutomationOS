# Capability System Design for AutomationOS Phase 2

**Document Version:** 1.0  
**Date:** 2026-05-26  
**Author:** Security Systems Architect  
**Status:** Design Complete

---

## Executive Summary

This document describes the design and implementation of a capability-based security system for AutomationOS Phase 2. The system enforces zero ambient authority, fine-grained permissions, and unforgeable capability tokens to provide robust security guarantees.

**Core Principles:**
- Zero Ambient Authority: No process has permissions by default
- Fine-Grained Control: Separate capabilities for read/write/execute operations
- Unforgeable: Kernel-managed tokens prevent forgery
- Delegatable: Processes can grant subsets of their capabilities
- Auditable: All capability checks are logged

---

## 1. Capability Types

The system supports five major capability categories with fine-grained subtypes:

### 1.1 File System Capabilities

| Capability | Value | Description |
|-----------|-------|-------------|
| `CAP_FILE_READ` | 1 | Read file contents |
| `CAP_FILE_WRITE` | 2 | Modify file contents |
| `CAP_FILE_EXECUTE` | 3 | Execute file as program |
| `CAP_FILE_CREATE` | 4 | Create new files/directories |
| `CAP_FILE_DELETE` | 5 | Delete files/directories |
| `CAP_FILE_CHOWN` | 6 | Change file ownership |
| `CAP_FILE_CHMOD` | 7 | Change file permissions |

**Path Pattern Matching:** File capabilities support glob patterns for flexible access control:
- `/home/user/*` - All files in user's home directory
- `/tmp/*.txt` - Only text files in /tmp
- `/var/log/app.log` - Specific file access

### 1.2 Network Capabilities

| Capability | Value | Description |
|-----------|-------|-------------|
| `CAP_NET_BIND` | 10 | Bind to network ports |
| `CAP_NET_CONNECT` | 11 | Connect to remote hosts |
| `CAP_NET_RAW` | 12 | Create raw sockets |
| `CAP_NET_LISTEN` | 13 | Listen for connections |

**Network Restrictions:** Network capabilities include host/port constraints:
- Specific host: `CAP_NET_CONNECT` to "api.example.com:443"
- Port range: `CAP_NET_BIND` to ports 8000-9000
- Wildcard: `CAP_NET_CONNECT` to "*:80" (any host, port 80)

### 1.3 Device Capabilities

| Capability | Value | Description |
|-----------|-------|-------------|
| `CAP_DEVICE_ACCESS` | 20 | Access specific device |
| `CAP_GPU` | 21 | GPU/graphics device access |
| `CAP_AUDIO` | 22 | Audio device access |
| `CAP_USB` | 23 | USB device access |
| `CAP_SERIAL` | 24 | Serial port access |

**Device Identification:** Devices identified by device ID or class:
- Specific device: `device_id = 0x1234`
- All devices of type: `device_id = 0xFFFFFFFF`

### 1.4 IPC Capabilities

| Capability | Value | Description |
|-----------|-------|-------------|
| `CAP_IPC` | 30 | Send IPC to specific process |
| `CAP_IPC_BROADCAST` | 31 | Send IPC to any process |
| `CAP_IPC_RECEIVE` | 32 | Receive IPC messages |
| `CAP_SHARED_MEM` | 33 | Create/access shared memory |

**IPC Targeting:** IPC capabilities can target specific processes:
- Specific PID: `target_pid = 42`
- Any process: `target_pid = 0` (requires `CAP_IPC_BROADCAST`)

### 1.5 System Capabilities

| Capability | Value | Description |
|-----------|-------|-------------|
| `CAP_SYS_ADMIN` | 40 | System administration |
| `CAP_SYS_MODULE` | 41 | Load kernel modules |
| `CAP_SYS_TIME` | 42 | Set system time |
| `CAP_SYS_BOOT` | 43 | Reboot system |
| `CAP_SYS_PTRACE` | 44 | Trace other processes |

### 1.6 Process Capabilities

| Capability | Value | Description |
|-----------|-------|-------------|
| `CAP_PROCESS_KILL` | 50 | Send signals to processes |
| `CAP_PROCESS_TRACE` | 51 | Debug/trace processes |
| `CAP_PROCESS_SETUID` | 52 | Change user ID |
| `CAP_PROCESS_SETGID` | 53 | Change group ID |
| `CAP_PROCESS_NICE` | 54 | Set process priority |

---

## 2. Capability Structure

### 2.1 Core Data Structures

```c
typedef enum {
    CAP_NONE = 0,
    // File capabilities (1-9)
    CAP_FILE_READ = 1,
    CAP_FILE_WRITE = 2,
    CAP_FILE_EXECUTE = 3,
    CAP_FILE_CREATE = 4,
    CAP_FILE_DELETE = 5,
    CAP_FILE_CHOWN = 6,
    CAP_FILE_CHMOD = 7,
    
    // Network capabilities (10-19)
    CAP_NET_BIND = 10,
    CAP_NET_CONNECT = 11,
    CAP_NET_RAW = 12,
    CAP_NET_LISTEN = 13,
    
    // Device capabilities (20-29)
    CAP_DEVICE_ACCESS = 20,
    CAP_GPU = 21,
    CAP_AUDIO = 22,
    CAP_USB = 23,
    CAP_SERIAL = 24,
    
    // IPC capabilities (30-39)
    CAP_IPC = 30,
    CAP_IPC_BROADCAST = 31,
    CAP_IPC_RECEIVE = 32,
    CAP_SHARED_MEM = 33,
    
    // System capabilities (40-49)
    CAP_SYS_ADMIN = 40,
    CAP_SYS_MODULE = 41,
    CAP_SYS_TIME = 42,
    CAP_SYS_BOOT = 43,
    CAP_SYS_PTRACE = 44,
    
    // Process capabilities (50-59)
    CAP_PROCESS_KILL = 50,
    CAP_PROCESS_TRACE = 51,
    CAP_PROCESS_SETUID = 52,
    CAP_PROCESS_SETGID = 53,
    CAP_PROCESS_NICE = 54,
    
    CAP_MAX = 64
} capability_type_t;
```

### 2.2 Capability Token Structure

Each capability is represented as a token with type-specific constraints:

```c
typedef struct capability {
    capability_type_t type;        // Capability type
    uint64_t flags;                // Flags (inheritable, delegatable, etc.)
    uint32_t ref_count;            // Reference counting
    
    // Type-specific data
    union {
        struct {
            char path_pattern[256]; // Glob pattern for file paths
        } file;
        
        struct {
            char host[256];         // Host pattern (e.g., "*.example.com")
            uint16_t port_min;      // Port range minimum
            uint16_t port_max;      // Port range maximum
        } net;
        
        struct {
            uint32_t device_id;     // Device ID or 0xFFFFFFFF for all
            char device_class[32];  // Device class name
        } device;
        
        struct {
            uint32_t target_pid;    // Target process or 0 for any
        } ipc;
    } data;
    
    struct capability* next;       // Linked list for capability set
} capability_t;
```

### 2.3 Capability Set Structure

Each process has a capability set stored in its PCB:

```c
typedef struct {
    capability_t* head;            // Linked list of capabilities
    uint32_t count;                // Number of capabilities
    uint64_t bitmask;              // Fast lookup for simple caps (types 0-63)
    uint32_t generation;           // Generation counter (for revocation)
} capability_set_t;
```

**Optimization Strategy:**
- **Bitmask**: Fast O(1) lookup for capabilities without constraints
- **Linked List**: Full scan for capabilities with path/host patterns
- **Generation Counter**: Efficient revocation without scanning all processes

---

## 3. Capability Management API

### 3.1 Initialization

```c
void capability_init(void);
```

Initialize the capability subsystem. Called during kernel boot.

### 3.2 Capability Set Operations

```c
capability_set_t* capability_set_create(void);
void capability_set_destroy(capability_set_t* set);
capability_set_t* capability_set_clone(capability_set_t* set);
```

Create, destroy, or clone capability sets. Used during process creation and fork.

### 3.3 Capability Modification

```c
int capability_add(capability_set_t* set, capability_t* cap);
int capability_remove(capability_set_t* set, capability_type_t type);
int capability_grant(process_t* granter, process_t* grantee, capability_t* cap);
int capability_revoke(process_t* proc, capability_type_t type);
```

Add, remove, grant, or revoke capabilities. Grant allows one process to give capabilities to another.

### 3.4 Capability Checking

```c
bool capability_has(capability_set_t* set, capability_type_t type);
bool capability_check_file(capability_set_t* set, const char* path, 
                          capability_type_t access);
bool capability_check_net(capability_set_t* set, const char* host, 
                         uint16_t port, capability_type_t access);
bool capability_check_device(capability_set_t* set, uint32_t device_id);
bool capability_check_ipc(capability_set_t* set, uint32_t target_pid);
```

Check if a capability set has the required permissions. Used at syscall entry points.

### 3.5 Inheritance and Delegation

```c
capability_set_t* capability_inherit(capability_set_t* parent, uint64_t inherit_mask);
bool capability_can_delegate(capability_t* cap);
int capability_restrict(capability_t* cap, const char* pattern);
```

- **Inheritance**: Child processes receive subset of parent's capabilities
- **Delegation**: Processes can grant their capabilities to others
- **Restriction**: Narrow capability scope (e.g., `/home/user/*` → `/home/user/docs/*`)

---

## 4. Integration with Process Management

### 4.1 Process Control Block Extension

The `process_t` structure is extended with capability-related fields:

```c
typedef struct process {
    uint32_t pid;
    uint32_t parent_pid;
    process_state_t state;
    cpu_context_t context;
    void* kernel_stack;
    void* user_stack;
    uint64_t time_slice;
    uint64_t total_time;
    struct process* next;
    char name[64];
    uint32_t ref_count;
    
    // NEW: Security fields
    capability_set_t* capabilities;    // Process capabilities
    uint32_t uid;                      // User ID (for future UID/GID support)
    uint32_t gid;                      // Group ID
    uint32_t sandbox_flags;            // Sandbox configuration
} process_t;
```

### 4.2 Process Creation with Capabilities

```c
process_t* process_create_with_caps(const char* name, void* entry_point, 
                                   capability_set_t* caps);
```

Create a process with a specific capability set. If `caps` is NULL, the process starts with no capabilities (zero ambient authority).

### 4.3 Fork and Exec

**Fork Behavior:**
1. Child inherits capabilities marked with `CAP_FLAG_INHERITABLE`
2. Default: File and IPC capabilities are inherited, system capabilities are not
3. Parent can customize inheritance mask

**Exec Behavior:**
1. Process capabilities are reset based on executable manifest
2. Only capabilities declared in manifest are granted
3. System capabilities require explicit administrator approval

---

## 5. Syscall Integration

### 5.1 Capability Checking at Syscall Entry

All syscalls perform capability checks before execution:

```c
int64_t syscall_dispatch(uint64_t syscall_num, uint64_t arg1, ...) {
    process_t* current = process_get_current();
    
    // Check if process has capability for this syscall
    if (!syscall_check_capability(syscall_num, current->capabilities, 
                                  (void*)arg1)) {
        audit_log_cap_violation(current, syscall_num);
        return EPERM;  // Permission denied
    }
    
    // Dispatch to handler
    return syscall_handlers[syscall_num](arg1, arg2, arg3, arg4, arg5, arg6);
}
```

### 5.2 Per-Syscall Capability Requirements

| Syscall | Required Capability | Additional Checks |
|---------|---------------------|-------------------|
| `sys_open` | `CAP_FILE_READ` or `CAP_FILE_WRITE` | Path pattern match |
| `sys_read` | `CAP_FILE_READ` | FD must have read permission |
| `sys_write` | `CAP_FILE_WRITE` | FD must have write permission |
| `sys_socket` | `CAP_NET_BIND` or `CAP_NET_CONNECT` | Host/port check |
| `sys_fork` | Inherited from parent | Inheritance mask |
| `sys_kill` | `CAP_PROCESS_KILL` | Target PID check |
| `sys_ioctl` | `CAP_DEVICE_ACCESS` | Device ID check |

### 5.3 New Capability Management Syscalls

```c
// SYS_CAP_GRANT (syscall #20) - Grant capability to another process
int64_t sys_cap_grant(uint64_t target_pid, uint64_t cap_type, 
                      uint64_t cap_data, ...);

// SYS_CAP_REVOKE (syscall #21) - Revoke capability from process
int64_t sys_cap_revoke(uint64_t target_pid, uint64_t cap_type, ...);

// SYS_CAP_CHECK (syscall #22) - Check if process has capability
int64_t sys_cap_check(uint64_t cap_type, uint64_t cap_data, ...);

// SYS_CAP_LIST (syscall #23) - List all capabilities of current process
int64_t sys_cap_list(uint64_t buf, uint64_t buf_size, ...);
```

---

## 6. Capability Revocation

### 6.1 Immediate Revocation

Capabilities are revoked immediately when:
1. Process terminates (all capabilities destroyed)
2. Administrator calls `sys_cap_revoke()`
3. Parent process revokes delegated capabilities

### 6.2 Generation-Based Revocation

To avoid scanning all processes during revocation:

1. Each capability has a `generation` field
2. Each process has a `capability_generation` counter
3. On revocation, increment global generation counter
4. Next capability check compares generations and updates if needed

```c
bool capability_check_file(capability_set_t* set, const char* path, 
                          capability_type_t access) {
    // Check if capability set is stale
    if (set->generation < global_capability_generation) {
        capability_set_refresh(set);  // Remove revoked capabilities
    }
    
    // Normal capability check
    // ...
}
```

### 6.3 Revocation Propagation

When a capability is revoked, all derived capabilities are also revoked:
- If process A grants capability to process B
- If process B grants (subset) to process C
- Revoking from A cascades to B and C

---

## 7. Capability Flags

Capabilities support the following flags:

| Flag | Value | Description |
|------|-------|-------------|
| `CAP_FLAG_INHERITABLE` | 0x01 | Child processes inherit this capability |
| `CAP_FLAG_DELEGATABLE` | 0x02 | Can be granted to other processes |
| `CAP_FLAG_PERMANENT` | 0x04 | Cannot be revoked (root init only) |
| `CAP_FLAG_AUDIT` | 0x08 | Log all uses of this capability |
| `CAP_FLAG_TIME_LIMITED` | 0x10 | Expires after timeout |

---

## 8. Auditing and Logging

### 8.1 Audit Events

The capability system logs the following events:

- **CAP_GRANTED**: Capability granted to process
- **CAP_REVOKED**: Capability revoked from process
- **CAP_DENIED**: Access denied due to missing capability
- **CAP_INHERITED**: Capability inherited by child process
- **CAP_DELEGATED**: Capability delegated to another process

### 8.2 Audit Log Format

```c
typedef struct {
    uint64_t timestamp;           // Nanoseconds since boot
    uint32_t pid;                 // Process ID
    capability_type_t cap_type;   // Capability type
    audit_event_t event;          // Event type
    char details[256];            // Event-specific details
} audit_entry_t;
```

### 8.3 Audit Log Interface

```c
void audit_log_init(void);
void audit_log_capability(process_t* proc, capability_type_t cap, 
                         audit_event_t event, const char* details);
int audit_query(uint64_t start_time, uint64_t end_time, 
                audit_entry_t* buf, size_t buf_size);
```

---

## 9. Performance Considerations

### 9.1 Fast Path Optimization

For capabilities without constraints (e.g., `CAP_SYS_ADMIN`):
- Use bitmask for O(1) lookup
- No string matching or pattern evaluation

### 9.2 Caching Strategy

Cache capability check results in file descriptors:
- When file is opened, cache the required capabilities
- Subsequent reads/writes skip path pattern matching
- Invalidate cache on capability revocation

### 9.3 Performance Targets

| Operation | Target Latency | Measured |
|-----------|----------------|----------|
| Simple capability check (bitmask) | < 10 ns | TBD |
| Path pattern capability check | < 500 ns | TBD |
| Network host/port check | < 200 ns | TBD |
| Capability grant/revoke | < 1 µs | TBD |
| Total syscall overhead | < 5% | TBD |

---

## 10. Security Properties

### 10.1 Formal Guarantees

The capability system provides the following security guarantees:

1. **Unforgeable**: Capabilities cannot be created or modified outside the kernel
2. **Zero Ambient Authority**: Processes start with no permissions
3. **Confinement**: Process can only grant subset of its own capabilities
4. **Least Privilege**: Fine-grained capabilities minimize attack surface
5. **Revocability**: Capabilities can be revoked at any time

### 10.2 Threat Model

**Threats Mitigated:**
- Unauthorized file access (e.g., reading `/etc/shadow`)
- Unauthorized network access (e.g., connecting to attacker C&C)
- Privilege escalation (e.g., gaining `CAP_SYS_ADMIN`)
- Process interference (e.g., killing unrelated processes)

**Threats Not Mitigated:**
- Covert channels (timing, cache)
- Kernel vulnerabilities (buffer overflows, use-after-free)
- Physical attacks (DMA, cold boot)

### 10.3 Attack Scenarios

**Scenario 1: Web Server Compromise**
- Web server has `CAP_FILE_READ(/var/www/*)` and `CAP_NET_BIND(*:80)`
- Attacker exploits web server vulnerability
- Attacker cannot read `/etc/passwd` (no capability)
- Attacker cannot connect to external C&C (no `CAP_NET_CONNECT`)

**Scenario 2: Malicious Child Process**
- Parent has `CAP_FILE_WRITE(/home/user/*)`
- Child inherits capabilities (including file write)
- Parent can revoke child's capabilities if misbehavior detected
- Other processes unaffected

---

## 11. Implementation Phases

### Phase 1: Core Capability System (Week 1)
- [ ] Implement capability data structures
- [ ] Implement capability set management
- [ ] Implement capability checking functions
- [ ] Write unit tests

### Phase 2: Process Integration (Week 1)
- [ ] Extend process_t with capabilities field
- [ ] Implement capability inheritance in fork
- [ ] Update process_create to initialize capabilities
- [ ] Test capability lifecycle

### Phase 3: Syscall Integration (Week 2)
- [ ] Update sys_open with capability checks
- [ ] Update sys_socket with capability checks
- [ ] Update sys_fork with capability inheritance
- [ ] Implement new capability management syscalls
- [ ] Write integration tests

### Phase 4: Advanced Features (Week 2)
- [ ] Implement capability delegation
- [ ] Implement revocation with generation counters
- [ ] Add audit logging
- [ ] Performance optimization
- [ ] Security testing

---

## 12. Testing Strategy

### 12.1 Unit Tests

- Test capability set create/destroy
- Test capability add/remove/check
- Test path pattern matching
- Test network host/port matching
- Test capability inheritance
- Test capability delegation
- Test revocation

### 12.2 Integration Tests

- Test process without capability cannot open file
- Test process with capability can open file
- Test child inherits parent capabilities
- Test revocation prevents access
- Test delegation allows capability transfer

### 12.3 Security Tests

- Test capability forgery attempts
- Test privilege escalation attempts
- Test sandbox escape attempts
- Fuzz capability syscalls
- Test against known exploit patterns

### 12.4 Performance Tests

- Measure syscall overhead with capability checks
- Benchmark capability check performance
- Measure fork latency with capability inheritance
- Profile capability-related CPU usage

---

## 13. Future Enhancements

### 13.1 Capability Manifests

Support declarative capability manifests for applications:

```json
{
  "name": "web-server",
  "capabilities": [
    {
      "type": "CAP_FILE_READ",
      "pattern": "/var/www/*"
    },
    {
      "type": "CAP_NET_BIND",
      "port": 80
    }
  ]
}
```

### 13.2 Capability Delegation Policies

Fine-grained control over delegation:
- Allow delegation to specific processes
- Limit delegation depth (prevent long chains)
- Time-limited delegation

### 13.3 AI-Assisted Capability Management

- Analyze application behavior to recommend capabilities
- Detect anomalous capability usage
- Suggest capability restrictions based on least privilege

### 13.4 Hardware-Backed Capabilities

- Use Intel SGX or ARM TrustZone for capability storage
- Prevent kernel compromise from bypassing capabilities

---

## 14. Conclusion

This capability system provides a robust foundation for AutomationOS security. By enforcing zero ambient authority and fine-grained permissions, the system minimizes the attack surface and prevents unauthorized access to system resources.

**Key Benefits:**
- Strong security guarantees (unforgeable, revocable)
- Fine-grained control (per-file, per-network, per-device)
- Performance optimizations (bitmask, caching)
- Auditable (all capability operations logged)
- Extensible (easy to add new capability types)

**Next Steps:**
1. Implement core capability system (this week)
2. Integrate with syscalls (next week)
3. Add auditing and delegation (following week)
4. Security testing and performance tuning

---

**Document Approval:**

- Design: Security Systems Architect (2026-05-26)
- Review: Kernel Team Lead (pending)
- Testing: QA Lead (pending)
- Security: Security Auditor (pending)

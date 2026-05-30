# Namespace Implementation - AutomationOS Phase 2

## Overview

This document describes the namespace isolation system implemented for AutomationOS Phase 2. Namespaces provide process isolation similar to Linux containers (Docker, LXC) and are a fundamental building block for secure sandboxing.

## What are Namespaces?

Namespaces partition kernel resources so that different sets of processes see different views of the system. This is the foundation of containerization technology.

### The 5 Namespace Types

1. **PID Namespace** - Process ID isolation
   - Each namespace has its own process ID space
   - Process 1 in a container is PID 1, but might be PID 157 on the host
   - Prevents containers from seeing or signaling host processes

2. **Mount Namespace** - Filesystem mount point isolation
   - Each namespace has its own view of the filesystem hierarchy
   - Mount/unmount operations don't affect other namespaces
   - Enables per-container filesystem layouts

3. **Network Namespace** - Network stack isolation
   - Each namespace has its own network devices, IP addresses, routing tables
   - Containers can have overlapping IP addresses
   - Enables network-level isolation and firewalling

4. **IPC Namespace** - Inter-process communication isolation
   - Isolates System V IPC objects (shared memory, semaphores, message queues)
   - Prevents containers from accessing each other's IPC objects
   - Different containers can use the same IPC keys safely

5. **UTS Namespace** - Hostname/domain name isolation
   - Each namespace can have its own hostname and domain name
   - Allows containers to set hostnames without affecting others
   - Simple but useful for identification

## Architecture

### File Structure

```
kernel/
├── include/
│   └── namespace.h              # Public namespace API and structures
├── security/
│   └── namespace.c              # Core namespace management
└── core/
    └── namespace/
        ├── ns_pid.c             # PID namespace implementation
        ├── ns_mount.c           # Mount namespace implementation
        ├── ns_net.c             # Network namespace implementation
        ├── ns_ipc.c             # IPC namespace implementation
        └── ns_uts.c             # UTS namespace implementation
```

### Key Data Structures

#### namespace_container_t
```c
typedef struct namespace_container {
    pid_namespace_t* pid_ns;      // Process ID namespace
    mount_namespace_t* mount_ns;  // Mount namespace
    net_namespace_t* net_ns;      // Network namespace
    ipc_namespace_t* ipc_ns;      // IPC namespace
    uts_namespace_t* uts_ns;      // UTS namespace
} namespace_container_t;
```

Each process has a `namespace_container_t` that points to one namespace of each type. Multiple processes can share namespaces (reference counting manages lifetime).

#### pid_namespace_t
```c
typedef struct pid_namespace {
    uint32_t id;                        // Unique namespace ID
    uint32_t next_pid;                  // Next PID to allocate
    struct process** process_table;     // Per-namespace process table
    uint32_t process_count;             // Number of processes
    struct pid_namespace* parent;       // Parent namespace (for nesting)
    uint32_t ref_count;                 // Reference count
    uint32_t level;                     // Nesting level (0 = root)
} pid_namespace_t;
```

PID namespaces support hierarchy - a child namespace can see its parent's processes (with translated PIDs), but not vice versa.

### Reference Counting

All namespaces use reference counting for lifetime management:

- When a process is created, it increments the ref_count of all its namespaces
- When a process exits, it decrements the ref_count
- When ref_count reaches 0, the namespace is destroyed
- This ensures namespaces persist as long as any process needs them

## API Functions

### Initialization

```c
void namespace_init(void);
```
Called once at boot to create root namespaces. All initial processes share root namespaces.

### Container Management

```c
namespace_container_t* namespace_create_container(uint32_t flags);
```
Creates a new container that initially shares root namespaces.

```c
void namespace_destroy_container(namespace_container_t* ns);
```
Decrements ref counts and frees the container. Namespaces are destroyed when their ref_count reaches 0.

```c
namespace_container_t* namespace_clone_container(
    namespace_container_t* parent,
    uint32_t flags
);
```
Clones a container for fork/clone. Flags control which namespaces are created vs shared:
- `CLONE_NEWPID` - Create new PID namespace
- `CLONE_NEWMOUNT` - Create new mount namespace
- `CLONE_NEWNET` - Create new network namespace
- `CLONE_NEWIPC` - Create new IPC namespace
- `CLONE_NEWUTS` - Create new UTS namespace

### PID Namespace Functions

```c
uint32_t pid_namespace_alloc_pid(pid_namespace_t* ns, process_t* proc);
```
Allocates a PID in the namespace and registers the process.

```c
void pid_namespace_free_pid(pid_namespace_t* ns, uint32_t pid);
```
Frees a PID when a process exits.

```c
process_t* pid_namespace_find_process(pid_namespace_t* ns, uint32_t pid);
```
Looks up a process by PID within a namespace.

```c
uint32_t pid_namespace_translate(
    pid_namespace_t* from,
    pid_namespace_t* to,
    uint32_t pid
);
```
Translates a PID from one namespace to another. Critical for container<->host communication.

### UTS Namespace Functions

```c
int uts_namespace_set_hostname(uts_namespace_t* ns, const char* hostname);
int uts_namespace_set_domainname(uts_namespace_t* ns, const char* domainname);
```
Set hostname/domain for a UTS namespace.

## Integration with Process Management

### process_t Structure

```c
typedef struct process {
    // ... existing fields ...
    namespace_container_t* namespaces;  // Process namespaces
    uint32_t sandbox_flags;             // Sandbox flags
} process_t;
```

Every process has a namespace container. When a process is created:
1. If it has a parent, it clones the parent's namespaces (based on flags)
2. If no parent (init process), it uses root namespaces

### Process Creation Flow

```c
process_t* proc = process_create("myprocess", entry_point);
// proc->namespaces now points to namespace container
// By default, shares parent's namespaces (fork semantics)
```

### Process Destruction Flow

```c
process_destroy(proc);
// Decrements ref_count on all namespaces
// Destroys namespaces if ref_count reaches 0
```

## Usage Examples

### Example 1: Create a Simple Container

```c
// Create a container with isolated PID, mount, and UTS namespaces
uint32_t flags = CLONE_NEWPID | CLONE_NEWMOUNT | CLONE_NEWUTS;
namespace_container_t* container = namespace_clone_container(
    current_process->namespaces,
    flags
);

// Create a process in the container
process_t* proc = process_create("container_init", container_main);
proc->namespaces = container;

// Set container hostname
uts_namespace_set_hostname(container->uts_ns, "my-container");
```

### Example 2: Nested Containers (Container in Container)

```c
// Create outer container
namespace_container_t* outer = namespace_clone_container(
    root_container,
    CLONE_NEWPID
);

// Create inner container inside outer
namespace_container_t* inner = namespace_clone_container(
    outer,
    CLONE_NEWPID
);

// inner->pid_ns->parent == outer->pid_ns
// inner->pid_ns->level == 2
```

### Example 3: Share Some Namespaces

```c
// Create container with isolated PID but shared network
uint32_t flags = CLONE_NEWPID;  // Only PID is new
namespace_container_t* container = namespace_clone_container(
    parent->namespaces,
    flags
);

// container->pid_ns is new
// container->net_ns == parent->net_ns (shared)
```

## PID Translation

One of the most complex aspects of PID namespaces is PID translation. A process with PID 1 in a container might be PID 157 on the host.

### Translation Rules

1. **Same namespace**: No translation needed
2. **Child to parent**: Process is visible with different PID
3. **Parent to child**: Only visible if process is in child namespace
4. **Sibling namespaces**: Translation not allowed (processes invisible)

### Example

```
Root namespace (host):
  PID 1: init
  PID 157: container_init

Container namespace:
  PID 1: container_init (same process as host PID 157)
  PID 2: container_app

Translation:
  Container PID 1 -> Host PID 157
  Container PID 2 -> Host PID 158
  Host PID 1 -> Container: N/A (not in container)
```

## Performance Considerations

### Design Goals

- **< 100 cycles overhead** for namespace operations during syscalls
- **Fast PID lookup** - O(1) process table access within namespace
- **Minimal memory overhead** - Only allocated namespaces consume memory
- **Reference counting** - Automatic cleanup, no memory leaks

### Optimizations

1. **Process table per namespace**: Direct array indexing for PID lookup
2. **Atomic ref counting**: Lock-free reference counting with atomics
3. **Lazy allocation**: Namespaces only created when explicitly requested
4. **Shared by default**: Fork shares namespaces unless CLONE_NEW* flags set

## Future Work

### TODO Items

1. **PID translation**: Complete implementation of `pid_namespace_translate()`
2. **Mount table integration**: Connect mount namespace to VFS subsystem
3. **Network stack integration**: Implement per-namespace network stacks
4. **IPC subsystem**: Implement System V IPC with namespace support
5. **setns() syscall**: Allow processes to enter existing namespaces
6. **unshare() syscall**: Allow processes to create new namespaces
7. **procfs namespace view**: `/proc` shows only processes in current namespace

### Linux Compatibility

For future Linux compatibility, we may add:
- **User namespaces** - UID/GID isolation for unprivileged containers
- **Cgroup namespaces** - Isolate cgroup views
- **Time namespaces** - Per-container system time (Linux 5.6+)

## Testing

### Unit Tests

Located in `tests/unit/test_namespace.c`:
- Namespace creation and destruction
- Reference counting
- PID allocation and lookup
- Namespace cloning with flags
- UTS hostname isolation
- Stress testing (10+ containers)

Run with:
```bash
make test-namespace
```

### Integration Tests

Located in `tests/integration/test_container_isolation.py`:
- PID namespace isolation
- Mount namespace isolation
- Network namespace isolation
- IPC namespace isolation
- UTS namespace isolation
- Nested containers
- PID translation
- setns/unshare syscalls

Run with:
```bash
python3 tests/integration/test_container_isolation.py
```

## Security Implications

### Isolation Guarantees

1. **Process isolation**: Processes in different PID namespaces cannot see or signal each other
2. **Filesystem isolation**: Mount changes in one namespace don't affect others
3. **Network isolation**: Network traffic is isolated by namespace (once network stack implemented)
4. **IPC isolation**: Shared memory and IPC objects cannot leak between namespaces
5. **Identity isolation**: Each container can have its own hostname

### Attack Surface

- **Namespace creation**: Only privileged processes (CAP_SYS_ADMIN) should create namespaces
- **setns()**: Joining namespaces should require appropriate capabilities
- **Nested limits**: Enforce max nesting depth to prevent resource exhaustion
- **Reference leaks**: Ensure namespaces are properly destroyed to avoid memory leaks

### Integration with Capabilities

Namespaces work alongside the capability system (Task 1):
- `CAP_SYS_ADMIN` required to create new namespaces
- `CAP_SYS_CHROOT` may be required for mount namespace operations
- `CAP_NET_ADMIN` required for network namespace configuration

## References

### Linux Documentation

- `man 7 namespaces` - Overview of Linux namespaces
- `man 2 clone` - Creating processes with new namespaces
- `man 2 unshare` - Disassociate parts of process execution context
- `man 2 setns` - Reassociate thread with a namespace

### Academic Papers

- "Namespaces in Operation" - Michael Kerrisk (LWN.net)
- "Containers from Scratch" - Liz Rice

### Related Code

- Linux kernel: `kernel/nsproxy.c`, `kernel/pid_namespace.c`
- Docker: Uses Linux namespaces for container isolation
- LXC: Lightweight containers using namespaces

## Implementation Checklist

- [x] Define namespace structures (`namespace.h`)
- [x] Implement core namespace management (`namespace.c`)
- [x] Implement PID namespace (`ns_pid.c`)
- [x] Implement mount namespace (`ns_mount.c`)
- [x] Implement network namespace (`ns_net.c`)
- [x] Implement IPC namespace (`ns_ipc.c`)
- [x] Implement UTS namespace (`ns_uts.c`)
- [x] Integrate with process structure
- [x] Update process creation/destruction
- [x] Write unit tests
- [x] Write integration tests
- [ ] Connect to VFS for mount namespace
- [ ] Connect to network stack
- [ ] Implement IPC subsystem
- [ ] Implement PID translation
- [ ] Add syscall support (clone, unshare, setns)
- [ ] Add procfs namespace filtering
- [ ] Performance testing and optimization

---

**Author**: Container Systems Engineer  
**Date**: 2026-05-26  
**Status**: Phase 2 Task 2 - Core implementation complete

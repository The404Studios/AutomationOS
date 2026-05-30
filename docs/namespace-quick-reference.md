# Namespace Quick Reference

## Common Operations

### Initialize Namespace System
```c
void kernel_main(void) {
    // ... other initialization ...
    namespace_init();  // Creates root namespaces
}
```

### Create a Container (Simple)
```c
// Create isolated PID and UTS namespaces, share everything else
uint32_t flags = CLONE_NEWPID | CLONE_NEWUTS;
namespace_container_t* container = namespace_clone_container(
    process_get_current()->namespaces,
    flags
);

// Set container hostname
uts_namespace_set_hostname(container->uts_ns, "my-container");

// Create process in container
process_t* proc = process_create("container_init", entry_point);
namespace_destroy_container(proc->namespaces);  // Free default
proc->namespaces = container;  // Use our container
container->pid_ns->ref_count++;  // We're using it now
```

### Create a Container (Full Isolation)
```c
// Isolate all 5 namespace types
uint32_t flags = CLONE_NEWPID | CLONE_NEWMOUNT | CLONE_NEWNET | 
                 CLONE_NEWIPC | CLONE_NEWUTS;
namespace_container_t* container = namespace_clone_container(
    root_container,
    flags
);
```

### Check Current Namespace
```c
process_t* current = process_get_current();
kprintf("My PID namespace ID: %d\n", current->namespaces->pid_ns->id);
kprintf("My hostname: %s\n", current->namespaces->uts_ns->hostname);
kprintf("Namespace level: %d\n", current->namespaces->pid_ns->level);
```

### Allocate PID in Namespace
```c
pid_namespace_t* ns = current->namespaces->pid_ns;
uint32_t pid = pid_namespace_alloc_pid(ns, proc);
// pid is namespace-local (might be 1 in container, 157 on host)
```

### Find Process in Namespace
```c
pid_namespace_t* ns = current->namespaces->pid_ns;
process_t* proc = pid_namespace_find_process(ns, pid);
if (proc) {
    kprintf("Found process: %s\n", proc->name);
}
```

### Translate PID Between Namespaces
```c
pid_namespace_t* container_ns = container->pid_ns;
pid_namespace_t* host_ns = root_container->pid_ns;

// Translate container PID 1 to host PID
uint32_t host_pid = pid_namespace_translate(container_ns, host_ns, 1);
// host_pid might be 157

// Translate host PID back to container
uint32_t container_pid = pid_namespace_translate(host_ns, container_ns, 157);
// container_pid should be 1
```

## Namespace Flags

### Individual Flags
```c
CLONE_NEWPID    // Create new PID namespace
CLONE_NEWMOUNT  // Create new mount namespace
CLONE_NEWNET    // Create new network namespace
CLONE_NEWIPC    // Create new IPC namespace
CLONE_NEWUTS    // Create new UTS namespace
```

### Common Combinations
```c
// Docker-like container (all isolated)
uint32_t docker_flags = CLONE_NEWPID | CLONE_NEWMOUNT | CLONE_NEWNET | 
                        CLONE_NEWIPC | CLONE_NEWUTS;

// Process isolation only
uint32_t pid_only = CLONE_NEWPID;

// Process + hostname isolation
uint32_t pid_uts = CLONE_NEWPID | CLONE_NEWUTS;

// Share everything (fork semantics)
uint32_t share_all = 0;
```

## Namespace Types

### PID Namespace
```c
typedef struct pid_namespace {
    uint32_t id;                    // Unique ID
    uint32_t next_pid;              // Next PID to allocate
    struct process** process_table; // Process array
    uint32_t process_count;         // Number of processes
    struct pid_namespace* parent;   // Parent namespace
    uint32_t level;                 // Nesting level (0=root)
    uint32_t ref_count;             // Reference count
} pid_namespace_t;

// Key functions
uint32_t pid_namespace_alloc_pid(pid_namespace_t* ns, process_t* proc);
void pid_namespace_free_pid(pid_namespace_t* ns, uint32_t pid);
process_t* pid_namespace_find_process(pid_namespace_t* ns, uint32_t pid);
```

### Mount Namespace
```c
typedef struct mount_namespace {
    uint32_t id;                    // Unique ID
    struct mount_table* mounts;     // Mount table (TODO: VFS integration)
    char root_path[256];            // Root directory
    uint32_t flags;                 // Mount flags
    uint32_t ref_count;             // Reference count
} mount_namespace_t;

// Key functions
mount_namespace_t* mount_namespace_create(void);
mount_namespace_t* mount_namespace_clone(mount_namespace_t* parent);
```

### Network Namespace
```c
typedef struct net_namespace {
    uint32_t id;                    // Unique ID
    struct network_stack* stack;    // Network stack (TODO: net integration)
    uint32_t ref_count;             // Reference count
} net_namespace_t;

// Key functions
net_namespace_t* net_namespace_create(void);
```

### IPC Namespace
```c
typedef struct ipc_namespace {
    uint32_t id;                    // Unique ID
    struct ipc_table* table;        // IPC objects (TODO: IPC integration)
    uint32_t ref_count;             // Reference count
} ipc_namespace_t;

// Key functions
ipc_namespace_t* ipc_namespace_create(void);
```

### UTS Namespace
```c
typedef struct uts_namespace {
    uint32_t id;                    // Unique ID
    char hostname[256];             // Hostname
    char domainname[256];           // Domain name
    uint32_t ref_count;             // Reference count
} uts_namespace_t;

// Key functions
int uts_namespace_set_hostname(uts_namespace_t* ns, const char* hostname);
int uts_namespace_set_domainname(uts_namespace_t* ns, const char* domainname);
```

## Common Patterns

### Pattern 1: Container with Custom Hostname
```c
namespace_container_t* container = namespace_clone_container(
    parent->namespaces,
    CLONE_NEWPID | CLONE_NEWUTS
);

uts_namespace_set_hostname(container->uts_ns, "web-1");
```

### Pattern 2: Nested Container
```c
// Create parent container
namespace_container_t* parent = namespace_clone_container(
    root_container,
    CLONE_NEWPID
);

// Create child container inside parent
namespace_container_t* child = namespace_clone_container(
    parent,
    CLONE_NEWPID
);

// Verify: child->pid_ns->parent == parent->pid_ns
```

### Pattern 3: Share Network, Isolate Everything Else
```c
// Isolate PID, mount, IPC, UTS but share network
uint32_t flags = CLONE_NEWPID | CLONE_NEWMOUNT | CLONE_NEWIPC | CLONE_NEWUTS;
namespace_container_t* container = namespace_clone_container(
    parent->namespaces,
    flags
);

// container->net_ns == parent->net_ns (shared)
```

### Pattern 4: Check if in Root Namespace
```c
process_t* current = process_get_current();
namespace_container_t* root = namespace_get_root();

if (current->namespaces->pid_ns == root->pid_ns) {
    kprintf("Running in root PID namespace\n");
} else {
    kprintf("Running in container (level %d)\n",
            current->namespaces->pid_ns->level);
}
```

## Error Handling

### Check for NULL Returns
```c
namespace_container_t* container = namespace_clone_container(parent, flags);
if (!container) {
    kprintf("Failed to create container\n");
    return -1;
}
```

### Verify PID Allocation
```c
uint32_t pid = pid_namespace_alloc_pid(ns, proc);
if (pid == 0) {
    kprintf("Failed to allocate PID (namespace full?)\n");
    return -1;
}
```

### Check Process Lookup
```c
process_t* proc = pid_namespace_find_process(ns, pid);
if (!proc) {
    kprintf("Process %d not found in namespace\n", pid);
    return -1;
}
```

## Debugging

### Print Namespace Info
```c
void print_namespace_info(namespace_container_t* ns) {
    kprintf("Namespace Container:\n");
    kprintf("  PID NS:   ID=%d, level=%d, parent=%d\n",
            ns->pid_ns->id, ns->pid_ns->level,
            ns->pid_ns->parent ? ns->pid_ns->parent->id : 0);
    kprintf("  Mount NS: ID=%d, ref_count=%d\n",
            ns->mount_ns->id, ns->mount_ns->ref_count);
    kprintf("  Net NS:   ID=%d, ref_count=%d\n",
            ns->net_ns->id, ns->net_ns->ref_count);
    kprintf("  IPC NS:   ID=%d, ref_count=%d\n",
            ns->ipc_ns->id, ns->ipc_ns->ref_count);
    kprintf("  UTS NS:   ID=%d, hostname=%s, ref_count=%d\n",
            ns->uts_ns->id, ns->uts_ns->hostname, ns->uts_ns->ref_count);
}
```

### Check Reference Counts
```c
// Before creating container
uint32_t initial_refs = root->pid_ns->ref_count;

// Create and destroy container
namespace_container_t* container = namespace_create_container(0);
namespace_destroy_container(container);

// After destruction
if (root->pid_ns->ref_count != initial_refs) {
    kprintf("WARNING: Reference count leak detected!\n");
}
```

## Performance Tips

1. **Share namespaces when possible** - Only create new namespaces when isolation is needed
2. **Use atomic operations** - Reference counting uses atomics for thread safety
3. **Batch namespace operations** - Create all namespaces at once rather than incrementally
4. **Limit nesting depth** - Deep nesting (>10 levels) may impact performance
5. **PID lookup is O(1)** - Direct array indexing, very fast

## Security Notes

1. **Capability checks** - Creating namespaces should require `CAP_SYS_ADMIN`
2. **Reference counting** - Prevents use-after-free vulnerabilities
3. **PID translation** - Ensures processes can't access PIDs outside their view
4. **Isolation verification** - Test that containers can't escape their namespaces

## TODO / Future Work

- [ ] Implement PID translation between nested namespaces
- [ ] Integrate mount namespace with VFS
- [ ] Integrate network namespace with network stack
- [ ] Implement IPC subsystem with namespace support
- [ ] Add `setns()` syscall for entering existing namespaces
- [ ] Add `unshare()` syscall for creating new namespaces
- [ ] Add procfs filtering by namespace
- [ ] Implement max nesting depth limit
- [ ] Add namespace lookup by ID for `setns()`

## Testing

### Unit Tests
```bash
make test-namespace
```

### Integration Tests
```bash
python3 tests/integration/test_container_isolation.py
```

### Run Demo
```bash
make namespace-demo
```

---

For more details, see `docs/namespace-implementation.md`

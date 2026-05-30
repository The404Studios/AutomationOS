# MAC Quick Start Guide

**Get started with AutomationOS Mandatory Access Control in 5 minutes**

---

## What is MAC?

MAC (Mandatory Access Control) is a security system that:
- **Controls access** to files, network, processes, and IPC
- **Cannot be bypassed** by applications or users
- **Logs all denials** for security auditing
- **Uses simple policies** that are easy to write and understand

---

## Quick Example

### Problem: Protect `/etc/shadow` from unprivileged users

**Without MAC**:
```c
// User process can attempt to read shadow file
int fd = open("/etc/shadow", O_RDONLY);  // Blocked by DAC (discretionary)
```

**With MAC**:
```c
// Policy explicitly denies access
deny user_t shadow_t:file { read write };

// Now even with DAC permissions, access is denied
int fd = open("/etc/shadow", O_RDONLY);  // Returns -EACCES
// Audit log: [MAC-AUDIT] DENIED: pid=123 domain=user_t path=/etc/shadow
```

---

## 3-Minute Tutorial

### Step 1: Create a Policy

Create `my_app.policy`:

```
# My application can access its data directory
allow my_app_t app_data_t:file { read write create delete };

# My application can connect to network
allow my_app_t unrestricted_port_t:socket { connect };

# My application CANNOT read system files
deny my_app_t etc_t:file { read };
deny my_app_t shadow_t:file { read };
```

### Step 2: Compile the Policy

```bash
$ python3 policy_compiler.py my_app.policy my_app.bin
Compiling policy: my_app.policy

Successfully parsed:
  4 rules
  0 transitions

Binary policy generated: 640 bytes
✓ Compilation successful!
```

### Step 3: Load the Policy

```c
// In kernel initialization
int fd = open("my_app.bin", O_RDONLY);
void* policy = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

int result = sys_mac_load_policy(policy, size);
if (result == MAC_SUCCESS) {
    printf("Policy loaded successfully\n");
}
```

### Step 4: Test Enforcement

```c
// Set process domain
security_label_t label;
strcpy(label.domain, "my_app_t");
label.type = LABEL_TYPE_USER;
label.level = MLS_LEVEL_UNCLASSIFIED;

sys_mac_set_label(getpid(), &label);

// Test file access
int fd1 = open("/app/data/config.txt", O_RDONLY);  // ✓ ALLOWED
int fd2 = open("/etc/shadow", O_RDONLY);            // ✗ DENIED (errno = EACCES)

// Check audit log
mac_audit_print_recent(10);
// [MAC-AUDIT] DENIED: pid=123 domain=my_app_t path=/etc/shadow perms=0x1
```

---

## Common Use Cases

### 1. Web Server Isolation

**Goal**: Web server can only access web content, cannot read user files

**Policy**:
```
allow web_t www_content_t:file { read };
allow web_t http_port_t:socket { bind listen };
deny web_t home_t:file { read };
deny web_t shadow_t:file { read };
```

**Result**:
- ✅ Serves web pages from `/var/www`
- ✅ Binds to port 80
- ❌ Cannot read `/home/user/.ssh/id_rsa`
- ❌ Cannot read `/etc/shadow`

### 2. Untrusted Application Sandbox

**Goal**: Untrusted app can only access `/tmp`, no network, no IPC

**Policy**:
```
allow untrusted_t tmp_t:file { read write create delete };
deny untrusted_t *:socket { connect bind };
deny untrusted_t *:process { signal };
```

**Result**:
- ✅ Can create files in `/tmp`
- ❌ Cannot access network
- ❌ Cannot signal other processes
- ❌ Cannot access `/etc` or `/home`

### 3. Database Server

**Goal**: Database accesses only its data directory and port 5432

**Policy**:
```
allow db_t db_data_t:file { read write create delete };
allow db_t db_port_t:socket { bind listen };
deny db_t *:file { execute };
```

**Result**:
- ✅ Full access to `/var/lib/postgres`
- ✅ Binds to port 5432
- ❌ Cannot execute binaries
- ❌ Cannot access user files

---

## Policy Language Cheat Sheet

### Rule Types

```
# Allow access
allow source_domain target_type:object_class { permissions };

# Deny access (overrides allow)
deny source_domain target_type:object_class { permissions };

# Domain transition on exec
transition source_domain target_type:path -> result_domain;
```

### Object Classes

| Class | Description | Example |
|-------|-------------|---------|
| `file` | Regular files | `/etc/config` |
| `dir` | Directories | `/home/user` |
| `socket` | Network sockets | TCP/UDP |
| `process` | Processes | Running programs |
| `device` | Device files | `/dev/sda` |
| `shm` | Shared memory | IPC |

### Permissions

| Class | Permissions |
|-------|-------------|
| `file` | `read`, `write`, `execute`, `create`, `delete`, `chmod`, `chown` |
| `socket` | `bind`, `connect`, `listen`, `accept`, `send`, `recv`, `raw` |
| `process` | `signal`, `ptrace`, `kill`, `fork`, `exec`, `transition` |

### Wildcards

```
# Match any domain or type
allow user_t *:file { read };          # Can read any file type
deny untrusted_t *:socket { connect }; # Cannot connect to any socket
```

---

## API Reference

### System Calls

```c
// Load compiled policy (requires privileged domain)
int sys_mac_load_policy(const void* policy_data, size_t size);

// Get process security label
int sys_mac_get_label(uint32_t pid, security_label_t* label);

// Set process security label (requires privileged domain)
int sys_mac_set_label(uint32_t pid, const security_label_t* label);

// Check if current process can access a file
int sys_mac_check_access(const char* path, uint32_t perms);

// Enable/disable enforcement mode
int sys_mac_set_enforcing(bool enforcing);

// Get MAC statistics
int sys_mac_get_stats(mac_stats_t* stats);
```

### Kernel Functions

```c
// Check file access
int mac_check_file_read(const process_t* proc, const char* path);
int mac_check_file_write(const process_t* proc, const char* path);
int mac_check_file_execute(const process_t* proc, const char* path);

// Check network access
int mac_check_net_bind(const process_t* proc, uint16_t port);
int mac_check_net_connect(const process_t* proc, const char* host, uint16_t port);

// Check process access
int mac_check_process_signal(const process_t* source, const process_t* target);
int mac_check_process_kill(const process_t* source, const process_t* target);
```

---

## Debugging Tips

### 1. Use Permissive Mode

```c
// Disable enforcement (logs denials but doesn't block)
sys_mac_set_enforcing(false);

// Run your application
// ...

// Check audit log for denials
mac_audit_print_summary();

// Fix policy based on denials
// ...

// Re-enable enforcement
sys_mac_set_enforcing(true);
```

### 2. Check Audit Log

```c
// Print recent denials
mac_audit_print_recent(20);

// Get audit statistics
mac_stats_t stats;
sys_mac_get_stats(&stats);
printf("Checks: %llu, Denied: %llu\n", stats.checks_total, stats.checks_denied);
```

### 3. Verify Domain Name

```c
// Check if domain name is valid
bool valid = mac_is_valid_domain("my_app_t");  // Must end with '_t'

// Check if domain is privileged
bool priv = mac_is_privileged_domain("kernel_t");  // kernel_t, init_t
```

---

## Best Practices

### DO ✅

- **Start with minimal permissions**, add only what's needed
- **Use explicit denies** for critical resources (shadow file, kernel memory)
- **Test in permissive mode** before enabling enforcement
- **Review audit logs** regularly for unexpected denials
- **Use descriptive domain names** (`web_server_t`, `db_worker_t`)

### DON'T ❌

- **Don't use wildcards everywhere** (defeats purpose of MAC)
- **Don't skip domain validation** (all domains must end with `_t`)
- **Don't ignore audit denials** (may indicate security issue)
- **Don't disable enforcement in production** (use permissive for testing only)
- **Don't grant excessive permissions** (follow least privilege)

---

## Example: Securing a Web Server

### Scenario

You have a web server that should:
- ✅ Read web content from `/var/www`
- ✅ Write logs to `/var/log/httpd`
- ✅ Bind to ports 80 and 443
- ❌ NOT access user home directories
- ❌ NOT read `/etc/shadow`
- ❌ NOT execute shells

### Solution

**1. Create policy** (`web_server.policy`):

```
# File access
allow web_t www_content_t:file { read };
allow web_t log_t:file { write append create };
allow web_t etc_t:file { read };  # For config files

# Network access
allow web_t http_port_t:socket { bind listen accept };

# Deny dangerous operations
deny web_t shadow_t:file { read write };
deny web_t home_t:file { read write };
deny web_t shell_exec_t:file { execute };

# Process operations
allow web_t web_t:process { fork signal };
deny web_t *:process { ptrace };
```

**2. Compile**:

```bash
python3 policy_compiler.py web_server.policy web_server.bin
```

**3. Load and set domain**:

```c
// Load policy (in init)
sys_mac_load_policy(policy_data, policy_size);

// Set web server domain
security_label_t label;
strcpy(label.domain, "web_t");
label.type = LABEL_TYPE_SYSTEM;
label.level = MLS_LEVEL_UNCLASSIFIED;
sys_mac_set_label(web_server_pid, &label);
```

**4. Test**:

```c
// Should succeed
int fd1 = open("/var/www/index.html", O_RDONLY);      // ✓
int sock = socket(AF_INET, SOCK_STREAM, 0);           // ✓
bind(sock, &addr, sizeof(addr));  // port 80          // ✓

// Should fail
int fd2 = open("/etc/shadow", O_RDONLY);              // ✗ EACCES
int fd3 = open("/home/user/.ssh/id_rsa", O_RDONLY);  // ✗ EACCES
execve("/bin/sh", argv, envp);                        // ✗ EACCES
```

---

## Testing Your Policy

### Unit Test Template

```c
#include "../../kernel/include/mac.h"
#include "../../kernel/include/kernel.h"

void test_my_policy(void) {
    // Load policy
    mac_init();
    // ... load your policy ...

    // Create test process
    process_t proc;
    proc.pid = 100;
    strcpy(proc.name, "test");

    // Test allowed access
    int result = mac_check_file_read(&proc, "/app/data/file.txt");
    ASSERT(result == MAC_SUCCESS);

    // Test denied access
    result = mac_check_file_read(&proc, "/etc/shadow");
    ASSERT(result == MAC_ERR_DENIED);

    kprintf("Test passed!\n");
}
```

---

## Troubleshooting

### Problem: Policy not loaded

**Symptoms**: All accesses allowed, no denials

**Solution**:
```c
// Check if MAC is initialized
if (!mac_is_enforcing()) {
    kprintf("MAC not enforcing!\n");
}

// Check policy version
uint64_t version = mac_policy_get_version();
kprintf("Policy version: %llu\n", version);
```

### Problem: Unexpected denials

**Symptoms**: Application fails with EACCES

**Solution**:
```c
// Enable permissive mode
sys_mac_set_enforcing(false);

// Run application
// ...

// Check audit log
mac_audit_print_recent(50);
// Look for DENIED entries

// Fix policy based on denials
// Add missing allow rules
```

### Problem: Domain name rejected

**Symptoms**: `mac_is_valid_domain()` returns false

**Solution**:
```c
// Domain names MUST end with "_t"
// ✗ "my_app" - invalid
// ✓ "my_app_t" - valid

// Use only alphanumeric and underscore
// ✗ "my-app_t" - invalid (hyphen)
// ✓ "my_app_123_t" - valid
```

---

## Next Steps

1. **Read the full reference**: `docs/security/mac-policy-reference.md`
2. **Study examples**: `userspace/security/policy-tools/examples/`
3. **Run tests**: `tests/unit/test_mac.c`, `tests/integration/test_mac_enforcement.py`
4. **Write your policy**: Start simple, add rules incrementally
5. **Test thoroughly**: Use permissive mode, check audit logs

---

## Resources

- **Full Documentation**: `docs/security/mac-policy-reference.md`
- **Implementation Details**: `kernel/security/mac/README.md`
- **Header File**: `kernel/include/mac.h`
- **Example Policies**: `userspace/security/policy-tools/examples/`
- **Policy Compiler**: `userspace/security/policy-tools/policy_compiler.py`

---

**Happy Securing! 🔒**

For questions or issues, refer to the full MAC Policy Reference or check the audit logs for hints about what's being denied.

# Seccomp Quick Reference Card

## Quick Start

### Load Predefined Profile
```c
#include "kernel/include/seccomp.h"
#include "kernel/include/sched.h"

process_t* proc = process_create("app", entry_point);
seccomp_load_profile(proc, SECCOMP_PROFILE_BROWSER);
```

### Create Custom Filter
```c
struct bpf_insn filter[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_READ, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
};

struct bpf_prog* prog = bpf_prog_create(filter, 7);
seccomp_install_filter(proc, prog, SECCOMP_MODE_FILTER, SECCOMP_FILTER_STRICT);
```

---

## Seccomp Modes

| Mode | Value | Use Case |
|------|-------|----------|
| `SECCOMP_MODE_DISABLED` | 0 | No filtering (default) |
| `SECCOMP_MODE_STRICT` | 1 | read/write/exit only |
| `SECCOMP_MODE_FILTER` | 2 | Custom BPF filter |

---

## Seccomp Actions

| Action | Value | Effect |
|--------|-------|--------|
| `SECCOMP_RET_KILL` | 0x00000000 | Terminate process |
| `SECCOMP_RET_TRAP` | 0x00030000 | Send SIGSYS |
| `SECCOMP_RET_ERRNO` | 0x00050000 | Return -errno |
| `SECCOMP_RET_TRACE` | 0x7ff00000 | Notify tracer |
| `SECCOMP_RET_LOG` | 0x7ffc0000 | Allow + log |
| `SECCOMP_RET_ALLOW` | 0x7fff0000 | Allow syscall |

---

## Predefined Profiles

### SECCOMP_PROFILE_STRICT
**Syscalls:** read, write, exit only  
**Use:** Pure computation

### SECCOMP_PROFILE_BROWSER
**Syscalls:** Network, files, GPU (no exec)  
**Use:** Web browsers

### SECCOMP_PROFILE_NETWORK
**Syscalls:** Network, files (no exec/fork)  
**Use:** Network services

### SECCOMP_PROFILE_UNTRUSTED
**Syscalls:** Minimal (stdin/stdout only)  
**Use:** Untrusted executables

---

## BPF Instructions

### Load Operations
```c
// Load syscall number
BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr)

// Load architecture
BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch)

// Load immediate value
BPF_STMT(BPF_LD | BPF_IMM, value)
```

### Jump Operations
```c
// Jump if equal
BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, value, jt, jf)

// Jump if greater than
BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, value, jt, jf)

// Jump if greater or equal
BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, value, jt, jf)
```

### Return Operations
```c
// Allow syscall
BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

// Kill process
BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL)

// Return errno (EPERM = 1)
BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1)
```

---

## Filter Template

```c
struct bpf_insn my_filter[] = {
    // 1. Check architecture
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_arch),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

    // 2. Load syscall number
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof_nr),

    // 3. Allow specific syscalls
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_READ, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_WRITE, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),

    // 4. Default action
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
};
```

---

## API Reference

### Installation
```c
int seccomp_install_filter(process_t* proc, struct bpf_prog* prog,
                          uint32_t mode, uint32_t flags);
```

### Profile Loading
```c
int seccomp_load_profile(process_t* proc, uint32_t profile_id);
```

### Enforcement
```c
uint32_t seccomp_check_syscall(process_t* proc, uint32_t syscall_nr,
                               uint64_t* args, uint64_t ip);
```

### BPF Program
```c
struct bpf_prog* bpf_prog_create(const struct bpf_insn* insns, uint32_t len);
void bpf_prog_destroy(struct bpf_prog* prog);
int bpf_prog_validate(const struct bpf_prog* prog);
```

### Audit
```c
void seccomp_audit_log(process_t* proc, uint32_t syscall_nr,
                      uint32_t action, uint64_t* args, uint64_t ip);
struct seccomp_audit_entry* seccomp_get_audit_log(uint32_t* count);
```

### Statistics
```c
void seccomp_get_stats(struct seccomp_stats* stats);
```

---

## seccomp_data Offsets

```c
#define offsetof_nr    0   // Syscall number
#define offsetof_arch  4   // Architecture
#define offsetof_ip    8   // Instruction pointer
#define offsetof_args  16  // Arguments array
```

---

## Syscall Numbers

| Syscall | Number |
|---------|--------|
| SYS_EXIT | 0 |
| SYS_FORK | 1 |
| SYS_READ | 2 |
| SYS_WRITE | 3 |
| SYS_OPEN | 4 |
| SYS_CLOSE | 5 |
| SYS_WAITPID | 6 |
| SYS_EXECVE | 7 |
| SYS_GETPID | 8 |
| SYS_SLEEP | 9 |

---

## Error Codes

| Error | Value |
|-------|-------|
| EPERM | 1 |
| ENOENT | 2 |
| ESRCH | 3 |
| EBADF | 9 |
| ENOMEM | 12 |
| EACCES | 13 |
| EINVAL | 22 |
| ENOSYS | 38 |
| ENETDOWN | 100 |

---

## Profile File Format

```profile
NAME: myapp
VERSION: 1.0
DESCRIPTION: My application sandbox

DEFAULT_ACTION: KILL
ARCH: x86_64

# Syscall rules
ALLOW sys_read
ALLOW sys_write
DENY sys_fork EPERM
KILL sys_execve

# Capabilities
CAP_REQUIRE CAP_FILE_READ
CAP_DENY CAP_SYS_ADMIN

# Resource limits
RLIMIT_CPU: 3600
RLIMIT_AS: 1073741824

# Namespace isolation
NS_PID: new
NS_MOUNT: new
```

---

## Compile Profile

```bash
# Binary bytecode
python tools/sandbox-compiler.py profile.profile -o filter.bpf

# C array
python tools/sandbox-compiler.py profile.profile --format=c -o filter.c

# Assembly listing
python tools/sandbox-compiler.py profile.profile --format=asm -o filter.asm

# Optimize + validate
python tools/sandbox-compiler.py profile.profile --optimize --validate
```

---

## Common Patterns

### Deny with errno
```c
BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_FORK, 0, 1),
BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
```

### Allow range of syscalls
```c
// Allow syscalls 10-20
BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 10, 0, 3),
BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, 20, 2, 0),
BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
```

### Multiple allowed syscalls
```c
#define ALLOW_SYSCALL(nr) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

ALLOW_SYSCALL(SYS_READ),
ALLOW_SYSCALL(SYS_WRITE),
ALLOW_SYSCALL(SYS_EXIT),
```

---

## Debugging

### Enable logging
```c
seccomp_install_filter(proc, prog, SECCOMP_MODE_FILTER, SECCOMP_FILTER_LOG);
```

### Retrieve audit log
```c
uint32_t count;
struct seccomp_audit_entry* log = seccomp_get_audit_log(&count);
for (uint32_t i = 0; i < count; i++) {
    kprintf("PID %u: syscall %u denied\n", log[i].pid, log[i].syscall_nr);
}
```

### Performance stats
```c
struct seccomp_stats stats;
seccomp_get_stats(&stats);
kprintf("Checks: %llu, Denied: %llu, Avg cycles: %llu\n",
        stats.total_checks, stats.denied, stats.avg_check_cycles);
```

---

## Best Practices

1. ✅ Always check architecture first
2. ✅ Use predefined profiles when possible
3. ✅ Test with escape test suite
4. ✅ Enable logging in production
5. ✅ Combine with capabilities + namespaces
6. ✅ Start restrictive, relax if needed
7. ❌ Don't skip validation
8. ❌ Don't allow exec in sandboxes
9. ❌ Don't trust user pointers
10. ❌ Don't create infinite loops

---

## Performance Targets

| Metric | Target |
|--------|--------|
| Filter check | < 50 cycles |
| Strict mode | < 10 cycles |
| Total overhead | < 3% |
| BPF instructions | < 4096 |

---

## Security Checklist

- [ ] Architecture check present
- [ ] Default action is KILL
- [ ] Filter validated before installation
- [ ] exec() denied in sandboxes
- [ ] Strict flag set (one-way)
- [ ] Capabilities configured
- [ ] Namespace isolation enabled
- [ ] Resource limits set
- [ ] Audit logging enabled
- [ ] Escape tests passing

---

**Last Updated:** 2026-05-26  
**Version:** 1.0

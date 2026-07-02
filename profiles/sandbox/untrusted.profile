# Sandbox Profile: Untrusted Executable
# Purpose: Maximum isolation for untrusted/user-provided code
# Security Level: Very High (minimal syscalls, heavy restrictions)

NAME: untrusted
VERSION: 1.0
DESCRIPTION: Maximum security sandbox for untrusted executables

DEFAULT_ACTION: KILL
ARCH: x86_64

# ==================================================
# Minimal File I/O (stdio only)
# ==================================================

# Allow only stdio (FDs 0, 1, 2)
ALLOW sys_read ARG0 <= 2
ALLOW sys_write ARG0 <= 2

# Deny all file operations
DENY sys_open EACCES
DENY sys_openat EACCES
DENY sys_close ARG0 > 2
DENY sys_creat EACCES

# ==================================================
# Process Management (exit only)
# ==================================================

ALLOW sys_exit
ALLOW sys_exit_group
ALLOW sys_getpid
ALLOW sys_gettid

# Deny everything else
KILL sys_fork
KILL sys_vfork
KILL sys_clone
KILL sys_execve
KILL sys_execveat
KILL sys_ptrace
KILL sys_kill
KILL sys_waitpid

# ==================================================
# Memory Management (limited)
# ==================================================

# Allow basic memory operations
ALLOW sys_mmap ARG3 & (PROT_READ | PROT_WRITE)
ALLOW sys_munmap
ALLOW sys_brk

# Deny executable mappings (prevent code injection)
DENY sys_mmap ARG3 & PROT_EXEC EACCES
DENY sys_mprotect ARG2 & PROT_EXEC EACCES

# ==================================================
# Network I/O (DENIED)
# ==================================================

DENY sys_socket ENETDOWN
DENY sys_connect ENETDOWN
DENY sys_bind EACCES
DENY sys_listen EACCES
DENY sys_accept EACCES

# ==================================================
# Threading (DENIED - single threaded only)
# ==================================================

DENY sys_clone ENOSYS
DENY sys_futex ENOSYS

# ==================================================
# Time (read-only)
# ==================================================

ALLOW sys_clock_gettime
ALLOW sys_gettimeofday

# Deny time modification
DENY sys_settimeofday EPERM
DENY sys_clock_settime EPERM

# ==================================================
# Signals (minimal)
# ==================================================

ALLOW sys_rt_sigaction
ALLOW sys_rt_sigprocmask
ALLOW sys_rt_sigreturn

# Deny signal sending
DENY sys_rt_sigqueueinfo EPERM
DENY sys_kill EPERM

# ==================================================
# Information (minimal)
# ==================================================

ALLOW sys_getpid
ALLOW sys_gettid
ALLOW sys_getuid
ALLOW sys_getgid

# Deny process enumeration
DENY sys_getppid ESRCH

# ==================================================
# Computation (ALLOWED)
# ==================================================

# Pure computation syscalls (if any)
# Most computation happens in userspace without syscalls

# ==================================================
# Everything Else (DENIED)
# ==================================================

KILL sys_init_module
KILL sys_finit_module
KILL sys_delete_module
KILL sys_reboot
KILL sys_kexec_load
KILL sys_mount
KILL sys_umount
KILL sys_pivot_root
KILL sys_chroot
KILL sys_ioperm
KILL sys_iopl
KILL sys_acct
KILL sys_syslog

# ==================================================
# Capability Requirements (NONE)
# ==================================================

# Untrusted code gets zero capabilities
CAP_DENY CAP_FILE_READ
CAP_DENY CAP_FILE_WRITE
CAP_DENY CAP_FILE_EXECUTE
CAP_DENY CAP_NET_BIND
CAP_DENY CAP_NET_CONNECT
CAP_DENY CAP_DEVICE_ACCESS
CAP_DENY CAP_IPC
CAP_DENY CAP_SYS_ADMIN
CAP_DENY CAP_SYS_MODULE
CAP_DENY CAP_PROCESS_KILL
CAP_DENY CAP_PROCESS_TRACE

# ==================================================
# Resource Limits (strict)
# ==================================================

RLIMIT_CPU: 60          # 1 minute CPU time
RLIMIT_AS: 134217728    # 128MB address space
RLIMIT_NPROC: 1         # Single process only
RLIMIT_NOFILE: 3        # Only stdin/stdout/stderr
RLIMIT_FSIZE: 0         # No file writes
RLIMIT_CORE: 0          # No core dumps

# ==================================================
# Namespace Isolation (full)
# ==================================================

NAMESPACE_ISOLATION: true
NS_PID: new
NS_NET: new             # Isolated network namespace (no network)
NS_MOUNT: new
NS_IPC: new
NS_UTS: new

NO_NEW_PRIVS: true
DROP_AMBIENT_CAPS: true
LOCK_PERSONALITY: true

# ==================================================
# Additional Restrictions
# ==================================================

# Disallow setuid/setgid executables
NO_SETUID: true

# Read-only root filesystem
READONLY_ROOT: true

# Temporary filesystem for /tmp (limited size)
TMPFS_SIZE: 10485760    # 10MB

# No device access
NO_DEVICES: true

# Audit all syscalls (for forensics)
AUDIT_ALL: true

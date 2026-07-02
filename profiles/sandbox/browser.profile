# Sandbox Profile: Web Browser
# Purpose: Isolate web browser rendering engine
# Security Level: Medium (allow network, files, GPU but no exec/ptrace)
#
# Format:
#   ALLOW syscall_name [arg_constraints]
#   DENY syscall_name [errno]
#   TRAP syscall_name
#   KILL syscall_name
#
# Capabilities: Combine with capability restrictions
#   CAP_REQUIRE capability_name
#   CAP_DENY capability_name

# Profile metadata
NAME: browser
VERSION: 1.0
DESCRIPTION: Web browser rendering engine sandbox

# Default action if no rule matches
DEFAULT_ACTION: KILL

# Architecture restriction
ARCH: x86_64

# ==================================================
# File I/O (restricted to specific directories)
# ==================================================

# Allow read/write but require capability checks
ALLOW sys_read
ALLOW sys_write
ALLOW sys_open
ALLOW sys_close
ALLOW sys_lseek
ALLOW sys_stat
ALLOW sys_fstat
ALLOW sys_access

# Deny dangerous file operations
DENY sys_unlink EPERM
DENY sys_rmdir EPERM
DENY sys_chmod EPERM
DENY sys_chown EPERM

# ==================================================
# Process Management
# ==================================================

# Allow exit
ALLOW sys_exit
ALLOW sys_exit_group

# Deny process creation and execution
DENY sys_fork EPERM
DENY sys_vfork EPERM
DENY sys_clone EPERM
KILL sys_execve
KILL sys_execveat

# Deny dangerous process operations
KILL sys_ptrace
DENY sys_kill EPERM

# ==================================================
# Memory Management
# ==================================================

# Allow memory allocation and management
ALLOW sys_mmap
ALLOW sys_munmap
ALLOW sys_mprotect
ALLOW sys_brk
ALLOW sys_mremap

# ==================================================
# Network I/O
# ==================================================

# Allow network operations (browser needs this)
ALLOW sys_socket
ALLOW sys_connect
ALLOW sys_bind
ALLOW sys_listen
ALLOW sys_accept
ALLOW sys_sendto
ALLOW sys_recvfrom
ALLOW sys_sendmsg
ALLOW sys_recvmsg
ALLOW sys_shutdown
ALLOW sys_getsockopt
ALLOW sys_setsockopt

# ==================================================
# Threading & Synchronization
# ==================================================

# Allow threading (modern browsers are multi-threaded)
ALLOW sys_clone ARG0 & CLONE_THREAD
ALLOW sys_futex
ALLOW sys_set_robust_list
ALLOW sys_get_robust_list

# ==================================================
# Time & Timers
# ==================================================

ALLOW sys_clock_gettime
ALLOW sys_gettimeofday
ALLOW sys_nanosleep
ALLOW sys_timer_create
ALLOW sys_timer_settime
ALLOW sys_timer_delete

# ==================================================
# Signals
# ==================================================

ALLOW sys_rt_sigaction
ALLOW sys_rt_sigprocmask
ALLOW sys_rt_sigreturn
ALLOW sys_sigaltstack

# ==================================================
# Information & Introspection
# ==================================================

ALLOW sys_getpid
ALLOW sys_gettid
ALLOW sys_getuid
ALLOW sys_getgid
ALLOW sys_getppid

# ==================================================
# Graphics & GPU (needed for rendering)
# ==================================================

# These would be custom syscalls or ioctls
# ALLOW sys_gpu_alloc
# ALLOW sys_gpu_submit
# ALLOW sys_framebuffer_blit

# ==================================================
# Denied Dangerous Operations
# ==================================================

# Module loading
KILL sys_init_module
KILL sys_finit_module
KILL sys_delete_module

# System configuration
KILL sys_reboot
KILL sys_syslog
KILL sys_kexec_load
DENY sys_settimeofday EPERM
DENY sys_mount EPERM
DENY sys_umount EPERM

# Kernel memory access
KILL sys_ioperm
KILL sys_iopl

# ==================================================
# Capability Requirements
# ==================================================

# Browser needs these capabilities
CAP_REQUIRE CAP_NET_CONNECT
CAP_REQUIRE CAP_FILE_READ
CAP_REQUIRE CAP_FILE_WRITE
CAP_REQUIRE CAP_GPU

# Browser must NOT have these
CAP_DENY CAP_SYS_ADMIN
CAP_DENY CAP_SYS_MODULE
CAP_DENY CAP_PROCESS_TRACE
CAP_DENY CAP_SYS_BOOT

# ==================================================
# Resource Limits
# ==================================================

RLIMIT_CPU: 3600        # 1 hour CPU time
RLIMIT_AS: 4294967296   # 4GB address space
RLIMIT_NPROC: 100       # Max 100 threads
RLIMIT_NOFILE: 1024     # Max 1024 file descriptors
RLIMIT_FSIZE: 1073741824  # Max 1GB file size

# ==================================================
# Namespace Isolation
# ==================================================

NAMESPACE_ISOLATION: true
NS_PID: new             # Create new PID namespace
NS_NET: shared          # Share network namespace (needs network access)
NS_MOUNT: new           # Private mount namespace
NS_IPC: new             # Private IPC namespace
NS_UTS: new             # Private hostname

# ==================================================
# Additional Security Features
# ==================================================

# No new privileges (can't gain capabilities)
NO_NEW_PRIVS: true

# Drop all ambient capabilities
DROP_AMBIENT_CAPS: true

# Restrict personality (prevent switching to 32-bit mode)
LOCK_PERSONALITY: true

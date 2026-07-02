# Sandbox Profile: Network Service
# Purpose: Isolate network-facing services (web servers, API servers)
# Security Level: Medium-High (network I/O allowed, no exec/ptrace)

NAME: network
VERSION: 1.0
DESCRIPTION: Network service sandbox (web servers, API servers)

DEFAULT_ACTION: KILL
ARCH: x86_64

# ==================================================
# File I/O (restricted)
# ==================================================

ALLOW sys_read
ALLOW sys_write
ALLOW sys_open
ALLOW sys_close
ALLOW sys_lseek
ALLOW sys_stat
ALLOW sys_fstat

# Deny file modifications
DENY sys_unlink EACCES
DENY sys_rename EACCES
DENY sys_mkdir EACCES
DENY sys_rmdir EACCES
DENY sys_chmod EACCES
DENY sys_chown EACCES

# ==================================================
# Process Management
# ==================================================

ALLOW sys_exit
ALLOW sys_exit_group
ALLOW sys_getpid
ALLOW sys_gettid

# Deny dangerous operations
KILL sys_execve
KILL sys_execveat
KILL sys_ptrace
DENY sys_fork ENOSYS
DENY sys_vfork ENOSYS
DENY sys_clone ENOSYS

# ==================================================
# Memory Management
# ==================================================

ALLOW sys_mmap
ALLOW sys_munmap
ALLOW sys_mprotect
ALLOW sys_brk

# ==================================================
# Network I/O (full access)
# ==================================================

ALLOW sys_socket
ALLOW sys_connect
ALLOW sys_bind
ALLOW sys_listen
ALLOW sys_accept
ALLOW sys_accept4
ALLOW sys_sendto
ALLOW sys_recvfrom
ALLOW sys_sendmsg
ALLOW sys_recvmsg
ALLOW sys_shutdown
ALLOW sys_getsockname
ALLOW sys_getpeername
ALLOW sys_getsockopt
ALLOW sys_setsockopt
ALLOW sys_poll
ALLOW sys_epoll_create
ALLOW sys_epoll_ctl
ALLOW sys_epoll_wait

# ==================================================
# Threading
# ==================================================

ALLOW sys_clone ARG0 & CLONE_THREAD
ALLOW sys_futex
ALLOW sys_set_robust_list

# ==================================================
# Time
# ==================================================

ALLOW sys_clock_gettime
ALLOW sys_gettimeofday
ALLOW sys_nanosleep

# ==================================================
# Signals
# ==================================================

ALLOW sys_rt_sigaction
ALLOW sys_rt_sigprocmask
ALLOW sys_rt_sigreturn
ALLOW sys_sigaltstack

# ==================================================
# Denied Operations
# ==================================================

KILL sys_init_module
KILL sys_finit_module
KILL sys_delete_module
KILL sys_reboot
KILL sys_kexec_load
DENY sys_mount EPERM
DENY sys_umount EPERM

# ==================================================
# Capability Requirements
# ==================================================

CAP_REQUIRE CAP_NET_BIND
CAP_REQUIRE CAP_NET_CONNECT
CAP_REQUIRE CAP_FILE_READ

CAP_DENY CAP_SYS_ADMIN
CAP_DENY CAP_SYS_MODULE
CAP_DENY CAP_PROCESS_TRACE

# ==================================================
# Resource Limits
# ==================================================

RLIMIT_CPU: 7200
RLIMIT_AS: 2147483648   # 2GB
RLIMIT_NPROC: 50
RLIMIT_NOFILE: 4096     # Network services need more FDs
RLIMIT_FSIZE: 536870912 # 512MB

# ==================================================
# Namespace Isolation
# ==================================================

NAMESPACE_ISOLATION: true
NS_PID: new
NS_NET: shared          # Needs network access
NS_MOUNT: new
NS_IPC: new
NS_UTS: new

NO_NEW_PRIVS: true
DROP_AMBIENT_CAPS: true

#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "errno.h"   /* canonical negative errno set (replaces the inline block below) */

// System call numbers
#define SYS_EXIT    0
#define SYS_FORK    1
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_WAITPID 6
#define SYS_EXECVE  7
#define SYS_GETPID  8
#define SYS_SLEEP   9     // Blocking sleep, argument is MILLISECONDS (1 tick = 1 ms)
#define SYS_SETRLIMIT 10  // Set resource limit
#define SYS_GETRLIMIT 11  // Get resource limit
#define SYS_GETRUSAGE 12  // Get resource usage
#define SYS_PRLIMIT   13  // Set limits for other process (privileged)
#define SYS_READ_EVENT 14 // Read input event
#define SYS_YIELD   15    // Yield CPU timeslice
#define SYS_SPAWN   16    // Spawn new process from ELF path
#define SYS_MAP_FILE 17   // Map initrd file into userspace (zero-copy)
#define SYS_SHMGET  18    // Get shared memory segment
#define SYS_SHMAT   19    // Attach shared memory segment
#define SYS_SHMDT   20    // Detach shared memory segment
#define SYS_SHMCTL  21    // Shared memory control
#define SYS_MSGGET  22    // Get message queue
#define SYS_MSGSND  23    // Send message
#define SYS_MSGRCV  24    // Receive message
#define SYS_MSGCTL  25    // Message queue control
#define SYS_KILL    26    // Send signal to process
#define SYS_NICE    27    // Adjust process priority
#define SYS_GETPRIORITY 28 // Get process priority
#define SYS_SETPRIORITY 29 // Set process priority
#define SYS_OPENDIR 30    // Open directory for reading
#define SYS_READDIR 31    // Read directory entry
#define SYS_CLOSEDIR 32   // Close directory
#define SYS_STAT    33    // Get file status
#define SYS_UNLINK  34    // Delete file
#define SYS_RENAME  35    // Rename/move file
#define SYS_IOCTL   36    // I/O control (for PTY, terminals, etc.)
#define SYS_MMAP        37 // Anonymous memory mapping (large buffers)
#define SYS_MUNMAP      38 // Unmap anonymous memory
#define SYS_FB_ACQUIRE  39 // Map framebuffer into caller, return geometry
#define SYS_GET_TICKS_MS 40 // Monotonic milliseconds since boot

// ---- Canonical extended syscall map (integrator-assigned, collision-free) ----
// Staged kernel features land on these numbers. Userspace apps that proposed a
// different number must be rebuilt to match these before their feature works.
#define SYS_TIME        41 // RTC: seconds since Unix epoch (rtc_unix_time)
#define SYS_GETTIME     42 // RTC: fill user rtc_time_t* with broken-down time
#define SYS_RANDOM      43 // CSPRNG bytes        (kernel/drivers/rng.c)
#define SYS_PROCLIST    44 // enumerate processes (sysmon)
#define SYS_BEEP        45 // PC speaker / HDA tone
#define SYS_POWEROFF    46 // ACPI S5 power off
#define SYS_REBOOT      47 // reboot
#define SYS_SECCOMP     48 // install seccomp filter
#define SYS_BLK_READ    49 // block device read
#define SYS_BLK_WRITE   50 // block device write
#define SYS_SOCKET      51 // BSD-ish sockets (kernel/net/socket.c)
#define SYS_CONNECT     52
#define SYS_SEND        53
#define SYS_RECV        54
#define SYS_CLOSE_SK    55 // distinct from SYS_CLOSE (5)
#define SYS_SENDTO      56
#define SYS_RECVFROM    57
#define SYS_SOCK_POLL   58
#define SYS_BIND        76 // bind socket to local port/addr
#define SYS_LISTEN      77 // mark socket as passive (server)
#define SYS_ACCEPT      78 // accept incoming connection
#define SYS_THREAD_CREATE 79 // create a thread sharing the caller's address space
#define SYS_THREAD_EXIT   80 // terminate the calling thread, store retval, wake joiner
#define SYS_THREAD_JOIN   81 // block until target thread exits; copy out its retval
#define SYS_NET_INFO    59 // query IP/MAC/link state
#define SYS_PROC_QUERY  60 // procapi: rich per-process detail
#define SYS_PROC_CTL    61 // procapi: suspend/resume/kill/setprio
#define SYS_SYSINFO     62 // procapi: system memory/uptime/proc-count
#define SYS_CLIP_SET    63 // clipboard write
#define SYS_CLIP_GET    64 // clipboard read
#define SYS_NOTIFY      65 // post desktop notification (title\0body\0)
#define SYS_NOTIFY_POLL 66 // dequeue one notification into user buffer
#define SYS_MKDIR       67 // create directory (recursive)            (handlers.c)
#define SYS_NET_SEND    68 // transmit one raw Ethernet frame   (kernel/net/netsyscall.c)
#define SYS_NET_RECV    69 // poll one received Ethernet frame   (kernel/net/netsyscall.c)
#define SYS_FUTEX       70 // fast userspace mutex (wait/wake)
#define SYS_SENDFILE    71 // zero-copy file-to-socket transfer (kernel/core/syscall/sendfile.c)
#define SYS_PERF_REPORT 72 // print performance monitoring statistics
#define SYS_EPOLL_CREATE 73 // create epoll instance            (kernel/core/syscall/epoll.c)
#define SYS_EPOLL_CTL    74 // add/modify/remove watched fds     (kernel/core/syscall/epoll.c)
#define SYS_EPOLL_WAIT   75 // block until events ready          (kernel/core/syscall/epoll.c)
#define SYS_BATCH_SUBMIT 82 // batch syscall submission (io_uring-style)
#define SYS_VMA_TEST    200 // VMA red-black tree testing and benchmarking

// ---- SMP coprocessor offload (GATED: only registered under SMP_FOUNDATION) ----
// SYS_CPU1_OFFLOAD lets a userspace process hand a kernel-owned compute job (today
// an integer matrix multiply) to CPU1, the TRUSTED coprocessor. The number is a
// FIXED, currently-unused slot (83) chosen so it shifts NO existing syscall number
// and does NOT change MAX_SYSCALLS — the DEFAULT kernel (SMP_FOUNDATION undefined)
// never registers a handler at this index, so the binary is byte-for-byte
// unchanged and the syscall returns ENOTSUP (the app then prints SKIP). It is
// registered + handled ONLY inside #ifdef SMP_FOUNDATION in syscall.c / handlers.c.
#define SYS_CPU1_OFFLOAD 83 // offload a kernel matmul to CPU1 (trusted coprocessor)

// Job-type selector for SYS_CPU1_OFFLOAD's first argument.
#define CPU1_JOB_MATMUL  1  // user_arg = { int32 n; int32 A[n*n]; int32 B[n*n]; }

#define MAX_SYSCALLS 256

// Framebuffer info returned by SYS_FB_ACQUIRE (mapped into the caller).
typedef struct {
    uint64_t vaddr;   // user virtual address the framebuffer is mapped at
    uint32_t width;
    uint32_t height;
    uint32_t pitch;   // bytes per row
    uint32_t bpp;     // bits per pixel
} fb_acquire_t;

// Buffer size limits
#define MAX_READ_SIZE   (1024 * 1024)  // 1MB
#define MAX_WRITE_SIZE  (1024 * 1024)  // 1MB
#define MAX_PATH_LEN    4096
#define MAX_FDS         1024

// Syscall handler function type
typedef int64_t (*syscall_handler_t)(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                      uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Syscall initialization and dispatcher
void syscall_init(void);
void syscall_msr_init(void);  // Initialize SYSCALL/SYSRET MSRs (x86_64 specific)
int64_t syscall_dispatch(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Syscall handlers
int64_t sys_exit(uint64_t status, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_fork(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_close(uint64_t fd, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_getpid(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_sleep(uint64_t ms, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Resource limit syscalls
int64_t sys_setrlimit(uint64_t resource, uint64_t rlim, uint64_t arg3,
                      uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_getrlimit(uint64_t resource, uint64_t rlim, uint64_t arg3,
                      uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_getrusage(uint64_t who, uint64_t usage, uint64_t arg3,
                      uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_prlimit(uint64_t pid, uint64_t resource, uint64_t new_limit,
                    uint64_t old_limit, uint64_t arg5, uint64_t arg6);
int64_t sys_waitpid(uint64_t pid, uint64_t status_ptr, uint64_t options,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_execve(uint64_t path, uint64_t argv, uint64_t envp,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_read_event(uint64_t event_ptr, uint64_t arg2, uint64_t arg3,
                       uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_yield(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_spawn(uint64_t path, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_map_file(uint64_t path, uint64_t out_addr, uint64_t out_size,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6);

// IPC syscalls
int64_t sys_shmget(uint64_t key, uint64_t size, uint64_t shmflg,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_shmat(uint64_t shmid, uint64_t shmaddr, uint64_t shmflg,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_shmdt(uint64_t shmaddr, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_shmctl(uint64_t shmid, uint64_t cmd, uint64_t buf,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_msgget(uint64_t key, uint64_t msgflg, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_msgsnd(uint64_t msqid, uint64_t msgp, uint64_t msgsz,
                   uint64_t msgflg, uint64_t arg5, uint64_t arg6);
int64_t sys_msgrcv(uint64_t msqid, uint64_t msgp, uint64_t msgsz,
                   uint64_t msgtyp, uint64_t msgflg, uint64_t arg6);
int64_t sys_msgctl(uint64_t msqid, uint64_t cmd, uint64_t buf,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Signal syscalls
int64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Priority syscalls
int64_t sys_nice(uint64_t pid, uint64_t increment, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_getpriority(uint64_t which, uint64_t who, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_setpriority(uint64_t which, uint64_t who, uint64_t prio,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6);

// I/O control syscall
int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t argp,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Directory syscalls
int64_t sys_opendir(uint64_t path, uint64_t arg2, uint64_t arg3,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_readdir(uint64_t dirfd, uint64_t entry, uint64_t arg3,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_closedir(uint64_t dirfd, uint64_t arg2, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_stat(uint64_t path, uint64_t buf, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_unlink(uint64_t path, uint64_t arg2, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_rename(uint64_t oldpath, uint64_t newpath, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Memory / framebuffer / time syscalls (M1 graphics platform)
int64_t sys_mmap(uint64_t hint, uint64_t len, uint64_t prot, uint64_t flags,
                 uint64_t arg5, uint64_t arg6);
int64_t sys_munmap(uint64_t addr, uint64_t len, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_fb_acquire(uint64_t out_info, uint64_t arg2, uint64_t arg3,
                       uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_get_ticks_ms(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                         uint64_t arg4, uint64_t arg5, uint64_t arg6);

// RTC / wall-clock syscalls
int64_t sys_time(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_gettime(uint64_t uptr, uint64_t arg2, uint64_t arg3,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Entropy syscall
int64_t sys_random(uint64_t buf, uint64_t len, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Process enumeration (task manager / dashboard)
int64_t sys_proclist(uint64_t ubuf, uint64_t max, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Process API (introspect + control), clipboard, notifications
int64_t sys_proc_query(uint64_t pid, uint64_t out, uint64_t arg3,
                       uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_proc_ctl(uint64_t pid, uint64_t verb, uint64_t arg,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_sysinfo(uint64_t out, uint64_t arg2, uint64_t arg3,
                    uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_clip_set(uint64_t user_buf, uint64_t len, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_clip_get(uint64_t user_buf, uint64_t max, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_notify_post(uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_notify_poll(uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6);

// Block device syscalls (AHCI/SATA disk; 512-byte sectors).
int64_t sys_blk_read(uint64_t lba, uint64_t count, uint64_t ubuf,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_blk_write(uint64_t lba, uint64_t count, uint64_t ubuf,
                      uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Futex (fast userspace mutex) syscall
int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                  uint64_t timeout, uint64_t uaddr2, uint64_t val3);

// Thread syscalls (real threads sharing the caller's address space).
//   sys_thread_create(entry, arg, user_stack) -> tid (== new thread's pid)
//   sys_thread_exit(retval)                    -> does not return
//   sys_thread_join(tid, retval_out)           -> 0 on success
int64_t sys_thread_create(uint64_t entry, uint64_t arg, uint64_t user_stack,
                          uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_thread_exit(uint64_t retval, uint64_t arg2, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_thread_join(uint64_t tid, uint64_t retval_out, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Zero-copy file-to-socket transfer
int64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd, uint64_t offset_ptr,
                     uint64_t count, uint64_t arg5, uint64_t arg6);

// Batched syscall interface (io_uring-style)
int64_t sys_batch_submit(uint64_t ring_ptr, uint64_t count, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6);

// VMA testing syscall (benchmarking red-black tree)
int64_t sys_vma_test(uint64_t req_ptr, uint64_t arg2, uint64_t arg3,
                     uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Epoll syscalls (event-driven I/O multiplexing)
int64_t sys_epoll_create(uint64_t size_hint, uint64_t arg2, uint64_t arg3,
                         uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd,
                      uint64_t event_ptr, uint64_t arg5, uint64_t arg6);
int64_t sys_epoll_wait(uint64_t epfd, uint64_t events_ptr, uint64_t maxevents,
                       uint64_t timeout_ms, uint64_t arg5, uint64_t arg6);

// Epoll initialization (called from syscall_init)
void epoll_init(void);

// Performance monitoring syscall
int64_t sys_perf_report(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6);

// SMP coprocessor offload syscall (defined ONLY under SMP_FOUNDATION in
// handlers.c; this prototype emits no code, so the default build is unaffected).
//   sys_cpu1_offload(job_type, user_arg, arg_len, user_res, res_len)
int64_t sys_cpu1_offload(uint64_t job_type, uint64_t user_arg, uint64_t arg_len,
                         uint64_t user_res, uint64_t res_len);

// Error codes now live in errno.h (included above) — canonical negative set.

#endif

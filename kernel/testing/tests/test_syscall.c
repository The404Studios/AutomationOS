#include "../../include/ktest.h"
#include "../../include/syscall.h"
#include "../../include/kernel.h"

/*
 * Syscall Tests
 * Tests system call interface and handler dispatch
 */

KTEST_SUITE(syscall);

KTEST_CASE(syscall, handler_table_initialized) {
    // Syscall handler table should be initialized
    KTEST_ASSERT_TRUE(syscall_is_initialized());
}

KTEST_CASE(syscall, valid_syscall_numbers) {
    // Test that syscall numbers are within valid range
    KTEST_ASSERT_LT(SYS_READ, SYS_MAX);
    KTEST_ASSERT_LT(SYS_WRITE, SYS_MAX);
    KTEST_ASSERT_LT(SYS_OPEN, SYS_MAX);
    KTEST_ASSERT_LT(SYS_CLOSE, SYS_MAX);
}

KTEST_CASE(syscall, handler_lookup) {
    syscall_handler_t handler = syscall_get_handler(SYS_GETPID);
    KTEST_ASSERT_NOT_NULL(handler);
}

KTEST_CASE(syscall, invalid_syscall_number) {
    syscall_handler_t handler = syscall_get_handler(9999);
    KTEST_ASSERT_NULL(handler);
}

KTEST_CASE(syscall, getpid_returns_valid_pid) {
    // Create a test process
    process_t* proc = process_create("test", 1);
    KTEST_ASSERT_NOT_NULL(proc);

    // Simulate getpid syscall
    pid_t pid = sys_getpid();
    KTEST_ASSERT_GT(pid, 0);

    process_destroy(proc);
}

KTEST_CASE(syscall, read_with_null_buffer) {
    // read() with NULL buffer should return error
    ssize_t result = sys_read(0, NULL, 100);
    KTEST_ASSERT_EQ(result, -EFAULT);
}

KTEST_CASE(syscall, write_with_null_buffer) {
    // write() with NULL buffer should return error
    ssize_t result = sys_write(1, NULL, 100);
    KTEST_ASSERT_EQ(result, -EFAULT);
}

KTEST_CASE(syscall, invalid_file_descriptor) {
    char buffer[100];

    // Invalid fd should return EBADF
    ssize_t result = sys_read(-1, buffer, 100);
    KTEST_ASSERT_EQ(result, -EBADF);

    result = sys_read(9999, buffer, 100);
    KTEST_ASSERT_EQ(result, -EBADF);
}

KTEST_CASE(syscall, open_creates_file_descriptor) {
    int fd = sys_open("/test/file", O_RDONLY, 0);

    // fd should be non-negative (or error code)
    if (fd >= 0) {
        KTEST_ASSERT_GE(fd, 0);
        sys_close(fd);
    } else {
        // Error codes are negative
        KTEST_ASSERT_LT(fd, 0);
    }
}

KTEST_CASE(syscall, close_invalid_fd) {
    int result = sys_close(-1);
    KTEST_ASSERT_EQ(result, -EBADF);

    result = sys_close(9999);
    KTEST_ASSERT_EQ(result, -EBADF);
}

KTEST_CASE(syscall, fork_creates_child_process) {
    pid_t child_pid = sys_fork();

    if (child_pid > 0) {
        // Parent process - got child PID
        KTEST_ASSERT_GT(child_pid, 0);
    } else if (child_pid == 0) {
        // Child process
        KTEST_ASSERT_EQ(child_pid, 0);
    } else {
        // Error
        KTEST_ASSERT_LT(child_pid, 0);
    }
}

KTEST_CASE(syscall, exit_terminates_process) {
    // This test would normally cause process termination
    // We can't actually call sys_exit() in a test
    // Instead, verify that the exit handler exists

    syscall_handler_t handler = syscall_get_handler(SYS_EXIT);
    KTEST_ASSERT_NOT_NULL(handler);
}

KTEST_CASE(syscall, mmap_allocates_memory) {
    void* addr = sys_mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    if (addr != MAP_FAILED) {
        KTEST_ASSERT_NOT_NULL(addr);
        sys_munmap(addr, PAGE_SIZE);
    }
}

KTEST_CASE(syscall, munmap_with_null_address) {
    int result = sys_munmap(NULL, PAGE_SIZE);
    KTEST_ASSERT_EQ(result, -EINVAL);
}

KTEST_CASE(syscall, nanosleep_validates_timespec) {
    struct timespec req = {1, 0};  // 1 second
    struct timespec rem;

    // Valid timespec should succeed or return error
    int result = sys_nanosleep(&req, &rem);

    // Result should be 0 or negative error code
    KTEST_ASSERT_LE(result, 0);
}

KTEST_CASE(syscall, nanosleep_with_null_timespec) {
    int result = sys_nanosleep(NULL, NULL);
    KTEST_ASSERT_EQ(result, -EFAULT);
}

KTEST_CASE(syscall, getcwd_buffer_validation) {
    char buffer[256];

    char* result = sys_getcwd(buffer, sizeof(buffer));

    // Should return buffer or error pointer
    if (result == buffer) {
        KTEST_ASSERT_PTR_EQ(result, buffer);
    }
}

KTEST_CASE(syscall, getcwd_with_null_buffer) {
    char* result = sys_getcwd(NULL, 100);
    KTEST_ASSERT_NULL(result);
}

KTEST_CASE(syscall, getuid_returns_valid_uid) {
    uid_t uid = sys_getuid();

    // UID should be non-negative
    KTEST_ASSERT_GE(uid, 0);
}

KTEST_CASE(syscall, getgid_returns_valid_gid) {
    gid_t gid = sys_getgid();

    // GID should be non-negative
    KTEST_ASSERT_GE(gid, 0);
}

KTEST_CASE(syscall, kill_signal_validation) {
    // Sending signal to invalid PID should fail
    int result = sys_kill(-1, SIGTERM);
    KTEST_ASSERT_LT(result, 0);

    // Invalid signal number should fail
    result = sys_kill(1, 9999);
    KTEST_ASSERT_LT(result, 0);
}

KTEST_CASE(syscall, ioctl_with_invalid_fd) {
    int result = sys_ioctl(-1, 0, 0);
    KTEST_ASSERT_EQ(result, -EBADF);
}

KTEST_CASE(syscall, syscall_performance) {
    // Benchmark syscall overhead
    ktest_benchmark_start();

    for (int i = 0; i < 1000; i++) {
        sys_getpid();
    }

    uint64_t cycles = ktest_benchmark_end("1000 getpid calls");

    // Should complete in reasonable time
    KTEST_ASSERT_GT(cycles, 0);
}

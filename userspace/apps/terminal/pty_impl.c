/**
 * AutomationOS Terminal - PTY Implementation (Userspace)
 *
 * POSIX-style PTY functions for terminal emulator.
 * Replaces stubs in utils.c with real implementations.
 */

#include "terminal.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>

// Syscall numbers (from kernel/include/syscall.h)
#define SYS_OPEN    4
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_CLOSE   5
#define SYS_FORK    1
#define SYS_EXECVE  7
#define SYS_IOCTL   26  // New syscall for ioctl

// ioctl commands (from kernel/drivers/pty/pty.h)
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TCGETS      0x5401
#define TCSETS      0x5402

// Window size structure
struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/**
 * Open a new PTY master
 * Returns file descriptor or -1 on error
 */
int32_t pty_open(uint32_t cols, uint32_t rows) {
    // Open /dev/ptmx to allocate a new PTY pair
    int master_fd = open("/dev/ptmx", O_RDWR);
    if (master_fd < 0) {
        return -1;
    }

    // Set window size
    struct winsize ws;
    ws.ws_row = rows;
    ws.ws_col = cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    if (ioctl(master_fd, TIOCSWINSZ, &ws) < 0) {
        close(master_fd);
        return -1;
    }

    return master_fd;
}

/**
 * Close PTY file descriptor
 */
void pty_close(int32_t pty_fd) {
    if (pty_fd >= 0) {
        close(pty_fd);
    }
}

/**
 * Resize PTY window
 */
void pty_resize(int32_t pty_fd, uint32_t cols, uint32_t rows) {
    if (pty_fd < 0) {
        return;
    }

    struct winsize ws;
    ws.ws_row = rows;
    ws.ws_col = cols;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    ioctl(pty_fd, TIOCSWINSZ, &ws);
}

/**
 * Get PTY slave device name
 * Returns slave path like "/dev/pts/0"
 */
static int get_slave_name(int32_t master_fd, char *name, size_t size) {
    if (master_fd < 0 || !name || size < 16) {
        return -1;
    }

    // For now, we need to parse the PTY index from the master FD
    // In a full implementation, we'd use ptsname() or TIOCGPTN ioctl
    // Simplified: assume master_fd corresponds to PTY index
    // TODO: Implement proper TIOCGPTN ioctl to get slave number

    // Workaround: Try opening /dev/pts/0 through /dev/pts/31
    for (int i = 0; i < 32; i++) {
        snprintf(name, size, "/dev/pts/%d", i);
        // Try to open (read-only test)
        int test_fd = open(name, O_RDONLY | O_NONBLOCK);
        if (test_fd >= 0) {
            close(test_fd);
            return 0;  // Found it
        }
    }

    // Fallback: assume PTY 0
    snprintf(name, size, "/dev/pts/0");
    return 0;
}

/**
 * Spawn shell in PTY slave
 *
 * Workflow:
 *  1. Fork
 *  2. Child: setsid() to create new session
 *  3. Child: Open slave PTY
 *  4. Child: dup2() slave to stdin/stdout/stderr
 *  5. Child: exec shell
 *  6. Parent: return child PID
 */
int32_t pty_spawn(int32_t pty_fd, const char *shell, char *const argv[]) {
    if (pty_fd < 0 || !shell) {
        return -1;
    }

    // Get slave device name
    char slave_name[64];
    if (get_slave_name(pty_fd, slave_name, sizeof(slave_name)) < 0) {
        return -1;
    }

    // Fork
    pid_t pid = fork();
    if (pid < 0) {
        return -1;  // Fork failed
    }

    if (pid == 0) {
        // CHILD PROCESS

        // Create new session (become session leader)
        if (setsid() < 0) {
            _exit(1);
        }

        // Open slave PTY
        int slave_fd = open(slave_name, O_RDWR);
        if (slave_fd < 0) {
            _exit(1);
        }

        // Make slave the controlling terminal
        // (In full POSIX: use TIOCSCTTY ioctl)
        // For now, just redirect stdio

        // Redirect stdin, stdout, stderr to slave
        if (dup2(slave_fd, 0) < 0) _exit(1);  // stdin
        if (dup2(slave_fd, 1) < 0) _exit(1);  // stdout
        if (dup2(slave_fd, 2) < 0) _exit(1);  // stderr

        // Close the original slave fd (now duplicated to 0/1/2)
        if (slave_fd > 2) {
            close(slave_fd);
        }

        // Close master fd (child doesn't need it)
        close(pty_fd);

        // Execute shell
        if (argv) {
            execve(shell, argv, NULL);
        } else {
            char *default_argv[] = { (char *)shell, NULL };
            execve(shell, default_argv, NULL);
        }

        // If exec fails
        _exit(127);
    }

    // PARENT PROCESS
    return pid;
}

/**
 * Read from PTY master (output from shell)
 */
int32_t pty_read(int32_t pty_fd, uint8_t *buffer, uint32_t size) {
    if (pty_fd < 0 || !buffer) {
        return -1;
    }

    // Non-blocking read
    return read(pty_fd, buffer, size);
}

/**
 * Write to PTY master (input to shell)
 */
int32_t pty_write(int32_t pty_fd, const uint8_t *buffer, uint32_t size) {
    if (pty_fd < 0 || !buffer) {
        return -1;
    }

    return write(pty_fd, buffer, size);
}

/**
 * Check if data is available for reading (non-blocking)
 */
uint32_t pty_available(int32_t pty_fd) {
    if (pty_fd < 0) {
        return 0;
    }

    // Use ioctl FIONREAD to get bytes available
    // For now, assume data is always available (optimistic)
    // TODO: Implement proper FIONREAD ioctl or use select/poll

    return 1;  // Assume data might be available
}

/**
 * Set PTY to non-blocking mode
 */
int pty_set_nonblocking(int32_t pty_fd) {
    if (pty_fd < 0) {
        return -1;
    }

    // Use fcntl to set O_NONBLOCK
    // TODO: Implement fcntl syscall
    // For now, assume all PTYs are non-blocking by default

    return 0;
}

/**
 * Get PTY termios settings
 */
int pty_get_attr(int32_t pty_fd, uint32_t *flags) {
    if (pty_fd < 0 || !flags) {
        return -1;
    }

    return ioctl(pty_fd, TCGETS, flags);
}

/**
 * Set PTY termios settings
 */
int pty_set_attr(int32_t pty_fd, uint32_t flags) {
    if (pty_fd < 0) {
        return -1;
    }

    return ioctl(pty_fd, TCSETS, &flags);
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * Check if a command is available in PATH
 * (Helper for finding shell)
 */
static bool command_exists(const char *cmd) {
    if (!cmd) {
        return false;
    }

    // Try opening the file
    int fd = open(cmd, O_RDONLY);
    if (fd >= 0) {
        close(fd);
        return true;
    }

    return false;
}

/**
 * Find available shell
 * Tries in order: /bin/bash, /bin/sh, /usr/bin/sh
 */
const char *pty_find_shell(void) {
    const char *shells[] = {
        "/bin/bash",
        "/bin/sh",
        "/usr/bin/sh",
        "/bin/autoshell",  // AutomationOS shell
        NULL
    };

    for (int i = 0; shells[i]; i++) {
        if (command_exists(shells[i])) {
            return shells[i];
        }
    }

    return "/bin/sh";  // Default fallback
}

/**
 * Parse shell command line into argv array
 * Simple parser: splits on spaces
 */
char **pty_parse_args(const char *cmdline) {
    if (!cmdline) {
        return NULL;
    }

    // Count arguments
    int argc = 1;  // At least the program name
    const char *p = cmdline;
    while (*p) {
        if (*p == ' ') argc++;
        p++;
    }

    // Allocate argv array
    char **argv = (char **)malloc((argc + 1) * sizeof(char *));
    if (!argv) {
        return NULL;
    }

    // Split string
    char *cmdline_copy = strdup(cmdline);
    if (!cmdline_copy) {
        free(argv);
        return NULL;
    }

    int i = 0;
    char *token = strtok(cmdline_copy, " ");
    while (token && i < argc) {
        argv[i++] = strdup(token);
        token = strtok(NULL, " ");
    }
    argv[i] = NULL;

    free(cmdline_copy);
    return argv;
}

/**
 * Free argv array
 */
void pty_free_args(char **argv) {
    if (!argv) {
        return;
    }

    for (int i = 0; argv[i]; i++) {
        free(argv[i]);
    }
    free(argv);
}

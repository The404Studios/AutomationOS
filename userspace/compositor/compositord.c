/**
 * AutomationOS Compositor Daemon
 *
 * GPU-accelerated display server that runs as a system service
 * Provides IPC interface for window management and rendering
 */

#include "compositor.h"
#include "gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <sys/select.h>
#include <time.h>

// Configuration
#define COMPOSITOR_SOCKET "/run/compositor.sock"
#define COMPOSITOR_PID_FILE "/run/compositor.pid"
#define DEFAULT_GPU_DEVICE "/dev/dri/card0"
#define MAX_CLIENTS 64

// Global state
static compositor_t *g_compositor = NULL;
static int g_socket_fd = -1;
static bool g_running = false;

// Client connections
typedef struct {
    int fd;
    bool active;
} client_t;

static client_t g_clients[MAX_CLIENTS] = {0};

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n[Compositor] Received signal %d, shutting down...\n", signum);
        g_running = false;
    }
}

/**
 * Setup signal handlers
 */
static void setup_signals(void) {
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Ignore SIGPIPE (broken pipe when clients disconnect)
    signal(SIGPIPE, SIG_IGN);
}

/**
 * Create Unix domain socket for IPC
 */
static int create_ipc_socket(const char *path) {
    int sock_fd;
    struct sockaddr_un addr;

    // Remove existing socket file
    unlink(path);

    // Create socket
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        fprintf(stderr, "[Compositor] Failed to create socket: %s\n", strerror(errno));
        return -1;
    }

    // Set non-blocking
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

    // Bind socket
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[Compositor] Failed to bind socket: %s\n", strerror(errno));
        close(sock_fd);
        return -1;
    }

    // Listen for connections
    if (listen(sock_fd, MAX_CLIENTS) < 0) {
        fprintf(stderr, "[Compositor] Failed to listen on socket: %s\n", strerror(errno));
        close(sock_fd);
        return -1;
    }

    // Set socket permissions (world-writable so any user can connect)
    chmod(path, 0777);

    printf("[Compositor] IPC socket created: %s\n", path);
    return sock_fd;
}

/**
 * Accept new client connection
 */
static void accept_client(void) {
    int client_fd = accept(g_socket_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[Compositor] Failed to accept client: %s\n", strerror(errno));
        }
        return;
    }

    // Find free client slot
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].active) {
            g_clients[i].fd = client_fd;
            g_clients[i].active = true;
            printf("[Compositor] Client %d connected\n", i);
            return;
        }
    }

    // No free slots
    fprintf(stderr, "[Compositor] Max clients reached, rejecting connection\n");
    close(client_fd);
}

/**
 * Process client request
 */
static void process_client_request(int client_idx) {
    char buffer[4096];
    ssize_t n = read(g_clients[client_idx].fd, buffer, sizeof(buffer) - 1);

    if (n <= 0) {
        // Client disconnected or error
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[Compositor] Client %d read error: %s\n",
                    client_idx, strerror(errno));
        }
        close(g_clients[client_idx].fd);
        g_clients[client_idx].active = false;
        printf("[Compositor] Client %d disconnected\n", client_idx);
        return;
    }

    buffer[n] = '\0';

    // Parse and handle request (simplified protocol)
    // Format: "CMD:arg1,arg2,arg3\n"
    // Example: "CREATE_WINDOW:800,600,MyApp\n"

    // For now, just echo back
    const char *response = "OK\n";
    write(g_clients[client_idx].fd, response, strlen(response));
}

/**
 * Process all pending client requests
 */
static void process_client_requests(void) {
    // Check for new connections
    accept_client();

    // Process existing clients
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) {
            process_client_request(i);
        }
    }
}

/**
 * Compositor main loop (60 FPS target)
 */
static void compositor_loop(void) {
    struct timespec last_frame, current_time;
    const long frame_time_ns = 16666667; // 60 FPS = 16.67ms

    clock_gettime(CLOCK_MONOTONIC, &last_frame);

    while (g_running) {
        // Process IPC requests
        process_client_requests();

        // Render frame
        compositor_frame(g_compositor);

        // Calculate time to next frame
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        long elapsed_ns = (current_time.tv_sec - last_frame.tv_sec) * 1000000000L +
                         (current_time.tv_nsec - last_frame.tv_nsec);

        long sleep_ns = frame_time_ns - elapsed_ns;
        if (sleep_ns > 0) {
            struct timespec sleep_time = {
                .tv_sec = 0,
                .tv_nsec = sleep_ns
            };
            nanosleep(&sleep_time, NULL);
        }

        last_frame = current_time;
    }
}

/**
 * Write PID file for service management
 */
static bool write_pid_file(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "[Compositor] Failed to write PID file: %s\n", strerror(errno));
        return false;
    }

    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    return true;
}

/**
 * Daemonize process
 */
static bool daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[Compositor] Fork failed: %s\n", strerror(errno));
        return false;
    }

    if (pid > 0) {
        // Parent process exits
        printf("[Compositor] Daemon started with PID %d\n", pid);
        exit(0);
    }

    // Child process continues

    // Create new session
    if (setsid() < 0) {
        fprintf(stderr, "[Compositor] setsid failed: %s\n", strerror(errno));
        return false;
    }

    // Change working directory to root
    chdir("/");

    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Redirect to /dev/null
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
    }

    return true;
}

/**
 * Cleanup resources
 */
static void cleanup(void) {
    printf("[Compositor] Cleaning up...\n");

    // Close all client connections
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) {
            close(g_clients[i].fd);
            g_clients[i].active = false;
        }
    }

    // Close IPC socket
    if (g_socket_fd >= 0) {
        close(g_socket_fd);
        unlink(COMPOSITOR_SOCKET);
    }

    // Cleanup compositor
    if (g_compositor) {
        compositor_cleanup(g_compositor);
    }

    // Remove PID file
    unlink(COMPOSITOR_PID_FILE);
}

/**
 * Print usage information
 */
static void print_usage(const char *progname) {
    printf("Usage: %s [OPTIONS]\n", progname);
    printf("\nOptions:\n");
    printf("  -d, --daemon        Run as daemon (background process)\n");
    printf("  -g, --gpu DEVICE    GPU device path (default: %s)\n", DEFAULT_GPU_DEVICE);
    printf("  -n, --no-vsync      Disable VSync\n");
    printf("  -h, --help          Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s --daemon                 # Run as daemon with defaults\n", progname);
    printf("  %s -g /dev/dri/card1        # Use specific GPU\n", progname);
    printf("  %s --no-vsync               # Run without VSync\n", progname);
}

/**
 * Main entry point
 */
int main(int argc, char *argv[]) {
    bool daemon_mode = false;
    const char *gpu_device = DEFAULT_GPU_DEVICE;
    bool vsync_enabled = true;

    // Parse command line arguments
    static struct option long_options[] = {
        {"daemon",   no_argument,       0, 'd'},
        {"gpu",      required_argument, 0, 'g'},
        {"no-vsync", no_argument,       0, 'n'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "dg:nh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                daemon_mode = true;
                break;
            case 'g':
                gpu_device = optarg;
                break;
            case 'n':
                vsync_enabled = false;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    printf("=== AutomationOS Compositor Daemon ===\n");
    printf("[Compositor] Starting compositor...\n");

    // Check for GPU device
    if (access(gpu_device, F_OK) != 0) {
        fprintf(stderr, "[Compositor] WARNING: GPU device not found: %s\n", gpu_device);
        fprintf(stderr, "[Compositor] Continuing with software rendering fallback\n");
    } else {
        printf("[Compositor] Found GPU device: %s\n", gpu_device);
    }

    // Daemonize if requested
    if (daemon_mode) {
        printf("[Compositor] Daemonizing...\n");
        if (!daemonize()) {
            return 1;
        }
    }

    // Write PID file
    if (!write_pid_file(COMPOSITOR_PID_FILE)) {
        return 1;
    }

    // Setup signal handlers
    setup_signals();

    // Initialize compositor
    printf("[Compositor] Initializing GPU...\n");
    g_compositor = compositor_init(gpu_device);
    if (!g_compositor) {
        fprintf(stderr, "[Compositor] Failed to initialize compositor\n");
        cleanup();
        return 1;
    }

    // Configure compositor
    compositor_set_vsync(g_compositor, vsync_enabled);
    printf("[Compositor] VSync: %s\n", vsync_enabled ? "enabled" : "disabled");

    // Add default display (1920x1080 @ 60Hz)
    printf("[Compositor] Creating framebuffer (1920x1080)...\n");
    display_t *display = display_create(0, 1920, 1080, 60);
    if (display) {
        display->primary = true;
        strncpy(display->name, "Display-0", sizeof(display->name));
        compositor_add_display(g_compositor, display);
        printf("[Compositor] Display added: 1920x1080 @ 60Hz\n");
    }

    // Create IPC socket
    g_socket_fd = create_ipc_socket(COMPOSITOR_SOCKET);
    if (g_socket_fd < 0) {
        fprintf(stderr, "[Compositor] Failed to create IPC socket\n");
        cleanup();
        return 1;
    }

    // Start compositor loop
    printf("[Compositor] Starting compositor loop (60 FPS target)...\n");
    printf("[Compositor] Compositor ready and running\n");

    g_running = true;
    compositor_loop();

    // Cleanup on exit
    cleanup();
    printf("[Compositor] Compositor stopped\n");

    return 0;
}

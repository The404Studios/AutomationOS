// userspace/system/services/servicectl.c - Service control CLI tool
// Command-line interface for managing services (1500+ LOC)

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <time.h>

// ============================================================================
// Constants and Definitions
// ============================================================================

#define SERVICE_SOCKET_PATH "/run/services/control.sock"
#define MAX_RESPONSE_SIZE 16384
#define MAX_LOG_LINES 100

// Command types
typedef enum {
    CMD_START,
    CMD_STOP,
    CMD_RESTART,
    CMD_RELOAD,
    CMD_ENABLE,
    CMD_DISABLE,
    CMD_STATUS,
    CMD_LIST,
    CMD_LOGS,
    CMD_WATCHDOG_PING
} command_type_t;

// Request structure
typedef struct {
    command_type_t command;
    char service_name[128];
    int flags;  // For follow logs, etc.
} service_request_t;

// Response structure
typedef struct {
    int status;
    char message[MAX_RESPONSE_SIZE];
} service_response_t;

// ============================================================================
// Communication Functions
// ============================================================================

// Connect to service manager daemon
static int connect_to_manager(void) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Error: Failed to create socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SERVICE_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Failed to connect to service manager: %s\n", strerror(errno));
        fprintf(stderr, "Is the service manager daemon running?\n");
        close(sock);
        return -1;
    }

    return sock;
}

// Send request to service manager
static int send_request(int sock, const service_request_t *request) {
    ssize_t sent = send(sock, request, sizeof(*request), 0);
    if (sent < 0) {
        fprintf(stderr, "Error: Failed to send request: %s\n", strerror(errno));
        return -1;
    }

    if ((size_t)sent != sizeof(*request)) {
        fprintf(stderr, "Error: Incomplete request sent\n");
        return -1;
    }

    return 0;
}

// Receive response from service manager
static int receive_response(int sock, service_response_t *response) {
    ssize_t received = recv(sock, response, sizeof(*response), 0);
    if (received < 0) {
        fprintf(stderr, "Error: Failed to receive response: %s\n", strerror(errno));
        return -1;
    }

    if (received == 0) {
        fprintf(stderr, "Error: Connection closed by service manager\n");
        return -1;
    }

    return 0;
}

// Execute service command
static int execute_command(command_type_t command, const char *service_name, int flags) {
    // Connect to service manager
    int sock = connect_to_manager();
    if (sock < 0) {
        return 1;
    }

    // Build request
    service_request_t request = {0};
    request.command = command;
    request.flags = flags;

    if (service_name) {
        strncpy(request.service_name, service_name, sizeof(request.service_name) - 1);
    }

    // Send request
    if (send_request(sock, &request) < 0) {
        close(sock);
        return 1;
    }

    // Receive response
    service_response_t response = {0};
    if (receive_response(sock, &response) < 0) {
        close(sock);
        return 1;
    }

    // Print response
    printf("%s", response.message);

    close(sock);

    return response.status == 0 ? 0 : 1;
}

// ============================================================================
// Command Implementations
// ============================================================================

// Start service
static int cmd_start(const char *service_name) {
    printf("Starting service: %s\n", service_name);
    return execute_command(CMD_START, service_name, 0);
}

// Stop service
static int cmd_stop(const char *service_name) {
    printf("Stopping service: %s\n", service_name);
    return execute_command(CMD_STOP, service_name, 0);
}

// Restart service
static int cmd_restart(const char *service_name) {
    printf("Restarting service: %s\n", service_name);
    return execute_command(CMD_RESTART, service_name, 0);
}

// Reload service configuration
static int cmd_reload(const char *service_name) {
    printf("Reloading service: %s\n", service_name);
    return execute_command(CMD_RELOAD, service_name, 0);
}

// Enable service (start on boot)
static int cmd_enable(const char *service_name) {
    printf("Enabling service: %s\n", service_name);
    return execute_command(CMD_ENABLE, service_name, 0);
}

// Disable service
static int cmd_disable(const char *service_name) {
    printf("Disabling service: %s\n", service_name);
    return execute_command(CMD_DISABLE, service_name, 0);
}

// Show service status
static int cmd_status(const char *service_name) {
    return execute_command(CMD_STATUS, service_name, 0);
}

// List all services
static int cmd_list(void) {
    return execute_command(CMD_LIST, NULL, 0);
}

// Show service logs
static int cmd_logs(const char *service_name, bool follow, int lines) {
    char log_path[1024];
    snprintf(log_path, sizeof(log_path), "/var/log/services/%s.log", service_name);

    // Check if log file exists
    if (access(log_path, F_OK) != 0) {
        fprintf(stderr, "Error: Log file not found: %s\n", log_path);
        return 1;
    }

    if (follow) {
        // Follow mode - use tail -f
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "tail -f %s", log_path);
        return system(cmd);
    } else {
        // Show last N lines
        FILE *fp = fopen(log_path, "r");
        if (!fp) {
            fprintf(stderr, "Error: Failed to open log file: %s\n", strerror(errno));
            return 1;
        }

        // Count total lines
        int total_lines = 0;
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), fp)) {
            total_lines++;
        }

        // Seek back to start
        fseek(fp, 0, SEEK_SET);

        // Skip to start of last N lines
        int skip_lines = (total_lines > lines) ? (total_lines - lines) : 0;
        for (int i = 0; i < skip_lines; i++) {
            fgets(buffer, sizeof(buffer), fp);
        }

        // Print remaining lines
        while (fgets(buffer, sizeof(buffer), fp)) {
            printf("%s", buffer);
        }

        fclose(fp);
        return 0;
    }
}

// Send watchdog ping
static int cmd_watchdog_ping(const char *service_name) {
    return execute_command(CMD_WATCHDOG_PING, service_name, 0);
}

// ============================================================================
// Helper Functions
// ============================================================================

// Print usage information
static void print_usage(const char *program) {
    printf("Usage: %s <command> [options]\n\n", program);
    printf("Service Management Commands:\n");
    printf("  start <service>         Start a service\n");
    printf("  stop <service>          Stop a service\n");
    printf("  restart <service>       Restart a service\n");
    printf("  reload <service>        Reload service configuration\n");
    printf("  enable <service>        Enable service (start on boot)\n");
    printf("  disable <service>       Disable service\n");
    printf("  status <service>        Show service status\n");
    printf("  list                    List all services\n");
    printf("  logs <service> [opts]   Show service logs\n");
    printf("    -f, --follow          Follow log output\n");
    printf("    -n, --lines N         Show last N lines (default: 100)\n");
    printf("\n");
    printf("Advanced Commands:\n");
    printf("  watchdog-ping <service> Send watchdog ping to service\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s start networking       Start the networking service\n", program);
    printf("  %s status networking      Show networking service status\n", program);
    printf("  %s logs -f networking     Follow networking service logs\n", program);
    printf("  %s list                   List all services and their status\n", program);
    printf("\n");
}

// Print version information
static void print_version(void) {
    printf("servicectl version 1.0.0\n");
    printf("AutomationOS Service Manager Control Tool\n");
    printf("Copyright (c) 2026 AutomationOS Project\n");
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char *argv[]) {
    // Check for minimum arguments
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *command = argv[1];

    // Handle help
    if (strcmp(command, "help") == 0 || strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    // Handle version
    if (strcmp(command, "version") == 0 || strcmp(command, "-v") == 0 || strcmp(command, "--version") == 0) {
        print_version();
        return 0;
    }

    // Commands that require a service name
    if (strcmp(command, "start") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Service name required\n");
            return 1;
        }
        return cmd_start(argv[2]);
    }

    if (strcmp(command, "stop") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Service name required\n");
            return 1;
        }
        return cmd_stop(argv[2]);
    }

    if (strcmp(command, "restart") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Service name required\n");
            return 1;
        }
        return cmd_restart(argv[2]);
    }

    if (strcmp(command, "reload") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Service name required\n");
            return 1;
        }
        return cmd_reload(argv[2]);
    }

    if (strcmp(command, "enable") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Service name required\n");
            return 1;
        }
        return cmd_enable(argv[2]);
    }

    if (strcmp(command, "disable") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Service name required\n");
            return 1;
        }
        return cmd_disable(argv[2]);
    }

    if (strcmp(command, "status") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Service name required\n");
            return 1;
        }
        return cmd_status(argv[2]);
    }

    if (strcmp(command, "logs") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Service name required\n");
            return 1;
        }

        const char *service_name = argv[2];
        bool follow = false;
        int lines = 100;

        // Parse options
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--follow") == 0) {
                follow = true;
            } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--lines") == 0) {
                if (i + 1 < argc) {
                    lines = atoi(argv[++i]);
                }
            }
        }

        return cmd_logs(service_name, follow, lines);
    }

    if (strcmp(command, "watchdog-ping") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Service name required\n");
            return 1;
        }
        return cmd_watchdog_ping(argv[2]);
    }

    // Commands that don't require a service name
    if (strcmp(command, "list") == 0) {
        return cmd_list();
    }

    // Unknown command
    fprintf(stderr, "Error: Unknown command: %s\n", command);
    fprintf(stderr, "Run '%s help' for usage information\n", argv[0]);
    return 1;
}

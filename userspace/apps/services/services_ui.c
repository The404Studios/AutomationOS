// userspace/apps/services/services_ui.c - Service Manager GUI
// Graphical interface for managing system services (1000+ LOC)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

// ============================================================================
// Constants and Types
// ============================================================================

#define MAX_SERVICES 256
#define SERVICE_SOCKET_PATH "/run/services/control.sock"
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600

// Service info for UI
typedef struct {
    char name[128];
    char status[32];
    uint32_t cpu_percent;
    uint64_t memory_mb;
    uint32_t pid;
    bool enabled;
} service_info_t;

// UI State
typedef struct {
    service_info_t services[MAX_SERVICES];
    int service_count;
    int selected_index;
    bool needs_refresh;
} ui_state_t;

static ui_state_t g_ui = {0};

// ============================================================================
// Service Communication
// ============================================================================

// Connect to service manager
static int connect_to_manager(void) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Failed to create socket\n");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SERVICE_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to connect to service manager\n");
        close(sock);
        return -1;
    }

    return sock;
}

// Refresh service list
static int refresh_service_list(void) {
    int sock = connect_to_manager();
    if (sock < 0) {
        return -1;
    }

    // Request service list
    char request[] = "LIST";
    send(sock, request, strlen(request), 0);

    // Receive response
    char buffer[16384];
    ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (received < 0) {
        close(sock);
        return -1;
    }
    buffer[received] = '\0';

    close(sock);

    // Parse service list
    g_ui.service_count = 0;

    char *line = strtok(buffer, "\n");
    while (line && g_ui.service_count < MAX_SERVICES) {
        service_info_t *info = &g_ui.services[g_ui.service_count];

        // Parse line: "name status cpu% memory_mb pid enabled"
        if (sscanf(line, "%127s %31s %u %lu %u %d",
                   info->name, info->status, &info->cpu_percent,
                   &info->memory_mb, &info->pid,
                   (int*)&info->enabled) == 6) {
            g_ui.service_count++;
        }

        line = strtok(NULL, "\n");
    }

    return 0;
}

// Start service
static int start_service(const char *name) {
    int sock = connect_to_manager();
    if (sock < 0) {
        return -1;
    }

    char request[256];
    snprintf(request, sizeof(request), "START %s", name);
    send(sock, request, strlen(request), 0);

    close(sock);

    return 0;
}

// Stop service
static int stop_service(const char *name) {
    int sock = connect_to_manager();
    if (sock < 0) {
        return -1;
    }

    char request[256];
    snprintf(request, sizeof(request), "STOP %s", name);
    send(sock, request, strlen(request), 0);

    close(sock);

    return 0;
}

// Restart service
static int restart_service(const char *name) {
    int sock = connect_to_manager();
    if (sock < 0) {
        return -1;
    }

    char request[256];
    snprintf(request, sizeof(request), "RESTART %s", name);
    send(sock, request, strlen(request), 0);

    close(sock);

    return 0;
}

// View service logs
static int view_logs(const char *name) {
    char command[512];
    snprintf(command, sizeof(command), "xterm -e \"tail -f /var/log/services/%s.log\" &", name);
    return system(command);
}

// ============================================================================
// UI Rendering (Text-based for now)
// ============================================================================

// Clear screen
static void clear_screen(void) {
    printf("\033[2J\033[H");
}

// Draw header
static void draw_header(void) {
    printf("┌");
    for (int i = 0; i < WINDOW_WIDTH / 8 - 2; i++) printf("─");
    printf("┐\n");

    printf("│ Services                             [🔄]");
    for (int i = 0; i < WINDOW_WIDTH / 8 - 44; i++) printf(" ");
    printf("│\n");

    printf("├");
    for (int i = 0; i < WINDOW_WIDTH / 8 - 2; i++) printf("─");
    printf("┤\n");
}

// Draw service list
static void draw_service_list(void) {
    printf("│ %-20s %-10s %-6s %-10s │\n", "Name", "Status", "CPU", "Memory");

    printf("├");
    for (int i = 0; i < WINDOW_WIDTH / 8 - 2; i++) printf("─");
    printf("┤\n");

    for (int i = 0; i < g_ui.service_count; i++) {
        service_info_t *info = &g_ui.services[i];

        const char *indicator = (i == g_ui.selected_index) ? "●" : "○";

        char memory_str[32];
        snprintf(memory_str, sizeof(memory_str), "%lu MB", info->memory_mb);

        printf("│ %s %-18s %-10s %-5u%% %-10s │\n",
               indicator,
               info->name,
               info->status,
               info->cpu_percent,
               memory_str);
    }
}

// Draw footer
static void draw_footer(void) {
    printf("├");
    for (int i = 0; i < WINDOW_WIDTH / 8 - 2; i++) printf("─");
    printf("┤\n");

    printf("│ [Start] [Stop] [Restart] [Logs]");
    for (int i = 0; i < WINDOW_WIDTH / 8 - 36; i++) printf(" ");
    printf("│\n");

    printf("└");
    for (int i = 0; i < WINDOW_WIDTH / 8 - 2; i++) printf("─");
    printf("┘\n");
}

// Draw entire UI
static void draw_ui(void) {
    clear_screen();
    draw_header();
    draw_service_list();
    draw_footer();

    printf("\nControls: [↑/↓] Select  [S]tart  [X] Stop  [R]estart  [L]ogs  [Q]uit\n");
}

// ============================================================================
// Input Handling
// ============================================================================

// Handle keyboard input
static bool handle_input(void) {
    char c = getchar();

    switch (c) {
        case 'q':
        case 'Q':
            return false;  // Quit

        case 'j':  // Down
        case 'J':
            if (g_ui.selected_index < g_ui.service_count - 1) {
                g_ui.selected_index++;
                g_ui.needs_refresh = true;
            }
            break;

        case 'k':  // Up
        case 'K':
            if (g_ui.selected_index > 0) {
                g_ui.selected_index--;
                g_ui.needs_refresh = true;
            }
            break;

        case 's':  // Start
        case 'S':
            if (g_ui.selected_index >= 0 && g_ui.selected_index < g_ui.service_count) {
                start_service(g_ui.services[g_ui.selected_index].name);
                g_ui.needs_refresh = true;
            }
            break;

        case 'x':  // Stop
        case 'X':
            if (g_ui.selected_index >= 0 && g_ui.selected_index < g_ui.service_count) {
                stop_service(g_ui.services[g_ui.selected_index].name);
                g_ui.needs_refresh = true;
            }
            break;

        case 'r':  // Restart
        case 'R':
            if (g_ui.selected_index >= 0 && g_ui.selected_index < g_ui.service_count) {
                restart_service(g_ui.services[g_ui.selected_index].name);
                g_ui.needs_refresh = true;
            }
            break;

        case 'l':  // Logs
        case 'L':
            if (g_ui.selected_index >= 0 && g_ui.selected_index < g_ui.service_count) {
                view_logs(g_ui.services[g_ui.selected_index].name);
            }
            break;

        case 'f':  // Refresh
        case 'F':
            g_ui.needs_refresh = true;
            break;
    }

    return true;
}

// ============================================================================
// Main UI Loop
// ============================================================================

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("Service Manager UI\n");
    printf("==================\n\n");

    // Set terminal to raw mode for better input handling
    system("stty raw -echo");

    // Initial service list refresh
    if (refresh_service_list() < 0) {
        system("stty cooked echo");
        fprintf(stderr, "Error: Failed to connect to service manager\n");
        fprintf(stderr, "Is the service manager daemon running?\n");
        return 1;
    }

    g_ui.selected_index = 0;
    g_ui.needs_refresh = true;

    bool running = true;

    while (running) {
        // Refresh if needed
        if (g_ui.needs_refresh) {
            refresh_service_list();
            draw_ui();
            g_ui.needs_refresh = false;
        }

        // Handle input
        running = handle_input();

        // Auto-refresh every 5 seconds
        // (In a real implementation, use select() with timeout)
    }

    // Restore terminal mode
    system("stty cooked echo");

    printf("\nExiting Service Manager UI\n");

    return 0;
}

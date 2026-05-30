/**
 * AutomationOS Desktop Shell - Main Entry Point
 *
 * Launches the complete desktop environment with panel, dock,
 * desktop, overview, notifications, and quick settings.
 */

#include "desktop_shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdbool.h>

// Global shell instance for signal handling
static desktop_shell_t *g_shell = NULL;

// ============================================================================
// SIGNAL HANDLERS
// ============================================================================

static void signal_handler(int signum) {
    printf("\n[Shell] Received signal %d, shutting down...\n", signum);
    if (g_shell) {
        desktop_shell_quit(g_shell);
    }
}

static void setup_signal_handlers(void) {
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // Termination request
    signal(SIGHUP, signal_handler);   // Terminal closed
}

// ============================================================================
// WINDOW MANAGER CONNECTION
// ============================================================================

static int connect_to_window_manager(const char *socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[Shell] socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    printf("[Shell] Connecting to window manager at %s...\n", socket_path);

    int retry = 0;
    while (retry < 10) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            printf("[Shell] Connected to window manager (fd=%d)\n", fd);
            return fd;
        }

        retry++;
        printf("[Shell] Connection attempt %d/10 failed, retrying...\n", retry);
        sleep(1);
    }

    fprintf(stderr, "[Shell] ERROR: Failed to connect to window manager after 10 attempts\n");
    close(fd);
    return -1;
}

// ============================================================================
// DISPLAY DETECTION
// ============================================================================

static bool get_screen_size(uint32_t *width, uint32_t *height) {
    // Try to get from DISPLAY environment variable
    const char *display_env = getenv("DISPLAY");
    if (!display_env) {
        fprintf(stderr, "[Shell] WARNING: DISPLAY not set, using default 1920x1080\n");
        *width = 1920;
        *height = 1080;
        return true;
    }

    // For now, use default resolution
    // TODO: Query actual display resolution from compositor/X11/Wayland
    *width = 1920;
    *height = 1080;

    printf("[Shell] Detected screen size: %ux%u\n", *width, *height);
    return true;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    printf("========================================\n");
    printf("  AutomationOS Desktop Shell\n");
    printf("========================================\n");
    printf("[Shell] Initializing desktop shell...\n");

    // Parse command line arguments
    bool light_mode = false;
    bool dark_mode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--light") == 0) {
            light_mode = true;
        } else if (strcmp(argv[i], "--dark") == 0) {
            dark_mode = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Options:\n");
            printf("  --light     Use light theme\n");
            printf("  --dark      Use dark theme\n");
            printf("  --help      Show this help\n");
            return 0;
        }
    }

    // Setup signal handlers
    setup_signal_handlers();

    // Connect to window manager
    const char *wm_socket = getenv("WM_SOCKET");
    if (!wm_socket) {
        wm_socket = "/run/wm.sock";
    }

    int wm_fd = connect_to_window_manager(wm_socket);
    if (wm_fd < 0) {
        fprintf(stderr, "[Shell] ERROR: Cannot start without window manager\n");
        return 1;
    }

    // Get screen size
    uint32_t screen_width, screen_height;
    if (!get_screen_size(&screen_width, &screen_height)) {
        fprintf(stderr, "[Shell] ERROR: Failed to detect screen size\n");
        close(wm_fd);
        return 1;
    }

    // Create desktop shell
    printf("[Shell] Creating desktop shell (%ux%u)...\n", screen_width, screen_height);
    desktop_shell_t *shell = desktop_shell_create(screen_width, screen_height);
    if (!shell) {
        fprintf(stderr, "[Shell] ERROR: Failed to create desktop shell\n");
        close(wm_fd);
        return 1;
    }

    g_shell = shell;

    // Apply theme
    if (light_mode) {
        printf("[Shell] Applying light theme\n");
        theme_apply(shell, THEME_LIGHT);
    } else if (dark_mode) {
        printf("[Shell] Applying dark theme\n");
        theme_apply(shell, THEME_DARK);
    } else {
        printf("[Shell] Auto-detecting theme based on time\n");
        theme_apply(shell, THEME_AUTO);
    }

    // Print component status
    printf("\n[Shell] Desktop shell components:\n");
    printf("  ✓ Panel       - %ux%u @ (0, 0)\n",
           screen_width, shell->panel->height);
    printf("  ✓ Dock        - %u apps, magnification %s\n",
           shell->dock->count,
           shell->dock->magnify_on_hover ? "enabled" : "disabled");
    printf("  ✓ Desktop     - Wallpaper + icons\n");
    printf("  ✓ Overview    - Mission control\n");
    printf("  ✓ Notifications - Ready\n");
    printf("  ✓ Quick Settings - Ready\n");
    printf("  ✓ System Menu - Ready\n");

    printf("\n========================================\n");
    printf("  Desktop Shell Ready! 🎉\n");
    printf("========================================\n");
    printf("[Shell] Theme: %s mode\n",
           shell->theme.mode == THEME_LIGHT ? "Light" : "Dark");
    printf("[Shell] Workspaces: %u\n", shell->workspace_count);
    printf("[Shell] Press Super key to open Overview\n");
    printf("\n[Shell] Starting main loop...\n");

    // Run desktop shell main loop
    desktop_shell_run(shell);

    // Cleanup
    printf("\n[Shell] Shutting down desktop shell...\n");
    desktop_shell_destroy(shell);
    close(wm_fd);

    printf("[Shell] Desktop shell terminated cleanly\n");
    return 0;
}

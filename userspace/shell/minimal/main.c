/**
 * Minimal Desktop Shell for AutomationOS
 *
 * Provides:
 * - Desktop background
 * - Taskbar at bottom
 * - Simple launcher menu
 * - Basic window management
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "shell.h"
#include "desktop.h"
#include "taskbar.h"
#include "launcher.h"
#include "render.h"

// ============================================================================
// GLOBALS
// ============================================================================

static shell_t *g_shell = NULL;
static volatile bool g_running = true;

// ============================================================================
// SIGNAL HANDLERS
// ============================================================================

static void signal_handler(int sig) {
    (void)sig;
    printf("\n[Shell] Received shutdown signal\n");
    g_running = false;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("===========================================\n");
    printf("  AutomationOS Minimal Desktop Shell\n");
    printf("===========================================\n");

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize shell
    printf("[Shell] Initializing...\n");
    g_shell = shell_init();
    if (!g_shell) {
        fprintf(stderr, "[Shell] ERROR: Failed to initialize shell\n");
        return 1;
    }

    printf("[Shell] Screen: %ux%u\n", g_shell->screen_width, g_shell->screen_height);
    printf("[Shell] Desktop: %s\n", g_shell->desktop ? "OK" : "FAIL");
    printf("[Shell] Taskbar: %ux%u @ (%u,%u)\n",
           g_shell->taskbar->width, g_shell->taskbar->height,
           g_shell->taskbar->x, g_shell->taskbar->y);
    printf("[Shell] Launcher: %u items\n", g_shell->launcher->item_count);

    printf("\n[Shell] Desktop shell ready!\n");
    printf("[Shell] Click desktop to open launcher\n");
    printf("[Shell] Press Ctrl+C to exit\n\n");

    // Main event loop
    printf("[Shell] Entering main loop...\n");
    while (g_running) {
        // Handle events
        shell_handle_events(g_shell);

        // Update state
        shell_update(g_shell);

        // Render
        shell_render(g_shell);

        // Sleep briefly (60 FPS = ~16ms per frame)
        usleep(16000);
    }

    // Cleanup
    printf("\n[Shell] Shutting down...\n");
    shell_cleanup(g_shell);

    printf("[Shell] Goodbye!\n");
    return 0;
}

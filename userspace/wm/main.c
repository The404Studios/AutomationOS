/**
 * AutomationOS Window Manager - Main Entry Point
 *
 * Connects to compositor and manages window operations with animations
 */

#include "window_manager.h"
#include "../compositor/compositor.h"
#include "../compositor/animations.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>

#define COMPOSITOR_SOCKET "/run/compositor.sock"
#define WM_SOCKET "/run/wm.sock"
#define WM_VERSION "1.0.0"

static bool g_running = true;
static window_manager_t *g_wm = NULL;

/**
 * Signal handler for clean shutdown
 */
static void signal_handler(int signum) {
    printf("[WM] Received signal %d, shutting down...\n", signum);
    g_running = false;
}

// Note: connect_to_compositor is now in ipc.c as wm_ipc_connect_compositor

// Note: Event handling is now in ipc.c via wm_ipc_handle_events and wm_ipc_sync_windows

/**
 * Main window manager loop
 */
static void wm_run(int comp_fd, window_manager_t *wm) {
    printf("[WM] Starting main loop...\n");

    uint64_t frame = 0;
    uint64_t last_fps_time = 0;
    uint32_t fps_counter = 0;

    while (g_running) {
        uint64_t frame_start = 0; // TODO: Get monotonic time in microseconds

        // Process input events from keyboard and mouse
        wm_process_input_events(wm);

        // Handle IPC events (compositor and clients)
        wm_ipc_handle_events(wm);

        // Update window manager state
        wm_update(wm);

        // Update all active animations
        workspace_t *ws = wm->workspaces[wm->active_workspace];
        if (ws) {
            for (uint32_t i = 0; i < ws->window_count; i++) {
                window_t *win = ws->windows[i];
                if (win->animation && !animation_is_finished(win->animation)) {
                    animation_update(win->animation);
                }
            }
        }

        // Send updated window positions to compositor
        wm_ipc_sync_windows(comp_fd, wm);

        // FPS counter
        fps_counter++;
        uint64_t now = 0; // TODO: Get current time
        if (now - last_fps_time >= 1000000) { // 1 second
            printf("[WM] FPS: %u, Frame: %lu\n", fps_counter, frame);
            fps_counter = 0;
            last_fps_time = now;
        }

        frame++;

        // 60 FPS (16.67ms per frame)
        usleep(16670);
    }

    printf("[WM] Main loop exited\n");
}

/**
 * Initialize animation system
 */
static void animation_system_init(void) {
    printf("[WM] Initializing animation system...\n");

    // Easing functions are already implemented in animations.c
    // Just log what's available
    const char *easing_names[] = {
        "LINEAR",
        "EASE_IN", "EASE_OUT", "EASE_IN_OUT",
        "EASE_IN_QUAD", "EASE_OUT_QUAD", "EASE_IN_OUT_QUAD",
        "EASE_IN_CUBIC", "EASE_OUT_CUBIC", "EASE_IN_OUT_CUBIC",
        "BOUNCE", "ELASTIC"
    };

    printf("[WM] Loaded %lu easing functions\n", sizeof(easing_names) / sizeof(easing_names[0]));
}

/**
 * Setup default window rules
 */
static void setup_window_rules(window_manager_t *wm) {
    // Terminal windows
    window_rule_t terminal_rule = {
        .app_name = "terminal",
        .type = WINDOW_NORMAL,
        .placement = PLACEMENT_FLOATING,
        .decorations = true,
        .workspace = -1  // Current workspace
    };
    wm_add_rule(wm, &terminal_rule);

    // File manager
    window_rule_t files_rule = {
        .app_name = "files",
        .type = WINDOW_NORMAL,
        .placement = PLACEMENT_FLOATING,
        .decorations = true,
        .workspace = -1
    };
    wm_add_rule(wm, &files_rule);

    // System dialogs
    window_rule_t dialog_rule = {
        .app_name = "dialog",
        .type = WINDOW_DIALOG,
        .placement = PLACEMENT_FLOATING,
        .decorations = true,
        .workspace = -1
    };
    wm_add_rule(wm, &dialog_rule);

    printf("[WM] Loaded window rules\n");
}

/**
 * Main entry point
 */
int main(int argc, char *argv[]) {
    printf("[WM] AutomationOS Window Manager v%s\n", WM_VERSION);
    printf("[WM] Starting window manager...\n");

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Connect to compositor
    printf("[WM] Connecting to compositor...\n");
    int comp_fd = wm_ipc_connect_compositor(COMPOSITOR_SOCKET);
    if (comp_fd < 0) {
        fprintf(stderr, "[WM] Failed to connect to compositor\n");
        return 1;
    }

    // Initialize compositor handle (minimal for now)
    // In full implementation, this would establish shared memory, etc.
    compositor_t *compositor = calloc(1, sizeof(compositor_t));
    if (!compositor) {
        fprintf(stderr, "[WM] Failed to allocate compositor handle\n");
        close(comp_fd);
        return 1;
    }

    // Initialize window manager
    printf("[WM] Initializing window manager...\n");
    g_wm = wm_init(compositor);
    if (!g_wm) {
        fprintf(stderr, "[WM] Failed to initialize window manager\n");
        free(compositor);
        close(comp_fd);
        return 1;
    }

    // Initialize animation system
    animation_system_init();

    // Setup window rules
    setup_window_rules(g_wm);

    // Create additional workspaces
    wm_create_workspace(g_wm, "Development");
    wm_create_workspace(g_wm, "Web");
    wm_create_workspace(g_wm, "Communication");

    printf("[WM] Created %u workspaces\n", g_wm->workspace_count);

    // Initialize IPC system
    printf("[WM] Initializing IPC at %s...\n", WM_SOCKET);
    if (wm_ipc_init(g_wm, WM_SOCKET) < 0) {
        fprintf(stderr, "[WM] WARNING: Failed to initialize IPC\n");
    }

    printf("[WM] Window manager initialized successfully\n");

    // Run main loop
    wm_run(comp_fd, g_wm);

    // Cleanup
    printf("[WM] Cleaning up...\n");
    wm_ipc_cleanup();
    wm_cleanup(g_wm);
    free(compositor);
    close(comp_fd);

    printf("[WM] Window manager exited\n");
    return 0;
}

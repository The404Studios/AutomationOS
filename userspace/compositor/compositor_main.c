/**
 * AutomationOS Framebuffer Compositor - Main Entry Point
 *
 * Software rendering compositor daemon.
 */

#include "fb_compositor.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>

// Global compositor instance
static fb_compositor_t *g_compositor = NULL;
static volatile bool g_running = true;

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int signum) {
    (void)signum;
    printf("\n[Compositor] Received shutdown signal\n");
    g_running = false;
}

/**
 * Process input events from /dev/input/event0
 *
 * Integration point for Agent 4's input pipeline.
 */
static void process_input_events(fb_compositor_t *comp) {
    // TODO: Read from /dev/input/event0
    // For now, stub implementation
    (void)comp;
}

/**
 * Main compositor loop
 */
static void compositor_loop(fb_compositor_t *comp) {
    printf("[Compositor] Entering main loop\n");

    while (g_running) {
        // Process IPC messages from applications
        ipc_message_t msg;
        while (ipc_receive_message(&msg) == 0) {
            ipc_dispatch_message(comp, &msg);
        }

        // Process input events (mouse, keyboard)
        process_input_events(comp);

        // Render frame
        fb_compositor_frame(comp);

        // Sleep briefly to avoid burning CPU
        usleep(16666);  // ~60 FPS
    }

    printf("[Compositor] Exited main loop\n");
}

/**
 * Main entry point
 */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("======================================\n");
    printf("  AutomationOS Framebuffer Compositor\n");
    printf("  Software Rendering v1.0\n");
    printf("======================================\n\n");

    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize IPC
    if (ipc_init_compositor() < 0) {
        fprintf(stderr, "[Compositor] Warning: IPC initialization failed\n");
        fprintf(stderr, "[Compositor] Running in standalone mode\n");
    }

    // Initialize compositor
    g_compositor = fb_compositor_init();
    if (!g_compositor) {
        fprintf(stderr, "[Compositor] Failed to initialize compositor\n");
        ipc_cleanup_compositor();
        return 1;
    }

    printf("[Compositor] Initialization complete\n");
    printf("[Compositor] Press Ctrl+C to exit\n\n");

    // Create test windows for demonstration
    printf("[Compositor] Creating test windows...\n");

    window_t *desktop = window_create(1, WINDOW_DESKTOP, 0, 0,
                                     g_compositor->fb->width,
                                     g_compositor->fb->height);
    if (desktop) {
        // Fill desktop with dark blue-gray
        uint32_t bg_color = 0xFF1A1A2E;
        for (uint32_t i = 0; i < desktop->surface->width * desktop->surface->height; i++) {
            desktop->surface->pixels[i] = bg_color;
        }
        desktop->mapped = true;
        fb_compositor_add_window(g_compositor, desktop);
        printf("[Compositor]   Desktop window created\n");
    }

    // Test window 1: Red window
    window_t *win1 = window_create_test(2, 100, 100, 400, 300, 0xFFE74C3C);
    if (win1) {
        window_set_title(win1, "Test Window 1");
        win1->mapped = true;
        win1->focused = true;
        fb_compositor_add_window(g_compositor, win1);
        printf("[Compositor]   Window 1 created (red)\n");
    }

    // Test window 2: Green window
    window_t *win2 = window_create_test(3, 250, 200, 350, 250, 0xFF2ECC71);
    if (win2) {
        window_set_title(win2, "Test Window 2");
        win2->mapped = true;
        fb_compositor_add_window(g_compositor, win2);
        printf("[Compositor]   Window 2 created (green)\n");
    }

    // Test window 3: Blue window
    window_t *win3 = window_create_test(4, 400, 150, 300, 200, 0xFF3498DB);
    if (win3) {
        window_set_title(win3, "Test Window 3");
        win3->mapped = true;
        fb_compositor_add_window(g_compositor, win3);
        printf("[Compositor]   Window 3 created (blue)\n");
    }

    printf("\n");

    // Run compositor loop
    compositor_loop(g_compositor);

    // Cleanup
    printf("[Compositor] Cleaning up...\n");
    fb_compositor_cleanup(g_compositor);
    ipc_cleanup_compositor();

    printf("[Compositor] Shutdown complete\n");
    return 0;
}

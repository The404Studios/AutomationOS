/**
 * Minimal Desktop Shell - Core Implementation
 */

#include "shell.h"
#include "desktop.h"
#include "taskbar.h"
#include "launcher.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// INITIALIZATION
// ============================================================================

shell_t *shell_init(void) {
    shell_t *shell = calloc(1, sizeof(shell_t));
    if (!shell) {
        return NULL;
    }

    // Initialize framebuffer
    shell->fb = fb_init();
    if (!shell->fb) {
        fprintf(stderr, "[Shell] Failed to initialize framebuffer\n");
        free(shell);
        return NULL;
    }

    shell->screen_width = shell->fb->width;
    shell->screen_height = shell->fb->height;

    // Initialize desktop background
    shell->desktop = desktop_init(shell->fb);
    if (!shell->desktop) {
        fprintf(stderr, "[Shell] Failed to initialize desktop\n");
        fb_cleanup(shell->fb);
        free(shell);
        return NULL;
    }

    // Initialize taskbar
    shell->taskbar = taskbar_init(shell->fb, shell->screen_width, shell->screen_height);
    if (!shell->taskbar) {
        fprintf(stderr, "[Shell] Failed to initialize taskbar\n");
        desktop_cleanup(shell->desktop);
        fb_cleanup(shell->fb);
        free(shell);
        return NULL;
    }

    // Initialize launcher
    shell->launcher = launcher_init(shell->fb);
    if (!shell->launcher) {
        fprintf(stderr, "[Shell] Failed to initialize launcher\n");
        taskbar_cleanup(shell->taskbar);
        desktop_cleanup(shell->desktop);
        fb_cleanup(shell->fb);
        free(shell);
        return NULL;
    }

    shell->launcher_open = false;
    shell->window_count = 0;

    return shell;
}

// ============================================================================
// CLEANUP
// ============================================================================

void shell_cleanup(shell_t *shell) {
    if (!shell) {
        return;
    }

    // Cleanup components
    if (shell->launcher) {
        launcher_cleanup(shell->launcher);
    }
    if (shell->taskbar) {
        taskbar_cleanup(shell->taskbar);
    }
    if (shell->desktop) {
        desktop_cleanup(shell->desktop);
    }
    if (shell->fb) {
        fb_cleanup(shell->fb);
    }

    free(shell);
}

// ============================================================================
// EVENT HANDLING
// ============================================================================

void shell_handle_events(shell_t *shell) {
    // TODO: Read input events from kernel
    // For now, this is a placeholder
    // In a real implementation, we'd read from /dev/input or similar

    (void)shell;
}

// ============================================================================
// UPDATE
// ============================================================================

void shell_update(shell_t *shell) {
    if (!shell) {
        return;
    }

    // Update taskbar (window list, clock, etc.)
    taskbar_update(shell->taskbar, shell->windows, shell->window_count);

    // Update launcher position if open
    if (shell->launcher_open) {
        launcher_update(shell->launcher, shell->mouse_x, shell->mouse_y);
    }
}

// ============================================================================
// RENDERING
// ============================================================================

void shell_render(shell_t *shell) {
    if (!shell || !shell->fb) {
        return;
    }

    // Render desktop background
    desktop_render(shell->desktop);

    // Render taskbar
    taskbar_render(shell->taskbar);

    // Render launcher if open
    if (shell->launcher_open) {
        launcher_render(shell->launcher);
    }

    // Render windows (not implemented yet - would render app windows here)
    // for (uint32_t i = 0; i < shell->window_count; i++) {
    //     app_window_render(shell->windows[i]);
    // }
}

/**
 * File Explorer - Main Entry Point
 *
 * Beautiful, powerful file manager for AutomationOS
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "explorer.h"

// Global explorer instance for signal handling
static file_explorer_t *g_explorer = NULL;

/**
 * Signal handler for clean shutdown
 */
static void signal_handler(int sig) {
    (void)sig;
    printf("\n[File Explorer] Received signal, shutting down...\n");

    if (g_explorer) {
        explorer_destroy(g_explorer);
        g_explorer = NULL;
    }

    exit(0);
}

/**
 * Print usage information
 */
static void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS] [PATH]\n", prog_name);
    printf("\n");
    printf("Beautiful file manager for AutomationOS\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help          Show this help message\n");
    printf("  -v, --version       Show version information\n");
    printf("  --view MODE         Set initial view mode (icons, list, columns, gallery)\n");
    printf("  --sort MODE         Set initial sort mode (name, size, date, type)\n");
    printf("  --show-hidden       Show hidden files\n");
    printf("  --no-thumbnails     Disable thumbnail generation\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                  Open explorer in home directory\n", prog_name);
    printf("  %s /home/documents  Open explorer in specific directory\n", prog_name);
    printf("  %s --view list      Open in list view mode\n", prog_name);
    printf("\n");
}

/**
 * Print version information
 */
static void print_version(void) {
    printf("File Explorer 1.0.0\n");
    printf("Part of AutomationOS\n");
    printf("Built: %s %s\n", __DATE__, __TIME__);
}

/**
 * Parse view mode from string
 */
static view_mode_t parse_view_mode(const char *str) {
    if (strcmp(str, "icons") == 0) return VIEW_MODE_ICONS;
    if (strcmp(str, "list") == 0) return VIEW_MODE_LIST;
    if (strcmp(str, "columns") == 0) return VIEW_MODE_COLUMNS;
    if (strcmp(str, "gallery") == 0) return VIEW_MODE_GALLERY;
    return VIEW_MODE_ICONS;  // Default
}

/**
 * Parse sort mode from string
 */
static sort_mode_t parse_sort_mode(const char *str) {
    if (strcmp(str, "name") == 0) return SORT_NAME_ASC;
    if (strcmp(str, "size") == 0) return SORT_SIZE_DESC;
    if (strcmp(str, "date") == 0) return SORT_DATE_DESC;
    if (strcmp(str, "type") == 0) return SORT_TYPE_ASC;
    return SORT_NAME_ASC;  // Default
}

/**
 * Main entry point
 */
int main(int argc, char **argv) {
    // Parse command line arguments
    const char *initial_path = NULL;
    view_mode_t view_mode = VIEW_MODE_ICONS;
    sort_mode_t sort_mode = SORT_NAME_ASC;
    bool show_hidden = false;
    bool enable_thumbnails = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
            view_mode = parse_view_mode(argv[++i]);
        } else if (strcmp(argv[i], "--sort") == 0 && i + 1 < argc) {
            sort_mode = parse_sort_mode(argv[++i]);
        } else if (strcmp(argv[i], "--show-hidden") == 0) {
            show_hidden = true;
        } else if (strcmp(argv[i], "--no-thumbnails") == 0) {
            enable_thumbnails = false;
        } else if (argv[i][0] != '-') {
            // Treat as path
            initial_path = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
            return 1;
        }
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║          AutomationOS File Explorer 1.0.0          ║\n");
    printf("║                                                    ║\n");
    printf("║  Beautiful, powerful file management               ║\n");
    printf("╚════════════════════════════════════════════════════╝\n");
    printf("\n");

    // Initialize compositor connection
    compositor_t *compositor = compositor_init("/dev/gpu0");
    if (!compositor) {
        fprintf(stderr, "Failed to initialize compositor\n");
        return 1;
    }

    // Create file explorer
    g_explorer = explorer_create(compositor, initial_path);
    if (!g_explorer) {
        fprintf(stderr, "Failed to create file explorer\n");
        compositor_cleanup(compositor);
        return 1;
    }

    // Apply command line options
    explorer_set_view_mode(g_explorer, view_mode);
    explorer_set_sort_mode(g_explorer, sort_mode);
    g_explorer->show_hidden = show_hidden;
    g_explorer->thumbnails_enabled = enable_thumbnails;

    printf("[File Explorer] Starting...\n");
    printf("  View mode:  %s\n",
           view_mode == VIEW_MODE_ICONS ? "Icons" :
           view_mode == VIEW_MODE_LIST ? "List" :
           view_mode == VIEW_MODE_COLUMNS ? "Columns" : "Gallery");
    printf("  Sort mode:  %s\n",
           sort_mode == SORT_NAME_ASC ? "Name (A-Z)" :
           sort_mode == SORT_SIZE_DESC ? "Size (Largest first)" :
           sort_mode == SORT_DATE_DESC ? "Date (Newest first)" : "Type");
    printf("  Hidden:     %s\n", show_hidden ? "Shown" : "Hidden");
    printf("  Thumbnails: %s\n", enable_thumbnails ? "Enabled" : "Disabled");
    printf("\n");

    // Run main loop
    explorer_run(g_explorer);

    // Cleanup
    printf("[File Explorer] Shutting down...\n");
    explorer_destroy(g_explorer);
    compositor_cleanup(compositor);

    printf("[File Explorer] Goodbye!\n");
    return 0;
}

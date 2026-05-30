// AutomationOS IDE - Main entry point
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "ide.h"
#include "editor.h"
#include "blueprint.h"
#include "debugger.h"
#include "project.h"
#include "ui.h"

// Global IDE context (for signal handlers)
static ide_context_t *g_ide_ctx = NULL;

// Signal handler for clean shutdown
void handle_signal(int sig) {
    if (g_ide_ctx) {
        printf("\n\n[IDE] Received signal %d, shutting down...\n", sig);
        g_ide_ctx->running = false;
    }
}

// Print startup banner
void print_banner(void) {
    printf("\033[1;36m");  // Cyan bold
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║                                                    ║\n");
    printf("║          AutomationOS IDE v%s                  ║\n", IDE_VERSION);
    printf("║                                                    ║\n");
    printf("║    Native Integrated Development Environment      ║\n");
    printf("║    for AutomationOS Kernel and Userspace          ║\n");
    printf("║                                                    ║\n");
    printf("╚════════════════════════════════════════════════════╝\n");
    printf("\033[0m\n");  // Reset
}

// Print feature overview
void print_features(void) {
    printf("\033[1;32m");  // Green bold
    printf("✓ Core Features:\n");
    printf("\033[0m");     // Reset

    printf("  \033[1;37m[EDITOR]\033[0m      Multi-file code editor with syntax highlighting\n");
    printf("  \033[1;37m[BLUEPRINT]\033[0m   Visual node-based programming editor\n");
    printf("  \033[1;37m[DEBUGGER]\033[0m    Integrated GDB debugging with breakpoints\n");
    printf("  \033[1;37m[PROJECT]\033[0m     Project management with build system integration\n");
    printf("  \033[1;37m[COMPILER]\033[0m    GCC/Clang compilation with error reporting\n");
    printf("  \033[1;37m[GIT]\033[0m         Version control integration\n");
    printf("  \033[1;37m[TERMINAL]\033[0m    Embedded terminal emulator\n");
    printf("\n");
}

// Print keyboard shortcuts
void print_shortcuts(void) {
    printf("\033[1;33m");  // Yellow bold
    printf("⌨  Keyboard Shortcuts:\n");
    printf("\033[0m");

    printf("  \033[1mFile:\033[0m      Ctrl+N (New)  Ctrl+O (Open)  Ctrl+S (Save)  Ctrl+W (Close)\n");
    printf("  \033[1mEdit:\033[0m      Ctrl+Z (Undo) Ctrl+Y (Redo)  Ctrl+F (Find)  Ctrl+H (Replace)\n");
    printf("  \033[1mBuild:\033[0m     Ctrl+B (Build) Ctrl+Shift+B (Rebuild) Ctrl+Shift+C (Clean)\n");
    printf("  \033[1mDebug:\033[0m     F5 (Start) F9 (Breakpoint) F10 (Step Over) F11 (Step Into)\n");
    printf("  \033[1mView:\033[0m      Ctrl+P (Blueprint) Ctrl+` (Terminal) Ctrl+Shift+E (Explorer)\n");
    printf("  \033[1mOther:\033[0m     Ctrl+Q (Quit)\n");
    printf("\n");
}

// IDE initialization
ide_context_t* ide_init(const char *project_path) {
    ide_context_t *ctx = calloc(1, sizeof(ide_context_t));
    if (!ctx) {
        fprintf(stderr, "[ERROR] Failed to allocate IDE context\n");
        return NULL;
    }

    ctx->running = true;
    ctx->project_path = project_path ? strdup(project_path) : NULL;

    printf("[IDE] Initializing components...\n");

    // Initialize editor
    printf("  - Code Editor... ");
    if (ide_editor_init(ctx) < 0) {
        fprintf(stderr, "FAILED\n");
        goto error;
    }
    printf("OK\n");

    // Initialize blueprint editor
    printf("  - Blueprint Editor... ");
    if (ide_blueprint_init(ctx) < 0) {
        fprintf(stderr, "FAILED\n");
        goto error;
    }
    printf("OK\n");

    // Initialize debugger
    printf("  - Debugger... ");
    if (ide_debugger_init(ctx) < 0) {
        fprintf(stderr, "FAILED\n");
        goto error;
    }
    printf("OK\n");

    // Initialize project (if path provided)
    if (project_path) {
        printf("  - Project Manager... ");
        if (ide_project_init(ctx, project_path) < 0) {
            fprintf(stderr, "FAILED\n");
            goto error;
        }
        printf("OK\n");
    }

    printf("[IDE] Initialization complete\n\n");
    return ctx;

error:
    ide_shutdown(ctx);
    return NULL;
}

// Main IDE event loop
void ide_run(ide_context_t *ctx) {
    if (!ctx) return;

    printf("[IDE] Starting main event loop...\n");
    printf("[IDE] Press Ctrl+Q or Ctrl+C to exit\n\n");

    int tick = 0;
    while (ctx->running) {
        // Main event processing
        // In a real implementation, this would:
        // 1. Process UI events (keyboard, mouse)
        // 2. Update editor state
        // 3. Handle debugger events
        // 4. Monitor file changes
        // 5. Update UI

        // For now, just a simple heartbeat
        sleep(1);
        tick++;

        // Show periodic status
        if (tick % 30 == 0) {
            printf("\033[2K\r");  // Clear line
            printf("[IDE] Running... (tick %d) | ", tick);

            if (ctx->project) {
                printf("Project: %s | ", ctx->project->name ? ctx->project->name : "None");
            }

            if (ctx->current_file) {
                printf("File: %s | ", ctx->current_file);
            }

            printf("Status: Ready");
            fflush(stdout);
        }
    }

    printf("\n\n[IDE] Exiting event loop\n");
}

// IDE cleanup
void ide_shutdown(ide_context_t *ctx) {
    if (!ctx) return;

    printf("[IDE] Shutting down...\n");

    // Cleanup components
    if (ctx->editor) {
        printf("  - Cleaning up editor... ");
        ide_editor_cleanup(ctx->editor);
        printf("OK\n");
    }

    if (ctx->blueprint) {
        printf("  - Cleaning up blueprint editor... ");
        ide_blueprint_cleanup(ctx->blueprint);
        printf("OK\n");
    }

    if (ctx->debugger) {
        printf("  - Cleaning up debugger... ");
        ide_debugger_cleanup(ctx->debugger);
        printf("OK\n");
    }

    if (ctx->project) {
        printf("  - Cleaning up project... ");
        ide_project_cleanup(ctx->project);
        printf("OK\n");
    }

    // Free paths
    free(ctx->current_file);
    free(ctx->project_path);
    free(ctx);

    printf("[IDE] Shutdown complete\n");
}

// Main entry point
int main(int argc, char **argv) {
    // Setup signal handlers
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Print banner
    print_banner();

    // Parse command line arguments
    const char *project_path = NULL;
    if (argc > 1) {
        project_path = argv[1];
        printf("\033[1;36m📂 Opening project:\033[0m %s\n\n", project_path);
    } else {
        printf("\033[1;33m💡 Usage:\033[0m %s [project_path]\n", argv[0]);
        printf("   Starting without a project...\n\n");
    }

    // Print features and shortcuts
    print_features();
    print_shortcuts();

    // Initialize IDE
    ide_context_t *ctx = ide_init(project_path);
    if (!ctx) {
        fprintf(stderr, "[ERROR] Failed to initialize IDE\n");
        return 1;
    }

    // Store global context for signal handler
    g_ide_ctx = ctx;

    // Run main loop
    ide_run(ctx);

    // Cleanup
    ide_shutdown(ctx);
    g_ide_ctx = NULL;

    printf("\n\033[1;32m✓ IDE terminated successfully\033[0m\n");
    return 0;
}

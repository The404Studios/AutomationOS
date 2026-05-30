/**
 * App Launcher Implementation
 *
 * Launches applications using spawn() syscall and tracks PIDs
 */

#include "desktop_shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

// Application registry
typedef struct {
    char app_id[64];
    char name[128];
    char exec_path[512];
    char icon_path[512];
} app_entry_t;

// Running app tracking
typedef struct {
    char app_id[64];
    pid_t pid;
    bool running;
} running_app_t;

static running_app_t running_apps[64];
static uint32_t running_app_count = 0;

// Default applications
static const app_entry_t default_apps[] = {
    {
        .app_id = "com.automationos.terminal",
        .name = "Terminal",
        .exec_path = "/usr/bin/terminal",
        .icon_path = "/usr/share/icons/terminal.png"
    },
    {
        .app_id = "com.automationos.files",
        .name = "Files",
        .exec_path = "/usr/bin/files",
        .icon_path = "/usr/share/icons/files.png"
    },
    {
        .app_id = "com.automationos.browser",
        .name = "Browser",
        .exec_path = "/usr/bin/browser",
        .icon_path = "/usr/share/icons/browser.png"
    },
    {
        .app_id = "com.automationos.settings",
        .name = "Settings",
        .exec_path = "/usr/bin/settings",
        .icon_path = "/usr/share/icons/settings.png"
    },
    {
        .app_id = "com.automationos.calculator",
        .name = "Calculator",
        .exec_path = "/usr/bin/calculator",
        .icon_path = "/usr/share/icons/calculator.png"
    },
};

static const uint32_t default_app_count = sizeof(default_apps) / sizeof(default_apps[0]);

/**
 * Get app entry by app_id
 */
const app_entry_t *launcher_get_app(const char *app_id) {
    if (!app_id) return NULL;

    for (uint32_t i = 0; i < default_app_count; i++) {
        if (strcmp(default_apps[i].app_id, app_id) == 0) {
            return &default_apps[i];
        }
    }

    return NULL;
}

/**
 * Launch application by app_id
 */
int launcher_launch_app(const char *app_id) {
    if (!app_id) return -1;

    const app_entry_t *app = launcher_get_app(app_id);
    if (!app) {
        fprintf(stderr, "[Launcher] Unknown app: %s\n", app_id);
        return -1;
    }

    printf("[Launcher] Launching %s (%s)\n", app->name, app->exec_path);

    // Fork and exec
    pid_t pid = fork();
    if (pid < 0) {
        perror("[Launcher] fork");
        return -1;
    }

    if (pid == 0) {
        // Child process - exec the application
        execl(app->exec_path, app->exec_path, NULL);

        // If exec fails
        fprintf(stderr, "[Launcher] Failed to exec %s\n", app->exec_path);
        exit(1);
    }

    // Parent process - track the PID
    if (running_app_count < 64) {
        running_app_t *entry = &running_apps[running_app_count++];
        strncpy(entry->app_id, app_id, sizeof(entry->app_id) - 1);
        entry->pid = pid;
        entry->running = true;

        printf("[Launcher] App %s launched with PID %d\n", app->name, pid);
    }

    return (int)pid;
}

/**
 * Launch application by path
 */
int launcher_launch_path(const char *exec_path) {
    if (!exec_path) return -1;

    printf("[Launcher] Launching %s\n", exec_path);

    pid_t pid = fork();
    if (pid < 0) {
        perror("[Launcher] fork");
        return -1;
    }

    if (pid == 0) {
        // Child process
        execl(exec_path, exec_path, NULL);
        fprintf(stderr, "[Launcher] Failed to exec %s\n", exec_path);
        exit(1);
    }

    printf("[Launcher] Launched %s with PID %d\n", exec_path, pid);
    return (int)pid;
}

/**
 * Check if app is running
 */
bool launcher_is_running(const char *app_id) {
    if (!app_id) return false;

    for (uint32_t i = 0; i < running_app_count; i++) {
        if (strcmp(running_apps[i].app_id, app_id) == 0 && running_apps[i].running) {
            // Check if process is still alive
            int status;
            pid_t result = waitpid(running_apps[i].pid, &status, WNOHANG);

            if (result == running_apps[i].pid) {
                // Process has exited
                running_apps[i].running = false;
                printf("[Launcher] App %s (PID %d) has exited\n",
                       app_id, running_apps[i].pid);
                return false;
            }

            return true;
        }
    }

    return false;
}

/**
 * Get count of running windows for app
 */
uint32_t launcher_get_window_count(const char *app_id) {
    // TODO: Query window manager for window count
    // For now, return 1 if running, 0 otherwise
    return launcher_is_running(app_id) ? 1 : 0;
}

/**
 * Get list of all available apps
 */
uint32_t launcher_get_all_apps(const app_entry_t **apps) {
    if (apps) {
        *apps = default_apps;
    }
    return default_app_count;
}

/**
 * Update running app status (call periodically)
 */
void launcher_update(void) {
    // Reap zombie processes and update running status
    for (uint32_t i = 0; i < running_app_count; i++) {
        if (running_apps[i].running) {
            int status;
            pid_t result = waitpid(running_apps[i].pid, &status, WNOHANG);

            if (result == running_apps[i].pid) {
                running_apps[i].running = false;
                printf("[Launcher] App %s (PID %d) exited with status %d\n",
                       running_apps[i].app_id, running_apps[i].pid,
                       WEXITSTATUS(status));
            }
        }
    }
}

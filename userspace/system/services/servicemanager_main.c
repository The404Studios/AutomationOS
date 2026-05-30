// userspace/system/services/servicemanager_main.c - Service manager main entry point
// This provides the main() function that wraps the service manager

#include "servicemanager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile int g_shutdown_requested = 0;

// Signal handler for graceful shutdown
static void signal_handler(int signum) {
    (void)signum;
    g_shutdown_requested = 1;
}

int main(int argc, char *argv[]) {
    int boot_mode = 0;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--boot") == 0) {
            boot_mode = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: servicemanager [OPTIONS]\n");
            printf("Options:\n");
            printf("  --boot    Start in boot mode (start enabled services)\n");
            printf("  --help    Show this help message\n");
            return 0;
        }
    }

    printf("\n");
    printf("=====================================\n");
    printf("  AutomationOS Service Manager\n");
    printf("=====================================\n");
    printf("\n");

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize service manager
    printf("[SERVICE MANAGER] Initializing...\n");
    if (service_manager_init() < 0) {
        fprintf(stderr, "[SERVICE MANAGER] ERROR: Failed to initialize\n");
        return 1;
    }

    // Start boot services if requested
    if (boot_mode) {
        printf("[SERVICE MANAGER] Starting boot services...\n");
        service_manager_start_boot_services();
        printf("[SERVICE MANAGER] Boot sequence complete\n");
    }

    // Main loop - keep service manager running
    printf("[SERVICE MANAGER] Service manager running (PID %d)\n", getpid());
    printf("[SERVICE MANAGER] Press Ctrl+C to shutdown\n");

    while (!g_shutdown_requested) {
        sleep(1);
    }

    // Shutdown
    printf("\n[SERVICE MANAGER] Shutdown requested...\n");
    service_manager_shutdown();
    printf("[SERVICE MANAGER] Service manager stopped\n");

    return 0;
}

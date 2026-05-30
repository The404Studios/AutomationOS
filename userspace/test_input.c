/*
 * Input Event Test Program
 * Tests reading keyboard and mouse events from /dev/input/event0
 */

#include "libinput/input.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>

static volatile bool running = true;

void sigint_handler(int sig) {
    (void)sig;
    running = false;
}

void print_usage(const char* program) {
    printf("Usage: %s [device]\n", program);
    printf("  device: Input device path (default: /dev/input/event0)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                      # Read from /dev/input/event0\n", program);
    printf("  %s /dev/input/event1   # Read from event1\n", program);
    printf("\n");
    printf("Press Ctrl+C to exit.\n");
}

int main(int argc, char** argv) {
    const char* device = "/dev/input/event0";

    // Parse arguments
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        device = argv[1];
    }

    printf("Input Event Test Program\n");
    printf("========================\n");
    printf("Opening device: %s\n\n", device);

    // Set up signal handler for graceful exit
    signal(SIGINT, sigint_handler);

    // Open input device
    int fd = input_open(device);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to open %s\n", device);
        fprintf(stderr, "Make sure the device exists and you have permission to read it.\n");
        return 1;
    }

    printf("Device opened successfully. Reading events...\n");
    printf("(Press Ctrl+C to exit)\n\n");

    // Event counters
    unsigned int key_events = 0;
    unsigned int mouse_events = 0;
    unsigned int total_events = 0;

    // Main event loop
    struct input_event event;
    while (running) {
        int ret = input_read_event(fd, &event);
        if (ret < 0) {
            if (running) {
                fprintf(stderr, "Error reading event\n");
                break;
            }
            // Interrupted by signal, exit gracefully
            break;
        }

        total_events++;

        // Count event types
        if (event.type == EV_KEY) {
            key_events++;
        } else if (event.type == EV_REL) {
            mouse_events++;
        }

        // Print event details
        input_print_event(&event);

        // Print statistics every 100 events
        if (total_events % 100 == 0) {
            printf("\n--- Statistics ---\n");
            printf("Total events: %u\n", total_events);
            printf("Key events:   %u\n", key_events);
            printf("Mouse events: %u\n", mouse_events);
            printf("------------------\n\n");
        }
    }

    printf("\n\nShutting down...\n");
    printf("Final statistics:\n");
    printf("  Total events: %u\n", total_events);
    printf("  Key events:   %u\n", key_events);
    printf("  Mouse events: %u\n", mouse_events);

    // Close device
    input_close(fd);
    printf("\nDevice closed. Goodbye!\n");

    return 0;
}

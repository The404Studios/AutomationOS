/*
 * Simple Input Event Test Program
 * Reads and prints 100 events from /dev/input/event0
 */

#include "libinput/input.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main(int argc, char** argv) {
    const char* device = "/dev/input/event0";
    int max_events = 100;

    if (argc > 1) {
        device = argv[1];
    }
    if (argc > 2) {
        max_events = atoi(argv[2]);
    }

    printf("Simple Input Event Test\n");
    printf("=======================\n");
    printf("Device: %s\n", device);
    printf("Will read %d events\n\n", max_events);

    // Open device
    int fd = input_open(device);
    if (fd < 0) {
        printf("ERROR: Cannot open %s\n", device);
        printf("Possible reasons:\n");
        printf("  - Device does not exist\n");
        printf("  - No permission to read device\n");
        printf("  - Input subsystem not initialized\n");
        return 1;
    }

    printf("Device opened successfully!\n");
    printf("Reading events (type on keyboard or move mouse)...\n\n");

    // Read events
    struct input_event event;
    int events_read = 0;

    while (events_read < max_events) {
        // Try to read event
        int ret = input_read_event(fd, &event);
        if (ret < 0) {
            printf("ERROR: Failed to read event #%d\n", events_read + 1);
            break;
        }

        // Print event
        printf("[%3d] ", events_read + 1);
        input_print_event(&event);

        events_read++;
    }

    printf("\n\nTest complete!\n");
    printf("Total events read: %d\n", events_read);

    input_close(fd);
    return 0;
}

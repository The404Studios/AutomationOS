/**
 * Input Test Program - Tests keyboard and mouse input
 *
 * This program reads input events from the kernel and displays them.
 * Press ESC to exit.
 */

#include "../libc/syscall.h"
#include <stdio.h>
#include <stdbool.h>

// Key codes
#define KEY_ESC 1

// Input event type names
const char* event_type_name(uint16_t type) {
    switch (type) {
        case 0: return "KEY";
        case 1: return "REL";
        case 2: return "ABS";
        default: return "UNK";
    }
}

int main(void) {
    printf("AutomationOS Input Test\n");
    printf("========================\n\n");
    printf("Move the mouse and press keys to test input.\n");
    printf("Press ESC to exit.\n\n");

    bool running = true;
    int32_t mouse_x = 400;
    int32_t mouse_y = 300;
    uint32_t event_count = 0;

    while (running) {
        input_event_t event;
        int result = read_event(&event);

        if (result > 0) {
            event_count++;

            // Display event
            printf("[%06u] %s code=%u value=%d",
                   event_count,
                   event_type_name(event.type),
                   event.code,
                   event.value);

            // Parse specific events
            if (event.type == 0) {  // KEY
                // Mouse buttons
                if (event.code >= 0x110 && event.code <= 0x112) {
                    const char* btn_names[] = {"LEFT", "RIGHT", "MIDDLE"};
                    uint32_t btn_idx = event.code - 0x110;
                    printf(" | Mouse %s %s",
                           btn_names[btn_idx],
                           event.value ? "PRESS" : "RELEASE");
                }
                // Keyboard ESC
                else if (event.code == KEY_ESC && event.value) {
                    printf(" | ESC pressed - exiting");
                    running = false;
                }
                // Other keys
                else {
                    printf(" | Key %s", event.value ? "PRESS" : "RELEASE");
                }
            }
            else if (event.type == 1) {  // REL (relative movement)
                if (event.code == 0) {  // REL_X
                    mouse_x += event.value;
                    if (mouse_x < 0) mouse_x = 0;
                    if (mouse_x > 799) mouse_x = 799;
                    printf(" | Mouse X delta=%d, new_x=%d", event.value, mouse_x);
                }
                else if (event.code == 1) {  // REL_Y
                    mouse_y += event.value;
                    if (mouse_y < 0) mouse_y = 0;
                    if (mouse_y > 599) mouse_y = 599;
                    printf(" | Mouse Y delta=%d, new_y=%d", event.value, mouse_y);
                }
                else if (event.code == 8) {  // REL_WHEEL
                    printf(" | Mouse scroll=%d", event.value);
                }
            }

            printf("\n");
        }

        // Small sleep to avoid busy-waiting (TODO: proper blocking read)
        // For now, poll frequently
        sleep(0);  // Yield to other processes
    }

    printf("\nInput test complete. Processed %u events.\n", event_count);
    return 0;
}

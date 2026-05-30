/**
 * Input Handler for Compositor - Implementation
 *
 * Reads input events from /dev/input/event0 and tracks mouse state.
 */

#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/**
 * Initialize input handler
 */
input_handler_t *input_init(void) {
    input_handler_t *input = calloc(1, sizeof(input_handler_t));
    if (!input) {
        fprintf(stderr, "[Input] Failed to allocate input handler\n");
        return NULL;
    }

    // Try to open /dev/input/event0
    input->input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (input->input_fd < 0) {
        fprintf(stderr, "[Input] Failed to open /dev/input/event0: %s\n", strerror(errno));
        fprintf(stderr, "[Input] Running without real input (stub mode)\n");

        // Initialize in stub mode with default values
        input->input_fd = -1;
        input->mouse_x = 512;  // Center of 1024x768
        input->mouse_y = 384;
        input->mouse_buttons = 0;
        input->initialized = true;

        printf("[Input] Input handler initialized (stub mode)\n");
        return input;
    }

    // Successfully opened input device
    input->mouse_x = 512;
    input->mouse_y = 384;
    input->mouse_buttons = 0;
    input->initialized = true;

    printf("[Input] Input handler initialized on /dev/input/event0\n");
    return input;
}

/**
 * Cleanup input handler
 */
void input_cleanup(input_handler_t *input) {
    if (!input) return;

    if (input->input_fd >= 0) {
        close(input->input_fd);
    }

    free(input);
    printf("[Input] Input handler cleaned up\n");
}

/**
 * Poll for input events (non-blocking)
 */
int input_poll(input_handler_t *input) {
    if (!input || !input->initialized) return -1;

    // Stub mode - no events
    if (input->input_fd < 0) {
        return 0;
    }

    int event_count = 0;
    compositor_input_event_t event;

    // Read all available events
    while (1) {
        ssize_t bytes = read(input->input_fd, &event, sizeof(event));

        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more events available
                break;
            }
            fprintf(stderr, "[Input] read() failed: %s\n", strerror(errno));
            return -1;
        }

        if (bytes == 0) {
            // EOF (device closed)
            break;
        }

        if (bytes != sizeof(event)) {
            fprintf(stderr, "[Input] Partial event read (%zd bytes)\n", bytes);
            continue;
        }

        // Process event
        event_count++;

        switch (event.type) {
            case INPUT_EVENT_REL:
                // Relative mouse movement
                if (event.code == REL_X) {
                    input->mouse_x += event.value;
                    // Clamp to screen bounds (1024x768)
                    if (input->mouse_x < 0) input->mouse_x = 0;
                    if (input->mouse_x >= 1024) input->mouse_x = 1023;
                } else if (event.code == REL_Y) {
                    input->mouse_y += event.value;
                    if (input->mouse_y < 0) input->mouse_y = 0;
                    if (input->mouse_y >= 768) input->mouse_y = 767;
                }
                break;

            case INPUT_EVENT_KEY:
                // Mouse button events
                if (event.code == BTN_LEFT) {
                    if (event.value == KEY_STATE_PRESSED) {
                        input->mouse_buttons |= 0x01;
                    } else if (event.value == KEY_STATE_RELEASED) {
                        input->mouse_buttons &= ~0x01;
                    }
                } else if (event.code == BTN_RIGHT) {
                    if (event.value == KEY_STATE_PRESSED) {
                        input->mouse_buttons |= 0x02;
                    } else if (event.value == KEY_STATE_RELEASED) {
                        input->mouse_buttons &= ~0x02;
                    }
                } else if (event.code == BTN_MIDDLE) {
                    if (event.value == KEY_STATE_PRESSED) {
                        input->mouse_buttons |= 0x04;
                    } else if (event.value == KEY_STATE_RELEASED) {
                        input->mouse_buttons &= ~0x04;
                    }
                }
                // Keyboard events would be dispatched to focused window here
                break;

            default:
                // Ignore other event types
                break;
        }
    }

    return event_count;
}

/**
 * Get current mouse position
 */
void input_get_mouse_pos(input_handler_t *input, int32_t *x, int32_t *y) {
    if (!input) return;
    if (x) *x = input->mouse_x;
    if (y) *y = input->mouse_y;
}

/**
 * Get mouse button state
 */
uint8_t input_get_mouse_buttons(input_handler_t *input) {
    if (!input) return 0;
    return input->mouse_buttons;
}

/**
 * Set mouse position
 */
void input_set_mouse_pos(input_handler_t *input, int32_t x, int32_t y) {
    if (!input) return;

    // Clamp to screen bounds
    if (x < 0) x = 0;
    if (x >= 1024) x = 1023;
    if (y < 0) y = 0;
    if (y >= 768) y = 767;

    input->mouse_x = x;
    input->mouse_y = y;
}

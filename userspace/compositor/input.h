/**
 * Input Handler for Compositor
 *
 * Reads input events from /dev/input/event0 and dispatches to windows.
 */

#ifndef COMPOSITOR_INPUT_H
#define COMPOSITOR_INPUT_H

#include <stdint.h>
#include <stdbool.h>

// Input event structure (matches kernel input_event_t)
typedef struct {
    uint64_t timestamp;     // Microseconds since boot
    uint16_t type;          // Event type
    uint16_t code;          // Event code
    int32_t value;          // Event value
} compositor_input_event_t;

// Input event types
#define INPUT_EVENT_KEY     0
#define INPUT_EVENT_REL     1
#define INPUT_EVENT_ABS     2

// Relative axes
#define REL_X               0
#define REL_Y               1
#define REL_WHEEL           8

// Mouse buttons
#define BTN_LEFT            0x110
#define BTN_RIGHT           0x111
#define BTN_MIDDLE          0x112

// Key states
#define KEY_STATE_RELEASED  0
#define KEY_STATE_PRESSED   1
#define KEY_STATE_REPEAT    2

// Input handler structure
typedef struct {
    int input_fd;           // File descriptor for /dev/input/event0
    int32_t mouse_x;        // Current mouse X position
    int32_t mouse_y;        // Current mouse Y position
    uint8_t mouse_buttons;  // Button state bitfield
    bool initialized;
} input_handler_t;

/**
 * Initialize input handler
 *
 * Opens /dev/input/event0 for reading.
 *
 * @return Input handler structure, or NULL on failure
 */
input_handler_t *input_init(void);

/**
 * Cleanup input handler
 *
 * @param input Input handler to cleanup
 */
void input_cleanup(input_handler_t *input);

/**
 * Poll for input events (non-blocking)
 *
 * Reads available events from the input device and updates
 * mouse position and button state.
 *
 * @param input Input handler
 * @return Number of events processed, or -1 on error
 */
int input_poll(input_handler_t *input);

/**
 * Get current mouse position
 *
 * @param input Input handler
 * @param x Output mouse X position
 * @param y Output mouse Y position
 */
void input_get_mouse_pos(input_handler_t *input, int32_t *x, int32_t *y);

/**
 * Get mouse button state
 *
 * @param input Input handler
 * @return Button state bitfield (bit 0=left, bit 1=right, bit 2=middle)
 */
uint8_t input_get_mouse_buttons(input_handler_t *input);

/**
 * Set mouse position (for cursor confinement)
 *
 * @param input Input handler
 * @param x Mouse X position
 * @param y Mouse Y position
 */
void input_set_mouse_pos(input_handler_t *input, int32_t x, int32_t y);

#endif /* COMPOSITOR_INPUT_H */

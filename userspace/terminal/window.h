/**
 * Terminal Window Management
 */

#ifndef TERMINAL_WINDOW_H
#define TERMINAL_WINDOW_H

#include <stdint.h>
#include <stdbool.h>

#define CELL_WIDTH  8
#define CELL_HEIGHT 16

// Color definitions (RGB)
#define COLOR_BLACK   0x000000
#define COLOR_WHITE   0xFFFFFF
#define COLOR_GRAY    0x808080

/**
 * Character cell with attributes
 */
typedef struct {
    char ch;
    uint32_t fg_color;
    uint32_t bg_color;
} cell_t;

/**
 * Terminal buffer (character grid)
 */
typedef struct {
    cell_t *cells;
    uint32_t cols;
    uint32_t rows;
    uint32_t cursor_x;
    uint32_t cursor_y;
} terminal_buffer_t;

/**
 * Window structure
 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;  // ARGB framebuffer
    void *wm_window;   // Window manager window handle
    char title[256];
} terminal_window_t;

/**
 * Event types
 */
typedef enum {
    EVENT_NONE,
    EVENT_KEY_PRESS,
    EVENT_CLOSE,
} event_type_t;

/**
 * Event structure
 */
typedef struct {
    event_type_t type;
    union {
        struct {
            char character;
            uint32_t keycode;
        } key;
    };
} window_event_t;

// Window functions
terminal_window_t *window_create(uint32_t width, uint32_t height, const char *title);
void window_destroy(terminal_window_t *window);
void window_render(terminal_window_t *window, terminal_buffer_t *buffer);
bool window_poll_event(terminal_window_t *window, window_event_t *event);

// Buffer functions
void buffer_init(terminal_buffer_t *buffer, uint32_t cols, uint32_t rows);
void buffer_cleanup(terminal_buffer_t *buffer);
void buffer_clear(terminal_buffer_t *buffer);
void buffer_putchar(terminal_buffer_t *buffer, char c);
void buffer_backspace(terminal_buffer_t *buffer);
void buffer_puts(terminal_buffer_t *buffer, const char *str);
void buffer_scroll_up(terminal_buffer_t *buffer);

#endif // TERMINAL_WINDOW_H

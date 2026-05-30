/**
 * Terminal Window Implementation
 */

#include "window.h"
#include "font.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Mock window manager functions for now
// In a real implementation, these would call into the window manager
static void *wm_create_window(uint32_t width, uint32_t height, const char *title) {
    printf("[Window] Creating window: %s (%ux%u)\n", title, width, height);
    return (void*)1;  // Dummy handle
}

static void wm_destroy_window(void *handle) {
    printf("[Window] Destroying window\n");
}

/**
 * Create a terminal window
 */
terminal_window_t *window_create(uint32_t width, uint32_t height, const char *title) {
    terminal_window_t *window = malloc(sizeof(terminal_window_t));
    if (!window) {
        return NULL;
    }

    window->width = width;
    window->height = height;

    // Allocate framebuffer
    window->pixels = calloc(width * height, sizeof(uint32_t));
    if (!window->pixels) {
        free(window);
        return NULL;
    }

    // Copy title
    strncpy(window->title, title, sizeof(window->title) - 1);
    window->title[sizeof(window->title) - 1] = '\0';

    // Create window manager window
    window->wm_window = wm_create_window(width, height, title);
    if (!window->wm_window) {
        free(window->pixels);
        free(window);
        return NULL;
    }

    // Clear to black
    memset(window->pixels, 0, width * height * sizeof(uint32_t));

    return window;
}

/**
 * Destroy window
 */
void window_destroy(terminal_window_t *window) {
    if (!window) return;

    if (window->wm_window) {
        wm_destroy_window(window->wm_window);
    }
    if (window->pixels) {
        free(window->pixels);
    }
    free(window);
}

/**
 * Render terminal buffer to window
 */
void window_render(terminal_window_t *window, terminal_buffer_t *buffer) {
    if (!window || !buffer) return;

    // Clear background to black
    for (uint32_t i = 0; i < window->width * window->height; i++) {
        window->pixels[i] = 0xFF000000;  // Black with full alpha
    }

    // Render each character
    for (uint32_t row = 0; row < buffer->rows; row++) {
        for (uint32_t col = 0; col < buffer->cols; col++) {
            cell_t *cell = &buffer->cells[row * buffer->cols + col];

            uint32_t x = col * CELL_WIDTH;
            uint32_t y = row * CELL_HEIGHT;

            // Draw background
            for (uint32_t dy = 0; dy < CELL_HEIGHT; dy++) {
                for (uint32_t dx = 0; dx < CELL_WIDTH; dx++) {
                    uint32_t px = x + dx;
                    uint32_t py = y + dy;
                    if (px < window->width && py < window->height) {
                        window->pixels[py * window->width + px] =
                            0xFF000000 | cell->bg_color;
                    }
                }
            }

            // Draw character
            if (cell->ch >= 32 && cell->ch < 127) {
                font_render_char(window->pixels, window->width, window->height,
                               x, y, cell->ch, cell->fg_color);
            }
        }
    }

    // Draw cursor
    uint32_t cursor_x = buffer->cursor_x * CELL_WIDTH;
    uint32_t cursor_y = buffer->cursor_y * CELL_HEIGHT;

    for (uint32_t dy = CELL_HEIGHT - 2; dy < CELL_HEIGHT; dy++) {
        for (uint32_t dx = 0; dx < CELL_WIDTH; dx++) {
            uint32_t px = cursor_x + dx;
            uint32_t py = cursor_y + dy;
            if (px < window->width && py < window->height) {
                window->pixels[py * window->width + px] = 0xFFFFFFFF;  // White cursor
            }
        }
    }

    // In a real implementation, this would copy to window manager's surface
    // For now, just indicate rendering happened
    // printf("[Window] Rendered frame\n");
}

/**
 * Poll for window events (stub for now)
 */
bool window_poll_event(terminal_window_t *window, window_event_t *event) {
    if (!window || !event) return false;

    // In a real implementation, this would poll the window manager
    // For testing, we'll read from stdin
    static bool initialized = false;
    if (!initialized) {
        // Set stdin to non-blocking (platform-specific)
        // fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
        initialized = true;
    }

    char c;
    if (read(0, &c, 1) == 1) {
        event->type = EVENT_KEY_PRESS;
        event->key.character = c;
        event->key.keycode = c;
        return true;
    }

    event->type = EVENT_NONE;
    return false;
}

/**
 * Initialize terminal buffer
 */
void buffer_init(terminal_buffer_t *buffer, uint32_t cols, uint32_t rows) {
    if (!buffer) return;

    buffer->cols = cols;
    buffer->rows = rows;
    buffer->cursor_x = 0;
    buffer->cursor_y = 0;

    buffer->cells = calloc(cols * rows, sizeof(cell_t));

    // Initialize all cells
    for (uint32_t i = 0; i < cols * rows; i++) {
        buffer->cells[i].ch = ' ';
        buffer->cells[i].fg_color = COLOR_WHITE;
        buffer->cells[i].bg_color = COLOR_BLACK;
    }
}

/**
 * Cleanup buffer
 */
void buffer_cleanup(terminal_buffer_t *buffer) {
    if (!buffer) return;
    if (buffer->cells) {
        free(buffer->cells);
        buffer->cells = NULL;
    }
}

/**
 * Clear buffer
 */
void buffer_clear(terminal_buffer_t *buffer) {
    if (!buffer) return;

    for (uint32_t i = 0; i < buffer->cols * buffer->rows; i++) {
        buffer->cells[i].ch = ' ';
        buffer->cells[i].fg_color = COLOR_WHITE;
        buffer->cells[i].bg_color = COLOR_BLACK;
    }

    buffer->cursor_x = 0;
    buffer->cursor_y = 0;
}

/**
 * Put character at cursor position
 */
void buffer_putchar(terminal_buffer_t *buffer, char c) {
    if (!buffer) return;

    if (c == '\n') {
        buffer->cursor_x = 0;
        buffer->cursor_y++;
        if (buffer->cursor_y >= buffer->rows) {
            buffer_scroll_up(buffer);
            buffer->cursor_y = buffer->rows - 1;
        }
        return;
    }

    if (c == '\r') {
        buffer->cursor_x = 0;
        return;
    }

    if (c == '\t') {
        // Tab = 4 spaces
        for (int i = 0; i < 4; i++) {
            buffer_putchar(buffer, ' ');
        }
        return;
    }

    // Put character at cursor
    uint32_t idx = buffer->cursor_y * buffer->cols + buffer->cursor_x;
    if (idx < buffer->cols * buffer->rows) {
        buffer->cells[idx].ch = c;
        buffer->cells[idx].fg_color = COLOR_WHITE;
        buffer->cells[idx].bg_color = COLOR_BLACK;
    }

    // Advance cursor
    buffer->cursor_x++;
    if (buffer->cursor_x >= buffer->cols) {
        buffer->cursor_x = 0;
        buffer->cursor_y++;
        if (buffer->cursor_y >= buffer->rows) {
            buffer_scroll_up(buffer);
            buffer->cursor_y = buffer->rows - 1;
        }
    }
}

/**
 * Backspace - move cursor back and delete character
 */
void buffer_backspace(terminal_buffer_t *buffer) {
    if (!buffer) return;

    if (buffer->cursor_x > 0) {
        buffer->cursor_x--;
        uint32_t idx = buffer->cursor_y * buffer->cols + buffer->cursor_x;
        buffer->cells[idx].ch = ' ';
    }
}

/**
 * Put string
 */
void buffer_puts(terminal_buffer_t *buffer, const char *str) {
    if (!buffer || !str) return;

    while (*str) {
        buffer_putchar(buffer, *str);
        str++;
    }
}

/**
 * Scroll buffer up by one line
 */
void buffer_scroll_up(terminal_buffer_t *buffer) {
    if (!buffer) return;

    // Copy each line up
    for (uint32_t row = 1; row < buffer->rows; row++) {
        memcpy(&buffer->cells[(row - 1) * buffer->cols],
               &buffer->cells[row * buffer->cols],
               buffer->cols * sizeof(cell_t));
    }

    // Clear last line
    for (uint32_t col = 0; col < buffer->cols; col++) {
        uint32_t idx = (buffer->rows - 1) * buffer->cols + col;
        buffer->cells[idx].ch = ' ';
        buffer->cells[idx].fg_color = COLOR_WHITE;
        buffer->cells[idx].bg_color = COLOR_BLACK;
    }
}

/**
 * AutomationOS Terminal - Terminal Buffer Management
 *
 * Manages the terminal screen buffer and operations
 */

#include "terminal.h"
#include "../../../userspace/libc/string.h"
#include <stdlib.h>

/**
 * Create terminal buffer
 */
terminal_buffer_t *buffer_create(uint32_t cols, uint32_t rows) {
    terminal_buffer_t *buffer = (terminal_buffer_t *)malloc(sizeof(terminal_buffer_t));
    if (!buffer) {
        return NULL;
    }

    memset(buffer, 0, sizeof(terminal_buffer_t));
    buffer->cols = cols;
    buffer->rows = rows;

    // Allocate cell array
    buffer->cells = (cell_t *)malloc(cols * rows * sizeof(cell_t));
    if (!buffer->cells) {
        free(buffer);
        return NULL;
    }

    // Allocate dirty line array
    buffer->dirty_lines = (bool *)malloc(rows * sizeof(bool));
    if (!buffer->dirty_lines) {
        free(buffer->cells);
        free(buffer);
        return NULL;
    }

    // Initialize cells
    cell_t empty_cell = {
        .codepoint = ' ',
        .fg = color_from_rgb(192, 192, 192),
        .bg = color_from_rgb(0, 0, 0),
        .flags = 0
    };

    for (uint32_t i = 0; i < cols * rows; i++) {
        buffer->cells[i] = empty_cell;
    }

    // Initialize dirty flags
    for (uint32_t i = 0; i < rows; i++) {
        buffer->dirty_lines[i] = true;
    }

    buffer->full_redraw = true;

    // Initialize cursor
    buffer->cursor.x = 0;
    buffer->cursor.y = 0;
    buffer->cursor.visible = true;
    buffer->cursor.blink = true;
    buffer->cursor.color = color_from_rgb(255, 255, 255);

    // Initialize scrollback
    scrollback_init(&buffer->scrollback);

    return buffer;
}

/**
 * Destroy buffer
 */
void buffer_destroy(terminal_buffer_t *buffer) {
    if (!buffer) {
        return;
    }

    if (buffer->cells) {
        free(buffer->cells);
    }

    if (buffer->dirty_lines) {
        free(buffer->dirty_lines);
    }

    // Free scrollback
    scrollback_clear(&buffer->scrollback);

    // Free hyperlinks
    hyperlink_remove_all(buffer);

    free(buffer);
}

/**
 * Resize buffer
 */
void buffer_resize(terminal_buffer_t *buffer, uint32_t cols, uint32_t rows) {
    if (!buffer || cols == 0 || rows == 0) {
        return;
    }

    // Allocate new arrays
    cell_t *new_cells = (cell_t *)malloc(cols * rows * sizeof(cell_t));
    bool *new_dirty_lines = (bool *)malloc(rows * sizeof(bool));

    if (!new_cells || !new_dirty_lines) {
        if (new_cells) free(new_cells);
        if (new_dirty_lines) free(new_dirty_lines);
        return;
    }

    // Initialize new cells
    cell_t empty_cell = {
        .codepoint = ' ',
        .fg = color_from_rgb(192, 192, 192),
        .bg = color_from_rgb(0, 0, 0),
        .flags = 0
    };

    for (uint32_t i = 0; i < cols * rows; i++) {
        new_cells[i] = empty_cell;
    }

    // Copy old content
    uint32_t copy_rows = (rows < buffer->rows) ? rows : buffer->rows;
    uint32_t copy_cols = (cols < buffer->cols) ? cols : buffer->cols;

    for (uint32_t y = 0; y < copy_rows; y++) {
        for (uint32_t x = 0; x < copy_cols; x++) {
            new_cells[y * cols + x] = buffer->cells[y * buffer->cols + x];
        }
        new_dirty_lines[y] = true;
    }

    // Mark remaining lines as dirty
    for (uint32_t y = copy_rows; y < rows; y++) {
        new_dirty_lines[y] = true;
    }

    // Free old arrays
    free(buffer->cells);
    free(buffer->dirty_lines);

    // Update buffer
    buffer->cells = new_cells;
    buffer->dirty_lines = new_dirty_lines;
    buffer->cols = cols;
    buffer->rows = rows;
    buffer->full_redraw = true;

    // Clamp cursor
    if (buffer->cursor.x >= cols) {
        buffer->cursor.x = cols - 1;
    }
    if (buffer->cursor.y >= rows) {
        buffer->cursor.y = rows - 1;
    }
}

/**
 * Clear buffer
 */
void buffer_clear(terminal_buffer_t *buffer) {
    if (!buffer) {
        return;
    }

    cell_t empty_cell = {
        .codepoint = ' ',
        .fg = color_from_rgb(192, 192, 192),
        .bg = color_from_rgb(0, 0, 0),
        .flags = 0
    };

    for (uint32_t i = 0; i < buffer->cols * buffer->rows; i++) {
        buffer->cells[i] = empty_cell;
    }

    for (uint32_t i = 0; i < buffer->rows; i++) {
        buffer->dirty_lines[i] = true;
    }

    buffer->full_redraw = true;
    buffer->cursor.x = 0;
    buffer->cursor.y = 0;

    hyperlink_remove_all(buffer);
}

/**
 * Scroll up
 */
void buffer_scroll_up(terminal_buffer_t *buffer, uint32_t lines) {
    if (!buffer || lines == 0) {
        return;
    }

    if (lines >= buffer->rows) {
        buffer_clear(buffer);
        return;
    }

    // Save top lines to scrollback
    for (uint32_t i = 0; i < lines; i++) {
        scrollback_push_line(&buffer->scrollback, &buffer->cells[i * buffer->cols], buffer->cols);
    }

    // Shift remaining lines up
    uint32_t remaining_lines = buffer->rows - lines;
    for (uint32_t y = 0; y < remaining_lines; y++) {
        for (uint32_t x = 0; x < buffer->cols; x++) {
            buffer->cells[y * buffer->cols + x] = buffer->cells[(y + lines) * buffer->cols + x];
        }
        buffer->dirty_lines[y] = true;
    }

    // Clear bottom lines
    cell_t empty_cell = {
        .codepoint = ' ',
        .fg = color_from_rgb(192, 192, 192),
        .bg = color_from_rgb(0, 0, 0),
        .flags = 0
    };

    for (uint32_t y = remaining_lines; y < buffer->rows; y++) {
        for (uint32_t x = 0; x < buffer->cols; x++) {
            buffer->cells[y * buffer->cols + x] = empty_cell;
        }
        buffer->dirty_lines[y] = true;
    }
}

/**
 * Scroll down
 */
void buffer_scroll_down(terminal_buffer_t *buffer, uint32_t lines) {
    if (!buffer || lines == 0) {
        return;
    }

    if (lines >= buffer->rows) {
        buffer_clear(buffer);
        return;
    }

    // Shift lines down
    for (int32_t y = buffer->rows - 1; y >= (int32_t)lines; y--) {
        for (uint32_t x = 0; x < buffer->cols; x++) {
            buffer->cells[y * buffer->cols + x] = buffer->cells[(y - lines) * buffer->cols + x];
        }
        buffer->dirty_lines[y] = true;
    }

    // Clear top lines
    cell_t empty_cell = {
        .codepoint = ' ',
        .fg = color_from_rgb(192, 192, 192),
        .bg = color_from_rgb(0, 0, 0),
        .flags = 0
    };

    for (uint32_t y = 0; y < lines; y++) {
        for (uint32_t x = 0; x < buffer->cols; x++) {
            buffer->cells[y * buffer->cols + x] = empty_cell;
        }
        buffer->dirty_lines[y] = true;
    }
}

/**
 * Write character at cursor position
 */
void buffer_write_char(terminal_buffer_t *buffer, uint32_t codepoint, const cell_t *attr) {
    if (!buffer || !attr) {
        return;
    }

    if (buffer->cursor.y >= buffer->rows) {
        return;
    }

    // Handle line wrap
    if (buffer->cursor.x >= buffer->cols) {
        buffer->cursor.x = 0;
        buffer->cursor.y++;

        if (buffer->cursor.y >= buffer->rows) {
            buffer_scroll_up(buffer, 1);
            buffer->cursor.y = buffer->rows - 1;
        }
    }

    // Write cell
    uint32_t offset = buffer->cursor.y * buffer->cols + buffer->cursor.x;
    buffer->cells[offset] = *attr;
    buffer->cells[offset].codepoint = codepoint;
    buffer->dirty_lines[buffer->cursor.y] = true;

    // Advance cursor
    buffer->cursor.x++;
}

/**
 * Write string
 */
void buffer_write_string(terminal_buffer_t *buffer, const char *str, uint32_t length) {
    if (!buffer || !str) {
        return;
    }

    for (uint32_t i = 0; i < length; i++) {
        cell_t attr = {
            .codepoint = str[i],
            .fg = color_from_rgb(192, 192, 192),
            .bg = color_from_rgb(0, 0, 0),
            .flags = 0
        };
        buffer_write_char(buffer, str[i], &attr);
    }
}

/**
 * Set cursor position
 */
void buffer_set_cursor(terminal_buffer_t *buffer, int32_t x, int32_t y) {
    if (!buffer) {
        return;
    }

    buffer->cursor.x = (x >= 0 && x < buffer->cols) ? x : 0;
    buffer->cursor.y = (y >= 0 && y < buffer->rows) ? y : 0;
}

/**
 * Move cursor relative
 */
void buffer_move_cursor(terminal_buffer_t *buffer, int32_t dx, int32_t dy) {
    if (!buffer) {
        return;
    }

    int32_t new_x = buffer->cursor.x + dx;
    int32_t new_y = buffer->cursor.y + dy;

    buffer_set_cursor(buffer, new_x, new_y);
}

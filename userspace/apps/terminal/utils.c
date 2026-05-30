/**
 * AutomationOS Terminal - Utility Functions and Stub Implementations
 *
 * Includes: Font rendering, PTY, Selection, Search, History, Scrollback, Hyperlinks, UTF-8
 */

#include "terminal.h"
#include "../../../userspace/libc/stdio.h"
#include "../../../userspace/libc/string.h"
#include <stdlib.h>

// ============================================================================
// FONT RENDERING (Stub)
// ============================================================================

font_context_t *font_create(const char *font_name, uint32_t font_size) {
    font_context_t *ctx = (font_context_t *)malloc(sizeof(font_context_t));
    if (!ctx) {
        return NULL;
    }

    memset(ctx, 0, sizeof(font_context_t));
    strncpy(ctx->font_name, font_name, MAX_FONT_NAME - 1);
    ctx->font_size = font_size;
    ctx->ligatures_enabled = false;

    // TODO: Load font file (FreeType or stb_truetype)
    // TODO: Create GPU texture atlas for glyphs

    return ctx;
}

void font_destroy(font_context_t *ctx) {
    if (ctx) {
        // TODO: Free font data and GPU textures
        free(ctx);
    }
}

void font_render_glyph(font_context_t *ctx, gpu_context_t *gpu, uint32_t codepoint,
                      int32_t x, int32_t y, const color_t *color) {
    // TODO: Render glyph from texture atlas
    // For now, just draw a rectangle as placeholder
}

void font_render_text(font_context_t *ctx, gpu_context_t *gpu, const char *text,
                     int32_t x, int32_t y, const color_t *color) {
    if (!text) return;

    int32_t offset_x = 0;
    for (const char *p = text; *p; p++) {
        font_render_glyph(ctx, gpu, *p, x + offset_x, y, color);
        offset_x += CELL_WIDTH;
    }
}

uint32_t font_get_glyph_width(font_context_t *ctx, uint32_t codepoint) {
    return CELL_WIDTH;
}

void font_enable_ligatures(font_context_t *ctx, bool enabled) {
    if (ctx) {
        ctx->ligatures_enabled = enabled;
    }
}

// ============================================================================
// PTY MANAGEMENT (Stub)
// ============================================================================

int32_t pty_open(uint32_t cols, uint32_t rows) {
    // TODO: Open PTY using openpty() or posix_openpt()
    // TODO: Set terminal size with ioctl TIOCSWINSZ
    return -1; // Stub
}

void pty_close(int32_t pty_fd) {
    if (pty_fd >= 0) {
        // TODO: Close PTY file descriptor
    }
}

void pty_resize(int32_t pty_fd, uint32_t cols, uint32_t rows) {
    if (pty_fd < 0) return;
    // TODO: Send TIOCSWINSZ ioctl to update window size
}

int32_t pty_spawn(int32_t pty_fd, const char *shell, char *const argv[]) {
    if (pty_fd < 0 || !shell) return -1;
    // TODO: Fork and exec shell with PTY as stdin/stdout/stderr
    return -1; // Stub
}

int32_t pty_read(int32_t pty_fd, uint8_t *buffer, uint32_t size) {
    if (pty_fd < 0 || !buffer) return -1;
    // TODO: Read from PTY (non-blocking)
    return 0; // Stub
}

int32_t pty_write(int32_t pty_fd, const uint8_t *buffer, uint32_t size) {
    if (pty_fd < 0 || !buffer) return -1;
    // TODO: Write to PTY
    return size; // Stub
}

// ============================================================================
// SELECTION
// ============================================================================

void selection_start(selection_t *sel, int32_t x, int32_t y) {
    if (!sel) return;
    sel->start_x = x;
    sel->start_y = y;
    sel->end_x = x;
    sel->end_y = y;
    sel->active = true;
}

void selection_update(selection_t *sel, int32_t x, int32_t y) {
    if (!sel) return;
    sel->end_x = x;
    sel->end_y = y;
}

void selection_end(selection_t *sel) {
    if (!sel) return;
    // Selection stays active for copying
}

void selection_clear(selection_t *sel) {
    if (!sel) return;
    sel->active = false;
}

bool selection_contains(const selection_t *sel, int32_t x, int32_t y) {
    if (!sel || !sel->active) return false;

    int32_t start_x = sel->start_x;
    int32_t start_y = sel->start_y;
    int32_t end_x = sel->end_x;
    int32_t end_y = sel->end_y;

    // Normalize
    if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
        int32_t tmp_x = start_x, tmp_y = start_y;
        start_x = end_x;
        start_y = end_y;
        end_x = tmp_x;
        end_y = tmp_y;
    }

    if (y < start_y || y > end_y) return false;
    if (y == start_y && x < start_x) return false;
    if (y == end_y && x > end_x) return false;

    return true;
}

char *selection_get_text(const terminal_buffer_t *buffer) {
    if (!buffer || !buffer->selection.active) return NULL;
    // TODO: Extract selected text from buffer
    return NULL;
}

void selection_copy_to_clipboard(terminal_t *term) {
    if (!term) return;
    // TODO: Copy selection to system clipboard
}

void selection_paste_from_clipboard(terminal_t *term) {
    if (!term) return;
    // TODO: Paste from system clipboard to active PTY
}

// ============================================================================
// SCROLLBACK
// ============================================================================

void scrollback_init(scrollback_t *scrollback) {
    if (!scrollback) return;
    memset(scrollback, 0, sizeof(scrollback_t));
}

void scrollback_push_line(scrollback_t *scrollback, const cell_t *line, uint32_t cols) {
    if (!scrollback || !line) return;

    // Allocate line buffer
    cell_t *new_line = (cell_t *)malloc(cols * sizeof(cell_t));
    if (!new_line) return;

    memcpy(new_line, line, cols * sizeof(cell_t));

    // Add to circular buffer
    if (scrollback->lines[scrollback->head]) {
        free(scrollback->lines[scrollback->head]);
    }

    scrollback->lines[scrollback->head] = new_line;
    scrollback->head = (scrollback->head + 1) % SCROLLBACK_SIZE;

    if (scrollback->count < SCROLLBACK_SIZE) {
        scrollback->count++;
    }
}

cell_t *scrollback_get_line(scrollback_t *scrollback, int32_t index) {
    if (!scrollback || index < 0 || index >= scrollback->count) {
        return NULL;
    }

    int32_t actual_index = (scrollback->head - scrollback->count + index + SCROLLBACK_SIZE) % SCROLLBACK_SIZE;
    return scrollback->lines[actual_index];
}

void scrollback_scroll_to(scrollback_t *scrollback, int32_t offset) {
    if (!scrollback) return;
    scrollback->view_offset = offset;

    if (scrollback->view_offset < 0) {
        scrollback->view_offset = 0;
    }
    if (scrollback->view_offset > scrollback->count) {
        scrollback->view_offset = scrollback->count;
    }
}

void scrollback_clear(scrollback_t *scrollback) {
    if (!scrollback) return;

    for (uint32_t i = 0; i < SCROLLBACK_SIZE; i++) {
        if (scrollback->lines[i]) {
            free(scrollback->lines[i]);
            scrollback->lines[i] = NULL;
        }
    }

    scrollback->head = 0;
    scrollback->count = 0;
    scrollback->view_offset = 0;
}

// ============================================================================
// SEARCH
// ============================================================================

void search_start(terminal_t *term, const char *query) {
    if (!term || !query) return;
    strncpy(term->search_query, query, sizeof(term->search_query) - 1);
    term->search_active = true;
}

void search_next(terminal_t *term) {
    if (!term || !term->search_active) return;
    // TODO: Find next match
}

void search_prev(terminal_t *term) {
    if (!term || !term->search_active) return;
    // TODO: Find previous match
}

void search_cancel(terminal_t *term) {
    if (!term) return;
    term->search_active = false;
    term->search_query[0] = '\0';
}

bool search_matches_at(const terminal_buffer_t *buffer, int32_t x, int32_t y, const char *query) {
    if (!buffer || !query || x < 0 || y < 0 || y >= buffer->rows) {
        return false;
    }

    uint32_t query_len = strlen(query);
    if (x + query_len > buffer->cols) {
        return false;
    }

    uint32_t line_offset = y * buffer->cols;
    for (uint32_t i = 0; i < query_len; i++) {
        if (buffer->cells[line_offset + x + i].codepoint != (uint32_t)query[i]) {
            return false;
        }
    }

    return true;
}

// ============================================================================
// HYPERLINKS
// ============================================================================

void hyperlink_add(terminal_buffer_t *buffer, int32_t x, int32_t y, const char *url) {
    if (!buffer || !url) return;

    hyperlink_t *link = (hyperlink_t *)malloc(sizeof(hyperlink_t));
    if (!link) return;

    link->x = x;
    link->y = y;
    link->length = strlen(url);
    strncpy(link->url, url, sizeof(link->url) - 1);
    link->url[sizeof(link->url) - 1] = '\0';

    link->next = buffer->hyperlinks;
    buffer->hyperlinks = link;

    // Mark cells as URLs
    uint32_t line_offset = y * buffer->cols;
    for (uint32_t i = 0; i < link->length && x + i < buffer->cols; i++) {
        buffer->cells[line_offset + x + i].flags |= CELL_URL;
    }
}

void hyperlink_remove_all(terminal_buffer_t *buffer) {
    if (!buffer) return;

    hyperlink_t *current = buffer->hyperlinks;
    while (current) {
        hyperlink_t *next = current->next;
        free(current);
        current = next;
    }

    buffer->hyperlinks = NULL;
}

hyperlink_t *hyperlink_at_position(const terminal_buffer_t *buffer, int32_t x, int32_t y) {
    if (!buffer) return NULL;

    hyperlink_t *current = buffer->hyperlinks;
    while (current) {
        if (current->y == y && x >= current->x && x < current->x + current->length) {
            return current;
        }
        current = current->next;
    }

    return NULL;
}

void hyperlink_open(const char *url) {
    if (!url) return;
    // TODO: Open URL in default browser
    printf("Opening URL: %s\n", url);
}

// ============================================================================
// COMMAND HISTORY
// ============================================================================

void history_add(terminal_t *term, const char *command) {
    if (!term || !command || command[0] == '\0') return;

    if (term->history_count >= MAX_COMMAND_HISTORY) {
        // Remove oldest
        free(term->command_history[0]);
        for (uint32_t i = 0; i < term->history_count - 1; i++) {
            term->command_history[i] = term->command_history[i + 1];
        }
        term->history_count--;
    }

    term->command_history[term->history_count] = strdup(command);
    term->history_count++;
    term->history_index = -1;
}

const char *history_get(terminal_t *term, int32_t index) {
    if (!term || index < 0 || index >= term->history_count) {
        return NULL;
    }
    return term->command_history[index];
}

void history_prev(terminal_t *term) {
    if (!term || term->history_count == 0) return;

    if (term->history_index < 0) {
        term->history_index = term->history_count - 1;
    } else if (term->history_index > 0) {
        term->history_index--;
    }
}

void history_next(terminal_t *term) {
    if (!term || term->history_count == 0) return;

    if (term->history_index >= 0) {
        term->history_index++;
        if (term->history_index >= term->history_count) {
            term->history_index = -1;
        }
    }
}

void history_reset(terminal_t *term) {
    if (!term) return;
    term->history_index = -1;
}

void history_save(terminal_t *term, const char *filename) {
    // TODO: Save history to file
}

void history_load(terminal_t *term, const char *filename) {
    // TODO: Load history from file
}

// ============================================================================
// UTF-8 UTILITIES
// ============================================================================

uint32_t utf8_decode(const char **str) {
    if (!str || !*str) return 0;

    const uint8_t *s = (const uint8_t *)*str;
    uint32_t codepoint = 0;

    if (s[0] < 0x80) {
        // 1-byte sequence
        codepoint = s[0];
        *str += 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        // 2-byte sequence
        codepoint = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *str += 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        // 3-byte sequence
        codepoint = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *str += 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        // 4-byte sequence
        codepoint = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
                   ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *str += 4;
    } else {
        // Invalid
        codepoint = 0xFFFD;
        *str += 1;
    }

    return codepoint;
}

uint32_t utf8_encode(uint32_t codepoint, char *buffer) {
    if (!buffer) return 0;

    if (codepoint < 0x80) {
        buffer[0] = codepoint;
        return 1;
    } else if (codepoint < 0x800) {
        buffer[0] = 0xC0 | (codepoint >> 6);
        buffer[1] = 0x80 | (codepoint & 0x3F);
        return 2;
    } else if (codepoint < 0x10000) {
        buffer[0] = 0xE0 | (codepoint >> 12);
        buffer[1] = 0x80 | ((codepoint >> 6) & 0x3F);
        buffer[2] = 0x80 | (codepoint & 0x3F);
        return 3;
    } else if (codepoint < 0x110000) {
        buffer[0] = 0xF0 | (codepoint >> 18);
        buffer[1] = 0x80 | ((codepoint >> 12) & 0x3F);
        buffer[2] = 0x80 | ((codepoint >> 6) & 0x3F);
        buffer[3] = 0x80 | (codepoint & 0x3F);
        return 4;
    }

    return 0;
}

bool utf8_is_continuation(uint8_t byte) {
    return (byte & 0xC0) == 0x80;
}

uint32_t utf8_strlen(const char *str) {
    if (!str) return 0;

    uint32_t len = 0;
    while (*str) {
        if (!utf8_is_continuation(*str)) {
            len++;
        }
        str++;
    }

    return len;
}

// ============================================================================
// RECTANGLE UTILITIES
// ============================================================================

bool rect_intersects(const rect_t *a, const rect_t *b) {
    if (!a || !b) return false;

    return !(a->x + a->w <= b->x ||
             b->x + b->w <= a->x ||
             a->y + a->h <= b->y ||
             b->y + b->h <= a->y);
}

void rect_union(rect_t *result, const rect_t *a, const rect_t *b) {
    if (!result || !a || !b) return;

    int32_t x1 = (a->x < b->x) ? a->x : b->x;
    int32_t y1 = (a->y < b->y) ? a->y : b->y;
    int32_t x2 = (a->x + a->w > b->x + b->w) ? a->x + a->w : b->x + b->w;
    int32_t y2 = (a->y + a->h > b->y + b->h) ? a->y + a->h : b->y + b->h;

    result->x = x1;
    result->y = y1;
    result->w = x2 - x1;
    result->h = y2 - y1;
}

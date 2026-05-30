/**
 * AutomationOS Terminal - VT100/ANSI Escape Sequence Parser
 *
 * Implements VT100, VT220, xterm, and modern terminal escape sequences
 * Supports 256-color, true color, hyperlinks, and more
 */

#include "terminal.h"
#include "../../../userspace/libc/stdio.h"
#include "../../../userspace/libc/string.h"
#include <stdlib.h>

/**
 * Parser states (state machine)
 */
typedef enum {
    STATE_GROUND,           // Normal text
    STATE_ESCAPE,           // ESC received
    STATE_ESCAPE_INTERMEDIATE,
    STATE_CSI_ENTRY,        // CSI sequence started
    STATE_CSI_PARAM,        // CSI parameters
    STATE_CSI_INTERMEDIATE,
    STATE_CSI_IGNORE,
    STATE_DCS_ENTRY,        // Device Control String
    STATE_DCS_PARAM,
    STATE_DCS_INTERMEDIATE,
    STATE_DCS_PASSTHROUGH,
    STATE_DCS_IGNORE,
    STATE_OSC_STRING,       // Operating System Command
    STATE_SOS_PM_APC_STRING,
    STATE_UTF8_2,           // UTF-8 continuation bytes
    STATE_UTF8_3,
    STATE_UTF8_4,
} parser_state_t;

/**
 * Parser structure
 */
struct vt_parser {
    terminal_buffer_t *buffer;
    parser_state_t state;

    // Parameter storage
    int32_t params[32];
    uint32_t param_count;
    uint32_t current_param;

    // Intermediate bytes
    uint8_t intermediates[4];
    uint32_t intermediate_count;

    // OSC string buffer
    char osc_buffer[2048];
    uint32_t osc_length;

    // UTF-8 decoding
    uint32_t utf8_codepoint;
    uint32_t utf8_remaining;

    // Cursor save/restore
    int32_t saved_cursor_x;
    int32_t saved_cursor_y;

    // Current character attributes
    cell_t current_attr;

    // Tabs (every 8 columns)
    bool tab_stops[256];

    // Modes
    bool insert_mode;
    bool autowrap_mode;
    bool origin_mode;
    bool cursor_visible;
    bool cursor_keys_application_mode;
    bool keypad_application_mode;

    // Charset state (G0, G1)
    uint8_t charset_g0;
    uint8_t charset_g1;
    uint8_t active_charset;
};

// Forward declarations
static void execute_control(vt_parser_t *parser, uint8_t byte);
static void execute_csi(vt_parser_t *parser, uint8_t final);
static void execute_osc(vt_parser_t *parser);
static void execute_escape(vt_parser_t *parser, uint8_t final);
static void print_char(vt_parser_t *parser, uint32_t codepoint);

/**
 * Create VT parser
 */
vt_parser_t *vt_parser_create(terminal_buffer_t *buffer) {
    vt_parser_t *parser = (vt_parser_t *)malloc(sizeof(vt_parser_t));
    if (!parser) {
        return NULL;
    }

    memset(parser, 0, sizeof(vt_parser_t));
    parser->buffer = buffer;
    parser->state = STATE_GROUND;

    // Initialize default attributes
    parser->current_attr.fg = color_from_rgb(192, 192, 192);
    parser->current_attr.bg = color_from_rgb(0, 0, 0);
    parser->current_attr.flags = 0;

    // Set default tab stops (every 8 columns)
    for (uint32_t i = 0; i < 256; i += 8) {
        parser->tab_stops[i] = true;
    }

    // Default modes
    parser->autowrap_mode = true;
    parser->cursor_visible = true;

    return parser;
}

/**
 * Destroy parser
 */
void vt_parser_destroy(vt_parser_t *parser) {
    if (parser) {
        free(parser);
    }
}

/**
 * Reset parser state
 */
void vt_parser_reset(vt_parser_t *parser) {
    if (!parser) {
        return;
    }

    parser->state = STATE_GROUND;
    parser->param_count = 0;
    parser->current_param = 0;
    parser->intermediate_count = 0;
    parser->osc_length = 0;
}

/**
 * Process input data
 */
void vt_parser_process(vt_parser_t *parser, const uint8_t *data, uint32_t length) {
    if (!parser || !data) {
        return;
    }

    for (uint32_t i = 0; i < length; i++) {
        uint8_t byte = data[i];

        switch (parser->state) {
            case STATE_GROUND:
                if (byte < 0x20) {
                    // Control character
                    execute_control(parser, byte);
                } else if (byte >= 0x20 && byte < 0x7F) {
                    // Printable ASCII
                    print_char(parser, byte);
                } else if (byte == 0x7F) {
                    // DEL - ignore
                } else if (byte >= 0x80 && byte < 0xC0) {
                    // Invalid UTF-8 start byte
                    print_char(parser, 0xFFFD); // Replacement character
                } else if (byte >= 0xC0 && byte < 0xE0) {
                    // UTF-8 2-byte sequence
                    parser->utf8_codepoint = (byte & 0x1F) << 6;
                    parser->utf8_remaining = 1;
                    parser->state = STATE_UTF8_2;
                } else if (byte >= 0xE0 && byte < 0xF0) {
                    // UTF-8 3-byte sequence
                    parser->utf8_codepoint = (byte & 0x0F) << 12;
                    parser->utf8_remaining = 2;
                    parser->state = STATE_UTF8_3;
                } else if (byte >= 0xF0 && byte < 0xF8) {
                    // UTF-8 4-byte sequence
                    parser->utf8_codepoint = (byte & 0x07) << 18;
                    parser->utf8_remaining = 3;
                    parser->state = STATE_UTF8_4;
                }
                break;

            case STATE_UTF8_2:
            case STATE_UTF8_3:
            case STATE_UTF8_4:
                if ((byte & 0xC0) == 0x80) {
                    // Valid continuation byte
                    parser->utf8_codepoint |= (byte & 0x3F) << (6 * (parser->utf8_remaining - 1));
                    parser->utf8_remaining--;

                    if (parser->utf8_remaining == 0) {
                        print_char(parser, parser->utf8_codepoint);
                        parser->state = STATE_GROUND;
                    }
                } else {
                    // Invalid continuation byte
                    print_char(parser, 0xFFFD);
                    parser->state = STATE_GROUND;
                    // Re-process this byte
                    i--;
                }
                break;

            case STATE_ESCAPE:
                if (byte == '[') {
                    // CSI
                    parser->state = STATE_CSI_ENTRY;
                    parser->param_count = 0;
                    parser->current_param = 0;
                    parser->intermediate_count = 0;
                } else if (byte == ']') {
                    // OSC
                    parser->state = STATE_OSC_STRING;
                    parser->osc_length = 0;
                } else if (byte == 'P') {
                    // DCS
                    parser->state = STATE_DCS_ENTRY;
                } else if (byte >= 0x20 && byte < 0x30) {
                    // Intermediate
                    parser->state = STATE_ESCAPE_INTERMEDIATE;
                    parser->intermediates[0] = byte;
                    parser->intermediate_count = 1;
                } else if (byte >= 0x30 && byte < 0x7F) {
                    // Final byte
                    execute_escape(parser, byte);
                    parser->state = STATE_GROUND;
                }
                break;

            case STATE_ESCAPE_INTERMEDIATE:
                if (byte >= 0x20 && byte < 0x30) {
                    // Another intermediate
                    if (parser->intermediate_count < 4) {
                        parser->intermediates[parser->intermediate_count++] = byte;
                    }
                } else if (byte >= 0x30 && byte < 0x7F) {
                    // Final byte
                    execute_escape(parser, byte);
                    parser->state = STATE_GROUND;
                }
                break;

            case STATE_CSI_ENTRY:
            case STATE_CSI_PARAM:
                if (byte >= '0' && byte <= '9') {
                    parser->current_param = parser->current_param * 10 + (byte - '0');
                    parser->state = STATE_CSI_PARAM;
                } else if (byte == ';') {
                    if (parser->param_count < 32) {
                        parser->params[parser->param_count++] = parser->current_param;
                    }
                    parser->current_param = 0;
                    parser->state = STATE_CSI_PARAM;
                } else if (byte >= 0x20 && byte < 0x30) {
                    // Intermediate
                    if (parser->param_count < 32) {
                        parser->params[parser->param_count++] = parser->current_param;
                    }
                    parser->current_param = 0;
                    parser->state = STATE_CSI_INTERMEDIATE;
                    parser->intermediates[0] = byte;
                    parser->intermediate_count = 1;
                } else if (byte >= 0x40 && byte < 0x7F) {
                    // Final byte
                    if (parser->param_count < 32) {
                        parser->params[parser->param_count++] = parser->current_param;
                    }
                    execute_csi(parser, byte);
                    parser->state = STATE_GROUND;
                }
                break;

            case STATE_CSI_INTERMEDIATE:
                if (byte >= 0x20 && byte < 0x30) {
                    // Another intermediate
                    if (parser->intermediate_count < 4) {
                        parser->intermediates[parser->intermediate_count++] = byte;
                    }
                } else if (byte >= 0x40 && byte < 0x7F) {
                    // Final byte
                    execute_csi(parser, byte);
                    parser->state = STATE_GROUND;
                }
                break;

            case STATE_OSC_STRING:
                if (byte == 0x07 || byte == 0x1B) {
                    // BEL or ESC terminates OSC
                    if (byte == 0x1B) {
                        // Next byte should be backslash
                        if (i + 1 < length && data[i + 1] == '\\') {
                            i++; // Skip backslash
                        }
                    }
                    execute_osc(parser);
                    parser->state = STATE_GROUND;
                } else {
                    // Add to OSC buffer
                    if (parser->osc_length < sizeof(parser->osc_buffer) - 1) {
                        parser->osc_buffer[parser->osc_length++] = byte;
                    }
                }
                break;
        }

        // ESC transitions from any state
        if (byte == 0x1B && parser->state != STATE_ESCAPE) {
            parser->state = STATE_ESCAPE;
        }
    }
}

/**
 * Execute control character
 */
static void execute_control(vt_parser_t *parser, uint8_t byte) {
    terminal_buffer_t *buffer = parser->buffer;

    switch (byte) {
        case 0x07: // BEL - Bell
            // TODO: Trigger bell
            break;

        case 0x08: // BS - Backspace
            if (buffer->cursor.x > 0) {
                buffer->cursor.x--;
            }
            break;

        case 0x09: // HT - Horizontal Tab
            {
                // Move to next tab stop
                uint32_t next_tab = buffer->cursor.x + 1;
                while (next_tab < buffer->cols && !parser->tab_stops[next_tab]) {
                    next_tab++;
                }
                buffer->cursor.x = (next_tab < buffer->cols) ? next_tab : buffer->cols - 1;
            }
            break;

        case 0x0A: // LF - Line Feed
        case 0x0B: // VT - Vertical Tab
        case 0x0C: // FF - Form Feed
            buffer->cursor.y++;
            if (buffer->cursor.y >= buffer->rows) {
                // Scroll up
                buffer_scroll_up(buffer, 1);
                buffer->cursor.y = buffer->rows - 1;
            }
            break;

        case 0x0D: // CR - Carriage Return
            buffer->cursor.x = 0;
            break;

        case 0x0E: // SO - Shift Out (G1)
            parser->active_charset = 1;
            break;

        case 0x0F: // SI - Shift In (G0)
            parser->active_charset = 0;
            break;
    }
}

/**
 * Execute CSI sequence
 */
static void execute_csi(vt_parser_t *parser, uint8_t final) {
    terminal_buffer_t *buffer = parser->buffer;
    int32_t *params = parser->params;
    uint32_t count = parser->param_count;

    // Default parameter values
    for (uint32_t i = count; i < 32; i++) {
        params[i] = 0;
    }

    switch (final) {
        case 'A': // CUU - Cursor Up
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                buffer->cursor.y = (buffer->cursor.y >= n) ? buffer->cursor.y - n : 0;
            }
            break;

        case 'B': // CUD - Cursor Down
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                buffer->cursor.y += n;
                if (buffer->cursor.y >= buffer->rows) {
                    buffer->cursor.y = buffer->rows - 1;
                }
            }
            break;

        case 'C': // CUF - Cursor Forward
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                buffer->cursor.x += n;
                if (buffer->cursor.x >= buffer->cols) {
                    buffer->cursor.x = buffer->cols - 1;
                }
            }
            break;

        case 'D': // CUB - Cursor Back
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                buffer->cursor.x = (buffer->cursor.x >= n) ? buffer->cursor.x - n : 0;
            }
            break;

        case 'E': // CNL - Cursor Next Line
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                buffer->cursor.y += n;
                buffer->cursor.x = 0;
                if (buffer->cursor.y >= buffer->rows) {
                    buffer->cursor.y = buffer->rows - 1;
                }
            }
            break;

        case 'F': // CPL - Cursor Previous Line
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                buffer->cursor.y = (buffer->cursor.y >= n) ? buffer->cursor.y - n : 0;
                buffer->cursor.x = 0;
            }
            break;

        case 'G': // CHA - Cursor Horizontal Absolute
            {
                int32_t col = (count > 0 && params[0] > 0) ? params[0] - 1 : 0;
                buffer->cursor.x = (col < buffer->cols) ? col : buffer->cols - 1;
            }
            break;

        case 'H': // CUP - Cursor Position
        case 'f': // HVP - Horizontal Vertical Position
            {
                int32_t row = (count > 0 && params[0] > 0) ? params[0] - 1 : 0;
                int32_t col = (count > 1 && params[1] > 0) ? params[1] - 1 : 0;
                buffer->cursor.y = (row < buffer->rows) ? row : buffer->rows - 1;
                buffer->cursor.x = (col < buffer->cols) ? col : buffer->cols - 1;
            }
            break;

        case 'J': // ED - Erase in Display
            {
                int32_t mode = (count > 0) ? params[0] : 0;
                if (mode == 0) {
                    // Clear from cursor to end of screen
                    // TODO: Implement
                } else if (mode == 1) {
                    // Clear from start to cursor
                    // TODO: Implement
                } else if (mode == 2 || mode == 3) {
                    // Clear entire screen
                    buffer_clear(buffer);
                }
            }
            break;

        case 'K': // EL - Erase in Line
            {
                int32_t mode = (count > 0) ? params[0] : 0;
                uint32_t line_offset = buffer->cursor.y * buffer->cols;

                if (mode == 0) {
                    // Clear from cursor to end of line
                    for (uint32_t i = buffer->cursor.x; i < buffer->cols; i++) {
                        buffer->cells[line_offset + i] = (cell_t){
                            .codepoint = ' ',
                            .fg = parser->current_attr.fg,
                            .bg = parser->current_attr.bg,
                            .flags = 0
                        };
                    }
                } else if (mode == 1) {
                    // Clear from start to cursor
                    for (uint32_t i = 0; i <= buffer->cursor.x; i++) {
                        buffer->cells[line_offset + i] = (cell_t){
                            .codepoint = ' ',
                            .fg = parser->current_attr.fg,
                            .bg = parser->current_attr.bg,
                            .flags = 0
                        };
                    }
                } else if (mode == 2) {
                    // Clear entire line
                    for (uint32_t i = 0; i < buffer->cols; i++) {
                        buffer->cells[line_offset + i] = (cell_t){
                            .codepoint = ' ',
                            .fg = parser->current_attr.fg,
                            .bg = parser->current_attr.bg,
                            .flags = 0
                        };
                    }
                }
                buffer->dirty_lines[buffer->cursor.y] = true;
            }
            break;

        case 'L': // IL - Insert Lines
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                // TODO: Implement
            }
            break;

        case 'M': // DL - Delete Lines
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                // TODO: Implement
            }
            break;

        case 'P': // DCH - Delete Characters
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                // TODO: Implement
            }
            break;

        case 'S': // SU - Scroll Up
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                buffer_scroll_up(buffer, n);
            }
            break;

        case 'T': // SD - Scroll Down
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                buffer_scroll_down(buffer, n);
            }
            break;

        case 'X': // ECH - Erase Characters
            {
                int32_t n = (count > 0 && params[0] > 0) ? params[0] : 1;
                // TODO: Implement
            }
            break;

        case 'm': // SGR - Select Graphic Rendition
            {
                if (count == 0) {
                    // Reset to default
                    parser->current_attr.fg = color_from_rgb(192, 192, 192);
                    parser->current_attr.bg = color_from_rgb(0, 0, 0);
                    parser->current_attr.flags = 0;
                } else {
                    for (uint32_t i = 0; i < count; i++) {
                        int32_t param = params[i];

                        if (param == 0) {
                            // Reset
                            parser->current_attr.fg = color_from_rgb(192, 192, 192);
                            parser->current_attr.bg = color_from_rgb(0, 0, 0);
                            parser->current_attr.flags = 0;
                        } else if (param == 1) {
                            parser->current_attr.flags |= CELL_BOLD;
                        } else if (param == 2) {
                            parser->current_attr.flags |= CELL_DIM;
                        } else if (param == 3) {
                            parser->current_attr.flags |= CELL_ITALIC;
                        } else if (param == 4) {
                            parser->current_attr.flags |= CELL_UNDERLINE;
                        } else if (param == 5) {
                            parser->current_attr.flags |= CELL_BLINK;
                        } else if (param == 7) {
                            parser->current_attr.flags |= CELL_REVERSE;
                        } else if (param == 8) {
                            parser->current_attr.flags |= CELL_HIDDEN;
                        } else if (param == 9) {
                            parser->current_attr.flags |= CELL_STRIKETHROUGH;
                        } else if (param >= 30 && param <= 37) {
                            // Set foreground color (ANSI)
                            uint8_t idx = param - 30;
                            // TODO: Get color from theme
                            parser->current_attr.fg = color_from_rgb(
                                (idx & 1) ? 255 : 0,
                                (idx & 2) ? 255 : 0,
                                (idx & 4) ? 255 : 0
                            );
                        } else if (param == 38) {
                            // Extended foreground color
                            if (i + 1 < count && params[i + 1] == 5 && i + 2 < count) {
                                // 256-color mode
                                uint8_t color_idx = params[i + 2];
                                // TODO: Get color from 256-color palette
                                i += 2;
                            } else if (i + 1 < count && params[i + 1] == 2 && i + 4 < count) {
                                // True color mode
                                parser->current_attr.fg = color_from_rgb(
                                    params[i + 2],
                                    params[i + 3],
                                    params[i + 4]
                                );
                                i += 4;
                            }
                        } else if (param >= 40 && param <= 47) {
                            // Set background color (ANSI)
                            uint8_t idx = param - 40;
                            parser->current_attr.bg = color_from_rgb(
                                (idx & 1) ? 255 : 0,
                                (idx & 2) ? 255 : 0,
                                (idx & 4) ? 255 : 0
                            );
                        } else if (param == 48) {
                            // Extended background color
                            if (i + 1 < count && params[i + 1] == 5 && i + 2 < count) {
                                // 256-color mode
                                uint8_t color_idx = params[i + 2];
                                // TODO: Get color from 256-color palette
                                i += 2;
                            } else if (i + 1 < count && params[i + 1] == 2 && i + 4 < count) {
                                // True color mode
                                parser->current_attr.bg = color_from_rgb(
                                    params[i + 2],
                                    params[i + 3],
                                    params[i + 4]
                                );
                                i += 4;
                            }
                        }
                    }
                }
            }
            break;

        case 's': // SCP - Save Cursor Position
            parser->saved_cursor_x = buffer->cursor.x;
            parser->saved_cursor_y = buffer->cursor.y;
            break;

        case 'u': // RCP - Restore Cursor Position
            buffer->cursor.x = parser->saved_cursor_x;
            buffer->cursor.y = parser->saved_cursor_y;
            break;

        case 'h': // SM - Set Mode
        case 'l': // RM - Reset Mode
            {
                bool set = (final == 'h');
                // TODO: Handle various modes
            }
            break;
    }
}

/**
 * Execute OSC sequence
 */
static void execute_osc(vt_parser_t *parser) {
    char *osc = parser->osc_buffer;
    parser->osc_buffer[parser->osc_length] = '\0';

    // Parse OSC command
    int32_t command = 0;
    char *separator = strchr(osc, ';');

    if (separator) {
        *separator = '\0';
        command = atoi(osc);
        char *data = separator + 1;

        switch (command) {
            case 0: // Change icon name and window title
            case 2: // Change window title
                // TODO: Set window title
                break;

            case 8: // Hyperlink
                {
                    // Format: OSC 8 ; params ; URI ST
                    char *uri_start = strchr(data, ';');
                    if (uri_start) {
                        uri_start++;
                        hyperlink_add(parser->buffer,
                                    parser->buffer->cursor.x,
                                    parser->buffer->cursor.y,
                                    uri_start);
                    }
                }
                break;

            case 10: // Set foreground color
            case 11: // Set background color
            case 12: // Set cursor color
                // TODO: Set colors
                break;
        }
    }
}

/**
 * Execute escape sequence
 */
static void execute_escape(vt_parser_t *parser, uint8_t final) {
    switch (final) {
        case 'D': // IND - Index (move down)
            parser->buffer->cursor.y++;
            if (parser->buffer->cursor.y >= parser->buffer->rows) {
                buffer_scroll_up(parser->buffer, 1);
                parser->buffer->cursor.y = parser->buffer->rows - 1;
            }
            break;

        case 'E': // NEL - Next Line
            parser->buffer->cursor.x = 0;
            parser->buffer->cursor.y++;
            if (parser->buffer->cursor.y >= parser->buffer->rows) {
                buffer_scroll_up(parser->buffer, 1);
                parser->buffer->cursor.y = parser->buffer->rows - 1;
            }
            break;

        case 'M': // RI - Reverse Index (move up)
            if (parser->buffer->cursor.y > 0) {
                parser->buffer->cursor.y--;
            } else {
                // Scroll down
                buffer_scroll_down(parser->buffer, 1);
            }
            break;

        case '7': // DECSC - Save Cursor
            parser->saved_cursor_x = parser->buffer->cursor.x;
            parser->saved_cursor_y = parser->buffer->cursor.y;
            break;

        case '8': // DECRC - Restore Cursor
            parser->buffer->cursor.x = parser->saved_cursor_x;
            parser->buffer->cursor.y = parser->saved_cursor_y;
            break;

        case 'c': // RIS - Reset to Initial State
            vt_parser_reset(parser);
            buffer_clear(parser->buffer);
            break;
    }
}

/**
 * Print character to buffer
 */
static void print_char(vt_parser_t *parser, uint32_t codepoint) {
    terminal_buffer_t *buffer = parser->buffer;

    // Check for autowrap
    if (buffer->cursor.x >= buffer->cols) {
        if (parser->autowrap_mode) {
            buffer->cursor.x = 0;
            buffer->cursor.y++;
            if (buffer->cursor.y >= buffer->rows) {
                buffer_scroll_up(buffer, 1);
                buffer->cursor.y = buffer->rows - 1;
            }
        } else {
            buffer->cursor.x = buffer->cols - 1;
        }
    }

    // Write character
    uint32_t offset = buffer->cursor.y * buffer->cols + buffer->cursor.x;
    buffer->cells[offset] = (cell_t){
        .codepoint = codepoint,
        .fg = parser->current_attr.fg,
        .bg = parser->current_attr.bg,
        .flags = parser->current_attr.flags
    };

    buffer->dirty_lines[buffer->cursor.y] = true;
    buffer->cursor.x++;
}

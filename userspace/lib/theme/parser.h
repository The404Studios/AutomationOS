/**
 * AutomationOS Theme Parser
 *
 * Parses CSS-like theme configuration files into theme_t structures.
 *
 * SYNTAX EXAMPLE:
 * ---------------
 * [metadata]
 * name = "Beautiful Dark"
 * author = "AutomationOS Team"
 * version = "1.0"
 * mode = dark
 *
 * [colors]
 * primary = #0A84FF
 * background = #1C1C1E
 * text = #FFFFFF
 *
 * [window]
 * titlebar_height = 32
 * border_width = 1
 * corner_radius = 8
 */

#ifndef THEME_PARSER_H
#define THEME_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include "theme.h"

// ============================================================================
// PARSER TYPES
// ============================================================================

/**
 * Parse error
 */
typedef struct {
    uint32_t line;
    uint32_t column;
    char message[256];
} parse_error_t;

/**
 * Parser state
 */
typedef struct {
    const char *input;
    size_t length;
    size_t position;

    uint32_t current_line;
    uint32_t current_column;

    // Error tracking
    parse_error_t errors[32];
    uint32_t error_count;

    // Current section being parsed
    char current_section[64];
} parser_t;

// ============================================================================
// PARSER API
// ============================================================================

/**
 * Parse theme from file
 *
 * @param path Path to theme.conf
 * @return Parsed theme or NULL on error
 */
theme_t *theme_parse_file(const char *path);

/**
 * Parse theme from string
 *
 * @param input Theme configuration string
 * @return Parsed theme or NULL on error
 */
theme_t *theme_parse_string(const char *input);

/**
 * Get parse errors from last operation
 *
 * @param buffer Buffer to write errors
 * @param size Buffer size
 * @return Number of errors
 */
int theme_get_parse_errors(char *buffer, size_t size);

// ============================================================================
// INTERNAL PARSER FUNCTIONS
// ============================================================================

/**
 * Initialize parser
 */
parser_t *parser_init(const char *input, size_t length);

/**
 * Cleanup parser
 */
void parser_cleanup(parser_t *parser);

/**
 * Parse entire theme
 */
bool parser_parse_theme(parser_t *parser, theme_t *theme);

/**
 * Parse section header [section_name]
 */
bool parser_parse_section(parser_t *parser);

/**
 * Parse key-value pair: key = value
 */
bool parser_parse_property(parser_t *parser, theme_t *theme);

/**
 * Skip whitespace and comments
 */
void parser_skip_whitespace(parser_t *parser);

/**
 * Check if at end of input
 */
bool parser_at_end(parser_t *parser);

/**
 * Peek current character without advancing
 */
char parser_peek(parser_t *parser);

/**
 * Peek character at offset without advancing
 */
char parser_peek_offset(parser_t *parser, size_t offset);

/**
 * Advance and return current character
 */
char parser_advance(parser_t *parser);

/**
 * Check if current character matches expected
 */
bool parser_match(parser_t *parser, char expected);

/**
 * Add parse error
 */
void parser_error(parser_t *parser, const char *message);

// ============================================================================
// VALUE PARSERS
// ============================================================================

/**
 * Parse string value: "hello world"
 */
bool parser_parse_string(parser_t *parser, char *buffer, size_t size);

/**
 * Parse integer value: 42
 */
bool parser_parse_int(parser_t *parser, int32_t *out);

/**
 * Parse unsigned integer: 100
 */
bool parser_parse_uint(parser_t *parser, uint32_t *out);

/**
 * Parse float value: 1.5
 */
bool parser_parse_float(parser_t *parser, float *out);

/**
 * Parse boolean: true, false, yes, no, on, off
 */
bool parser_parse_bool(parser_t *parser, bool *out);

/**
 * Parse color: #RRGGBB or #RRGGBBAA
 */
bool parser_parse_color(parser_t *parser, color_rgba_t *out);

/**
 * Parse RGB color: rgb(255, 128, 0)
 */
bool parser_parse_rgb(parser_t *parser, color_rgba_t *out);

/**
 * Parse RGBA color: rgba(255, 128, 0, 128)
 */
bool parser_parse_rgba(parser_t *parser, color_rgba_t *out);

/**
 * Parse theme mode: light, dark, auto
 */
bool parser_parse_theme_mode(parser_t *parser, theme_mode_t *out);

/**
 * Parse shadow: 0 2 4 0 0.08
 * Format: offset_x offset_y blur spread opacity
 */
bool parser_parse_shadow(parser_t *parser, shadow_t *out);

// ============================================================================
// SECTION PARSERS
// ============================================================================

/**
 * Parse [metadata] section
 */
bool parser_parse_metadata_section(parser_t *parser, theme_t *theme);

/**
 * Parse [colors] section
 */
bool parser_parse_colors_section(parser_t *parser, theme_t *theme);

/**
 * Parse [window] section
 */
bool parser_parse_window_section(parser_t *parser, theme_t *theme);

/**
 * Parse [panel] section
 */
bool parser_parse_panel_section(parser_t *parser, theme_t *theme);

/**
 * Parse [dock] section
 */
bool parser_parse_dock_section(parser_t *parser, theme_t *theme);

/**
 * Parse [menu] section
 */
bool parser_parse_menu_section(parser_t *parser, theme_t *theme);

/**
 * Parse [notification] section
 */
bool parser_parse_notification_section(parser_t *parser, theme_t *theme);

/**
 * Parse [typography] section
 */
bool parser_parse_typography_section(parser_t *parser, theme_t *theme);

/**
 * Parse [animations] section
 */
bool parser_parse_animations_section(parser_t *parser, theme_t *theme);

/**
 * Parse [accessibility] section
 */
bool parser_parse_accessibility_section(parser_t *parser, theme_t *theme);

// ============================================================================
// THEME GENERATION (Reverse Operation)
// ============================================================================

/**
 * Generate theme configuration string from theme_t
 *
 * @param theme Theme to serialize
 * @param buffer Buffer to write configuration
 * @param size Buffer size
 * @return Number of bytes written or -1 on error
 */
int theme_generate_string(const theme_t *theme, char *buffer, size_t size);

/**
 * Save theme to file
 *
 * @param theme Theme to save
 * @param path Path to write theme.conf
 * @return 0 on success, -1 on error
 */
int theme_save_file(const theme_t *theme, const char *path);

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * Convert color to hex string: #RRGGBB or #RRGGBBAA
 */
void color_to_hex_string(color_rgba_t color, char *buffer, size_t size, bool include_alpha);

/**
 * Convert shadow to string: offset_x offset_y blur spread opacity
 */
void shadow_to_string(shadow_t shadow, char *buffer, size_t size);

/**
 * Validate theme configuration syntax without creating theme
 *
 * @param path Path to theme.conf
 * @param errors Buffer for error messages
 * @param error_size Error buffer size
 * @return true if valid, false otherwise
 */
bool theme_validate_syntax(const char *path, char *errors, size_t error_size);

#endif // THEME_PARSER_H

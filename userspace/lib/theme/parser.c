/**
 * AutomationOS Theme Parser Implementation
 */

#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

// Thread-local storage for parse errors
static __thread parse_error_t g_errors[32];
static __thread uint32_t g_error_count = 0;

// ============================================================================
// ERROR HANDLING
// ============================================================================

static void add_error(parser_t *parser, const char *format, ...) {
    if (g_error_count >= 32) return;

    va_list args;
    va_start(args, format);

    parse_error_t *err = &g_errors[g_error_count++];
    err->line = parser->current_line;
    err->column = parser->current_column;
    vsnprintf(err->message, sizeof(err->message), format, args);

    va_end(args);
}

int theme_get_parse_errors(char *buffer, size_t size) {
    if (!buffer || size == 0) return g_error_count;

    char *ptr = buffer;
    size_t remaining = size;

    for (uint32_t i = 0; i < g_error_count; i++) {
        parse_error_t *err = &g_errors[i];
        int written = snprintf(ptr, remaining, "Line %u:%u: %s\n",
                              err->line, err->column, err->message);
        ptr += written;
        remaining -= written;
    }

    return g_error_count;
}

// ============================================================================
// PARSER INITIALIZATION
// ============================================================================

parser_t *parser_init(const char *input, size_t length) {
    parser_t *parser = calloc(1, sizeof(parser_t));
    if (!parser) return NULL;

    parser->input = input;
    parser->length = length;
    parser->position = 0;
    parser->current_line = 1;
    parser->current_column = 1;
    parser->error_count = 0;

    g_error_count = 0;

    return parser;
}

void parser_cleanup(parser_t *parser) {
    free(parser);
}

// ============================================================================
// PARSER UTILITIES
// ============================================================================

bool parser_at_end(parser_t *parser) {
    return parser->position >= parser->length;
}

char parser_peek(parser_t *parser) {
    if (parser_at_end(parser)) return '\0';
    return parser->input[parser->position];
}

char parser_peek_offset(parser_t *parser, size_t offset) {
    if (parser->position + offset >= parser->length) return '\0';
    return parser->input[parser->position + offset];
}

char parser_advance(parser_t *parser) {
    if (parser_at_end(parser)) return '\0';

    char c = parser->input[parser->position++];

    if (c == '\n') {
        parser->current_line++;
        parser->current_column = 1;
    } else {
        parser->current_column++;
    }

    return c;
}

bool parser_match(parser_t *parser, char expected) {
    if (parser_peek(parser) == expected) {
        parser_advance(parser);
        return true;
    }
    return false;
}

void parser_skip_whitespace(parser_t *parser) {
    while (!parser_at_end(parser)) {
        char c = parser_peek(parser);

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            parser_advance(parser);
        } else if (c == '#') {
            // Skip comment line
            while (!parser_at_end(parser) && parser_peek(parser) != '\n') {
                parser_advance(parser);
            }
        } else {
            break;
        }
    }
}

// ============================================================================
// VALUE PARSERS
// ============================================================================

bool parser_parse_string(parser_t *parser, char *buffer, size_t size) {
    parser_skip_whitespace(parser);

    if (!parser_match(parser, '"')) {
        add_error(parser, "Expected string (missing opening quote)");
        return false;
    }

    size_t i = 0;
    while (!parser_at_end(parser) && parser_peek(parser) != '"') {
        if (i >= size - 1) {
            add_error(parser, "String too long");
            return false;
        }

        char c = parser_advance(parser);
        if (c == '\\') {
            // Handle escape sequences
            if (parser_at_end(parser)) break;
            c = parser_advance(parser);
            switch (c) {
                case 'n': buffer[i++] = '\n'; break;
                case 't': buffer[i++] = '\t'; break;
                case 'r': buffer[i++] = '\r'; break;
                case '\\': buffer[i++] = '\\'; break;
                case '"': buffer[i++] = '"'; break;
                default: buffer[i++] = c;
            }
        } else {
            buffer[i++] = c;
        }
    }

    buffer[i] = '\0';

    if (!parser_match(parser, '"')) {
        add_error(parser, "Expected closing quote");
        return false;
    }

    return true;
}

bool parser_parse_int(parser_t *parser, int32_t *out) {
    parser_skip_whitespace(parser);

    char buffer[32];
    size_t i = 0;

    // Handle negative sign
    if (parser_peek(parser) == '-') {
        buffer[i++] = parser_advance(parser);
    }

    while (!parser_at_end(parser) && isdigit(parser_peek(parser))) {
        if (i >= sizeof(buffer) - 1) {
            add_error(parser, "Integer too long");
            return false;
        }
        buffer[i++] = parser_advance(parser);
    }

    buffer[i] = '\0';

    if (i == 0 || (i == 1 && buffer[0] == '-')) {
        add_error(parser, "Expected integer");
        return false;
    }

    *out = atoi(buffer);
    return true;
}

bool parser_parse_uint(parser_t *parser, uint32_t *out) {
    int32_t val;
    if (!parser_parse_int(parser, &val)) return false;

    if (val < 0) {
        add_error(parser, "Expected unsigned integer");
        return false;
    }

    *out = (uint32_t)val;
    return true;
}

bool parser_parse_float(parser_t *parser, float *out) {
    parser_skip_whitespace(parser);

    char buffer[32];
    size_t i = 0;

    // Handle negative sign
    if (parser_peek(parser) == '-') {
        buffer[i++] = parser_advance(parser);
    }

    // Parse integer part
    while (!parser_at_end(parser) && isdigit(parser_peek(parser))) {
        if (i >= sizeof(buffer) - 1) {
            add_error(parser, "Float too long");
            return false;
        }
        buffer[i++] = parser_advance(parser);
    }

    // Parse decimal part
    if (parser_peek(parser) == '.') {
        buffer[i++] = parser_advance(parser);

        while (!parser_at_end(parser) && isdigit(parser_peek(parser))) {
            if (i >= sizeof(buffer) - 1) {
                add_error(parser, "Float too long");
                return false;
            }
            buffer[i++] = parser_advance(parser);
        }
    }

    buffer[i] = '\0';

    if (i == 0 || (i == 1 && buffer[0] == '-')) {
        add_error(parser, "Expected float");
        return false;
    }

    *out = atof(buffer);
    return true;
}

bool parser_parse_bool(parser_t *parser, bool *out) {
    parser_skip_whitespace(parser);

    char word[16];
    size_t i = 0;

    while (!parser_at_end(parser) && isalpha(parser_peek(parser))) {
        if (i >= sizeof(word) - 1) {
            add_error(parser, "Boolean value too long");
            return false;
        }
        word[i++] = tolower(parser_advance(parser));
    }
    word[i] = '\0';

    if (strcmp(word, "true") == 0 || strcmp(word, "yes") == 0 || strcmp(word, "on") == 0) {
        *out = true;
        return true;
    } else if (strcmp(word, "false") == 0 || strcmp(word, "no") == 0 || strcmp(word, "off") == 0) {
        *out = false;
        return true;
    }

    add_error(parser, "Invalid boolean value: %s", word);
    return false;
}

bool parser_parse_color(parser_t *parser, color_rgba_t *out) {
    parser_skip_whitespace(parser);

    // Check for hex color
    if (parser_peek(parser) == '#') {
        parser_advance(parser);

        char hex[9] = {0};
        size_t i = 0;

        while (!parser_at_end(parser) && isxdigit(parser_peek(parser))) {
            if (i >= 8) {
                add_error(parser, "Hex color too long");
                return false;
            }
            hex[i++] = parser_advance(parser);
        }

        if (i != 6 && i != 8) {
            add_error(parser, "Invalid hex color (expected #RRGGBB or #RRGGBBAA)");
            return false;
        }

        uint32_t value = strtoul(hex, NULL, 16);

        if (i == 6) {
            // #RRGGBB
            out->r = (value >> 16) & 0xFF;
            out->g = (value >> 8) & 0xFF;
            out->b = value & 0xFF;
            out->a = 255;
        } else {
            // #RRGGBBAA
            out->r = (value >> 24) & 0xFF;
            out->g = (value >> 16) & 0xFF;
            out->b = (value >> 8) & 0xFF;
            out->a = value & 0xFF;
        }

        return true;
    }

    // Check for rgb() or rgba()
    if (parser_peek(parser) == 'r') {
        char func[6];
        size_t i = 0;
        while (!parser_at_end(parser) && (isalpha(parser_peek(parser)) || parser_peek(parser) == '(')) {
            func[i++] = tolower(parser_advance(parser));
            if (i >= sizeof(func)) break;
        }
        func[i] = '\0';

        if (strcmp(func, "rgb(") == 0) {
            return parser_parse_rgb(parser, out);
        } else if (strcmp(func, "rgba(") == 0) {
            return parser_parse_rgba(parser, out);
        }
    }

    add_error(parser, "Invalid color format");
    return false;
}

bool parser_parse_rgb(parser_t *parser, color_rgba_t *out) {
    uint32_t r, g, b;

    if (!parser_parse_uint(parser, &r)) return false;
    parser_skip_whitespace(parser);
    if (!parser_match(parser, ',')) {
        add_error(parser, "Expected comma");
        return false;
    }

    if (!parser_parse_uint(parser, &g)) return false;
    parser_skip_whitespace(parser);
    if (!parser_match(parser, ',')) {
        add_error(parser, "Expected comma");
        return false;
    }

    if (!parser_parse_uint(parser, &b)) return false;
    parser_skip_whitespace(parser);
    if (!parser_match(parser, ')')) {
        add_error(parser, "Expected closing parenthesis");
        return false;
    }

    out->r = r;
    out->g = g;
    out->b = b;
    out->a = 255;

    return true;
}

bool parser_parse_rgba(parser_t *parser, color_rgba_t *out) {
    uint32_t r, g, b, a;

    if (!parser_parse_uint(parser, &r)) return false;
    parser_skip_whitespace(parser);
    if (!parser_match(parser, ',')) {
        add_error(parser, "Expected comma");
        return false;
    }

    if (!parser_parse_uint(parser, &g)) return false;
    parser_skip_whitespace(parser);
    if (!parser_match(parser, ',')) {
        add_error(parser, "Expected comma");
        return false;
    }

    if (!parser_parse_uint(parser, &b)) return false;
    parser_skip_whitespace(parser);
    if (!parser_match(parser, ',')) {
        add_error(parser, "Expected comma");
        return false;
    }

    if (!parser_parse_uint(parser, &a)) return false;
    parser_skip_whitespace(parser);
    if (!parser_match(parser, ')')) {
        add_error(parser, "Expected closing parenthesis");
        return false;
    }

    out->r = r;
    out->g = g;
    out->b = b;
    out->a = a;

    return true;
}

bool parser_parse_theme_mode(parser_t *parser, theme_mode_t *out) {
    parser_skip_whitespace(parser);

    char word[16];
    size_t i = 0;

    while (!parser_at_end(parser) && isalpha(parser_peek(parser))) {
        if (i >= sizeof(word) - 1) break;
        word[i++] = tolower(parser_advance(parser));
    }
    word[i] = '\0';

    if (strcmp(word, "light") == 0) {
        *out = THEME_MODE_LIGHT;
        return true;
    } else if (strcmp(word, "dark") == 0) {
        *out = THEME_MODE_DARK;
        return true;
    } else if (strcmp(word, "auto") == 0) {
        *out = THEME_MODE_AUTO;
        return true;
    }

    add_error(parser, "Invalid theme mode: %s", word);
    return false;
}

bool parser_parse_shadow(parser_t *parser, shadow_t *out) {
    int32_t offset_x, offset_y;
    uint32_t blur, spread, opacity;

    if (!parser_parse_int(parser, &offset_x)) return false;
    if (!parser_parse_int(parser, &offset_y)) return false;
    if (!parser_parse_uint(parser, &blur)) return false;
    if (!parser_parse_uint(parser, &spread)) return false;
    if (!parser_parse_uint(parser, &opacity)) return false;

    out->offset_x = offset_x;
    out->offset_y = offset_y;
    out->blur_radius = blur;
    out->spread = spread;
    out->opacity = opacity;

    return true;
}

// ============================================================================
// SECTION PARSERS
// ============================================================================

bool parser_parse_section(parser_t *parser) {
    parser_skip_whitespace(parser);

    if (!parser_match(parser, '[')) {
        add_error(parser, "Expected '[' for section header");
        return false;
    }

    size_t i = 0;
    while (!parser_at_end(parser) && parser_peek(parser) != ']') {
        if (i >= sizeof(parser->current_section) - 1) {
            add_error(parser, "Section name too long");
            return false;
        }
        parser->current_section[i++] = tolower(parser_advance(parser));
    }
    parser->current_section[i] = '\0';

    if (!parser_match(parser, ']')) {
        add_error(parser, "Expected ']' to close section");
        return false;
    }

    return true;
}

bool parser_parse_property(parser_t *parser, theme_t *theme) {
    parser_skip_whitespace(parser);

    // Parse property name
    char key[64];
    size_t i = 0;
    while (!parser_at_end(parser) && (isalnum(parser_peek(parser)) || parser_peek(parser) == '_')) {
        if (i >= sizeof(key) - 1) {
            add_error(parser, "Property name too long");
            return false;
        }
        key[i++] = tolower(parser_advance(parser));
    }
    key[i] = '\0';

    parser_skip_whitespace(parser);
    if (!parser_match(parser, '=')) {
        add_error(parser, "Expected '=' after property name");
        return false;
    }
    parser_skip_whitespace(parser);

    // Route to appropriate section parser based on current section
    if (strcmp(parser->current_section, "metadata") == 0) {
        if (strcmp(key, "name") == 0) {
            return parser_parse_string(parser, theme->meta.name, sizeof(theme->meta.name));
        } else if (strcmp(key, "author") == 0) {
            return parser_parse_string(parser, theme->meta.author, sizeof(theme->meta.author));
        } else if (strcmp(key, "description") == 0) {
            return parser_parse_string(parser, theme->meta.description, sizeof(theme->meta.description));
        } else if (strcmp(key, "version") == 0) {
            return parser_parse_string(parser, theme->meta.version, sizeof(theme->meta.version));
        } else if (strcmp(key, "mode") == 0) {
            return parser_parse_theme_mode(parser, &theme->meta.mode);
        }
    } else if (strcmp(parser->current_section, "colors") == 0) {
        color_rgba_t color;
        if (!parser_parse_color(parser, &color)) return false;

        if (strcmp(key, "primary") == 0) theme->colors.primary = color;
        else if (strcmp(key, "primary_hover") == 0) theme->colors.primary_hover = color;
        else if (strcmp(key, "primary_light") == 0) theme->colors.primary_light = color;
        else if (strcmp(key, "primary_dark") == 0) theme->colors.primary_dark = color;
        else if (strcmp(key, "secondary") == 0) theme->colors.secondary = color;
        else if (strcmp(key, "success") == 0) theme->colors.success = color;
        else if (strcmp(key, "warning") == 0) theme->colors.warning = color;
        else if (strcmp(key, "error") == 0) theme->colors.error = color;
        else if (strcmp(key, "bg_primary") == 0) theme->colors.bg_primary = color;
        else if (strcmp(key, "bg_secondary") == 0) theme->colors.bg_secondary = color;
        else if (strcmp(key, "bg_tertiary") == 0) theme->colors.bg_tertiary = color;
        else if (strcmp(key, "bg_quaternary") == 0) theme->colors.bg_quaternary = color;
        else if (strcmp(key, "text_primary") == 0) theme->colors.text_primary = color;
        else if (strcmp(key, "text_secondary") == 0) theme->colors.text_secondary = color;
        else if (strcmp(key, "text_tertiary") == 0) theme->colors.text_tertiary = color;
        else if (strcmp(key, "text_placeholder") == 0) theme->colors.text_placeholder = color;
        else if (strcmp(key, "panel_bg") == 0) theme->colors.panel_bg = color;
        else if (strcmp(key, "dock_bg") == 0) theme->colors.dock_bg = color;
        else if (strcmp(key, "window_bg") == 0) theme->colors.window_bg = color;
        else if (strcmp(key, "menu_bg") == 0) theme->colors.menu_bg = color;
        else if (strcmp(key, "border") == 0) theme->colors.border = color;
        else if (strcmp(key, "border_light") == 0) theme->colors.border_light = color;
        else if (strcmp(key, "separator") == 0) theme->colors.separator = color;
        else if (strcmp(key, "overlay_light") == 0) theme->colors.overlay_light = color;
        else if (strcmp(key, "overlay") == 0) theme->colors.overlay = color;
        else if (strcmp(key, "overlay_heavy") == 0) theme->colors.overlay_heavy = color;
        else if (strcmp(key, "focus_ring") == 0) theme->colors.focus_ring = color;
        else if (strcmp(key, "selection_bg") == 0) theme->colors.selection_bg = color;
        else if (strcmp(key, "selection_text") == 0) theme->colors.selection_text = color;

        return true;
    } else if (strcmp(parser->current_section, "window") == 0) {
        if (strcmp(key, "titlebar_height") == 0) {
            return parser_parse_uint(parser, &theme->window.titlebar_height);
        } else if (strcmp(key, "border_width") == 0) {
            return parser_parse_uint(parser, &theme->window.border_width);
        } else if (strcmp(key, "corner_radius") == 0) {
            return parser_parse_uint(parser, &theme->window.corner_radius);
        } else if (strcmp(key, "show_traffic_lights") == 0) {
            return parser_parse_bool(parser, &theme->window.show_traffic_lights);
        } else if (strcmp(key, "show_title_text") == 0) {
            return parser_parse_bool(parser, &theme->window.show_title_text);
        } else if (strcmp(key, "shadow") == 0) {
            // Shadow is handled separately in section parser
            return true;
        }
    } else if (strcmp(parser->current_section, "panel") == 0) {
        if (strcmp(key, "height") == 0) {
            return parser_parse_uint(parser, &theme->panel.height);
        } else if (strcmp(key, "padding") == 0) {
            return parser_parse_uint(parser, &theme->panel.padding);
        } else if (strcmp(key, "icon_size") == 0) {
            return parser_parse_uint(parser, &theme->panel.icon_size);
        } else if (strcmp(key, "shadow") == 0) {
            return parser_parse_shadow(parser, &theme->panel.shadow);
        }
    } else if (strcmp(parser->current_section, "dock") == 0) {
        if (strcmp(key, "icon_size") == 0) {
            return parser_parse_uint(parser, &theme->dock.icon_size);
        } else if (strcmp(key, "padding") == 0) {
            return parser_parse_uint(parser, &theme->dock.padding);
        } else if (strcmp(key, "margin") == 0) {
            return parser_parse_uint(parser, &theme->dock.margin);
        } else if (strcmp(key, "magnification_enabled") == 0) {
            return parser_parse_bool(parser, &theme->dock.magnification_enabled);
        } else if (strcmp(key, "shadow") == 0) {
            return parser_parse_shadow(parser, &theme->dock.shadow);
        }
    } else if (strcmp(parser->current_section, "menu") == 0) {
        if (strcmp(key, "corner_radius") == 0) {
            return parser_parse_uint(parser, &theme->menu.corner_radius);
        } else if (strcmp(key, "shadow") == 0) {
            return parser_parse_shadow(parser, &theme->menu.shadow);
        }
    } else if (strcmp(parser->current_section, "notification") == 0) {
        if (strcmp(key, "corner_radius") == 0) {
            return parser_parse_uint(parser, &theme->notification.corner_radius);
        } else if (strcmp(key, "width") == 0) {
            return parser_parse_uint(parser, &theme->notification.width);
        } else if (strcmp(key, "shadow") == 0) {
            return parser_parse_shadow(parser, &theme->notification.shadow);
        }
    } else if (strcmp(parser->current_section, "typography") == 0) {
        if (strcmp(key, "family") == 0) {
            char family[64];
            if (!parser_parse_string(parser, family, sizeof(family))) return false;
            // Note: Store as const char* - in real implementation would need strdup
            return true;
        } else if (strcmp(key, "base_size") == 0) {
            return parser_parse_uint(parser, &theme->typography.base_size);
        } else if (strcmp(key, "scale_factor") == 0) {
            return parser_parse_float(parser, &theme->typography.scale_factor);
        }
    } else if (strcmp(parser->current_section, "animations") == 0) {
        if (strcmp(key, "enabled") == 0) {
            return parser_parse_bool(parser, &theme->animations.enabled);
        } else if (strcmp(key, "speed_multiplier") == 0) {
            return parser_parse_float(parser, &theme->animations.speed_multiplier);
        }
    } else if (strcmp(parser->current_section, "accessibility") == 0) {
        if (strcmp(key, "high_contrast") == 0) {
            return parser_parse_bool(parser, &theme->accessibility.high_contrast);
        } else if (strcmp(key, "reduce_transparency") == 0) {
            return parser_parse_bool(parser, &theme->accessibility.reduce_transparency);
        } else if (strcmp(key, "reduce_motion") == 0) {
            return parser_parse_bool(parser, &theme->accessibility.reduce_motion);
        } else if (strcmp(key, "text_scale") == 0) {
            return parser_parse_float(parser, &theme->accessibility.text_scale);
        }
    }

    add_error(parser, "Unknown property: %s in section [%s]", key, parser->current_section);
    return false;
}

bool parser_parse_theme(parser_t *parser, theme_t *theme) {
    while (!parser_at_end(parser)) {
        parser_skip_whitespace(parser);
        if (parser_at_end(parser)) break;

        if (parser_peek(parser) == '[') {
            // New section
            if (!parser_parse_section(parser)) {
                return false;
            }
        } else {
            // Property
            if (!parser_parse_property(parser, theme)) {
                return false;
            }
        }
    }

    return g_error_count == 0;
}

// ============================================================================
// PUBLIC API
// ============================================================================

theme_t *theme_parse_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        add_error(NULL, "Failed to open file: %s", path);
        return NULL;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Read file
    char *input = malloc(size + 1);
    if (!input) {
        fclose(f);
        return NULL;
    }

    fread(input, 1, size, f);
    input[size] = '\0';
    fclose(f);

    // Parse
    theme_t *theme = theme_parse_string(input);
    free(input);

    return theme;
}

theme_t *theme_parse_string(const char *input) {
    g_error_count = 0;

    parser_t *parser = parser_init(input, strlen(input));
    if (!parser) return NULL;

    theme_t *theme = calloc(1, sizeof(theme_t));
    if (!theme) {
        parser_cleanup(parser);
        return NULL;
    }

    // Initialize theme with defaults
    theme_t *default_theme = theme_create_default_light();
    if (default_theme) {
        memcpy(theme, default_theme, sizeof(theme_t));
        theme_free(default_theme);
    }

    // Parse
    if (!parser_parse_theme(parser, theme)) {
        theme_free(theme);
        parser_cleanup(parser);
        return NULL;
    }

    parser_cleanup(parser);
    return theme;
}

// ============================================================================
// THEME GENERATION
// ============================================================================

void color_to_hex_string(color_rgba_t color, char *buffer, size_t size, bool include_alpha) {
    if (include_alpha) {
        snprintf(buffer, size, "#%02X%02X%02X%02X",
                color.r, color.g, color.b, color.a);
    } else {
        snprintf(buffer, size, "#%02X%02X%02X",
                color.r, color.g, color.b);
    }
}

void shadow_to_string(shadow_t shadow, char *buffer, size_t size) {
    snprintf(buffer, size, "%d %d %u %u %u",
            shadow.offset_x, shadow.offset_y,
            shadow.blur_radius, shadow.spread, shadow.opacity);
}

int theme_generate_string(const theme_t *theme, char *buffer, size_t size) {
    if (!theme || !buffer) return -1;

    char *ptr = buffer;
    size_t remaining = size;
    int written;

    // Metadata
    written = snprintf(ptr, remaining,
        "[metadata]\n"
        "name = \"%s\"\n"
        "author = \"%s\"\n"
        "description = \"%s\"\n"
        "version = \"%s\"\n"
        "mode = %s\n\n",
        theme->meta.name,
        theme->meta.author,
        theme->meta.description,
        theme->meta.version,
        theme->meta.mode == THEME_MODE_LIGHT ? "light" :
        theme->meta.mode == THEME_MODE_DARK ? "dark" : "auto");
    ptr += written;
    remaining -= written;

    // Colors section (abbreviated for space)
    written = snprintf(ptr, remaining, "[colors]\n");
    ptr += written;
    remaining -= written;

    char color_str[16];
    color_to_hex_string(theme->colors.primary, color_str, sizeof(color_str), false);
    written = snprintf(ptr, remaining, "primary = %s\n", color_str);
    ptr += written;
    remaining -= written;

    // Add more colors as needed...

    // Window section
    written = snprintf(ptr, remaining,
        "\n[window]\n"
        "titlebar_height = %u\n"
        "border_width = %u\n"
        "corner_radius = %u\n"
        "show_traffic_lights = %s\n"
        "show_title_text = %s\n\n",
        theme->window.titlebar_height,
        theme->window.border_width,
        theme->window.corner_radius,
        theme->window.show_traffic_lights ? "true" : "false",
        theme->window.show_title_text ? "true" : "false");
    ptr += written;
    remaining -= written;

    return ptr - buffer;
}

int theme_save_file(const theme_t *theme, const char *path) {
    char buffer[16384];
    int len = theme_generate_string(theme, buffer, sizeof(buffer));
    if (len < 0) return -1;

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fwrite(buffer, 1, len, f);
    fclose(f);

    return 0;
}

bool theme_validate_syntax(const char *path, char *errors, size_t error_size) {
    theme_t *theme = theme_parse_file(path);
    if (!theme) {
        theme_get_parse_errors(errors, error_size);
        return false;
    }

    theme_free(theme);
    return true;
}

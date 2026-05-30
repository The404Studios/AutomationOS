/**
 * AutomationOS Terminal Emulator
 *
 * Modern terminal with tabs, split panes, GPU acceleration,
 * themes, profiles, and rich text formatting.
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>
#include <stdbool.h>
#include "../../../userspace/compositor/gpu.h"

// Configuration limits
#define MAX_TABS 16
#define MAX_PANES 8
#define MAX_PROFILES 16
#define MAX_THEMES 16
#define SCROLLBACK_SIZE 10000
#define MAX_COMMAND_HISTORY 1000
#define MAX_TITLE_LENGTH 256
#define MAX_PROFILE_NAME 64
#define MAX_THEME_NAME 64
#define MAX_FONT_NAME 128

// Terminal dimensions
#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24
#define CELL_WIDTH 9        // Pixels per character
#define CELL_HEIGHT 18      // Pixels per line
#define TAB_BAR_HEIGHT 32   // Pixels
#define SCROLLBAR_WIDTH 12  // Pixels

// Color support
#define COLOR_PALETTE_SIZE 256
#define TRUECOLOR_SUPPORT 1

/**
 * Color structure (RGBA)
 */
typedef struct {
    uint8_t r, g, b, a;
} color_t;

/**
 * Cell attributes (character formatting)
 */
typedef struct {
    uint32_t codepoint;     // Unicode codepoint
    color_t fg;             // Foreground color
    color_t bg;             // Background color
    uint16_t flags;         // Formatting flags
} cell_t;

// Cell attribute flags
#define CELL_BOLD           (1 << 0)
#define CELL_ITALIC         (1 << 1)
#define CELL_UNDERLINE      (1 << 2)
#define CELL_STRIKETHROUGH  (1 << 3)
#define CELL_BLINK          (1 << 4)
#define CELL_REVERSE        (1 << 5)
#define CELL_HIDDEN         (1 << 6)
#define CELL_DIM            (1 << 7)
#define CELL_OVERLINE       (1 << 8)
#define CELL_DOUBLEWIDTH    (1 << 9)
#define CELL_URL            (1 << 10)

/**
 * Cursor structure
 */
typedef struct {
    int32_t x, y;           // Position (column, row)
    bool visible;
    bool blink;
    uint32_t blink_timer;
    color_t color;
} cursor_t;

/**
 * Selection structure
 */
typedef struct {
    int32_t start_x, start_y;
    int32_t end_x, end_y;
    bool active;
    bool rectangular;       // Block selection vs line selection
} selection_t;

/**
 * Scrollback buffer
 */
typedef struct {
    cell_t *lines[SCROLLBACK_SIZE];
    uint32_t head;          // Next write position
    uint32_t count;         // Number of lines stored
    int32_t view_offset;    // Current scroll position
} scrollback_t;

/**
 * URL/hyperlink structure
 */
typedef struct {
    int32_t x, y;           // Start position
    uint32_t length;        // Length in characters
    char url[512];
    struct hyperlink *next;
} hyperlink_t;

/**
 * Terminal buffer (screen contents)
 */
typedef struct {
    uint32_t cols, rows;
    cell_t *cells;          // 2D array [row][col]
    cursor_t cursor;
    selection_t selection;
    scrollback_t scrollback;
    hyperlink_t *hyperlinks;

    // Parser state
    void *parser_state;

    // Dirty tracking
    bool *dirty_lines;      // Per-line dirty flags
    bool full_redraw;
} terminal_buffer_t;

/**
 * Split pane structure
 */
typedef enum {
    SPLIT_NONE,
    SPLIT_HORIZONTAL,
    SPLIT_VERTICAL
} split_type_t;

typedef struct pane {
    uint32_t id;
    terminal_buffer_t *buffer;

    // Layout
    int32_t x, y;
    uint32_t width, height;

    // Splitting
    split_type_t split_type;
    struct pane *first_child;
    struct pane *second_child;
    float split_ratio;      // 0.0 to 1.0

    // Focus
    bool focused;

    // Process
    int32_t child_pid;
    int32_t pty_fd;
} pane_t;

/**
 * Tab structure
 */
typedef struct {
    uint32_t id;
    char title[MAX_TITLE_LENGTH];
    pane_t *root_pane;
    pane_t *focused_pane;
    uint32_t pane_count;

    // Profile override
    int32_t profile_id;     // -1 = use default
} tab_t;

/**
 * Terminal profile
 */
typedef struct {
    uint32_t id;
    char name[MAX_PROFILE_NAME];

    // Shell
    char shell[256];
    char shell_args[512];
    char working_dir[512];

    // Appearance
    char font_name[MAX_FONT_NAME];
    uint32_t font_size;
    int32_t theme_id;       // -1 = default theme

    // Behavior
    uint32_t scrollback_lines;
    bool scroll_on_output;
    bool scroll_on_keystroke;
    uint32_t cursor_blink_rate;

    // Bell
    bool visual_bell;
    bool audible_bell;
} profile_t;

/**
 * Terminal theme
 */
typedef struct {
    uint32_t id;
    char name[MAX_THEME_NAME];

    // Base colors
    color_t foreground;
    color_t background;
    color_t cursor;
    color_t selection_bg;
    color_t selection_fg;

    // ANSI colors (0-15)
    color_t ansi_colors[16];

    // Extended colors (16-255) - generated
    color_t extended_colors[240];

    // UI colors
    color_t tab_bar_bg;
    color_t tab_bar_fg;
    color_t tab_active_bg;
    color_t tab_active_fg;
    color_t scrollbar_bg;
    color_t scrollbar_fg;
    color_t border_color;
} theme_t;

/**
 * Font rendering context
 */
typedef struct {
    char font_name[MAX_FONT_NAME];
    uint32_t font_size;
    void *font_data;            // FreeType/stb_truetype font
    texture_t **glyph_cache;    // GPU texture atlas
    bool ligatures_enabled;
} font_context_t;

/**
 * Main terminal application
 */
typedef struct {
    // Window and rendering
    gpu_context_t *gpu;
    framebuffer_t *fb;
    uint32_t window_id;
    uint32_t width, height;

    // Tabs
    tab_t *tabs[MAX_TABS];
    uint32_t tab_count;
    uint32_t active_tab;

    // Profiles and themes
    profile_t *profiles[MAX_PROFILES];
    theme_t *themes[MAX_THEMES];
    uint32_t profile_count;
    uint32_t theme_count;
    uint32_t default_profile;
    uint32_t default_theme;

    // Font rendering
    font_context_t *font_ctx;

    // Input state
    char input_buffer[1024];
    uint32_t input_length;

    // Command history
    char *command_history[MAX_COMMAND_HISTORY];
    uint32_t history_count;
    int32_t history_index;

    // Search
    char search_query[256];
    bool search_active;
    bool search_case_sensitive;

    // UI state
    bool tab_bar_visible;
    bool scrollbar_visible;
    bool menu_open;
    int32_t hover_tab;          // -1 = none

    // Settings
    bool vsync_enabled;
    uint32_t fps_limit;
    bool gpu_acceleration;
    bool ligatures_enabled;
    bool emoji_support;

    // Statistics
    uint32_t fps;
    uint64_t frame_count;
    uint64_t last_fps_update;
} terminal_t;

// Core terminal functions (terminal.c)
terminal_t *terminal_create(uint32_t width, uint32_t height);
void terminal_destroy(terminal_t *term);
void terminal_render(terminal_t *term);
void terminal_process_input(terminal_t *term, const char *input, uint32_t length);
void terminal_handle_key(terminal_t *term, uint32_t keycode, uint32_t modifiers);
void terminal_handle_mouse(terminal_t *term, int32_t x, int32_t y, uint32_t button, uint32_t action);
void terminal_resize(terminal_t *term, uint32_t width, uint32_t height);

// Tab management (tabs.c)
tab_t *tab_create(terminal_t *term, profile_t *profile);
void tab_destroy(tab_t *tab);
void tab_set_title(tab_t *tab, const char *title);
void terminal_add_tab(terminal_t *term, tab_t *tab);
void terminal_remove_tab(terminal_t *term, uint32_t tab_id);
void terminal_switch_tab(terminal_t *term, uint32_t tab_index);
void terminal_next_tab(terminal_t *term);
void terminal_prev_tab(terminal_t *term);
void terminal_move_tab(terminal_t *term, uint32_t from_index, uint32_t to_index);

// Pane management (panes.c)
pane_t *pane_create(uint32_t cols, uint32_t rows);
void pane_destroy(pane_t *pane);
void pane_split_horizontal(pane_t *pane, float ratio);
void pane_split_vertical(pane_t *pane, float ratio);
void pane_close(pane_t *pane);
pane_t *pane_find_at_position(pane_t *root, int32_t x, int32_t y);
void pane_focus_next(pane_t *root);
void pane_focus_prev(pane_t *root);
void pane_resize(pane_t *pane, uint32_t width, uint32_t height);

// Terminal buffer (buffer.c)
terminal_buffer_t *buffer_create(uint32_t cols, uint32_t rows);
void buffer_destroy(terminal_buffer_t *buffer);
void buffer_resize(terminal_buffer_t *buffer, uint32_t cols, uint32_t rows);
void buffer_clear(terminal_buffer_t *buffer);
void buffer_scroll_up(terminal_buffer_t *buffer, uint32_t lines);
void buffer_scroll_down(terminal_buffer_t *buffer, uint32_t lines);
void buffer_write_char(terminal_buffer_t *buffer, uint32_t codepoint, const cell_t *attr);
void buffer_write_string(terminal_buffer_t *buffer, const char *str, uint32_t length);
void buffer_set_cursor(terminal_buffer_t *buffer, int32_t x, int32_t y);
void buffer_move_cursor(terminal_buffer_t *buffer, int32_t dx, int32_t dy);

// Scrollback (scrollback.c)
void scrollback_init(scrollback_t *scrollback);
void scrollback_push_line(scrollback_t *scrollback, const cell_t *line, uint32_t cols);
cell_t *scrollback_get_line(scrollback_t *scrollback, int32_t index);
void scrollback_scroll_to(scrollback_t *scrollback, int32_t offset);
void scrollback_clear(scrollback_t *scrollback);

// Selection (selection.c)
void selection_start(selection_t *sel, int32_t x, int32_t y);
void selection_update(selection_t *sel, int32_t x, int32_t y);
void selection_end(selection_t *sel);
void selection_clear(selection_t *sel);
bool selection_contains(const selection_t *sel, int32_t x, int32_t y);
char *selection_get_text(const terminal_buffer_t *buffer);
void selection_copy_to_clipboard(terminal_t *term);
void selection_paste_from_clipboard(terminal_t *term);

// Profile management (profiles.c)
profile_t *profile_create(const char *name);
void profile_destroy(profile_t *profile);
profile_t *profile_load(const char *filename);
void profile_save(const profile_t *profile, const char *filename);
profile_t *profile_get_default(void);
void terminal_add_profile(terminal_t *term, profile_t *profile);
profile_t *terminal_get_profile(terminal_t *term, uint32_t id);

// Theme management (themes.c)
theme_t *theme_create(const char *name);
void theme_destroy(theme_t *theme);
theme_t *theme_load(const char *filename);
void theme_save(const theme_t *theme, const char *filename);
void theme_generate_extended_colors(theme_t *theme);
color_t theme_get_color(const theme_t *theme, uint8_t index);
theme_t *theme_get_default_dark(void);
theme_t *theme_get_default_light(void);
theme_t *theme_get_solarized_dark(void);
theme_t *theme_get_solarized_light(void);
theme_t *theme_get_monokai(void);
theme_t *theme_get_dracula(void);
void terminal_add_theme(terminal_t *term, theme_t *theme);
theme_t *terminal_get_theme(terminal_t *term, uint32_t id);

// Font rendering (font.c)
font_context_t *font_create(const char *font_name, uint32_t font_size);
void font_destroy(font_context_t *ctx);
void font_render_glyph(font_context_t *ctx, gpu_context_t *gpu, uint32_t codepoint, int32_t x, int32_t y, const color_t *color);
void font_render_text(font_context_t *ctx, gpu_context_t *gpu, const char *text, int32_t x, int32_t y, const color_t *color);
uint32_t font_get_glyph_width(font_context_t *ctx, uint32_t codepoint);
void font_enable_ligatures(font_context_t *ctx, bool enabled);

// Rendering (renderer.c)
void renderer_render_buffer(terminal_t *term, const terminal_buffer_t *buffer, const pane_t *pane, const theme_t *theme);
void renderer_render_cursor(terminal_t *term, const terminal_buffer_t *buffer, const pane_t *pane, const theme_t *theme);
void renderer_render_selection(terminal_t *term, const terminal_buffer_t *buffer, const pane_t *pane, const theme_t *theme);
void renderer_render_scrollbar(terminal_t *term, const pane_t *pane, const theme_t *theme);
void renderer_render_tab_bar(terminal_t *term, const theme_t *theme);
void renderer_render_pane_borders(terminal_t *term, const pane_t *pane, const theme_t *theme);
void renderer_render_search_highlight(terminal_t *term, const terminal_buffer_t *buffer, const pane_t *pane, const theme_t *theme);

// VT parser (vt_parser.c)
typedef struct vt_parser vt_parser_t;

vt_parser_t *vt_parser_create(terminal_buffer_t *buffer);
void vt_parser_destroy(vt_parser_t *parser);
void vt_parser_process(vt_parser_t *parser, const uint8_t *data, uint32_t length);
void vt_parser_reset(vt_parser_t *parser);

// Search (search.c)
void search_start(terminal_t *term, const char *query);
void search_next(terminal_t *term);
void search_prev(terminal_t *term);
void search_cancel(terminal_t *term);
bool search_matches_at(const terminal_buffer_t *buffer, int32_t x, int32_t y, const char *query);

// Hyperlinks (hyperlinks.c)
void hyperlink_add(terminal_buffer_t *buffer, int32_t x, int32_t y, const char *url);
void hyperlink_remove_all(terminal_buffer_t *buffer);
hyperlink_t *hyperlink_at_position(const terminal_buffer_t *buffer, int32_t x, int32_t y);
void hyperlink_open(const char *url);

// PTY management (pty.c)
int32_t pty_open(uint32_t cols, uint32_t rows);
void pty_close(int32_t pty_fd);
void pty_resize(int32_t pty_fd, uint32_t cols, uint32_t rows);
int32_t pty_spawn(int32_t pty_fd, const char *shell, char *const argv[]);
int32_t pty_read(int32_t pty_fd, uint8_t *buffer, uint32_t size);
int32_t pty_write(int32_t pty_fd, const uint8_t *buffer, uint32_t size);

// History (history.c)
void history_add(terminal_t *term, const char *command);
const char *history_get(terminal_t *term, int32_t index);
void history_prev(terminal_t *term);
void history_next(terminal_t *term);
void history_reset(terminal_t *term);
void history_save(terminal_t *term, const char *filename);
void history_load(terminal_t *term, const char *filename);

// Utilities
color_t color_from_rgb(uint8_t r, uint8_t g, uint8_t b);
color_t color_from_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
uint32_t color_to_u32(const color_t *color);
bool color_equals(const color_t *a, const color_t *b);
void color_blend(color_t *result, const color_t *fg, const color_t *bg);

uint32_t utf8_decode(const char **str);
uint32_t utf8_encode(uint32_t codepoint, char *buffer);
bool utf8_is_continuation(uint8_t byte);
uint32_t utf8_strlen(const char *str);

#endif // TERMINAL_H

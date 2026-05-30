/**
 * File Explorer - Main Header
 *
 * Beautiful, animated file manager for AutomationOS
 */

#ifndef EXPLORER_H
#define EXPLORER_H

#include <stdint.h>
#include <stdbool.h>
#include "../../compositor/compositor.h"
#include "file_types.h"

// Forward declarations
typedef struct file_explorer file_explorer_t;
typedef struct toolbar toolbar_t;
typedef struct sidebar sidebar_t;
typedef struct breadcrumb breadcrumb_t;
typedef struct file_view file_view_t;
typedef struct statusbar statusbar_t;
typedef struct path_stack path_stack_t;

// Maximum limits
#define MAX_SELECTION 256
#define MAX_PATH_LEN 4096
#define MAX_FILENAME_LEN 256
#define MAX_FILES_PER_DIR 10000
#define MAX_RECENT_PATHS 20

/**
 * View modes
 */
typedef enum {
    VIEW_MODE_ICONS,        // Grid of icons with thumbnails
    VIEW_MODE_LIST,         // Detailed list view
    VIEW_MODE_COLUMNS,      // Multi-column browser (macOS Finder style)
    VIEW_MODE_GALLERY,      // Large thumbnails for photos
} view_mode_t;

/**
 * Sort modes
 */
typedef enum {
    SORT_NAME_ASC,
    SORT_NAME_DESC,
    SORT_SIZE_ASC,
    SORT_SIZE_DESC,
    SORT_DATE_ASC,
    SORT_DATE_DESC,
    SORT_TYPE_ASC,
    SORT_TYPE_DESC,
} sort_mode_t;

/**
 * Path stack for back/forward navigation
 */
struct path_stack {
    char paths[MAX_RECENT_PATHS][MAX_PATH_LEN];
    uint32_t count;
    uint32_t current;
};

/**
 * Sidebar place
 */
typedef struct {
    char name[64];
    char path[MAX_PATH_LEN];
    const char *icon;
    bool is_separator;
} sidebar_place_t;

/**
 * Toolbar
 */
struct toolbar {
    rect_t geometry;

    // Navigation buttons
    rect_t back_button;
    rect_t forward_button;
    rect_t home_button;
    rect_t parent_button;

    // View controls
    rect_t view_button;
    rect_t sort_button;

    // Search
    rect_t search_box;
    char search_text[256];
    bool search_focused;

    // Settings
    rect_t settings_button;

    // Hover/press states
    bool back_hover, back_pressed;
    bool forward_hover, forward_pressed;
    bool home_hover, home_pressed;
    bool parent_hover, parent_pressed;
    bool view_hover, view_pressed;
    bool sort_hover, sort_pressed;
    bool settings_hover, settings_pressed;
};

/**
 * Sidebar
 */
struct sidebar {
    rect_t geometry;

    // Places
    sidebar_place_t places[32];
    uint32_t place_count;

    // Selection
    int32_t selected_place;
    int32_t hovered_place;

    // Scroll
    int32_t scroll_offset;

    // Sections
    bool show_favorites;
    bool show_devices;
    bool show_network;
};

/**
 * Breadcrumb navigation
 */
struct breadcrumb {
    rect_t geometry;

    // Path segments
    struct {
        char name[MAX_FILENAME_LEN];
        char full_path[MAX_PATH_LEN];
        rect_t bounds;
        bool hovered;
    } segments[32];
    uint32_t segment_count;

    int32_t scroll_offset;
};

/**
 * File view item (for rendering)
 */
typedef struct {
    file_entry_t entry;
    rect_t bounds;
    thumbnail_t *thumbnail;
    bool selected;
    bool hovered;
    float animation_progress;  // For animations (0.0 to 1.0)
} file_view_item_t;

/**
 * File view (main file display area)
 */
struct file_view {
    rect_t geometry;
    view_mode_t mode;

    // Files
    file_view_item_t items[MAX_FILES_PER_DIR];
    uint32_t item_count;

    // Selection
    uint32_t selected_indices[MAX_SELECTION];
    uint32_t selection_count;
    int32_t focus_index;

    // Hover
    int32_t hovered_index;

    // Scrolling
    int32_t scroll_offset;
    int32_t max_scroll;
    float scroll_velocity;  // For kinetic scrolling

    // Grid layout (icons/gallery mode)
    uint32_t columns;
    uint32_t icon_size;     // 64, 96, 128, 256
    uint32_t spacing;

    // List layout
    struct {
        uint32_t name_width;
        uint32_t size_width;
        uint32_t type_width;
        uint32_t modified_width;
    } list_columns;

    // Drag & drop
    bool dragging;
    int32_t drag_start_x, drag_start_y;
    int32_t drag_current_x, drag_current_y;

    // Context menu
    bool show_context_menu;
    rect_t context_menu_bounds;
};

/**
 * Status bar
 */
struct statusbar {
    rect_t geometry;

    char status_text[256];
    uint32_t item_count;
    uint32_t selected_count;
    uint64_t total_size;

    // Progress (for operations)
    bool show_progress;
    float progress;
    char progress_text[128];
};

/**
 * File Explorer main structure
 */
struct file_explorer {
    // Window
    window_t *window;
    compositor_t *compositor;

    // UI Components
    toolbar_t toolbar;
    sidebar_t sidebar;
    breadcrumb_t breadcrumb;
    file_view_t view;
    statusbar_t statusbar;

    // Current state
    char current_path[MAX_PATH_LEN];
    path_stack_t history;
    sort_mode_t sort_mode;
    bool show_hidden;

    // Clipboard
    struct {
        char paths[MAX_SELECTION][MAX_PATH_LEN];
        uint32_t count;
        bool is_cut;  // Cut vs copy
    } clipboard;

    // Animation
    uint64_t last_frame_time;
    bool needs_redraw;

    // Settings
    bool thumbnails_enabled;
    uint32_t thumbnail_size;
    bool kinetic_scrolling;
    bool smooth_animations;
};

// Core functions
file_explorer_t* explorer_create(compositor_t *compositor, const char *initial_path);
void explorer_destroy(file_explorer_t *explorer);
void explorer_run(file_explorer_t *explorer);

// Navigation
void explorer_navigate_to(file_explorer_t *explorer, const char *path);
void explorer_navigate_back(file_explorer_t *explorer);
void explorer_navigate_forward(file_explorer_t *explorer);
void explorer_navigate_parent(file_explorer_t *explorer);
void explorer_navigate_home(file_explorer_t *explorer);

// View control
void explorer_set_view_mode(file_explorer_t *explorer, view_mode_t mode);
void explorer_set_sort_mode(file_explorer_t *explorer, sort_mode_t mode);
void explorer_toggle_hidden_files(file_explorer_t *explorer);

// Selection
void explorer_select_item(file_explorer_t *explorer, uint32_t index);
void explorer_select_range(file_explorer_t *explorer, uint32_t start, uint32_t end);
void explorer_select_all(file_explorer_t *explorer);
void explorer_clear_selection(file_explorer_t *explorer);

// Clipboard operations
void explorer_copy_selection(file_explorer_t *explorer);
void explorer_cut_selection(file_explorer_t *explorer);
void explorer_paste(file_explorer_t *explorer);
void explorer_delete_selection(file_explorer_t *explorer);

// File operations
void explorer_open_file(file_explorer_t *explorer, const char *path);
void explorer_rename_file(file_explorer_t *explorer, const char *old_path, const char *new_name);
void explorer_create_folder(file_explorer_t *explorer, const char *name);

// Rendering
void explorer_render(file_explorer_t *explorer);
void explorer_render_toolbar(file_explorer_t *explorer);
void explorer_render_sidebar(file_explorer_t *explorer);
void explorer_render_breadcrumb(file_explorer_t *explorer);
void explorer_render_file_view(file_explorer_t *explorer);
void explorer_render_statusbar(file_explorer_t *explorer);

// Event handling
void explorer_handle_mouse_move(file_explorer_t *explorer, int32_t x, int32_t y);
void explorer_handle_mouse_button(file_explorer_t *explorer, int32_t x, int32_t y, uint32_t button, bool pressed);
void explorer_handle_mouse_scroll(file_explorer_t *explorer, int32_t delta);
void explorer_handle_key(file_explorer_t *explorer, uint32_t keycode, bool pressed);

// Utility functions
void path_stack_push(path_stack_t *stack, const char *path);
const char* path_stack_current(path_stack_t *stack);
const char* path_stack_back(path_stack_t *stack);
const char* path_stack_forward(path_stack_t *stack);
bool path_stack_can_go_back(path_stack_t *stack);
bool path_stack_can_go_forward(path_stack_t *stack);

#endif // EXPLORER_H

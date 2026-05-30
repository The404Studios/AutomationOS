/**
 * File Explorer - Main Implementation
 *
 * Beautiful, animated file manager for AutomationOS
 */

#include "explorer.h"
#include "file_types.h"
#include "operations.h"
#include "search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

// UI Layout constants
#define WINDOW_WIDTH 1200
#define WINDOW_HEIGHT 800
#define TOOLBAR_HEIGHT 60
#define SIDEBAR_WIDTH 200
#define BREADCRUMB_HEIGHT 40
#define STATUSBAR_HEIGHT 30

// Colors (ARGB32)
#define COLOR_BACKGROUND 0xFFFFFFFF
#define COLOR_SIDEBAR_BG 0xFFF5F5F5
#define COLOR_TOOLBAR_BG 0xFFFAFAFA
#define COLOR_SELECTED 0x3307AFF
#define COLOR_HOVER 0x14F0F0F0
#define COLOR_BORDER 0xFFE0E0E0

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Initialize path stack
 */
static void path_stack_init(path_stack_t *stack) {
    if (!stack) return;
    stack->count = 0;
    stack->current = 0;
}

/**
 * Push path to stack
 */
void path_stack_push(path_stack_t *stack, const char *path) {
    if (!stack || !path) return;

    // Remove forward history when pushing new path
    stack->count = stack->current + 1;

    if (stack->count >= MAX_RECENT_PATHS) {
        // Shift array left
        memmove(stack->paths[0], stack->paths[1],
                (MAX_RECENT_PATHS - 1) * sizeof(stack->paths[0]));
        stack->count = MAX_RECENT_PATHS - 1;
    }

    strncpy(stack->paths[stack->count], path, MAX_PATH_LEN - 1);
    stack->current = stack->count;
    stack->count++;
}

/**
 * Get current path from stack
 */
const char* path_stack_current(path_stack_t *stack) {
    if (!stack || stack->count == 0) return NULL;
    return stack->paths[stack->current];
}

/**
 * Go back in history
 */
const char* path_stack_back(path_stack_t *stack) {
    if (!stack || stack->current == 0) return NULL;
    stack->current--;
    return stack->paths[stack->current];
}

/**
 * Go forward in history
 */
const char* path_stack_forward(path_stack_t *stack) {
    if (!stack || stack->current >= stack->count - 1) return NULL;
    stack->current++;
    return stack->paths[stack->current];
}

/**
 * Check if can go back
 */
bool path_stack_can_go_back(path_stack_t *stack) {
    return stack && stack->current > 0;
}

/**
 * Check if can go forward
 */
bool path_stack_can_go_forward(path_stack_t *stack) {
    return stack && stack->current < stack->count - 1;
}

/**
 * Initialize toolbar
 */
static void toolbar_init(file_explorer_t *explorer) {
    toolbar_t *toolbar = &explorer->toolbar;

    toolbar->geometry.x = 0;
    toolbar->geometry.y = 0;
    toolbar->geometry.w = WINDOW_WIDTH;
    toolbar->geometry.h = TOOLBAR_HEIGHT;

    // Layout buttons horizontally
    int x = 10;
    int button_size = 36;
    int spacing = 8;

    toolbar->back_button = (rect_t){x, 12, button_size, button_size};
    x += button_size + spacing;

    toolbar->forward_button = (rect_t){x, 12, button_size, button_size};
    x += button_size + spacing;

    toolbar->home_button = (rect_t){x, 12, button_size, button_size};
    x += button_size + spacing;

    toolbar->parent_button = (rect_t){x, 12, button_size, button_size};
    x += button_size + spacing * 2;

    // Search box (larger)
    toolbar->search_box = (rect_t){x, 10, 300, 40};
    x += 300 + spacing * 2;

    // View and sort buttons
    toolbar->view_button = (rect_t){WINDOW_WIDTH - 120, 12, button_size, button_size};
    toolbar->sort_button = (rect_t){WINDOW_WIDTH - 80, 12, button_size, button_size};
    toolbar->settings_button = (rect_t){WINDOW_WIDTH - 40, 12, button_size, button_size};

    toolbar->search_focused = false;
    toolbar->search_text[0] = '\0';
}

/**
 * Initialize sidebar
 */
static void sidebar_init(file_explorer_t *explorer) {
    sidebar_t *sidebar = &explorer->sidebar;

    sidebar->geometry.x = 0;
    sidebar->geometry.y = TOOLBAR_HEIGHT;
    sidebar->geometry.w = SIDEBAR_WIDTH;
    sidebar->geometry.h = WINDOW_HEIGHT - TOOLBAR_HEIGHT;

    sidebar->place_count = 0;
    sidebar->selected_place = -1;
    sidebar->hovered_place = -1;
    sidebar->scroll_offset = 0;

    sidebar->show_favorites = true;
    sidebar->show_devices = true;
    sidebar->show_network = true;

    // Add default places
    // Favorites
    sidebar->places[sidebar->place_count++] = (sidebar_place_t){
        .name = "Recent", .path = "/recent", .icon = "clock", .is_separator = false
    };
    sidebar->places[sidebar->place_count++] = (sidebar_place_t){
        .name = "Home", .path = "/home", .icon = "home", .is_separator = false
    };
    sidebar->places[sidebar->place_count++] = (sidebar_place_t){
        .name = "Desktop", .path = "/home/desktop", .icon = "desktop", .is_separator = false
    };
    sidebar->places[sidebar->place_count++] = (sidebar_place_t){
        .name = "Documents", .path = "/home/documents", .icon = "document", .is_separator = false
    };
    sidebar->places[sidebar->place_count++] = (sidebar_place_t){
        .name = "Downloads", .path = "/home/downloads", .icon = "download", .is_separator = false
    };
    sidebar->places[sidebar->place_count++] = (sidebar_place_t){
        .name = "Pictures", .path = "/home/pictures", .icon = "image", .is_separator = false
    };
    sidebar->places[sidebar->place_count++] = (sidebar_place_t){
        .name = "Videos", .path = "/home/videos", .icon = "video", .is_separator = false
    };
    sidebar->places[sidebar->place_count++] = (sidebar_place_t){
        .name = "Music", .path = "/home/music", .icon = "music", .is_separator = false
    };

    // Separator
    sidebar->places[sidebar->place_count++] = (sidebar_place_t){
        .is_separator = true
    };

    // Devices
    sidebar->places[sidebar->place_count++] = (sidebar_place_t){
        .name = "System Drive", .path = "/", .icon = "drive", .is_separator = false
    };
}

/**
 * Initialize breadcrumb
 */
static void breadcrumb_init(file_explorer_t *explorer) {
    breadcrumb_t *breadcrumb = &explorer->breadcrumb;

    breadcrumb->geometry.x = SIDEBAR_WIDTH;
    breadcrumb->geometry.y = TOOLBAR_HEIGHT;
    breadcrumb->geometry.w = WINDOW_WIDTH - SIDEBAR_WIDTH;
    breadcrumb->geometry.h = BREADCRUMB_HEIGHT;

    breadcrumb->segment_count = 0;
    breadcrumb->scroll_offset = 0;
}

/**
 * Initialize file view
 */
static void file_view_init(file_explorer_t *explorer) {
    file_view_t *view = &explorer->view;

    view->geometry.x = SIDEBAR_WIDTH;
    view->geometry.y = TOOLBAR_HEIGHT + BREADCRUMB_HEIGHT;
    view->geometry.w = WINDOW_WIDTH - SIDEBAR_WIDTH;
    view->geometry.h = WINDOW_HEIGHT - TOOLBAR_HEIGHT - BREADCRUMB_HEIGHT - STATUSBAR_HEIGHT;

    view->mode = VIEW_MODE_ICONS;
    view->item_count = 0;
    view->selection_count = 0;
    view->focus_index = -1;
    view->hovered_index = -1;
    view->scroll_offset = 0;
    view->max_scroll = 0;
    view->scroll_velocity = 0.0f;

    // Grid layout settings
    view->columns = 5;
    view->icon_size = 96;
    view->spacing = 16;

    // List column widths
    view->list_columns.name_width = 300;
    view->list_columns.size_width = 100;
    view->list_columns.type_width = 150;
    view->list_columns.modified_width = 200;

    view->dragging = false;
    view->show_context_menu = false;
}

/**
 * Initialize status bar
 */
static void statusbar_init(file_explorer_t *explorer) {
    statusbar_t *statusbar = &explorer->statusbar;

    statusbar->geometry.x = 0;
    statusbar->geometry.y = WINDOW_HEIGHT - STATUSBAR_HEIGHT;
    statusbar->geometry.w = WINDOW_WIDTH;
    statusbar->geometry.h = STATUSBAR_HEIGHT;

    statusbar->status_text[0] = '\0';
    statusbar->item_count = 0;
    statusbar->selected_count = 0;
    statusbar->total_size = 0;
    statusbar->show_progress = false;
    statusbar->progress = 0.0f;
}

/**
 * Create file explorer
 */
file_explorer_t* explorer_create(compositor_t *compositor, const char *initial_path) {
    file_explorer_t *explorer = calloc(1, sizeof(file_explorer_t));
    if (!explorer) {
        fprintf(stderr, "Failed to allocate file explorer\n");
        return NULL;
    }

    explorer->compositor = compositor;

    // Create window
    rect_t window_geometry = {100, 100, WINDOW_WIDTH, WINDOW_HEIGHT};
    explorer->window = window_create(1, WINDOW_NORMAL, &window_geometry);
    if (!explorer->window) {
        fprintf(stderr, "Failed to create window\n");
        free(explorer);
        return NULL;
    }

    window_set_title(explorer->window, "File Explorer");

    // Initialize UI components
    toolbar_init(explorer);
    sidebar_init(explorer);
    breadcrumb_init(explorer);
    file_view_init(explorer);
    statusbar_init(explorer);

    // Initialize state
    if (initial_path) {
        strncpy(explorer->current_path, initial_path, MAX_PATH_LEN - 1);
    } else {
        strcpy(explorer->current_path, "/home");
    }

    path_stack_init(&explorer->history);
    path_stack_push(&explorer->history, explorer->current_path);

    explorer->sort_mode = SORT_NAME_ASC;
    explorer->show_hidden = false;

    // Clipboard
    explorer->clipboard.count = 0;
    explorer->clipboard.is_cut = false;

    // Animation
    explorer->last_frame_time = get_time_us();
    explorer->needs_redraw = true;

    // Settings
    explorer->thumbnails_enabled = true;
    explorer->thumbnail_size = 128;
    explorer->kinetic_scrolling = true;
    explorer->smooth_animations = true;

    printf("[File Explorer] Created (window ID: %u)\n", explorer->window->id);

    // Load initial directory
    explorer_navigate_to(explorer, explorer->current_path);

    return explorer;
}

/**
 * Destroy file explorer
 */
void explorer_destroy(file_explorer_t *explorer) {
    if (!explorer) return;

    // TODO: Cleanup window, free resources

    free(explorer);
}

/**
 * Load directory contents
 */
static void load_directory(file_explorer_t *explorer, const char *path) {
    file_view_t *view = &explorer->view;

    // Clear current items
    view->item_count = 0;
    view->selection_count = 0;
    view->focus_index = -1;

    // Open directory
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "Failed to open directory: %s\n", path);
        return;
    }

    // Read entries
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && view->item_count < MAX_FILES_PER_DIR) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Skip hidden files if not shown
        if (!explorer->show_hidden && entry->d_name[0] == '.') {
            continue;
        }

        file_view_item_t *item = &view->items[view->item_count];

        // Get file stats
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        // Fill entry
        strncpy(item->entry.name, entry->d_name, sizeof(item->entry.name) - 1);
        strncpy(item->entry.path, full_path, sizeof(item->entry.path) - 1);
        item->entry.size = st.st_size;
        item->entry.modified_time = st.st_mtime;
        item->entry.created_time = st.st_ctime;
        item->entry.accessed_time = st.st_atime;
        item->entry.permissions = st.st_mode & 0777;
        item->entry.uid = st.st_uid;
        item->entry.gid = st.st_gid;
        item->entry.is_directory = S_ISDIR(st.st_mode);
        item->entry.is_hidden = (entry->d_name[0] == '.');
        item->entry.is_symlink = S_ISLNK(st.st_mode);
        item->entry.is_readonly = !(st.st_mode & S_IWUSR);
        item->entry.type = detect_file_type(entry->d_name, item->entry.is_directory);

        item->thumbnail = NULL;
        item->selected = false;
        item->hovered = false;
        item->animation_progress = 0.0f;

        view->item_count++;
    }

    closedir(dir);

    // Sort files
    // TODO: Implement sorting based on explorer->sort_mode

    // Update status bar
    explorer->statusbar.item_count = view->item_count;
    snprintf(explorer->statusbar.status_text, sizeof(explorer->statusbar.status_text),
             "%u items", view->item_count);

    printf("[File Explorer] Loaded %u items from: %s\n", view->item_count, path);
}

/**
 * Update breadcrumb from current path
 */
static void update_breadcrumb(file_explorer_t *explorer) {
    breadcrumb_t *breadcrumb = &explorer->breadcrumb;
    breadcrumb->segment_count = 0;

    char path_copy[MAX_PATH_LEN];
    strncpy(path_copy, explorer->current_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    // Split path into segments
    char *token = strtok(path_copy, "/");
    char accumulated_path[MAX_PATH_LEN] = "";
    size_t accumulated_len = 0;

    while (token && breadcrumb->segment_count < 32) {
        size_t token_len = strlen(token);
        if (accumulated_len + 1 + token_len >= sizeof(accumulated_path)) {
            break;
        }
        accumulated_path[accumulated_len++] = '/';
        memcpy(accumulated_path + accumulated_len, token, token_len + 1);
        accumulated_len += token_len;

        __typeof__(&breadcrumb->segments[0]) segment = &breadcrumb->segments[breadcrumb->segment_count];
        strncpy(segment->name, token, sizeof(segment->name) - 1);
        segment->name[sizeof(segment->name) - 1] = '\0';
        strncpy(segment->full_path, accumulated_path, sizeof(segment->full_path) - 1);
        segment->full_path[sizeof(segment->full_path) - 1] = '\0';
        segment->hovered = false;

        breadcrumb->segment_count++;
        token = strtok(NULL, "/");
    }
}

/**
 * Navigate to path
 */
void explorer_navigate_to(file_explorer_t *explorer, const char *path) {
    if (!explorer || !path) return;

    printf("[File Explorer] Navigating to: %s\n", path);

    strncpy(explorer->current_path, path, MAX_PATH_LEN - 1);
    path_stack_push(&explorer->history, path);

    load_directory(explorer, path);
    update_breadcrumb(explorer);

    explorer->needs_redraw = true;
}

/**
 * Navigate back
 */
void explorer_navigate_back(file_explorer_t *explorer) {
    if (!explorer) return;

    const char *path = path_stack_back(&explorer->history);
    if (path) {
        strncpy(explorer->current_path, path, MAX_PATH_LEN - 1);
        load_directory(explorer, path);
        update_breadcrumb(explorer);
        explorer->needs_redraw = true;
    }
}

/**
 * Navigate forward
 */
void explorer_navigate_forward(file_explorer_t *explorer) {
    if (!explorer) return;

    const char *path = path_stack_forward(&explorer->history);
    if (path) {
        strncpy(explorer->current_path, path, MAX_PATH_LEN - 1);
        load_directory(explorer, path);
        update_breadcrumb(explorer);
        explorer->needs_redraw = true;
    }
}

/**
 * Navigate to parent directory
 */
void explorer_navigate_parent(file_explorer_t *explorer) {
    if (!explorer) return;

    char parent_path[MAX_PATH_LEN];
    strncpy(parent_path, explorer->current_path, sizeof(parent_path) - 1);

    char *last_slash = strrchr(parent_path, '/');
    if (last_slash && last_slash != parent_path) {
        *last_slash = '\0';
        explorer_navigate_to(explorer, parent_path);
    } else if (last_slash == parent_path && strlen(parent_path) > 1) {
        explorer_navigate_to(explorer, "/");
    }
}

/**
 * Navigate to home
 */
void explorer_navigate_home(file_explorer_t *explorer) {
    if (!explorer) return;
    explorer_navigate_to(explorer, "/home");
}

/**
 * Set view mode
 */
void explorer_set_view_mode(file_explorer_t *explorer, view_mode_t mode) {
    if (!explorer) return;
    explorer->view.mode = mode;
    explorer->needs_redraw = true;
}

/**
 * Set sort mode
 */
void explorer_set_sort_mode(file_explorer_t *explorer, sort_mode_t mode) {
    if (!explorer) return;
    explorer->sort_mode = mode;
    // TODO: Re-sort items
    explorer->needs_redraw = true;
}

/**
 * Toggle hidden files
 */
void explorer_toggle_hidden_files(file_explorer_t *explorer) {
    if (!explorer) return;
    explorer->show_hidden = !explorer->show_hidden;
    load_directory(explorer, explorer->current_path);
    explorer->needs_redraw = true;
}

/**
 * Select item
 */
void explorer_select_item(file_explorer_t *explorer, uint32_t index) {
    if (!explorer || index >= explorer->view.item_count) return;

    file_view_t *view = &explorer->view;

    // Clear previous selection
    for (uint32_t i = 0; i < view->item_count; i++) {
        view->items[i].selected = false;
    }

    view->items[index].selected = true;
    view->selection_count = 1;
    view->selected_indices[0] = index;
    view->focus_index = index;

    explorer->needs_redraw = true;
}

/**
 * Clear selection
 */
void explorer_clear_selection(file_explorer_t *explorer) {
    if (!explorer) return;

    file_view_t *view = &explorer->view;
    for (uint32_t i = 0; i < view->item_count; i++) {
        view->items[i].selected = false;
    }
    view->selection_count = 0;

    explorer->needs_redraw = true;
}

/**
 * Select all
 */
void explorer_select_all(file_explorer_t *explorer) {
    if (!explorer) return;

    file_view_t *view = &explorer->view;
    view->selection_count = 0;

    for (uint32_t i = 0; i < view->item_count && view->selection_count < MAX_SELECTION; i++) {
        view->items[i].selected = true;
        view->selected_indices[view->selection_count++] = i;
    }

    explorer->needs_redraw = true;
}

/**
 * Open file
 */
void explorer_open_file(file_explorer_t *explorer, const char *path) {
    if (!explorer || !path) return;

    printf("[File Explorer] Opening: %s\n", path);

    // Check if directory
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        explorer_navigate_to(explorer, path);
    } else {
        // TODO: Launch default application for file type
        printf("[File Explorer] TODO: Open file with default app\n");
    }
}

/**
 * Copy selection to clipboard
 */
void explorer_copy_selection(file_explorer_t *explorer) {
    if (!explorer) return;

    file_view_t *view = &explorer->view;
    explorer->clipboard.count = 0;
    explorer->clipboard.is_cut = false;

    for (uint32_t i = 0; i < view->selection_count && i < MAX_SELECTION; i++) {
        uint32_t idx = view->selected_indices[i];
        strncpy(explorer->clipboard.paths[explorer->clipboard.count],
                view->items[idx].entry.path,
                MAX_PATH_LEN - 1);
        explorer->clipboard.count++;
    }

    printf("[File Explorer] Copied %u items\n", explorer->clipboard.count);
}

/**
 * Cut selection to clipboard
 */
void explorer_cut_selection(file_explorer_t *explorer) {
    if (!explorer) return;

    explorer_copy_selection(explorer);
    explorer->clipboard.is_cut = true;

    printf("[File Explorer] Cut %u items\n", explorer->clipboard.count);
}

/**
 * Paste from clipboard
 */
void explorer_paste(file_explorer_t *explorer) {
    if (!explorer || explorer->clipboard.count == 0) return;

    const char *sources[MAX_SELECTION];
    for (uint32_t i = 0; i < explorer->clipboard.count; i++) {
        sources[i] = explorer->clipboard.paths[i];
    }

    file_operation_t *op;
    if (explorer->clipboard.is_cut) {
        op = operation_move_files(sources, explorer->clipboard.count,
                                 explorer->current_path);
    } else {
        op = operation_copy_files(sources, explorer->clipboard.count,
                                 explorer->current_path);
    }

    if (op) {
        operation_start(op);
        printf("[File Explorer] Started %s operation\n",
               explorer->clipboard.is_cut ? "move" : "copy");
    }

    // Clear clipboard if cut
    if (explorer->clipboard.is_cut) {
        explorer->clipboard.count = 0;
    }
}

/**
 * Delete selection
 */
void explorer_delete_selection(file_explorer_t *explorer) {
    if (!explorer) return;

    file_view_t *view = &explorer->view;
    if (view->selection_count == 0) return;

    const char *paths[MAX_SELECTION];
    for (uint32_t i = 0; i < view->selection_count; i++) {
        paths[i] = view->items[view->selected_indices[i]].entry.path;
    }

    file_operation_t *op = operation_delete_files(paths, view->selection_count, false);
    if (op) {
        operation_start(op);
        printf("[File Explorer] Started delete operation\n");
    }
}

/**
 * Main render function
 */
void explorer_render(file_explorer_t *explorer) {
    if (!explorer || !explorer->needs_redraw) return;

    // Render all components
    explorer_render_toolbar(explorer);
    explorer_render_sidebar(explorer);
    explorer_render_breadcrumb(explorer);
    explorer_render_file_view(explorer);
    explorer_render_statusbar(explorer);

    // Update window surface
    // TODO: Composite to window texture and present

    explorer->needs_redraw = false;
}

/**
 * Render toolbar (stub - full implementation would use compositor API)
 */
void explorer_render_toolbar(file_explorer_t *explorer) {
    if (!explorer) return;
    // TODO: Draw toolbar background, buttons, search box
}

/**
 * Render sidebar (stub)
 */
void explorer_render_sidebar(file_explorer_t *explorer) {
    if (!explorer) return;
    // TODO: Draw sidebar background, places list
}

/**
 * Render breadcrumb (stub)
 */
void explorer_render_breadcrumb(file_explorer_t *explorer) {
    if (!explorer) return;
    // TODO: Draw breadcrumb segments with separators
}

/**
 * Render file view (stub)
 */
void explorer_render_file_view(file_explorer_t *explorer) {
    if (!explorer) return;
    // TODO: Draw files based on view mode
}

/**
 * Render status bar (stub)
 */
void explorer_render_statusbar(file_explorer_t *explorer) {
    if (!explorer) return;
    // TODO: Draw status text, item count, progress
}

/**
 * Main run loop
 */
void explorer_run(file_explorer_t *explorer) {
    if (!explorer) return;

    printf("[File Explorer] Starting main loop\n");

    bool running = true;
    while (running) {
        uint64_t frame_start = get_time_us();

        // Handle events
        // TODO: Get events from compositor

        // Update animations
        float delta = (frame_start - explorer->last_frame_time) / 1000000.0f;
        explorer->last_frame_time = frame_start;

        // Render
        explorer_render(explorer);

        // Frame limiting (60 FPS)
        uint64_t frame_time = get_time_us() - frame_start;
        if (frame_time < 16667) {
            usleep(16667 - frame_time);
        }
    }
}

/**
 * Event handlers (stubs)
 */
void explorer_handle_mouse_move(file_explorer_t *explorer, int32_t x, int32_t y) {
    // TODO: Update hover states, handle drag
}

void explorer_handle_mouse_button(file_explorer_t *explorer, int32_t x, int32_t y,
                                  uint32_t button, bool pressed) {
    // TODO: Handle clicks on buttons, files, etc.
}

void explorer_handle_mouse_scroll(file_explorer_t *explorer, int32_t delta) {
    // TODO: Scroll file view
}

void explorer_handle_key(file_explorer_t *explorer, uint32_t keycode, bool pressed) {
    // TODO: Handle keyboard shortcuts (Ctrl+C, Ctrl+V, etc.)
}

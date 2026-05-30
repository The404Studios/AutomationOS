/**
 * AutomationOS Terminal Emulator - Main Implementation
 *
 * Modern terminal with tabs, split panes, GPU acceleration
 */

#include "terminal.h"
#include "../../../userspace/libc/stdio.h"
#include "../../../userspace/libc/string.h"
#include "../../../userspace/libc/syscall.h"
#include <stdlib.h>

static void render_pane_recursive(terminal_t *term, pane_t *pane, theme_t *theme);
static void handle_tab_bar_click(terminal_t *term, int32_t x, int32_t y,
                                 uint32_t button, uint32_t action);
static uint32_t keycode_to_string(uint32_t keycode, uint32_t modifiers,
                                  char *buffer, uint32_t size);
static void poll_pane_pty(terminal_t *term, pane_t *pane);
static uint64_t get_tick_count(void);
static void sleep_ms(uint32_t ms);

int main(int argc, char **argv);

void _start(void) {
    char *argv[] = { "terminal", NULL };
    int status = main(1, argv);
    exit(status);
}

/**
 * Create a new terminal instance
 */
terminal_t *terminal_create(uint32_t width, uint32_t height) {
    terminal_t *term = (terminal_t *)malloc(sizeof(terminal_t));
    if (!term) {
        return NULL;
    }

    memset(term, 0, sizeof(terminal_t));

    // Initialize GPU context
    term->gpu = gpu_init("/dev/dri/card0");
    if (!term->gpu) {
        printf("Failed to initialize GPU\n");
        free(term);
        return NULL;
    }

    // Create framebuffer
    term->fb = gpu_create_framebuffer(term->gpu, width, height);
    if (!term->fb) {
        printf("Failed to create framebuffer\n");
        gpu_cleanup(term->gpu);
        free(term);
        return NULL;
    }

    term->width = width;
    term->height = height;

    // Load default themes
    theme_t *dark = theme_get_default_dark();
    theme_t *light = theme_get_default_light();
    theme_t *monokai = theme_get_monokai();
    theme_t *dracula = theme_get_dracula();

    terminal_add_theme(term, dark);
    terminal_add_theme(term, light);
    terminal_add_theme(term, monokai);
    terminal_add_theme(term, dracula);

    term->default_theme = 0; // Dark theme

    // Load default profile
    profile_t *default_profile = profile_get_default();
    terminal_add_profile(term, default_profile);
    term->default_profile = 0;

    // Initialize font rendering
    term->font_ctx = font_create("JetBrains Mono", 14);
    if (!term->font_ctx) {
        // Fallback to monospace
        term->font_ctx = font_create("Monospace", 14);
    }

    // Create initial tab
    tab_t *initial_tab = tab_create(term, default_profile);
    if (initial_tab) {
        tab_set_title(initial_tab, "Terminal");
        terminal_add_tab(term, initial_tab);
        term->active_tab = 0;
    }

    // Initialize settings
    term->vsync_enabled = true;
    term->gpu_acceleration = true;
    term->ligatures_enabled = true;
    term->emoji_support = true;
    term->tab_bar_visible = true;
    term->scrollbar_visible = true;
    term->fps_limit = 60;

    // Initialize history
    term->history_count = 0;
    term->history_index = -1;

    printf("Terminal initialized: %dx%d\n", width, height);
    printf("  Tabs: %d\n", term->tab_count);
    printf("  Themes: %d\n", term->theme_count);
    printf("  GPU: %s\n", gpu_get_renderer(term->gpu));

    return term;
}

/**
 * Destroy terminal and free resources
 */
void terminal_destroy(terminal_t *term) {
    if (!term) {
        return;
    }

    // Destroy all tabs
    for (uint32_t i = 0; i < term->tab_count; i++) {
        if (term->tabs[i]) {
            tab_destroy(term->tabs[i]);
        }
    }

    // Destroy profiles
    for (uint32_t i = 0; i < term->profile_count; i++) {
        if (term->profiles[i]) {
            profile_destroy(term->profiles[i]);
        }
    }

    // Destroy themes
    for (uint32_t i = 0; i < term->theme_count; i++) {
        if (term->themes[i]) {
            theme_destroy(term->themes[i]);
        }
    }

    // Destroy font context
    if (term->font_ctx) {
        font_destroy(term->font_ctx);
    }

    // Free command history
    for (uint32_t i = 0; i < term->history_count; i++) {
        if (term->command_history[i]) {
            free(term->command_history[i]);
        }
    }

    // Cleanup GPU resources
    if (term->fb) {
        gpu_destroy_framebuffer(term->gpu, term->fb);
    }
    if (term->gpu) {
        gpu_cleanup(term->gpu);
    }

    free(term);
}

/**
 * Main rendering function
 */
void terminal_render(terminal_t *term) {
    if (!term || !term->gpu || !term->fb) {
        return;
    }

    // Get current theme
    theme_t *theme = terminal_get_theme(term, term->default_theme);
    if (!theme) {
        return;
    }

    // Begin frame
    gpu_begin_frame(term->gpu, term->fb);

    // Clear background
    gpu_clear(term->gpu,
              theme->background.r / 255.0f,
              theme->background.g / 255.0f,
              theme->background.b / 255.0f,
              1.0f);

    // Calculate content area
    uint32_t content_y = term->tab_bar_visible ? TAB_BAR_HEIGHT : 0;
    uint32_t content_height = term->height - content_y;

    // Render tab bar
    if (term->tab_bar_visible) {
        renderer_render_tab_bar(term, theme);
    }

    // Get active tab
    if (term->active_tab < term->tab_count) {
        tab_t *tab = term->tabs[term->active_tab];
        if (tab && tab->root_pane) {
            // Render all panes recursively
            render_pane_recursive(term, tab->root_pane, theme);
        }
    }

    // End frame and present
    gpu_end_frame(term->gpu);
    gpu_present(term->gpu, term->fb, term->vsync_enabled);

    // Update FPS counter
    term->frame_count++;
    uint64_t current_time = get_tick_count(); // Assuming we have this function
    if (current_time - term->last_fps_update >= 1000) {
        term->fps = term->frame_count;
        term->frame_count = 0;
        term->last_fps_update = current_time;
    }
}

/**
 * Recursively render a pane and its children
 */
static void render_pane_recursive(terminal_t *term, pane_t *pane, theme_t *theme) {
    if (!pane) {
        return;
    }

    if (pane->split_type == SPLIT_NONE) {
        // Leaf pane - render terminal buffer
        if (pane->buffer) {
            renderer_render_buffer(term, pane->buffer, pane, theme);
            renderer_render_selection(term, pane->buffer, pane, theme);
            renderer_render_cursor(term, pane->buffer, pane, theme);

            if (term->scrollbar_visible) {
                renderer_render_scrollbar(term, pane, theme);
            }

            if (term->search_active) {
                renderer_render_search_highlight(term, pane->buffer, pane, theme);
            }
        }

        // Draw border if focused
        if (pane->focused) {
            rect_t border = {
                pane->x - 1, pane->y - 1,
                pane->width + 2, pane->height + 2
            };
            gpu_draw_rect(term->gpu, &border, color_to_u32(&theme->border_color));
        }
    } else {
        // Split pane - render children
        if (pane->first_child) {
            render_pane_recursive(term, pane->first_child, theme);
        }
        if (pane->second_child) {
            render_pane_recursive(term, pane->second_child, theme);
        }

        // Render split divider
        renderer_render_pane_borders(term, pane, theme);
    }
}

/**
 * Process terminal input (from PTY)
 */
void terminal_process_input(terminal_t *term, const char *input, uint32_t length) {
    if (!term || !input || length == 0) {
        return;
    }

    // Get active tab and focused pane
    if (term->active_tab >= term->tab_count) {
        return;
    }

    tab_t *tab = term->tabs[term->active_tab];
    if (!tab || !tab->focused_pane || !tab->focused_pane->buffer) {
        return;
    }

    // Parse VT100/ANSI sequences and update buffer
    terminal_buffer_t *buffer = tab->focused_pane->buffer;
    if (buffer->parser_state) {
        vt_parser_t *parser = (vt_parser_t *)buffer->parser_state;
        vt_parser_process(parser, (const uint8_t *)input, length);
    }
}

/**
 * Handle keyboard input
 */
void terminal_handle_key(terminal_t *term, uint32_t keycode, uint32_t modifiers) {
    if (!term) {
        return;
    }

    bool ctrl = modifiers & 0x01;
    bool shift = modifiers & 0x02;
    bool alt = modifiers & 0x04;

    // Global shortcuts
    if (ctrl && !shift && !alt) {
        switch (keycode) {
            case 'T': // Ctrl+T - New tab
                {
                    profile_t *profile = terminal_get_profile(term, term->default_profile);
                    tab_t *new_tab = tab_create(term, profile);
                    if (new_tab) {
                        tab_set_title(new_tab, "Terminal");
                        terminal_add_tab(term, new_tab);
                        terminal_switch_tab(term, term->tab_count - 1);
                    }
                }
                return;

            case 'W': // Ctrl+W - Close tab
                if (term->tab_count > 1) {
                    terminal_remove_tab(term, term->tabs[term->active_tab]->id);
                }
                return;

            case '\t': // Ctrl+Tab - Next tab
                terminal_next_tab(term);
                return;

            case 'N': // Ctrl+N - New window (not implemented)
                return;

            case 'F': // Ctrl+F - Search
                search_start(term, "");
                return;

            case 'C': // Ctrl+C - Copy
                selection_copy_to_clipboard(term);
                return;

            case 'V': // Ctrl+V - Paste
                selection_paste_from_clipboard(term);
                return;

            case '+': // Ctrl++ - Increase font size
                if (term->font_ctx) {
                    term->font_ctx->font_size++;
                    // Reload font
                }
                return;

            case '-': // Ctrl+- - Decrease font size
                if (term->font_ctx && term->font_ctx->font_size > 6) {
                    term->font_ctx->font_size--;
                    // Reload font
                }
                return;
        }
    }

    // Tab shortcuts (Ctrl+1-9)
    if (ctrl && keycode >= '1' && keycode <= '9') {
        uint32_t tab_index = keycode - '1';
        if (tab_index < term->tab_count) {
            terminal_switch_tab(term, tab_index);
        }
        return;
    }

    // Pane shortcuts
    if (alt) {
        switch (keycode) {
            case 'H': // Alt+H - Split horizontal
                if (term->active_tab < term->tab_count) {
                    tab_t *tab = term->tabs[term->active_tab];
                    if (tab->focused_pane) {
                        pane_split_horizontal(tab->focused_pane, 0.5f);
                        tab->pane_count++;
                    }
                }
                return;

            case 'V': // Alt+V - Split vertical
                if (term->active_tab < term->tab_count) {
                    tab_t *tab = term->tabs[term->active_tab];
                    if (tab->focused_pane) {
                        pane_split_vertical(tab->focused_pane, 0.5f);
                        tab->pane_count++;
                    }
                }
                return;

            case 'X': // Alt+X - Close pane
                if (term->active_tab < term->tab_count) {
                    tab_t *tab = term->tabs[term->active_tab];
                    if (tab->focused_pane && tab->pane_count > 1) {
                        pane_close(tab->focused_pane);
                        tab->pane_count--;
                    }
                }
                return;
        }
    }

    // Forward to active pane's PTY
    if (term->active_tab < term->tab_count) {
        tab_t *tab = term->tabs[term->active_tab];
        if (tab && tab->focused_pane) {
            pane_t *pane = tab->focused_pane;
            if (pane->pty_fd >= 0) {
                // Convert keycode to string and write to PTY
                char key_buf[16];
                uint32_t key_len = keycode_to_string(keycode, modifiers, key_buf, sizeof(key_buf));
                if (key_len > 0) {
                    pty_write(pane->pty_fd, (const uint8_t *)key_buf, key_len);
                }
            }
        }
    }
}

/**
 * Handle mouse input
 */
void terminal_handle_mouse(terminal_t *term, int32_t x, int32_t y, uint32_t button, uint32_t action) {
    if (!term) {
        return;
    }

    // Check if click is in tab bar
    if (term->tab_bar_visible && y < TAB_BAR_HEIGHT) {
        handle_tab_bar_click(term, x, y, button, action);
        return;
    }

    // Check if click is in a pane
    if (term->active_tab < term->tab_count) {
        tab_t *tab = term->tabs[term->active_tab];
        if (tab && tab->root_pane) {
            pane_t *clicked_pane = pane_find_at_position(tab->root_pane, x, y);
            if (clicked_pane) {
                // Focus the pane
                if (tab->focused_pane) {
                    tab->focused_pane->focused = false;
                }
                clicked_pane->focused = true;
                tab->focused_pane = clicked_pane;

                // Handle selection
                if (button == 1) { // Left button
                    if (action == 0) { // Press
                        selection_start(&clicked_pane->buffer->selection,
                                      (x - clicked_pane->x) / CELL_WIDTH,
                                      (y - clicked_pane->y) / CELL_HEIGHT);
                    } else if (action == 1) { // Release
                        selection_end(&clicked_pane->buffer->selection);
                    }
                } else if (button == 3) { // Right button - context menu
                    // TODO: Show context menu
                }

                // Check for hyperlink
                if (button == 1 && action == 1) {
                    int32_t cell_x = (x - clicked_pane->x) / CELL_WIDTH;
                    int32_t cell_y = (y - clicked_pane->y) / CELL_HEIGHT;
                    hyperlink_t *link = hyperlink_at_position(clicked_pane->buffer, cell_x, cell_y);
                    if (link) {
                        hyperlink_open(link->url);
                    }
                }
            }
        }
    }
}

/**
 * Handle window resize
 */
void terminal_resize(terminal_t *term, uint32_t width, uint32_t height) {
    if (!term) {
        return;
    }

    term->width = width;
    term->height = height;

    // Recreate framebuffer
    if (term->fb) {
        gpu_destroy_framebuffer(term->gpu, term->fb);
    }
    term->fb = gpu_create_framebuffer(term->gpu, width, height);

    // Resize all terminal buffers
    uint32_t content_height = height - (term->tab_bar_visible ? TAB_BAR_HEIGHT : 0);
    uint32_t cols = width / CELL_WIDTH;
    uint32_t rows = content_height / CELL_HEIGHT;

    for (uint32_t i = 0; i < term->tab_count; i++) {
        if (term->tabs[i] && term->tabs[i]->root_pane) {
            pane_resize(term->tabs[i]->root_pane, cols, rows);
        }
    }
}

/**
 * Handle tab bar clicks
 */
static void handle_tab_bar_click(terminal_t *term, int32_t x, int32_t y, uint32_t button, uint32_t action) {
    if (action != 1) { // Only handle button release
        return;
    }

    // Check for new tab button
    if (x >= term->width - 40 && x < term->width - 10) {
        // New tab button clicked
        profile_t *profile = terminal_get_profile(term, term->default_profile);
        tab_t *new_tab = tab_create(term, profile);
        if (new_tab) {
            tab_set_title(new_tab, "Terminal");
            terminal_add_tab(term, new_tab);
            terminal_switch_tab(term, term->tab_count - 1);
        }
        return;
    }

    // Check which tab was clicked
    int32_t tab_x = 10;
    int32_t tab_width = 150;

    for (uint32_t i = 0; i < term->tab_count; i++) {
        if (x >= tab_x && x < tab_x + tab_width) {
            if (button == 1) {
                // Left click - switch to tab
                terminal_switch_tab(term, i);
            } else if (button == 2) {
                // Middle click - close tab
                if (term->tab_count > 1) {
                    terminal_remove_tab(term, term->tabs[i]->id);
                }
            }
            return;
        }
        tab_x += tab_width + 5;
    }
}

/**
 * Convert keycode to string for PTY
 */
static uint32_t keycode_to_string(uint32_t keycode, uint32_t modifiers, char *buffer, uint32_t size) {
    bool ctrl = modifiers & 0x01;
    bool shift = modifiers & 0x02;
    bool alt = modifiers & 0x04;

    // Handle special keys
    switch (keycode) {
        case 0xFF50: // Home
            return snprintf(buffer, size, "\x1b[H");
        case 0xFF51: // Left
            return snprintf(buffer, size, "\x1b[D");
        case 0xFF52: // Up
            return snprintf(buffer, size, "\x1b[A");
        case 0xFF53: // Right
            return snprintf(buffer, size, "\x1b[C");
        case 0xFF54: // Down
            return snprintf(buffer, size, "\x1b[B");
        case 0xFF55: // Page Up
            return snprintf(buffer, size, "\x1b[5~");
        case 0xFF56: // Page Down
            return snprintf(buffer, size, "\x1b[6~");
        case 0xFF57: // End
            return snprintf(buffer, size, "\x1b[F");
        case 0xFF63: // Insert
            return snprintf(buffer, size, "\x1b[2~");
        case 0xFFFF: // Delete
            return snprintf(buffer, size, "\x1b[3~");
        case '\n':
        case '\r':
            return snprintf(buffer, size, "\r");
        case '\t':
            return snprintf(buffer, size, "\t");
        case 0x08: // Backspace
            return snprintf(buffer, size, "\x7f");
        case 0x1b: // Escape
            return snprintf(buffer, size, "\x1b");
    }

    // Function keys
    if (keycode >= 0xFFBE && keycode <= 0xFFC9) {
        uint32_t fn = keycode - 0xFFBE + 1;
        if (fn <= 4) {
            return snprintf(buffer, size, "\x1bO%c", 'P' + fn - 1);
        } else {
            return snprintf(buffer, size, "\x1b[%d~", fn + 10);
        }
    }

    // Control characters
    if (ctrl && keycode >= 'A' && keycode <= 'Z') {
        buffer[0] = keycode - 'A' + 1;
        return 1;
    }
    if (ctrl && keycode >= 'a' && keycode <= 'z') {
        buffer[0] = keycode - 'a' + 1;
        return 1;
    }

    // Regular ASCII
    if (keycode < 128) {
        buffer[0] = (char)keycode;
        return 1;
    }

    return 0;
}

/**
 * Main entry point
 */
int main(int argc, char **argv) {
    printf("AutomationOS Terminal Emulator v1.0\n");

    // Parse command line arguments
    uint32_t width = 1280;
    uint32_t height = 720;
    const char *profile_name = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            profile_name = argv[++i];
        }
    }

    // Create terminal
    terminal_t *term = terminal_create(width, height);
    if (!term) {
        printf("Failed to create terminal\n");
        return 1;
    }

    // Main event loop
    bool running = true;
    while (running) {
        // Poll for PTY data
        for (uint32_t i = 0; i < term->tab_count; i++) {
            tab_t *tab = term->tabs[i];
            if (tab && tab->root_pane) {
                poll_pane_pty(term, tab->root_pane);
            }
        }

        // Handle window events (keyboard, mouse, etc.)
        // TODO: Integrate with window manager event system

        // Render frame
        terminal_render(term);

        // Rate limiting
        if (term->fps_limit > 0) {
            uint32_t frame_time_ms = 1000 / term->fps_limit;
            sleep_ms(frame_time_ms);
        }
    }

    // Cleanup
    terminal_destroy(term);
    printf("Terminal exited\n");
    return 0;
}

/**
 * Poll PTY for output data
 */
static void poll_pane_pty(terminal_t *term, pane_t *pane) {
    if (!pane) {
        return;
    }

    if (pane->split_type == SPLIT_NONE) {
        if (pane->pty_fd >= 0) {
            uint8_t buffer[4096];
            int32_t bytes_read = pty_read(pane->pty_fd, buffer, sizeof(buffer));
            if (bytes_read > 0) {
                terminal_process_input(term, (const char *)buffer, bytes_read);
            }
        }
    } else {
        if (pane->first_child) {
            poll_pane_pty(term, pane->first_child);
        }
        if (pane->second_child) {
            poll_pane_pty(term, pane->second_child);
        }
    }
}

/**
 * Utility: Get tick count in milliseconds
 */
static uint64_t get_tick_count(void) {
    // TODO: Implement using system clock syscall
    return 0;
}

/**
 * Utility: Sleep for milliseconds
 */
static void sleep_ms(uint32_t ms) {
    // TODO: Implement using nanosleep syscall
}

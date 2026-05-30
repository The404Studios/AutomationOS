/**
 * AutomationOS Terminal - Tab Management
 *
 * Multiple tab support with profiles
 */

#include "terminal.h"
#include "../../../userspace/libc/stdio.h"
#include "../../../userspace/libc/string.h"
#include <stdlib.h>

static uint32_t next_tab_id = 1;

/**
 * Create a new tab
 */
tab_t *tab_create(terminal_t *term, profile_t *profile) {
    if (!term) {
        return NULL;
    }

    tab_t *tab = (tab_t *)malloc(sizeof(tab_t));
    if (!tab) {
        return NULL;
    }

    memset(tab, 0, sizeof(tab_t));
    tab->id = next_tab_id++;
    strcpy(tab->title, "Terminal");
    tab->profile_id = profile ? profile->id : -1;
    tab->pane_count = 1;

    // Calculate terminal dimensions
    uint32_t content_height = term->height - (term->tab_bar_visible ? TAB_BAR_HEIGHT : 0);
    uint32_t cols = term->width / CELL_WIDTH;
    uint32_t rows = content_height / CELL_HEIGHT;

    // Create root pane
    tab->root_pane = pane_create(cols, rows);
    if (!tab->root_pane) {
        free(tab);
        return NULL;
    }

    tab->root_pane->x = 0;
    tab->root_pane->y = term->tab_bar_visible ? TAB_BAR_HEIGHT : 0;
    tab->root_pane->width = term->width;
    tab->root_pane->height = content_height;
    tab->root_pane->focused = true;

    tab->focused_pane = tab->root_pane;

    // Open PTY and spawn shell
    const char *shell = profile ? profile->shell : "/bin/bash";
    const char *working_dir = profile ? profile->working_dir : "/";

    tab->root_pane->pty_fd = pty_open(cols, rows);
    if (tab->root_pane->pty_fd >= 0) {
        // Spawn shell process
        char *argv[] = { (char *)shell, NULL };
        tab->root_pane->child_pid = pty_spawn(tab->root_pane->pty_fd, shell, argv);

        if (tab->root_pane->child_pid < 0) {
            printf("Failed to spawn shell\n");
            pty_close(tab->root_pane->pty_fd);
            tab->root_pane->pty_fd = -1;
        }
    }

    return tab;
}

/**
 * Destroy tab and free resources
 */
void tab_destroy(tab_t *tab) {
    if (!tab) {
        return;
    }

    // Destroy root pane (recursively destroys children)
    if (tab->root_pane) {
        pane_destroy(tab->root_pane);
    }

    free(tab);
}

/**
 * Set tab title
 */
void tab_set_title(tab_t *tab, const char *title) {
    if (!tab || !title) {
        return;
    }

    strncpy(tab->title, title, MAX_TITLE_LENGTH - 1);
    tab->title[MAX_TITLE_LENGTH - 1] = '\0';
}

/**
 * Add tab to terminal
 */
void terminal_add_tab(terminal_t *term, tab_t *tab) {
    if (!term || !tab) {
        return;
    }

    if (term->tab_count >= MAX_TABS) {
        printf("Maximum tabs reached\n");
        tab_destroy(tab);
        return;
    }

    term->tabs[term->tab_count] = tab;
    term->tab_count++;
}

/**
 * Remove tab from terminal
 */
void terminal_remove_tab(terminal_t *term, uint32_t tab_id) {
    if (!term) {
        return;
    }

    // Find tab index
    int32_t tab_index = -1;
    for (uint32_t i = 0; i < term->tab_count; i++) {
        if (term->tabs[i] && term->tabs[i]->id == tab_id) {
            tab_index = i;
            break;
        }
    }

    if (tab_index < 0) {
        return;
    }

    // Destroy tab
    tab_destroy(term->tabs[tab_index]);

    // Shift remaining tabs
    for (uint32_t i = tab_index; i < term->tab_count - 1; i++) {
        term->tabs[i] = term->tabs[i + 1];
    }

    term->tab_count--;
    term->tabs[term->tab_count] = NULL;

    // Update active tab if needed
    if (term->active_tab >= term->tab_count) {
        term->active_tab = term->tab_count > 0 ? term->tab_count - 1 : 0;
    }
}

/**
 * Switch to specific tab
 */
void terminal_switch_tab(terminal_t *term, uint32_t tab_index) {
    if (!term || tab_index >= term->tab_count) {
        return;
    }

    term->active_tab = tab_index;
}

/**
 * Switch to next tab
 */
void terminal_next_tab(terminal_t *term) {
    if (!term || term->tab_count == 0) {
        return;
    }

    term->active_tab = (term->active_tab + 1) % term->tab_count;
}

/**
 * Switch to previous tab
 */
void terminal_prev_tab(terminal_t *term) {
    if (!term || term->tab_count == 0) {
        return;
    }

    if (term->active_tab == 0) {
        term->active_tab = term->tab_count - 1;
    } else {
        term->active_tab--;
    }
}

/**
 * Move tab to new position
 */
void terminal_move_tab(terminal_t *term, uint32_t from_index, uint32_t to_index) {
    if (!term || from_index >= term->tab_count || to_index >= term->tab_count) {
        return;
    }

    if (from_index == to_index) {
        return;
    }

    // Save tab pointer
    tab_t *tab = term->tabs[from_index];

    // Shift tabs
    if (from_index < to_index) {
        // Moving right
        for (uint32_t i = from_index; i < to_index; i++) {
            term->tabs[i] = term->tabs[i + 1];
        }
    } else {
        // Moving left
        for (uint32_t i = from_index; i > to_index; i--) {
            term->tabs[i] = term->tabs[i - 1];
        }
    }

    // Place tab at new position
    term->tabs[to_index] = tab;

    // Update active tab if needed
    if (term->active_tab == from_index) {
        term->active_tab = to_index;
    } else if (from_index < term->active_tab && to_index >= term->active_tab) {
        term->active_tab--;
    } else if (from_index > term->active_tab && to_index <= term->active_tab) {
        term->active_tab++;
    }
}

/**
 * Create a pane
 */
pane_t *pane_create(uint32_t cols, uint32_t rows) {
    static uint32_t next_pane_id = 1;

    pane_t *pane = (pane_t *)malloc(sizeof(pane_t));
    if (!pane) {
        return NULL;
    }

    memset(pane, 0, sizeof(pane_t));
    pane->id = next_pane_id++;
    pane->split_type = SPLIT_NONE;
    pane->pty_fd = -1;
    pane->child_pid = -1;

    // Create terminal buffer
    pane->buffer = buffer_create(cols, rows);
    if (!pane->buffer) {
        free(pane);
        return NULL;
    }

    // Create VT parser
    pane->buffer->parser_state = vt_parser_create(pane->buffer);

    return pane;
}

/**
 * Destroy pane (recursively)
 */
void pane_destroy(pane_t *pane) {
    if (!pane) {
        return;
    }

    // Destroy children first
    if (pane->first_child) {
        pane_destroy(pane->first_child);
    }
    if (pane->second_child) {
        pane_destroy(pane->second_child);
    }

    // Close PTY
    if (pane->pty_fd >= 0) {
        pty_close(pane->pty_fd);
    }

    // Terminate child process
    if (pane->child_pid > 0) {
        // TODO: Send SIGTERM/SIGKILL
    }

    // Destroy buffer and parser
    if (pane->buffer) {
        if (pane->buffer->parser_state) {
            vt_parser_destroy((vt_parser_t *)pane->buffer->parser_state);
        }
        buffer_destroy(pane->buffer);
    }

    free(pane);
}

/**
 * Split pane horizontally
 */
void pane_split_horizontal(pane_t *pane, float ratio) {
    if (!pane || pane->split_type != SPLIT_NONE) {
        return;
    }

    // Calculate dimensions
    uint32_t first_height = (uint32_t)(pane->height * ratio);
    uint32_t second_height = pane->height - first_height - 2; // 2px for divider

    uint32_t cols = pane->width / CELL_WIDTH;
    uint32_t first_rows = first_height / CELL_HEIGHT;
    uint32_t second_rows = second_height / CELL_HEIGHT;

    // Create child panes
    pane_t *first = pane_create(cols, first_rows);
    pane_t *second = pane_create(cols, second_rows);

    if (!first || !second) {
        if (first) pane_destroy(first);
        if (second) pane_destroy(second);
        return;
    }

    // Set positions
    first->x = pane->x;
    first->y = pane->y;
    first->width = pane->width;
    first->height = first_height;

    second->x = pane->x;
    second->y = pane->y + first_height + 2;
    second->width = pane->width;
    second->height = second_height;

    // Open PTYs for both panes
    first->pty_fd = pty_open(cols, first_rows);
    second->pty_fd = pty_open(cols, second_rows);

    if (first->pty_fd >= 0) {
        char *argv[] = { "/bin/bash", NULL };
        first->child_pid = pty_spawn(first->pty_fd, "/bin/bash", argv);
    }

    if (second->pty_fd >= 0) {
        char *argv[] = { "/bin/bash", NULL };
        second->child_pid = pty_spawn(second->pty_fd, "/bin/bash", argv);
    }

    // Convert this pane to split pane
    pane->split_type = SPLIT_HORIZONTAL;
    pane->first_child = first;
    pane->second_child = second;
    pane->split_ratio = ratio;

    // Transfer focus to first child
    first->focused = pane->focused;
    pane->focused = false;

    // Destroy this pane's buffer (no longer needed)
    if (pane->buffer) {
        if (pane->buffer->parser_state) {
            vt_parser_destroy((vt_parser_t *)pane->buffer->parser_state);
        }
        buffer_destroy(pane->buffer);
        pane->buffer = NULL;
    }

    // Close PTY
    if (pane->pty_fd >= 0) {
        pty_close(pane->pty_fd);
        pane->pty_fd = -1;
    }
}

/**
 * Split pane vertically
 */
void pane_split_vertical(pane_t *pane, float ratio) {
    if (!pane || pane->split_type != SPLIT_NONE) {
        return;
    }

    // Calculate dimensions
    uint32_t first_width = (uint32_t)(pane->width * ratio);
    uint32_t second_width = pane->width - first_width - 2; // 2px for divider

    uint32_t first_cols = first_width / CELL_WIDTH;
    uint32_t second_cols = second_width / CELL_WIDTH;
    uint32_t rows = pane->height / CELL_HEIGHT;

    // Create child panes
    pane_t *first = pane_create(first_cols, rows);
    pane_t *second = pane_create(second_cols, rows);

    if (!first || !second) {
        if (first) pane_destroy(first);
        if (second) pane_destroy(second);
        return;
    }

    // Set positions
    first->x = pane->x;
    first->y = pane->y;
    first->width = first_width;
    first->height = pane->height;

    second->x = pane->x + first_width + 2;
    second->y = pane->y;
    second->width = second_width;
    second->height = pane->height;

    // Open PTYs for both panes
    first->pty_fd = pty_open(first_cols, rows);
    second->pty_fd = pty_open(second_cols, rows);

    if (first->pty_fd >= 0) {
        char *argv[] = { "/bin/bash", NULL };
        first->child_pid = pty_spawn(first->pty_fd, "/bin/bash", argv);
    }

    if (second->pty_fd >= 0) {
        char *argv[] = { "/bin/bash", NULL };
        second->child_pid = pty_spawn(second->pty_fd, "/bin/bash", argv);
    }

    // Convert this pane to split pane
    pane->split_type = SPLIT_VERTICAL;
    pane->first_child = first;
    pane->second_child = second;
    pane->split_ratio = ratio;

    // Transfer focus to first child
    first->focused = pane->focused;
    pane->focused = false;

    // Destroy this pane's buffer (no longer needed)
    if (pane->buffer) {
        if (pane->buffer->parser_state) {
            vt_parser_destroy((vt_parser_t *)pane->buffer->parser_state);
        }
        buffer_destroy(pane->buffer);
        pane->buffer = NULL;
    }

    // Close PTY
    if (pane->pty_fd >= 0) {
        pty_close(pane->pty_fd);
        pane->pty_fd = -1;
    }
}

/**
 * Close pane (not implemented - complex)
 */
void pane_close(pane_t *pane) {
    // TODO: Implement pane closing with proper parent updates
    // This is complex as it requires restructuring the pane tree
}

/**
 * Find pane at screen position
 */
pane_t *pane_find_at_position(pane_t *root, int32_t x, int32_t y) {
    if (!root) {
        return NULL;
    }

    if (root->split_type == SPLIT_NONE) {
        // Leaf pane - check if point is inside
        if (x >= root->x && x < root->x + root->width &&
            y >= root->y && y < root->y + root->height) {
            return root;
        }
        return NULL;
    }

    // Check children
    pane_t *result = pane_find_at_position(root->first_child, x, y);
    if (result) {
        return result;
    }

    return pane_find_at_position(root->second_child, x, y);
}

/**
 * Focus next pane
 */
void pane_focus_next(pane_t *root) {
    // TODO: Implement pane focus cycling
}

/**
 * Focus previous pane
 */
void pane_focus_prev(pane_t *root) {
    // TODO: Implement pane focus cycling
}

/**
 * Resize pane
 */
void pane_resize(pane_t *pane, uint32_t width, uint32_t height) {
    if (!pane) {
        return;
    }

    pane->width = width;
    pane->height = height;

    if (pane->split_type == SPLIT_NONE) {
        // Leaf pane - resize buffer
        uint32_t cols = width / CELL_WIDTH;
        uint32_t rows = height / CELL_HEIGHT;

        if (pane->buffer) {
            buffer_resize(pane->buffer, cols, rows);
        }

        if (pane->pty_fd >= 0) {
            pty_resize(pane->pty_fd, cols, rows);
        }
    } else {
        // Split pane - resize children
        if (pane->split_type == SPLIT_HORIZONTAL) {
            uint32_t first_height = (uint32_t)(height * pane->split_ratio);
            uint32_t second_height = height - first_height - 2;

            if (pane->first_child) {
                pane->first_child->height = first_height;
                pane_resize(pane->first_child, width, first_height);
            }

            if (pane->second_child) {
                pane->second_child->y = pane->y + first_height + 2;
                pane->second_child->height = second_height;
                pane_resize(pane->second_child, width, second_height);
            }
        } else if (pane->split_type == SPLIT_VERTICAL) {
            uint32_t first_width = (uint32_t)(width * pane->split_ratio);
            uint32_t second_width = width - first_width - 2;

            if (pane->first_child) {
                pane->first_child->width = first_width;
                pane_resize(pane->first_child, first_width, height);
            }

            if (pane->second_child) {
                pane->second_child->x = pane->x + first_width + 2;
                pane->second_child->width = second_width;
                pane_resize(pane->second_child, second_width, height);
            }
        }
    }
}

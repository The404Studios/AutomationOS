/**
 * AutomationOS Terminal - GPU-Accelerated Renderer
 *
 * High-performance terminal rendering with GPU acceleration
 */

#include "terminal.h"
#include "../../../userspace/compositor/gpu.h"
#include "../../../userspace/libc/string.h"
#include <stdlib.h>

/**
 * Render terminal buffer to pane
 */
void renderer_render_buffer(terminal_t *term, const terminal_buffer_t *buffer,
                            const pane_t *pane, const theme_t *theme) {
    if (!term || !buffer || !pane || !theme) {
        return;
    }

    // Only render if pane has changes or full redraw requested
    if (!buffer->full_redraw) {
        bool has_changes = false;
        for (uint32_t y = 0; y < buffer->rows; y++) {
            if (buffer->dirty_lines[y]) {
                has_changes = true;
                break;
            }
        }
        if (!has_changes) {
            return;
        }
    }

    // Render each line
    for (uint32_t y = 0; y < buffer->rows; y++) {
        if (!buffer->full_redraw && !buffer->dirty_lines[y]) {
            continue;
        }

        uint32_t line_offset = y * buffer->cols;
        int32_t screen_y = pane->y + y * CELL_HEIGHT;

        for (uint32_t x = 0; x < buffer->cols; x++) {
            const cell_t *cell = &buffer->cells[line_offset + x];
            int32_t screen_x = pane->x + x * CELL_WIDTH;

            // Determine colors (handle reverse video)
            color_t fg = cell->fg;
            color_t bg = cell->bg;

            if (cell->flags & CELL_REVERSE) {
                color_t temp = fg;
                fg = bg;
                bg = temp;
            }

            // Handle dim
            if (cell->flags & CELL_DIM) {
                fg.r = fg.r * 2 / 3;
                fg.g = fg.g * 2 / 3;
                fg.b = fg.b * 2 / 3;
            }

            // Handle bold (brighten)
            if (cell->flags & CELL_BOLD) {
                fg.r = (fg.r + 255) / 2;
                fg.g = (fg.g + 255) / 2;
                fg.b = (fg.b + 255) / 2;
            }

            // Draw background
            rect_t bg_rect = {
                screen_x, screen_y,
                CELL_WIDTH, CELL_HEIGHT
            };
            gpu_draw_rect(term->gpu, &bg_rect, color_to_u32(&bg));

            // Draw character
            if (cell->codepoint != ' ' && cell->codepoint != 0) {
                if (!(cell->flags & CELL_HIDDEN)) {
                    font_render_glyph(term->font_ctx, term->gpu,
                                    cell->codepoint, screen_x, screen_y, &fg);
                }
            }

            // Draw underline
            if (cell->flags & CELL_UNDERLINE) {
                rect_t underline = {
                    screen_x, screen_y + CELL_HEIGHT - 2,
                    CELL_WIDTH, 1
                };
                gpu_draw_rect(term->gpu, &underline, color_to_u32(&fg));
            }

            // Draw overline
            if (cell->flags & CELL_OVERLINE) {
                rect_t overline = {
                    screen_x, screen_y,
                    CELL_WIDTH, 1
                };
                gpu_draw_rect(term->gpu, &overline, color_to_u32(&fg));
            }

            // Draw strikethrough
            if (cell->flags & CELL_STRIKETHROUGH) {
                rect_t strike = {
                    screen_x, screen_y + CELL_HEIGHT / 2,
                    CELL_WIDTH, 1
                };
                gpu_draw_rect(term->gpu, &strike, color_to_u32(&fg));
            }

            // Draw URL indicator
            if (cell->flags & CELL_URL) {
                rect_t url_indicator = {
                    screen_x, screen_y + CELL_HEIGHT - 1,
                    CELL_WIDTH, 1
                };
                color_t url_color = color_from_rgb(100, 149, 237); // Cornflower blue
                gpu_draw_rect(term->gpu, &url_indicator, color_to_u32(&url_color));
            }
        }
    }

    // Clear dirty flags
    if (buffer->full_redraw) {
        ((terminal_buffer_t *)buffer)->full_redraw = false;
    }
    for (uint32_t y = 0; y < buffer->rows; y++) {
        ((terminal_buffer_t *)buffer)->dirty_lines[y] = false;
    }
}

/**
 * Render cursor
 */
void renderer_render_cursor(terminal_t *term, const terminal_buffer_t *buffer,
                           const pane_t *pane, const theme_t *theme) {
    if (!term || !buffer || !pane || !theme || !buffer->cursor.visible) {
        return;
    }

    const cursor_t *cursor = &buffer->cursor;

    // Check cursor blink
    if (cursor->blink) {
        // TODO: Implement blink timer
        // For now, just show it
    }

    // Calculate screen position
    int32_t screen_x = pane->x + cursor->x * CELL_WIDTH;
    int32_t screen_y = pane->y + cursor->y * CELL_HEIGHT;

    // Draw cursor (block style)
    rect_t cursor_rect = {
        screen_x, screen_y,
        CELL_WIDTH, CELL_HEIGHT
    };

    // Use theme cursor color or default
    color_t cursor_color = theme->cursor;
    cursor_color.a = 200; // Semi-transparent

    // Blend mode for cursor
    gpu_set_blend_mode(term->gpu, true);
    gpu_draw_rect(term->gpu, &cursor_rect, color_to_u32(&cursor_color));
    gpu_set_blend_mode(term->gpu, false);
}

/**
 * Render selection
 */
void renderer_render_selection(terminal_t *term, const terminal_buffer_t *buffer,
                               const pane_t *pane, const theme_t *theme) {
    if (!term || !buffer || !pane || !theme || !buffer->selection.active) {
        return;
    }

    const selection_t *sel = &buffer->selection;

    // Normalize selection coordinates
    int32_t start_x = sel->start_x;
    int32_t start_y = sel->start_y;
    int32_t end_x = sel->end_x;
    int32_t end_y = sel->end_y;

    if (start_y > end_y || (start_y == end_y && start_x > end_x)) {
        // Swap
        int32_t temp_x = start_x;
        int32_t temp_y = start_y;
        start_x = end_x;
        start_y = end_y;
        end_x = temp_x;
        end_y = temp_y;
    }

    // Enable blending
    gpu_set_blend_mode(term->gpu, true);

    // Render selection rectangles
    for (int32_t y = start_y; y <= end_y; y++) {
        if (y < 0 || y >= buffer->rows) {
            continue;
        }

        int32_t line_start_x = (y == start_y) ? start_x : 0;
        int32_t line_end_x = (y == end_y) ? end_x : buffer->cols - 1;

        if (sel->rectangular) {
            // Block selection
            line_start_x = start_x;
            line_end_x = end_x;
        }

        int32_t screen_x = pane->x + line_start_x * CELL_WIDTH;
        int32_t screen_y = pane->y + y * CELL_HEIGHT;
        int32_t width = (line_end_x - line_start_x + 1) * CELL_WIDTH;

        rect_t sel_rect = {
            screen_x, screen_y,
            width, CELL_HEIGHT
        };

        color_t sel_color = theme->selection_bg;
        sel_color.a = 128; // Semi-transparent

        gpu_draw_rect(term->gpu, &sel_rect, color_to_u32(&sel_color));
    }

    gpu_set_blend_mode(term->gpu, false);
}

/**
 * Render scrollbar
 */
void renderer_render_scrollbar(terminal_t *term, const pane_t *pane, const theme_t *theme) {
    if (!term || !pane || !theme || !pane->buffer) {
        return;
    }

    const scrollback_t *scrollback = &pane->buffer->scrollback;

    // Only show scrollbar if there's scrollback content
    if (scrollback->count == 0) {
        return;
    }

    // Calculate scrollbar dimensions
    int32_t scrollbar_x = pane->x + pane->width - SCROLLBAR_WIDTH;
    int32_t scrollbar_y = pane->y;
    int32_t scrollbar_height = pane->height;

    // Draw scrollbar background
    rect_t bg_rect = {
        scrollbar_x, scrollbar_y,
        SCROLLBAR_WIDTH, scrollbar_height
    };
    gpu_draw_rect(term->gpu, &bg_rect, color_to_u32(&theme->scrollbar_bg));

    // Calculate thumb size and position
    uint32_t total_lines = scrollback->count + pane->buffer->rows;
    uint32_t visible_lines = pane->buffer->rows;

    if (total_lines > visible_lines) {
        float thumb_ratio = (float)visible_lines / total_lines;
        uint32_t thumb_height = (uint32_t)(scrollbar_height * thumb_ratio);
        if (thumb_height < 20) {
            thumb_height = 20; // Minimum thumb size
        }

        float scroll_ratio = (float)scrollback->view_offset / (total_lines - visible_lines);
        uint32_t thumb_y = scrollbar_y + (uint32_t)((scrollbar_height - thumb_height) * scroll_ratio);

        // Draw thumb
        rect_t thumb_rect = {
            scrollbar_x + 2, thumb_y,
            SCROLLBAR_WIDTH - 4, thumb_height
        };
        gpu_draw_rounded_rect(term->gpu, &thumb_rect, 3.0f, color_to_u32(&theme->scrollbar_fg));
    }
}

/**
 * Render tab bar
 */
void renderer_render_tab_bar(terminal_t *term, const theme_t *theme) {
    if (!term || !theme) {
        return;
    }

    // Draw tab bar background
    rect_t tab_bar_bg = {
        0, 0,
        term->width, TAB_BAR_HEIGHT
    };
    gpu_draw_rect(term->gpu, &tab_bar_bg, color_to_u32(&theme->tab_bar_bg));

    // Draw tabs
    int32_t tab_x = 10;
    int32_t tab_width = 150;
    int32_t tab_height = TAB_BAR_HEIGHT - 4;

    for (uint32_t i = 0; i < term->tab_count; i++) {
        tab_t *tab = term->tabs[i];
        if (!tab) {
            continue;
        }

        bool is_active = (i == term->active_tab);
        bool is_hover = (i == term->hover_tab);

        // Draw tab background
        rect_t tab_rect = {
            tab_x, 2,
            tab_width, tab_height
        };

        color_t tab_color = is_active ? theme->tab_active_bg : theme->tab_bar_bg;
        if (is_hover && !is_active) {
            // Slightly lighter on hover
            tab_color.r = (tab_color.r + 20 > 255) ? 255 : tab_color.r + 20;
            tab_color.g = (tab_color.g + 20 > 255) ? 255 : tab_color.g + 20;
            tab_color.b = (tab_color.b + 20 > 255) ? 255 : tab_color.b + 20;
        }

        gpu_draw_rounded_rect(term->gpu, &tab_rect, 4.0f, color_to_u32(&tab_color));

        // Draw tab title
        color_t text_color = is_active ? theme->tab_active_fg : theme->tab_bar_fg;
        font_render_text(term->font_ctx, term->gpu, tab->title,
                        tab_x + 10, 2 + (tab_height - CELL_HEIGHT) / 2,
                        &text_color);

        // Draw close button (small X)
        int32_t close_x = tab_x + tab_width - 20;
        int32_t close_y = 2 + tab_height / 2;

        // Only show close button on hover or active tab
        if (is_active || is_hover) {
            font_render_text(term->font_ctx, term->gpu, "×",
                           close_x, close_y - CELL_HEIGHT / 2,
                           &text_color);
        }

        tab_x += tab_width + 5;
    }

    // Draw new tab button
    int32_t new_tab_x = term->width - 40;
    rect_t new_tab_rect = {
        new_tab_x, 2,
        30, tab_height
    };
    gpu_draw_rounded_rect(term->gpu, &new_tab_rect, 4.0f, color_to_u32(&theme->tab_bar_bg));

    color_t plus_color = theme->tab_bar_fg;
    font_render_text(term->font_ctx, term->gpu, "+",
                    new_tab_x + 10, 2 + (tab_height - CELL_HEIGHT) / 2,
                    &plus_color);

    // Draw separator line
    rect_t separator = {
        0, TAB_BAR_HEIGHT - 1,
        term->width, 1
    };
    gpu_draw_rect(term->gpu, &separator, color_to_u32(&theme->border_color));
}

/**
 * Render pane borders
 */
void renderer_render_pane_borders(terminal_t *term, const pane_t *pane, const theme_t *theme) {
    if (!term || !pane || !theme) {
        return;
    }

    if (pane->split_type == SPLIT_NONE) {
        return;
    }

    // Draw split divider
    if (pane->split_type == SPLIT_HORIZONTAL) {
        // Horizontal split - draw horizontal line
        int32_t divider_y = pane->first_child ?
            pane->first_child->y + pane->first_child->height : pane->y;

        rect_t divider = {
            pane->x, divider_y,
            pane->width, 2
        };
        gpu_draw_rect(term->gpu, &divider, color_to_u32(&theme->border_color));
    } else if (pane->split_type == SPLIT_VERTICAL) {
        // Vertical split - draw vertical line
        int32_t divider_x = pane->first_child ?
            pane->first_child->x + pane->first_child->width : pane->x;

        rect_t divider = {
            divider_x, pane->y,
            2, pane->height
        };
        gpu_draw_rect(term->gpu, &divider, color_to_u32(&theme->border_color));
    }

    // Recursively render child borders
    if (pane->first_child) {
        renderer_render_pane_borders(term, pane->first_child, theme);
    }
    if (pane->second_child) {
        renderer_render_pane_borders(term, pane->second_child, theme);
    }
}

/**
 * Render search highlights
 */
void renderer_render_search_highlight(terminal_t *term, const terminal_buffer_t *buffer,
                                     const pane_t *pane, const theme_t *theme) {
    if (!term || !buffer || !pane || !theme) {
        return;
    }

    if (!term->search_active || term->search_query[0] == '\0') {
        return;
    }

    uint32_t query_len = strlen(term->search_query);
    if (query_len == 0) {
        return;
    }

    // Enable blending
    gpu_set_blend_mode(term->gpu, true);

    // Search through visible buffer
    for (uint32_t y = 0; y < buffer->rows; y++) {
        for (uint32_t x = 0; x < buffer->cols; x++) {
            if (search_matches_at(buffer, x, y, term->search_query)) {
                // Highlight match
                int32_t screen_x = pane->x + x * CELL_WIDTH;
                int32_t screen_y = pane->y + y * CELL_HEIGHT;

                rect_t highlight = {
                    screen_x, screen_y,
                    query_len * CELL_WIDTH, CELL_HEIGHT
                };

                // Yellow highlight
                color_t highlight_color = color_from_rgba(255, 255, 0, 100);
                gpu_draw_rect(term->gpu, &highlight, color_to_u32(&highlight_color));
            }
        }
    }

    gpu_set_blend_mode(term->gpu, false);
}

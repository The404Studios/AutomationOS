/**
 * AutomationOS Settings - Rendering Implementation
 *
 * Renders the settings UI using basic drawing primitives.
 */

#include "settings.h"
#include <stdio.h>
#include <string.h>

// ============================================================================
// COLOR UTILITIES
// ============================================================================

static uint32_t color_to_rgba(color_t color) {
    return (color.a << 24) | (color.r << 16) | (color.g << 8) | color.b;
}

static color_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (color_t){r, g, b, a};
}

// ============================================================================
// DRAWING PRIMITIVES (TODO: Replace with actual compositor API)
// ============================================================================

static void draw_rect(uint32_t *framebuffer, uint32_t fb_width, uint32_t fb_height,
                     rect_t rect, color_t color) {
    // Placeholder: Would call actual compositor drawing API
    // For now, just log
    // printf("[Draw] Rect (%d,%d,%dx%d) color=0x%08x\n",
    //        rect.x, rect.y, rect.width, rect.height, color_to_rgba(color));
}

static void draw_text(uint32_t *framebuffer, uint32_t fb_width, uint32_t fb_height,
                     int32_t x, int32_t y, const char *text, color_t color, uint32_t size) {
    // Placeholder: Would call actual text rendering API
    // printf("[Draw] Text at (%d,%d): \"%s\" size=%u\n", x, y, text, size);
}

static void draw_circle(uint32_t *framebuffer, uint32_t fb_width, uint32_t fb_height,
                       int32_t cx, int32_t cy, uint32_t radius, color_t color) {
    // Placeholder
}

static void draw_rounded_rect(uint32_t *framebuffer, uint32_t fb_width, uint32_t fb_height,
                             rect_t rect, uint32_t radius, color_t color) {
    // Placeholder
}

// ============================================================================
// MAIN RENDERING
// ============================================================================

/**
 * Render the entire settings application
 */
void settings_app_render(settings_app_t *app) {
    if (!app || !app->window) return;

    // Get framebuffer (TODO: Get from actual window surface)
    uint32_t *framebuffer = NULL;  // Placeholder
    uint32_t fb_width = app->window->bounds.width;
    uint32_t fb_height = app->window->bounds.height;

    // Clear background
    color_t bg_color = app->theme->bg_primary;
    draw_rect(framebuffer, fb_width, fb_height,
             (rect_t){0, 0, fb_width, fb_height}, bg_color);

    // Render components
    settings_render_titlebar(app);
    settings_render_sidebar(app);
    settings_render_content(app);

    // TODO: Present framebuffer to compositor
}

/**
 * Render titlebar
 */
void settings_render_titlebar(settings_app_t *app) {
    if (!app) return;

    uint32_t *framebuffer = NULL;
    uint32_t fb_width = app->window->bounds.width;
    uint32_t fb_height = app->window->bounds.height;

    // Draw titlebar background
    color_t titlebar_bg = app->theme->panel_bg;
    draw_rect(framebuffer, fb_width, fb_height, app->titlebar_rect, titlebar_bg);

    // Draw title text
    color_t text_color = app->theme->text_primary;
    draw_text(framebuffer, fb_width, fb_height,
             app->titlebar_rect.x + 16,
             app->titlebar_rect.y + 12,
             "Settings", text_color, 16);

    // Draw window controls (close, minimize, maximize) - macOS style
    uint32_t control_y = app->titlebar_rect.y + 10;
    uint32_t control_spacing = 20;
    uint32_t control_radius = 6;

    // Close button (red)
    draw_circle(framebuffer, fb_width, fb_height,
               app->titlebar_rect.width - 16 - control_spacing * 2, control_y,
               control_radius, rgba(255, 59, 48, 255));

    // Minimize button (yellow)
    draw_circle(framebuffer, fb_width, fb_height,
               app->titlebar_rect.width - 16 - control_spacing, control_y,
               control_radius, rgba(255, 149, 0, 255));

    // Maximize button (green)
    draw_circle(framebuffer, fb_width, fb_height,
               app->titlebar_rect.width - 16, control_y,
               control_radius, rgba(52, 199, 89, 255));
}

/**
 * Render sidebar
 */
void settings_render_sidebar(settings_app_t *app) {
    if (!app) return;

    uint32_t *framebuffer = NULL;
    uint32_t fb_width = app->window->bounds.width;
    uint32_t fb_height = app->window->bounds.height;

    // Draw sidebar background
    color_t sidebar_bg = app->theme->bg_secondary;
    draw_rect(framebuffer, fb_width, fb_height, app->sidebar_rect, sidebar_bg);

    // Draw category items
    for (uint32_t i = 0; i < CATEGORY_COUNT; i++) {
        category_item_t *item = &app->categories[i];

        // Calculate absolute position
        rect_t abs_bounds = item->bounds;
        abs_bounds.y += app->sidebar_rect.y;

        // Draw selection background
        if (item->selected) {
            color_t selection_color = app->theme->primary;
            selection_color.a = 50;  // Semi-transparent
            draw_rounded_rect(framebuffer, fb_width, fb_height,
                            (rect_t){abs_bounds.x + 8, abs_bounds.y + 4,
                                   abs_bounds.width - 16, abs_bounds.height - 8},
                            8, selection_color);
        }

        // Draw icon (placeholder: just a colored square)
        color_t icon_color = item->selected ? app->theme->primary : app->theme->text_secondary;
        draw_rect(framebuffer, fb_width, fb_height,
                 (rect_t){abs_bounds.x + 16, abs_bounds.y + 16, 24, 24},
                 icon_color);

        // Draw label
        color_t text_color = item->selected ? app->theme->text_primary : app->theme->text_secondary;
        draw_text(framebuffer, fb_width, fb_height,
                 abs_bounds.x + 48, abs_bounds.y + 20,
                 item->label, text_color, 14);
    }

    // Draw separator line
    color_t separator_color = app->theme->separator;
    draw_rect(framebuffer, fb_width, fb_height,
             (rect_t){app->sidebar_rect.x + app->sidebar_rect.width - 1,
                     app->sidebar_rect.y, 1, app->sidebar_rect.height},
             separator_color);
}

/**
 * Render content area
 */
void settings_render_content(settings_app_t *app) {
    if (!app) return;

    uint32_t *framebuffer = NULL;
    uint32_t fb_width = app->window->bounds.width;
    uint32_t fb_height = app->window->bounds.height;

    // Draw content background
    color_t content_bg = app->theme->bg_primary;
    draw_rect(framebuffer, fb_width, fb_height, app->content_rect, content_bg);

    // Get current panel
    settings_panel_t *panel = &app->panels[app->current_category];

    // Draw panel title
    color_t title_color = app->theme->text_primary;
    draw_text(framebuffer, fb_width, fb_height,
             app->content_rect.x + CONTENT_PADDING,
             app->content_rect.y + 16,
             panel->title, title_color, 24);

    // Draw widgets
    widget_t *widget = panel->widgets;
    while (widget) {
        if (widget->visible) {
            settings_render_widget(app, widget);
        }
        widget = widget->next;
    }

    // Draw scrollbar if needed
    if (panel->content_height > app->content_rect.height) {
        uint32_t scrollbar_x = app->content_rect.x + app->content_rect.width - 8;
        uint32_t scrollbar_height = app->content_rect.height;
        uint32_t thumb_height = (scrollbar_height * scrollbar_height) / panel->content_height;
        uint32_t thumb_y = app->content_rect.y +
                          (panel->scroll_offset * scrollbar_height) / panel->content_height;

        // Draw scrollbar track
        color_t track_color = app->theme->separator;
        draw_rect(framebuffer, fb_width, fb_height,
                 (rect_t){scrollbar_x, app->content_rect.y, 4, scrollbar_height},
                 track_color);

        // Draw scrollbar thumb
        color_t thumb_color = app->theme->text_tertiary;
        draw_rounded_rect(framebuffer, fb_width, fb_height,
                        (rect_t){scrollbar_x, thumb_y, 4, thumb_height},
                        2, thumb_color);
    }
}

/**
 * Render a single widget
 */
void settings_render_widget(settings_app_t *app, widget_t *widget) {
    if (!app || !widget || !widget->visible) return;

    uint32_t *framebuffer = NULL;
    uint32_t fb_width = app->window->bounds.width;
    uint32_t fb_height = app->window->bounds.height;

    settings_panel_t *panel = &app->panels[app->current_category];

    // Calculate absolute position accounting for scroll
    rect_t abs_bounds = widget->bounds;
    abs_bounds.x += app->content_rect.x;
    abs_bounds.y += app->content_rect.y - panel->scroll_offset;

    // Clip to content area
    if (abs_bounds.y + abs_bounds.height < app->content_rect.y ||
        abs_bounds.y > app->content_rect.y + app->content_rect.height) {
        return;  // Widget is scrolled out of view
    }

    color_t text_color = app->theme->text_primary;
    color_t secondary_color = app->theme->text_secondary;

    switch (widget->type) {
        case WIDGET_SECTION_HEADER: {
            // Draw header text
            color_t header_color = app->theme->text_primary;
            draw_text(framebuffer, fb_width, fb_height,
                     abs_bounds.x, abs_bounds.y + 8,
                     widget->label, header_color, 16);

            // Draw underline
            color_t line_color = app->theme->separator;
            draw_rect(framebuffer, fb_width, fb_height,
                     (rect_t){abs_bounds.x, abs_bounds.y + 28, abs_bounds.width, 1},
                     line_color);
            break;
        }

        case WIDGET_LABEL: {
            draw_text(framebuffer, fb_width, fb_height,
                     abs_bounds.x, abs_bounds.y,
                     widget->label, secondary_color, 13);
            break;
        }

        case WIDGET_TOGGLE: {
            // Draw label
            draw_text(framebuffer, fb_width, fb_height,
                     abs_bounds.x, abs_bounds.y + 16,
                     widget->label, text_color, 14);

            // Draw toggle switch (macOS-style)
            uint32_t toggle_x = abs_bounds.x + abs_bounds.width - TOGGLE_WIDTH - 16;
            uint32_t toggle_y = abs_bounds.y + (abs_bounds.height - TOGGLE_HEIGHT) / 2;

            color_t toggle_bg = widget->data.toggle.value ?
                               app->theme->primary :
                               rgba(200, 200, 200, 255);

            draw_rounded_rect(framebuffer, fb_width, fb_height,
                            (rect_t){toggle_x, toggle_y, TOGGLE_WIDTH, TOGGLE_HEIGHT},
                            TOGGLE_HEIGHT / 2, toggle_bg);

            // Draw toggle knob
            uint32_t knob_x = widget->data.toggle.value ?
                             toggle_x + TOGGLE_WIDTH - TOGGLE_HEIGHT + 2 :
                             toggle_x + 2;
            uint32_t knob_y = toggle_y + 2;
            uint32_t knob_size = TOGGLE_HEIGHT - 4;

            draw_circle(framebuffer, fb_width, fb_height,
                       knob_x + knob_size / 2, knob_y + knob_size / 2,
                       knob_size / 2, rgba(255, 255, 255, 255));
            break;
        }

        case WIDGET_SLIDER: {
            // Draw label
            draw_text(framebuffer, fb_width, fb_height,
                     abs_bounds.x, abs_bounds.y,
                     widget->label, text_color, 14);

            // Draw slider track
            uint32_t track_y = abs_bounds.y + 28;
            uint32_t track_width = abs_bounds.width - 16;
            uint32_t track_height = 4;

            color_t track_color = rgba(200, 200, 200, 255);
            draw_rounded_rect(framebuffer, fb_width, fb_height,
                            (rect_t){abs_bounds.x, track_y, track_width, track_height},
                            2, track_color);

            // Draw filled portion
            uint32_t fill_width = track_width * widget->data.slider.value;
            draw_rounded_rect(framebuffer, fb_width, fb_height,
                            (rect_t){abs_bounds.x, track_y, fill_width, track_height},
                            2, app->theme->primary);

            // Draw slider thumb
            uint32_t thumb_x = abs_bounds.x + fill_width - 8;
            uint32_t thumb_y = track_y + track_height / 2;

            draw_circle(framebuffer, fb_width, fb_height,
                       thumb_x, thumb_y, 12,
                       rgba(255, 255, 255, 255));

            // Draw value text
            char value_text[32];
            snprintf(value_text, sizeof(value_text), "%.0f%%",
                    widget->data.slider.value * 100);
            draw_text(framebuffer, fb_width, fb_height,
                     abs_bounds.x + track_width + 16, abs_bounds.y,
                     value_text, secondary_color, 14);
            break;
        }

        case WIDGET_DROPDOWN: {
            // Draw label
            draw_text(framebuffer, fb_width, fb_height,
                     abs_bounds.x, abs_bounds.y,
                     widget->label, text_color, 14);

            // Draw dropdown button
            uint32_t dropdown_y = abs_bounds.y + 20;
            uint32_t dropdown_width = 300;
            uint32_t dropdown_height = 36;

            color_t dropdown_bg = widget->hovered ?
                                 rgba(240, 240, 240, 255) :
                                 rgba(255, 255, 255, 255);

            draw_rounded_rect(framebuffer, fb_width, fb_height,
                            (rect_t){abs_bounds.x, dropdown_y, dropdown_width, dropdown_height},
                            6, dropdown_bg);

            // Draw selected item text
            if (widget->data.dropdown.selected_index >= 0 &&
                widget->data.dropdown.selected_index < (int32_t)widget->data.dropdown.item_count) {
                draw_text(framebuffer, fb_width, fb_height,
                         abs_bounds.x + 12, dropdown_y + 10,
                         widget->data.dropdown.items[widget->data.dropdown.selected_index],
                         text_color, 14);
            }

            // Draw dropdown arrow
            // TODO: Draw actual arrow icon
            draw_text(framebuffer, fb_width, fb_height,
                     abs_bounds.x + dropdown_width - 24, dropdown_y + 10,
                     "▼", secondary_color, 12);

            // Draw dropdown menu if expanded
            if (widget->data.dropdown.expanded) {
                uint32_t menu_y = dropdown_y + dropdown_height + 4;
                uint32_t menu_height = widget->data.dropdown.item_count * 32;

                // Draw menu background
                draw_rounded_rect(framebuffer, fb_width, fb_height,
                                (rect_t){abs_bounds.x, menu_y, dropdown_width, menu_height},
                                6, rgba(255, 255, 255, 255));

                // Draw menu items
                for (uint32_t i = 0; i < widget->data.dropdown.item_count; i++) {
                    uint32_t item_y = menu_y + i * 32;
                    bool is_selected = (i == (uint32_t)widget->data.dropdown.selected_index);

                    if (is_selected) {
                        color_t sel_color = app->theme->primary;
                        sel_color.a = 50;
                        draw_rect(framebuffer, fb_width, fb_height,
                                 (rect_t){abs_bounds.x + 4, item_y + 2, dropdown_width - 8, 28},
                                 sel_color);
                    }

                    draw_text(framebuffer, fb_width, fb_height,
                             abs_bounds.x + 12, item_y + 8,
                             widget->data.dropdown.items[i],
                             is_selected ? app->theme->primary : text_color, 14);
                }
            }
            break;
        }

        case WIDGET_BUTTON: {
            // Draw button
            color_t button_bg = widget->hovered ?
                               rgba(0, 122, 255, 255) :
                               rgba(0, 122, 255, 200);

            draw_rounded_rect(framebuffer, fb_width, fb_height,
                            abs_bounds, 6, button_bg);

            // Draw button text
            draw_text(framebuffer, fb_width, fb_height,
                     abs_bounds.x + (abs_bounds.width / 2) - 40,  // Approximate centering
                     abs_bounds.y + 12,
                     widget->label, rgba(255, 255, 255, 255), 14);
            break;
        }

        case WIDGET_TEXT_INPUT: {
            // Draw label
            draw_text(framebuffer, fb_width, fb_height,
                     abs_bounds.x, abs_bounds.y,
                     widget->label, text_color, 14);

            // Draw input box
            uint32_t input_y = abs_bounds.y + 20;
            uint32_t input_height = 36;

            color_t input_bg = rgba(255, 255, 255, 255);
            draw_rounded_rect(framebuffer, fb_width, fb_height,
                            (rect_t){abs_bounds.x, input_y, abs_bounds.width, input_height},
                            6, input_bg);

            // Draw input text
            draw_text(framebuffer, fb_width, fb_height,
                     abs_bounds.x + 12, input_y + 10,
                     widget->data.text_input.value, text_color, 14);

            // Draw cursor if focused
            if (widget->data.text_input.focused) {
                // TODO: Draw blinking cursor
            }
            break;
        }

        default:
            break;
    }
}

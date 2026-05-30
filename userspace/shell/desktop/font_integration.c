/**
 * Desktop Shell Font Integration Implementation
 */

#include "font_integration.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Font paths
#define FONT_PATH "/fonts/DejaVuSans.ttf"

// Font sizes
#define PANEL_FONT_SIZE 13.0f
#define DOCK_FONT_SIZE 10.0f
#define MENU_FONT_SIZE 12.0f
#define NOTIFICATION_FONT_SIZE 11.0f

// Colors (macOS-inspired)
#define COLOR_PANEL_TEXT      0xFFFFFFFF  // White for dark panel
#define COLOR_DOCK_LABEL      0xFFFFFFFF  // White with shadow
#define COLOR_MENU_NORMAL     0xFF000000  // Black
#define COLOR_MENU_SELECTED   0xFFFFFFFF  // White on blue
#define COLOR_NOTIFICATION_TITLE 0xFF000000  // Black
#define COLOR_NOTIFICATION_BODY  0xFF666666  // Gray

/**
 * Initialize desktop font system
 */
desktop_font_pool_t* desktop_font_init(void) {
    // Allocate pool
    desktop_font_pool_t* pool = calloc(1, sizeof(desktop_font_pool_t));
    if (!pool) {
        fprintf(stderr, "[DESKTOP] Failed to allocate font pool\n");
        return NULL;
    }

    // Load panel font
    pool->panel_font = font_load(FONT_PATH);
    if (!pool->panel_font) {
        fprintf(stderr, "[DESKTOP] Failed to load panel font\n");
        desktop_font_shutdown(pool);
        return NULL;
    }
    font_set_size(pool->panel_font, PANEL_FONT_SIZE);
    font_set_quality(pool->panel_font, FONT_QUALITY_HIGH);

    // Load dock font
    pool->dock_font = font_load(FONT_PATH);
    if (!pool->dock_font) {
        fprintf(stderr, "[DESKTOP] Failed to load dock font\n");
        desktop_font_shutdown(pool);
        return NULL;
    }
    font_set_size(pool->dock_font, DOCK_FONT_SIZE);
    font_set_quality(pool->dock_font, FONT_QUALITY_HIGH);

    // Load menu font
    pool->menu_font = font_load(FONT_PATH);
    if (!pool->menu_font) {
        fprintf(stderr, "[DESKTOP] Failed to load menu font\n");
        desktop_font_shutdown(pool);
        return NULL;
    }
    font_set_size(pool->menu_font, MENU_FONT_SIZE);
    font_set_quality(pool->menu_font, FONT_QUALITY_HIGH);

    // Load notification font
    pool->notification_font = font_load(FONT_PATH);
    if (!pool->notification_font) {
        fprintf(stderr, "[DESKTOP] Failed to load notification font\n");
        desktop_font_shutdown(pool);
        return NULL;
    }
    font_set_size(pool->notification_font, NOTIFICATION_FONT_SIZE);
    font_set_quality(pool->notification_font, FONT_QUALITY_MEDIUM);

    fprintf(stderr, "[DESKTOP] Font system initialized\n");
    return pool;
}

/**
 * Shutdown desktop font system
 */
void desktop_font_shutdown(desktop_font_pool_t* pool) {
    if (!pool) return;

    if (pool->panel_font) font_free(pool->panel_font);
    if (pool->dock_font) font_free(pool->dock_font);
    if (pool->menu_font) font_free(pool->menu_font);
    if (pool->notification_font) font_free(pool->notification_font);

    free(pool);
    fprintf(stderr, "[DESKTOP] Font system shutdown\n");
}

/**
 * Render panel clock
 */
void desktop_render_clock(desktop_font_pool_t* pool,
                           uint32_t* fb, uint32_t fb_width,
                           uint32_t fb_height, uint32_t fb_pitch,
                           int32_t x, int32_t y,
                           const char* time_str) {
    if (!pool || !pool->panel_font || !fb || !time_str) return;

    // Get font metrics
    font_metrics_t metrics;
    font_get_metrics(pool->panel_font, &metrics);

    // Calculate vertical centering (panel is typically 28px)
    int32_t text_y = y + 14 + (metrics.ascent / 2);

    // Set rendering options (right-aligned)
    font_render_opts_t opts = {
        .color = COLOR_PANEL_TEXT,
        .align = FONT_ALIGN_RIGHT,
        .wrap_width = 0,
        .line_spacing = 0,
        .style = FONT_STYLE_NORMAL
    };

    // Render time
    font_render_text(pool->panel_font, fb, fb_width, fb_height, fb_pitch,
                     x, text_y, time_str, &opts);
}

/**
 * Render dock app label
 */
void desktop_render_dock_label(desktop_font_pool_t* pool,
                                 uint32_t* fb, uint32_t fb_width,
                                 uint32_t fb_height, uint32_t fb_pitch,
                                 int32_t x, int32_t y,
                                 const char* label) {
    if (!pool || !pool->dock_font || !fb || !label) return;

    // Get font metrics
    font_metrics_t metrics;
    font_get_metrics(pool->dock_font, &metrics);

    // Measure text for centering
    font_measure_text(pool->dock_font, label, &metrics);

    // Center text horizontally
    int32_t text_x = x - (metrics.width / 2);
    int32_t text_y = y + metrics.ascent;

    // Render shadow first (1px offset, darker)
    font_render_opts_t shadow_opts = {
        .color = 0x80000000,  // Semi-transparent black
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 0,
        .style = FONT_STYLE_NORMAL
    };
    font_render_text(pool->dock_font, fb, fb_width, fb_height, fb_pitch,
                     text_x + 1, text_y + 1, label, &shadow_opts);

    // Render main text
    font_render_opts_t opts = {
        .color = COLOR_DOCK_LABEL,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 0,
        .style = FONT_STYLE_NORMAL
    };
    font_render_text(pool->dock_font, fb, fb_width, fb_height, fb_pitch,
                     text_x, text_y, label, &opts);
}

/**
 * Render menu item
 */
void desktop_render_menu_item(desktop_font_pool_t* pool,
                                uint32_t* fb, uint32_t fb_width,
                                uint32_t fb_height, uint32_t fb_pitch,
                                int32_t x, int32_t y,
                                uint32_t item_height,
                                const char* text, bool selected) {
    if (!pool || !pool->menu_font || !fb || !text) return;

    // Get font metrics
    font_metrics_t metrics;
    font_get_metrics(pool->menu_font, &metrics);

    // Center text vertically in item
    int32_t text_y = y + (item_height / 2) + (metrics.ascent / 2);

    // Set rendering options
    font_render_opts_t opts = {
        .color = selected ? COLOR_MENU_SELECTED : COLOR_MENU_NORMAL,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 0,
        .style = FONT_STYLE_NORMAL
    };

    // Render text
    font_render_text(pool->menu_font, fb, fb_width, fb_height, fb_pitch,
                     x + 16, text_y, text, &opts);
}

/**
 * Render notification
 */
void desktop_render_notification(desktop_font_pool_t* pool,
                                   uint32_t* fb, uint32_t fb_width,
                                   uint32_t fb_height, uint32_t fb_pitch,
                                   int32_t x, int32_t y,
                                   uint32_t width,
                                   const char* title,
                                   const char* body) {
    if (!pool || !pool->notification_font || !fb) return;

    // Get font metrics
    font_metrics_t metrics;
    font_get_metrics(pool->notification_font, &metrics);

    int32_t current_y = y + metrics.ascent;

    // Render title (bold)
    if (title) {
        font_render_opts_t title_opts = {
            .color = COLOR_NOTIFICATION_TITLE,
            .align = FONT_ALIGN_LEFT,
            .wrap_width = width - 32,
            .line_spacing = 0,
            .style = FONT_STYLE_BOLD
        };
        font_render_text(pool->notification_font, fb, fb_width, fb_height, fb_pitch,
                         x + 16, current_y, title, &title_opts);
        current_y += metrics.height + 4;
    }

    // Render body
    if (body) {
        font_render_opts_t body_opts = {
            .color = COLOR_NOTIFICATION_BODY,
            .align = FONT_ALIGN_LEFT,
            .wrap_width = width - 32,
            .line_spacing = 2,
            .style = FONT_STYLE_NORMAL
        };
        font_render_text(pool->notification_font, fb, fb_width, fb_height, fb_pitch,
                         x + 16, current_y, body, &body_opts);
    }
}

/**
 * Measure text
 */
bool desktop_measure_text(desktop_font_pool_t* pool,
                            const char* text, int font_type,
                            font_metrics_t* metrics) {
    if (!pool || !text || !metrics) return false;

    font_t* font = NULL;
    switch (font_type) {
        case 0: font = pool->panel_font; break;
        case 1: font = pool->dock_font; break;
        case 2: font = pool->menu_font; break;
        case 3: font = pool->notification_font; break;
        default: return false;
    }

    if (!font) return false;
    return font_measure_text(font, text, metrics);
}

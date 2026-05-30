/**
 * Compositor Font Integration Implementation
 */

#include "font_integration.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Default font paths
#define DEFAULT_FONT_PATH "/fonts/DejaVuSans.ttf"
#define DEFAULT_MONO_PATH "/fonts/DejaVuSansMono.ttf"

// Font sizes (in points)
#define TITLE_FONT_SIZE 12.0f
#define MENU_FONT_SIZE 12.0f
#define UI_FONT_SIZE 11.0f
#define MONO_FONT_SIZE 10.0f

// Colors
#define COLOR_TITLE_FOCUSED   0xFF000000  // Black
#define COLOR_TITLE_UNFOCUSED 0xFF808080  // Gray
#define COLOR_MENU_NORMAL     0xFF000000  // Black
#define COLOR_MENU_SELECTED   0xFFFFFFFF  // White

/**
 * Initialize compositor font system
 */
compositor_font_pool_t* compositor_font_init(size_t cache_size) {
    // Initialize font subsystem
    if (!font_init(cache_size)) {
        fprintf(stderr, "[COMPOSITOR] Failed to initialize font system\n");
        return NULL;
    }

    // Allocate font pool
    compositor_font_pool_t* pool = calloc(1, sizeof(compositor_font_pool_t));
    if (!pool) {
        fprintf(stderr, "[COMPOSITOR] Failed to allocate font pool\n");
        font_shutdown();
        return NULL;
    }

    // Load title font
    pool->title_font = font_load(DEFAULT_FONT_PATH);
    if (!pool->title_font) {
        fprintf(stderr, "[COMPOSITOR] Failed to load title font from %s\n", DEFAULT_FONT_PATH);
        compositor_font_shutdown(pool);
        return NULL;
    }
    font_set_size(pool->title_font, TITLE_FONT_SIZE);
    font_set_quality(pool->title_font, FONT_QUALITY_HIGH);

    // Load menu font (same as title)
    pool->menu_font = font_load(DEFAULT_FONT_PATH);
    if (!pool->menu_font) {
        fprintf(stderr, "[COMPOSITOR] Failed to load menu font\n");
        compositor_font_shutdown(pool);
        return NULL;
    }
    font_set_size(pool->menu_font, MENU_FONT_SIZE);
    font_set_quality(pool->menu_font, FONT_QUALITY_HIGH);

    // Load UI font
    pool->ui_font = font_load(DEFAULT_FONT_PATH);
    if (!pool->ui_font) {
        fprintf(stderr, "[COMPOSITOR] Failed to load UI font\n");
        compositor_font_shutdown(pool);
        return NULL;
    }
    font_set_size(pool->ui_font, UI_FONT_SIZE);
    font_set_quality(pool->ui_font, FONT_QUALITY_MEDIUM);

    // Load monospace font
    pool->mono_font = font_load(DEFAULT_MONO_PATH);
    if (!pool->mono_font) {
        fprintf(stderr, "[COMPOSITOR] Warning: Failed to load mono font, using default\n");
        // Fall back to regular font
        pool->mono_font = font_load(DEFAULT_FONT_PATH);
    }
    if (pool->mono_font) {
        font_set_size(pool->mono_font, MONO_FONT_SIZE);
        font_set_quality(pool->mono_font, FONT_QUALITY_MEDIUM);
    }

    fprintf(stderr, "[COMPOSITOR] Font system initialized\n");
    return pool;
}

/**
 * Shutdown font system
 */
void compositor_font_shutdown(compositor_font_pool_t* pool) {
    if (!pool) return;

    if (pool->title_font) font_free(pool->title_font);
    if (pool->menu_font) font_free(pool->menu_font);
    if (pool->ui_font) font_free(pool->ui_font);
    if (pool->mono_font) font_free(pool->mono_font);

    free(pool);
    font_shutdown();

    fprintf(stderr, "[COMPOSITOR] Font system shutdown\n");
}

/**
 * Render window title
 */
void compositor_render_title(compositor_font_pool_t* pool,
                              uint32_t* fb, uint32_t fb_width,
                              uint32_t fb_height, uint32_t fb_pitch,
                              int32_t x, int32_t y,
                              uint32_t width, uint32_t height,
                              const char* title, bool focused) {
    if (!pool || !pool->title_font || !fb || !title) return;

    // Get font metrics
    font_metrics_t metrics;
    font_get_metrics(pool->title_font, &metrics);

    // Calculate vertical centering
    int32_t text_y = y + (height / 2) + (metrics.ascent / 2);

    // Set rendering options
    font_render_opts_t opts = {
        .color = focused ? COLOR_TITLE_FOCUSED : COLOR_TITLE_UNFOCUSED,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = width - 16,  // Leave margin for buttons
        .line_spacing = 0,
        .style = FONT_STYLE_NORMAL
    };

    // Render title text
    font_render_text(pool->title_font, fb, fb_width, fb_height, fb_pitch,
                     x + 8, text_y, title, &opts);
}

/**
 * Render menu item
 */
void compositor_render_menu_item(compositor_font_pool_t* pool,
                                  uint32_t* fb, uint32_t fb_width,
                                  uint32_t fb_height, uint32_t fb_pitch,
                                  int32_t x, int32_t y,
                                  const char* text, bool selected) {
    if (!pool || !pool->menu_font || !fb || !text) return;

    // Get font metrics
    font_metrics_t metrics;
    font_get_metrics(pool->menu_font, &metrics);

    // Calculate vertical position
    int32_t text_y = y + metrics.ascent + 4;

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
                     x + 12, text_y, text, &opts);
}

/**
 * Measure text
 */
bool compositor_measure_text(compositor_font_pool_t* pool,
                              const char* text, int font_type,
                              font_metrics_t* metrics) {
    if (!pool || !text || !metrics) return false;

    font_t* font = NULL;
    switch (font_type) {
        case 0: font = pool->title_font; break;
        case 1: font = pool->menu_font; break;
        case 2: font = pool->ui_font; break;
        case 3: font = pool->mono_font; break;
        default: return false;
    }

    if (!font) return false;
    return font_measure_text(font, text, metrics);
}

/**
 * Warm font cache
 */
void compositor_warm_cache(compositor_font_pool_t* pool) {
    if (!pool) return;

    const char* common = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .,!?-_/\\:;";

    // Warm each font
    if (pool->title_font) {
        for (const char* p = common; *p; p++) {
            font_get_glyph(pool->title_font, *p);
        }
    }

    if (pool->menu_font) {
        for (const char* p = common; *p; p++) {
            font_get_glyph(pool->menu_font, *p);
        }
    }

    if (pool->ui_font) {
        for (const char* p = common; *p; p++) {
            font_get_glyph(pool->ui_font, *p);
        }
    }

    if (pool->mono_font) {
        for (const char* p = common; *p; p++) {
            font_get_glyph(pool->mono_font, *p);
        }
    }

    fprintf(stderr, "[COMPOSITOR] Font cache warmed\n");
}

/**
 * Get cache statistics
 */
void compositor_font_stats(size_t* hits, size_t* misses, size_t* size) {
    font_cache_stats(hits, misses, size);
}

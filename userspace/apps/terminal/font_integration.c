/**
 * Terminal Font Integration Implementation
 */

#include "font_integration.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Font paths
#define MONO_FONT_PATH "/fonts/DejaVuSansMono.ttf"
#define FALLBACK_FONT_PATH "/fonts/DejaVuSans.ttf"

// Default font size
#define DEFAULT_FONT_SIZE 12.0f

// Tab bar height
#define TAB_BAR_HEIGHT 32

/**
 * Calculate cell dimensions from font metrics
 */
static void calculate_cell_dimensions(terminal_font_ctx_t* ctx) {
    if (!ctx || !ctx->mono_font) return;

    font_metrics_t metrics;
    font_get_metrics(ctx->mono_font, &metrics);

    // Get 'M' width (widest character in most fonts)
    const font_glyph_t* glyph = font_get_glyph(ctx->mono_font, 'M');
    if (glyph) {
        ctx->cell_width = glyph->advance;
    } else {
        ctx->cell_width = 9;  // Fallback
    }

    ctx->cell_height = metrics.height;
    ctx->ascent = metrics.ascent;
    ctx->descent = metrics.descent;

    fprintf(stderr, "[TERMINAL] Cell size: %dx%d (ascent=%d, descent=%d)\n",
            ctx->cell_width, ctx->cell_height, ctx->ascent, ctx->descent);
}

/**
 * Initialize terminal font system
 */
terminal_font_ctx_t* terminal_font_init(float font_size) {
    // Allocate context
    terminal_font_ctx_t* ctx = calloc(1, sizeof(terminal_font_ctx_t));
    if (!ctx) {
        fprintf(stderr, "[TERMINAL] Failed to allocate font context\n");
        return NULL;
    }

    // Load monospace font
    ctx->mono_font = font_load(MONO_FONT_PATH);
    if (!ctx->mono_font) {
        fprintf(stderr, "[TERMINAL] Failed to load mono font, trying fallback\n");
        ctx->mono_font = font_load(FALLBACK_FONT_PATH);
    }

    if (!ctx->mono_font) {
        fprintf(stderr, "[TERMINAL] Failed to load any font\n");
        terminal_font_shutdown(ctx);
        return NULL;
    }

    // Set font size and quality
    font_set_size(ctx->mono_font, font_size > 0 ? font_size : DEFAULT_FONT_SIZE);
    font_set_quality(ctx->mono_font, FONT_QUALITY_HIGH);

    // Calculate cell dimensions
    calculate_cell_dimensions(ctx);

    fprintf(stderr, "[TERMINAL] Font system initialized (size=%.1f)\n", font_size);
    return ctx;
}

/**
 * Shutdown terminal font system
 */
void terminal_font_shutdown(terminal_font_ctx_t* ctx) {
    if (!ctx) return;

    if (ctx->mono_font) font_free(ctx->mono_font);
    free(ctx);

    fprintf(stderr, "[TERMINAL] Font system shutdown\n");
}

/**
 * Fill rectangle (helper)
 */
static void fill_rect(uint32_t* fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                       int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb || x < 0 || y < 0) return;

    for (uint32_t row = 0; row < h; row++) {
        if ((uint32_t)(y + row) >= fb_height) break;

        uint32_t* line = (uint32_t*)((uint8_t*)fb + (y + row) * fb_pitch);
        for (uint32_t col = 0; col < w; col++) {
            if ((uint32_t)(x + col) >= fb_width) break;
            line[x + col] = color;
        }
    }
}

/**
 * Render terminal cell
 */
void terminal_render_cell(terminal_font_ctx_t* ctx,
                           uint32_t* fb, uint32_t fb_width,
                           uint32_t fb_height, uint32_t fb_pitch,
                           uint32_t col, uint32_t row,
                           uint32_t codepoint,
                           uint32_t fg_color, uint32_t bg_color,
                           bool bold, bool italic) {
    if (!ctx || !ctx->mono_font || !fb) return;

    // Calculate pixel position
    int32_t x = col * ctx->cell_width;
    int32_t y = row * ctx->cell_height + TAB_BAR_HEIGHT;

    // Draw background
    fill_rect(fb, fb_width, fb_height, fb_pitch,
              x, y, ctx->cell_width, ctx->cell_height, bg_color);

    // Draw character
    if (codepoint > 0 && codepoint != ' ') {
        int32_t text_y = y + ctx->ascent;

        // TODO: Handle bold/italic with font_render_opts_t style flags
        // For now, render normal
        font_render_glyph(ctx->mono_font, fb, fb_width, fb_height, fb_pitch,
                          x, text_y, codepoint, fg_color);
    }
}

/**
 * Render terminal line (optimized)
 */
void terminal_render_line(terminal_font_ctx_t* ctx,
                           uint32_t* fb, uint32_t fb_width,
                           uint32_t fb_height, uint32_t fb_pitch,
                           uint32_t row,
                           const char* text,
                           uint32_t fg_color, uint32_t bg_color) {
    if (!ctx || !ctx->mono_font || !fb || !text) return;

    // Calculate row position
    int32_t y = row * ctx->cell_height + TAB_BAR_HEIGHT;

    // Fill background for entire line
    fill_rect(fb, fb_width, fb_height, fb_pitch,
              0, y, fb_width, ctx->cell_height, bg_color);

    // Render text
    font_render_opts_t opts = {
        .color = fg_color,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 0,
        .style = FONT_STYLE_NORMAL
    };

    int32_t text_y = y + ctx->ascent;
    font_render_text(ctx->mono_font, fb, fb_width, fb_height, fb_pitch,
                     0, text_y, text, &opts);
}

/**
 * Render cursor
 */
void terminal_render_cursor(terminal_font_ctx_t* ctx,
                              uint32_t* fb, uint32_t fb_width,
                              uint32_t fb_height, uint32_t fb_pitch,
                              uint32_t col, uint32_t row,
                              uint32_t color, uint32_t style) {
    if (!ctx || !fb) return;

    int32_t x = col * ctx->cell_width;
    int32_t y = row * ctx->cell_height + TAB_BAR_HEIGHT;

    switch (style) {
        case 0: // Block cursor
            fill_rect(fb, fb_width, fb_height, fb_pitch,
                      x, y, ctx->cell_width, ctx->cell_height, color);
            break;

        case 1: // Underline cursor
            fill_rect(fb, fb_width, fb_height, fb_pitch,
                      x, y + ctx->cell_height - 2, ctx->cell_width, 2, color);
            break;

        case 2: // Bar cursor
            fill_rect(fb, fb_width, fb_height, fb_pitch,
                      x, y, 2, ctx->cell_height, color);
            break;
    }
}

/**
 * Calculate terminal dimensions
 */
void terminal_calculate_dimensions(terminal_font_ctx_t* ctx,
                                     uint32_t window_width,
                                     uint32_t window_height,
                                     uint32_t* cols, uint32_t* rows) {
    if (!ctx || !cols || !rows) return;

    // Account for tab bar
    uint32_t usable_height = window_height - TAB_BAR_HEIGHT;

    *cols = window_width / ctx->cell_width;
    *rows = usable_height / ctx->cell_height;

    // Minimum dimensions
    if (*cols < 20) *cols = 20;
    if (*rows < 6) *rows = 6;
}

/**
 * Set font size
 */
bool terminal_set_font_size(terminal_font_ctx_t* ctx, float font_size) {
    if (!ctx || !ctx->mono_font || font_size <= 0) return false;

    font_set_size(ctx->mono_font, font_size);
    calculate_cell_dimensions(ctx);

    return true;
}

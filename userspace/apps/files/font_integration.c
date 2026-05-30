/**
 * File Explorer Font Integration Implementation
 */

#include "font_integration.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Font paths
#define FONT_PATH "/fonts/DejaVuSans.ttf"

// Font sizes
#define FILE_FONT_SIZE 12.0f
#define PATH_FONT_SIZE 11.0f
#define STATUS_FONT_SIZE 10.0f
#define HEADING_FONT_SIZE 14.0f

// Colors
#define COLOR_FILE_NORMAL       0xFF000000  // Black
#define COLOR_FILE_SELECTED     0xFFFFFFFF  // White on blue
#define COLOR_METADATA_NORMAL   0xFF666666  // Gray
#define COLOR_METADATA_SELECTED 0xFFCCCCCC  // Light gray
#define COLOR_BREADCRUMB        0xFF333333  // Dark gray
#define COLOR_BREADCRUMB_HOVER  0xFF0078D7  // Blue
#define COLOR_STATUS            0xFF666666  // Gray
#define COLOR_HEADING           0xFF000000  // Black

/**
 * Initialize file explorer font system
 */
explorer_font_ctx_t* explorer_font_init(void) {
    // Allocate context
    explorer_font_ctx_t* ctx = calloc(1, sizeof(explorer_font_ctx_t));
    if (!ctx) {
        fprintf(stderr, "[EXPLORER] Failed to allocate font context\n");
        return NULL;
    }

    // Load file font
    ctx->file_font = font_load(FONT_PATH);
    if (!ctx->file_font) {
        fprintf(stderr, "[EXPLORER] Failed to load file font\n");
        explorer_font_shutdown(ctx);
        return NULL;
    }
    font_set_size(ctx->file_font, FILE_FONT_SIZE);
    font_set_quality(ctx->file_font, FONT_QUALITY_HIGH);

    // Load path font
    ctx->path_font = font_load(FONT_PATH);
    if (!ctx->path_font) {
        fprintf(stderr, "[EXPLORER] Failed to load path font\n");
        explorer_font_shutdown(ctx);
        return NULL;
    }
    font_set_size(ctx->path_font, PATH_FONT_SIZE);
    font_set_quality(ctx->path_font, FONT_QUALITY_MEDIUM);

    // Load status font
    ctx->status_font = font_load(FONT_PATH);
    if (!ctx->status_font) {
        fprintf(stderr, "[EXPLORER] Failed to load status font\n");
        explorer_font_shutdown(ctx);
        return NULL;
    }
    font_set_size(ctx->status_font, STATUS_FONT_SIZE);
    font_set_quality(ctx->status_font, FONT_QUALITY_MEDIUM);

    // Load heading font
    ctx->heading_font = font_load(FONT_PATH);
    if (!ctx->heading_font) {
        fprintf(stderr, "[EXPLORER] Failed to load heading font\n");
        explorer_font_shutdown(ctx);
        return NULL;
    }
    font_set_size(ctx->heading_font, HEADING_FONT_SIZE);
    font_set_quality(ctx->heading_font, FONT_QUALITY_HIGH);

    fprintf(stderr, "[EXPLORER] Font system initialized\n");
    return ctx;
}

/**
 * Shutdown file explorer font system
 */
void explorer_font_shutdown(explorer_font_ctx_t* ctx) {
    if (!ctx) return;

    if (ctx->file_font) font_free(ctx->file_font);
    if (ctx->path_font) font_free(ctx->path_font);
    if (ctx->status_font) font_free(ctx->status_font);
    if (ctx->heading_font) font_free(ctx->heading_font);

    free(ctx);
    fprintf(stderr, "[EXPLORER] Font system shutdown\n");
}

/**
 * Render file name
 */
void explorer_render_filename(explorer_font_ctx_t* ctx,
                                uint32_t* fb, uint32_t fb_width,
                                uint32_t fb_height, uint32_t fb_pitch,
                                int32_t x, int32_t y,
                                uint32_t row_height,
                                const char* filename,
                                bool selected) {
    if (!ctx || !ctx->file_font || !fb || !filename) return;

    // Get font metrics
    font_metrics_t metrics;
    font_get_metrics(ctx->file_font, &metrics);

    // Center text vertically in row
    int32_t text_y = y + (row_height / 2) + (metrics.ascent / 2);

    // Set rendering options
    font_render_opts_t opts = {
        .color = selected ? COLOR_FILE_SELECTED : COLOR_FILE_NORMAL,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 0,
        .style = FONT_STYLE_NORMAL
    };

    // Render filename
    font_render_text(ctx->file_font, fb, fb_width, fb_height, fb_pitch,
                     x, text_y, filename, &opts);
}

/**
 * Render file metadata
 */
void explorer_render_metadata(explorer_font_ctx_t* ctx,
                                uint32_t* fb, uint32_t fb_width,
                                uint32_t fb_height, uint32_t fb_pitch,
                                int32_t x, int32_t y,
                                uint32_t row_height,
                                const char* metadata,
                                bool selected) {
    if (!ctx || !ctx->file_font || !fb || !metadata) return;

    // Get font metrics
    font_metrics_t metrics;
    font_get_metrics(ctx->file_font, &metrics);

    // Center text vertically
    int32_t text_y = y + (row_height / 2) + (metrics.ascent / 2);

    // Set rendering options (smaller, grayed out)
    font_render_opts_t opts = {
        .color = selected ? COLOR_METADATA_SELECTED : COLOR_METADATA_NORMAL,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 0,
        .style = FONT_STYLE_NORMAL
    };

    // Render metadata
    font_render_text(ctx->file_font, fb, fb_width, fb_height, fb_pitch,
                     x, text_y, metadata, &opts);
}

/**
 * Render breadcrumb
 */
uint32_t explorer_render_breadcrumb(explorer_font_ctx_t* ctx,
                                      uint32_t* fb, uint32_t fb_width,
                                      uint32_t fb_height, uint32_t fb_pitch,
                                      int32_t x, int32_t y,
                                      const char* segment,
                                      bool hover) {
    if (!ctx || !ctx->path_font || !fb || !segment) return 0;

    // Get font metrics
    font_metrics_t metrics;
    font_measure_text(ctx->path_font, segment, &metrics);

    // Set rendering options
    font_render_opts_t opts = {
        .color = hover ? COLOR_BREADCRUMB_HOVER : COLOR_BREADCRUMB,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 0,
        .style = FONT_STYLE_NORMAL
    };

    font_get_metrics(ctx->path_font, &metrics);
    int32_t text_y = y + metrics.ascent;

    // Render segment
    font_render_text(ctx->path_font, fb, fb_width, fb_height, fb_pitch,
                     x, text_y, segment, &opts);

    // Render separator (">")
    const char* separator = " > ";
    font_metrics_t sep_metrics;
    font_measure_text(ctx->path_font, separator, &sep_metrics);

    int32_t sep_x = x + metrics.width + 4;
    font_render_text(ctx->path_font, fb, fb_width, fb_height, fb_pitch,
                     sep_x, text_y, separator, &opts);

    // Return total width
    return metrics.width + sep_metrics.width + 8;
}

/**
 * Render status bar
 */
void explorer_render_status(explorer_font_ctx_t* ctx,
                              uint32_t* fb, uint32_t fb_width,
                              uint32_t fb_height, uint32_t fb_pitch,
                              int32_t x, int32_t y,
                              uint32_t status_height,
                              const char* text) {
    if (!ctx || !ctx->status_font || !fb || !text) return;

    // Get font metrics
    font_metrics_t metrics;
    font_get_metrics(ctx->status_font, &metrics);

    // Center text vertically in status bar
    int32_t text_y = y + (status_height / 2) + (metrics.ascent / 2);

    // Set rendering options
    font_render_opts_t opts = {
        .color = COLOR_STATUS,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 0,
        .style = FONT_STYLE_NORMAL
    };

    // Render status text
    font_render_text(ctx->status_font, fb, fb_width, fb_height, fb_pitch,
                     x + 8, text_y, text, &opts);
}

/**
 * Render column header
 */
void explorer_render_column_header(explorer_font_ctx_t* ctx,
                                     uint32_t* fb, uint32_t fb_width,
                                     uint32_t fb_height, uint32_t fb_pitch,
                                     int32_t x, int32_t y,
                                     uint32_t width, uint32_t header_height,
                                     const char* title,
                                     uint32_t sort_ascending) {
    if (!ctx || !ctx->heading_font || !fb || !title) return;

    // Get font metrics
    font_metrics_t metrics;
    font_get_metrics(ctx->heading_font, &metrics);

    // Center text vertically
    int32_t text_y = y + (header_height / 2) + (metrics.ascent / 2);

    // Set rendering options
    font_render_opts_t opts = {
        .color = COLOR_HEADING,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = width - 32,
        .line_spacing = 0,
        .style = FONT_STYLE_BOLD
    };

    // Render title
    font_render_text(ctx->heading_font, fb, fb_width, fb_height, fb_pitch,
                     x + 8, text_y, title, &opts);

    // Render sort indicator
    if (sort_ascending > 0) {
        font_metrics_t title_metrics;
        font_measure_text(ctx->heading_font, title, &title_metrics);

        const char* arrow = (sort_ascending == 1) ? " ▲" : " ▼";
        int32_t arrow_x = x + 8 + title_metrics.width + 4;

        font_render_text(ctx->heading_font, fb, fb_width, fb_height, fb_pitch,
                         arrow_x, text_y, arrow, &opts);
    }
}

/**
 * Measure text
 */
bool explorer_measure_text(explorer_font_ctx_t* ctx,
                             const char* text, int font_type,
                             font_metrics_t* metrics) {
    if (!ctx || !text || !metrics) return false;

    font_t* font = NULL;
    switch (font_type) {
        case 0: font = ctx->file_font; break;
        case 1: font = ctx->path_font; break;
        case 2: font = ctx->status_font; break;
        case 3: font = ctx->heading_font; break;
        default: return false;
    }

    if (!font) return false;
    return font_measure_text(font, text, metrics);
}

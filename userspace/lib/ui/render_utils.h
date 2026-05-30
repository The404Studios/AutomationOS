/**
 * UI Rendering Utilities
 *
 * High-quality rendering functions for polished UI elements with
 * anti-aliasing, shadows, blur, and effects.
 */

#ifndef RENDER_UTILS_H
#define RENDER_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include "../../shell/theme/theme_colors.h"
#include "../../shell/theme/design_system.h"

// Forward declarations
typedef struct gpu_context gpu_context_t;
typedef struct texture texture_t;

// ============================================================================
// GEOMETRY STRUCTURES
// ============================================================================

typedef struct {
    int32_t x, y;
} point_t;

typedef struct {
    int32_t x, y;
    uint32_t width, height;
} rect_t;

typedef struct {
    float x, y;
    uint32_t width, height;
} rect_f_t;

// ============================================================================
// BASIC SHAPES
// ============================================================================

/**
 * Draw filled rectangle
 */
void draw_rect(gpu_context_t *gpu, const rect_t *rect, color_rgba_t color);

/**
 * Draw rectangle outline
 */
void draw_rect_outline(gpu_context_t *gpu, const rect_t *rect, uint32_t width, color_rgba_t color);

/**
 * Draw filled circle
 */
void draw_circle(gpu_context_t *gpu, int32_t center_x, int32_t center_y,
                 float radius, color_rgba_t color);

/**
 * Draw circle outline
 */
void draw_circle_outline(gpu_context_t *gpu, int32_t center_x, int32_t center_y,
                        float radius, uint32_t width, color_rgba_t color);

/**
 * Draw line with anti-aliasing
 */
void draw_line(gpu_context_t *gpu, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
               uint32_t width, color_rgba_t color);

// ============================================================================
// ROUNDED SHAPES
// ============================================================================

/**
 * Draw filled rounded rectangle
 */
void draw_rounded_rect(gpu_context_t *gpu, const rect_t *rect, uint32_t radius,
                       color_rgba_t color);

/**
 * Draw rounded rectangle outline
 */
void draw_rounded_rect_outline(gpu_context_t *gpu, const rect_t *rect, uint32_t radius,
                               uint32_t width, color_rgba_t color);

/**
 * Draw rounded rectangle with individual corner radii
 */
void draw_rounded_rect_corners(gpu_context_t *gpu, const rect_t *rect,
                               uint32_t top_left, uint32_t top_right,
                               uint32_t bottom_right, uint32_t bottom_left,
                               color_rgba_t color);

/**
 * Draw pill shape (fully rounded ends)
 */
void draw_pill(gpu_context_t *gpu, const rect_t *rect, color_rgba_t color);

// ============================================================================
// EFFECTS
// ============================================================================

/**
 * Apply Gaussian blur to region
 */
void apply_blur_effect(gpu_context_t *gpu, const rect_t *region, uint32_t radius);

/**
 * Draw drop shadow
 */
void draw_shadow(gpu_context_t *gpu, const rect_t *rect, shadow_t shadow,
                 color_rgba_t color);

/**
 * Draw shadow for rounded rectangle
 */
void draw_shadow_rounded(gpu_context_t *gpu, const rect_t *rect, uint32_t corner_radius,
                        shadow_t shadow, color_rgba_t color);

/**
 * Draw inner shadow (inset)
 */
void draw_inner_shadow(gpu_context_t *gpu, const rect_t *rect, uint32_t corner_radius,
                       shadow_t shadow, color_rgba_t color);

/**
 * Draw glow effect
 */
void draw_glow(gpu_context_t *gpu, const rect_t *rect, uint32_t corner_radius,
               uint32_t spread, color_rgba_t color);

/**
 * Apply dimming overlay
 */
void draw_dim_overlay(gpu_context_t *gpu, const rect_t *rect, float factor);

// ============================================================================
// GRADIENTS
// ============================================================================

typedef struct {
    color_rgba_t start;
    color_rgba_t end;
} gradient_t;

/**
 * Draw linear gradient (top to bottom)
 */
void draw_gradient_vertical(gpu_context_t *gpu, const rect_t *rect, gradient_t gradient);

/**
 * Draw linear gradient (left to right)
 */
void draw_gradient_horizontal(gpu_context_t *gpu, const rect_t *rect, gradient_t gradient);

/**
 * Draw radial gradient
 */
void draw_gradient_radial(gpu_context_t *gpu, int32_t center_x, int32_t center_y,
                         float radius, gradient_t gradient);

// ============================================================================
// TEXT RENDERING
// ============================================================================

typedef struct {
    const char *font_family;
    uint32_t font_size;
    uint32_t font_weight;
    bool italic;
    bool underline;
} text_style_t;

typedef enum {
    TEXT_ALIGN_LEFT,
    TEXT_ALIGN_CENTER,
    TEXT_ALIGN_RIGHT,
} text_align_t;

typedef enum {
    TEXT_VALIGN_TOP,
    TEXT_VALIGN_MIDDLE,
    TEXT_VALIGN_BOTTOM,
} text_valign_t;

/**
 * Draw text at position
 */
void draw_text(gpu_context_t *gpu, const char *text, int32_t x, int32_t y,
               text_style_t style, color_rgba_t color);

/**
 * Draw text centered in rectangle
 */
void draw_text_centered(gpu_context_t *gpu, const char *text, const rect_t *rect,
                       text_style_t style, color_rgba_t color);

/**
 * Draw text with alignment
 */
void draw_text_aligned(gpu_context_t *gpu, const char *text, const rect_t *rect,
                      text_align_t align, text_valign_t valign,
                      text_style_t style, color_rgba_t color);

/**
 * Draw text with shadow
 */
void draw_text_with_shadow(gpu_context_t *gpu, const char *text, int32_t x, int32_t y,
                          text_style_t style, color_rgba_t color,
                          int32_t shadow_offset_x, int32_t shadow_offset_y,
                          color_rgba_t shadow_color);

/**
 * Measure text dimensions
 */
void measure_text(const char *text, text_style_t style, uint32_t *width, uint32_t *height);

// ============================================================================
// ICONS & IMAGES
// ============================================================================

/**
 * Draw icon (UTF-8 symbol or emoji)
 */
void draw_icon(gpu_context_t *gpu, const char *icon, int32_t x, int32_t y,
               uint32_t size, color_rgba_t color);

/**
 * Draw texture/image
 */
void draw_texture(gpu_context_t *gpu, texture_t *texture, const rect_t *dest);

/**
 * Draw texture with opacity
 */
void draw_texture_alpha(gpu_context_t *gpu, texture_t *texture, const rect_t *dest,
                       float opacity);

/**
 * Draw texture tinted with color
 */
void draw_texture_tinted(gpu_context_t *gpu, texture_t *texture, const rect_t *dest,
                        color_rgba_t tint);

// ============================================================================
// CLIPPING & MASKING
// ============================================================================

/**
 * Set clipping rectangle
 */
void set_clip_rect(gpu_context_t *gpu, const rect_t *rect);

/**
 * Reset clipping
 */
void reset_clip_rect(gpu_context_t *gpu);

/**
 * Push clipping rectangle (stack)
 */
void push_clip_rect(gpu_context_t *gpu, const rect_t *rect);

/**
 * Pop clipping rectangle
 */
void pop_clip_rect(gpu_context_t *gpu);

// ============================================================================
// COMPOSITING
// ============================================================================

typedef enum {
    BLEND_NORMAL,       // Standard alpha blending
    BLEND_MULTIPLY,     // Multiply colors
    BLEND_SCREEN,       // Screen blend
    BLEND_OVERLAY,      // Overlay blend
    BLEND_ADD,          // Additive blending
} blend_mode_t;

/**
 * Set blend mode
 */
void set_blend_mode(gpu_context_t *gpu, blend_mode_t mode);

/**
 * Draw with custom blend mode
 */
void draw_rect_blend(gpu_context_t *gpu, const rect_t *rect, color_rgba_t color,
                    blend_mode_t mode);

// ============================================================================
// COMPLEX UI ELEMENTS
// ============================================================================

/**
 * Draw traffic light buttons (macOS style)
 */
void draw_traffic_lights(gpu_context_t *gpu, int32_t x, int32_t y,
                        bool close_hover, bool minimize_hover, bool maximize_hover);

/**
 * Draw focus ring
 */
void draw_focus_ring(gpu_context_t *gpu, const rect_t *rect, uint32_t corner_radius,
                    color_rgba_t color);

/**
 * Draw selection highlight
 */
void draw_selection(gpu_context_t *gpu, const rect_t *rect, color_rgba_t color);

/**
 * Draw separator line
 */
void draw_separator(gpu_context_t *gpu, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                   color_rgba_t color);

/**
 * Draw progress bar
 */
void draw_progress_bar(gpu_context_t *gpu, const rect_t *rect, float progress,
                      color_rgba_t bg_color, color_rgba_t fill_color);

/**
 * Draw indeterminate progress bar (animated)
 */
void draw_progress_indeterminate(gpu_context_t *gpu, const rect_t *rect, float offset,
                                color_rgba_t bg_color, color_rgba_t fill_color);

/**
 * Draw slider track and thumb
 */
void draw_slider(gpu_context_t *gpu, const rect_t *track, float value,
                bool hovered, color_rgba_t track_color, color_rgba_t thumb_color);

/**
 * Draw switch/toggle
 */
void draw_switch(gpu_context_t *gpu, const rect_t *rect, bool checked,
                color_rgba_t off_color, color_rgba_t on_color);

/**
 * Draw checkbox
 */
void draw_checkbox(gpu_context_t *gpu, const rect_t *rect, bool checked,
                  float check_progress, color_rgba_t color);

/**
 * Draw radio button
 */
void draw_radio(gpu_context_t *gpu, const rect_t *rect, bool selected,
               color_rgba_t color);

// ============================================================================
// ANTI-ALIASING
// ============================================================================

/**
 * Enable/disable anti-aliasing
 */
void set_antialiasing(gpu_context_t *gpu, bool enabled);

/**
 * Set anti-aliasing quality (1-4)
 */
void set_antialiasing_quality(gpu_context_t *gpu, uint32_t quality);

// ============================================================================
// PERFORMANCE UTILITIES
// ============================================================================

/**
 * Batch multiple draw calls
 */
typedef struct batch_renderer batch_renderer_t;

batch_renderer_t *batch_create(gpu_context_t *gpu);
void batch_destroy(batch_renderer_t *batch);
void batch_begin(batch_renderer_t *batch);
void batch_end(batch_renderer_t *batch);
void batch_flush(batch_renderer_t *batch);

/**
 * Add rectangle to batch
 */
void batch_add_rect(batch_renderer_t *batch, const rect_t *rect, color_rgba_t color);

/**
 * Add rounded rectangle to batch
 */
void batch_add_rounded_rect(batch_renderer_t *batch, const rect_t *rect,
                           uint32_t radius, color_rgba_t color);

// ============================================================================
// DEBUG UTILITIES
// ============================================================================

/**
 * Draw bounding box (for debugging layout)
 */
void debug_draw_bounds(gpu_context_t *gpu, const rect_t *rect, color_rgba_t color);

/**
 * Draw grid overlay (for alignment)
 */
void debug_draw_grid(gpu_context_t *gpu, uint32_t spacing, color_rgba_t color);

/**
 * Draw FPS counter
 */
void debug_draw_fps(gpu_context_t *gpu, int32_t x, int32_t y, uint32_t fps);

#endif // RENDER_UTILS_H

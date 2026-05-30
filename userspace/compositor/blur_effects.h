/**
 * AutomationOS Background Blur Effects
 *
 * GPU-accelerated Gaussian blur for panels, menus, and windows
 */

#ifndef BLUR_EFFECTS_H
#define BLUR_EFFECTS_H

#include <stdint.h>
#include <stdbool.h>
#include "gpu.h"
#include "compositor.h"

/**
 * Blur quality levels
 */
typedef enum {
    BLUR_QUALITY_LOW,       // Fast, radius up to 10px
    BLUR_QUALITY_MEDIUM,    // Balanced, radius up to 20px
    BLUR_QUALITY_HIGH,      // Slower, radius up to 40px
    BLUR_QUALITY_ULTRA,     // Slowest, radius up to 80px
} blur_quality_t;

/**
 * Blur parameters
 */
typedef struct {
    float radius;           // Blur radius in pixels
    blur_quality_t quality;
    float opacity;          // Opacity of blurred content (0.0 - 1.0)
    bool saturation_boost;  // Increase saturation (iOS-style)
} blur_params_t;

/**
 * Blur context (for caching)
 */
typedef struct blur_context blur_context_t;

/**
 * Initialize blur system
 */
blur_context_t *blur_init(gpu_context_t *gpu);

/**
 * Cleanup blur system
 */
void blur_cleanup(blur_context_t *ctx);

/**
 * Blur a rectangular region of the screen
 * (e.g., for panel backgrounds)
 */
void blur_region(blur_context_t *ctx,
                 const rect_t *region,
                 const blur_params_t *params);

/**
 * Blur behind a window
 * (for semi-transparent windows)
 */
void blur_behind_window(blur_context_t *ctx,
                        window_t *window,
                        const blur_params_t *params);

/**
 * Apply iOS-style vibrancy effect
 * (blur + saturation + brightness adjustment)
 */
void blur_vibrancy(blur_context_t *ctx,
                   const rect_t *region,
                   float blur_radius,
                   float saturation,
                   float brightness);

/**
 * Two-pass Gaussian blur (horizontal + vertical)
 * Returns blurred texture
 */
texture_t *blur_gaussian_two_pass(blur_context_t *ctx,
                                  texture_t *input,
                                  float radius,
                                  blur_quality_t quality);

/**
 * Single-pass box blur (faster but lower quality)
 */
texture_t *blur_box(blur_context_t *ctx,
                    texture_t *input,
                    float radius);

/**
 * Kawase blur (multiple downsampling passes, very fast)
 */
texture_t *blur_kawase(blur_context_t *ctx,
                       texture_t *input,
                       int iterations);

/**
 * Create blur shader programs
 */
shader_t *blur_create_gaussian_shader(gpu_context_t *gpu, bool horizontal);
shader_t *blur_create_box_shader(gpu_context_t *gpu);
shader_t *blur_create_kawase_shader(gpu_context_t *gpu);

/**
 * Preset blur parameters
 */
extern const blur_params_t BLUR_PANEL_BACKGROUND;    // For top panel
extern const blur_params_t BLUR_DOCK_BACKGROUND;     // For dock
extern const blur_params_t BLUR_MENU_BACKGROUND;     // For menus
extern const blur_params_t BLUR_DIALOG_BACKGROUND;   // For dialogs
extern const blur_params_t BLUR_NOTIFICATION;        // For notifications

#endif // BLUR_EFFECTS_H

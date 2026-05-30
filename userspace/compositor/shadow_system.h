/**
 * AutomationOS Enhanced Shadow System
 *
 * 5-level shadow system matching Material Design 3 elevation
 */

#ifndef SHADOW_SYSTEM_H
#define SHADOW_SYSTEM_H

#include <stdint.h>
#include "compositor.h"
#include "gpu.h"

/**
 * Shadow elevation levels
 */
typedef enum {
    SHADOW_SM,      // Small - Buttons, cards (elevation 1)
    SHADOW_MD,      // Medium - Raised buttons (elevation 2)
    SHADOW_LG,      // Large - Dialogs, menus (elevation 4)
    SHADOW_XL,      // Extra Large - Active windows (elevation 8)
    SHADOW_XXL,     // Extra Extra Large - Modals (elevation 16)
} shadow_level_t;

/**
 * Shadow properties for each level
 */
typedef struct {
    // Key shadow (sharp, directional)
    int32_t key_offset_y;
    float key_blur;
    float key_opacity;

    // Ambient shadow (soft, diffuse)
    int32_t ambient_offset_y;
    float ambient_blur;
    float ambient_opacity;

    // Color
    uint32_t color;  // RGBA
} shadow_spec_t;

/**
 * Material Design 3 shadow specifications
 */
extern const shadow_spec_t SHADOW_SPECS[5];

/**
 * Draw layered shadow (key + ambient)
 */
void shadow_draw_layered(gpu_context_t *gpu,
                         const rect_t *rect,
                         shadow_level_t level,
                         bool active);

/**
 * Draw shadow with custom parameters
 */
void shadow_draw_custom(gpu_context_t *gpu,
                        const rect_t *rect,
                        const shadow_spec_t *spec);

/**
 * Get shadow specification for level
 */
const shadow_spec_t *shadow_get_spec(shadow_level_t level);

/**
 * Interpolate between shadow levels
 * (for smooth elevation transitions)
 */
shadow_spec_t shadow_interpolate(shadow_level_t from,
                                  shadow_level_t to,
                                  float t);

/**
 * Draw rounded shadow (for rounded windows)
 */
void shadow_draw_rounded(gpu_context_t *gpu,
                         const rect_t *rect,
                         float corner_radius,
                         shadow_level_t level);

#endif // SHADOW_SYSTEM_H

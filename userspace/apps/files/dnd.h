/**
 * File Explorer - Drag and Drop
 *
 * Handles drag and drop operations for files
 */

#ifndef DND_H
#define DND_H

#include <stdint.h>
#include <stdbool.h>

typedef struct dnd_context dnd_context_t;

/**
 * Drag and drop operation types
 */
typedef enum {
    DND_NONE,
    DND_COPY,
    DND_MOVE,
    DND_LINK,
} dnd_operation_t;

/**
 * Drag and drop context
 */
struct dnd_context {
    bool active;
    dnd_operation_t operation;

    // Source files
    char source_paths[256][4096];
    uint32_t source_count;

    // Drag state
    int32_t start_x, start_y;
    int32_t current_x, current_y;

    // Drop target
    char target_path[4096];
    bool valid_target;

    // Visual feedback
    uint32_t *drag_image;
    uint32_t drag_image_width;
    uint32_t drag_image_height;
};

// Drag and drop operations
dnd_context_t* dnd_create(void);
void dnd_destroy(dnd_context_t *dnd);

void dnd_start(dnd_context_t *dnd, const char **paths, uint32_t count,
               int32_t x, int32_t y);
void dnd_update(dnd_context_t *dnd, int32_t x, int32_t y);
void dnd_finish(dnd_context_t *dnd);
void dnd_cancel(dnd_context_t *dnd);

void dnd_set_target(dnd_context_t *dnd, const char *target_path);
bool dnd_can_drop(dnd_context_t *dnd);

void dnd_render(dnd_context_t *dnd);

#endif // DND_H

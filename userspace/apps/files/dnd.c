/**
 * File Explorer - Drag and Drop Implementation
 */

#include "dnd.h"
#include "operations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

dnd_context_t* dnd_create(void) {
    dnd_context_t *dnd = calloc(1, sizeof(dnd_context_t));
    if (!dnd) return NULL;

    dnd->active = false;
    dnd->operation = DND_NONE;
    dnd->source_count = 0;
    dnd->valid_target = false;
    dnd->drag_image = NULL;

    return dnd;
}

void dnd_destroy(dnd_context_t *dnd) {
    if (!dnd) return;

    if (dnd->drag_image) {
        free(dnd->drag_image);
    }

    free(dnd);
}

void dnd_start(dnd_context_t *dnd, const char **paths, uint32_t count,
               int32_t x, int32_t y) {
    if (!dnd || !paths || count == 0) return;

    dnd->active = true;
    dnd->operation = DND_MOVE;  // Default to move
    dnd->start_x = x;
    dnd->start_y = y;
    dnd->current_x = x;
    dnd->current_y = y;

    // Copy source paths
    dnd->source_count = count;
    for (uint32_t i = 0; i < count && i < 256; i++) {
        strncpy(dnd->source_paths[i], paths[i], sizeof(dnd->source_paths[i]) - 1);
    }

    // TODO: Generate drag image

    printf("[DnD] Started drag with %u files\n", count);
}

void dnd_update(dnd_context_t *dnd, int32_t x, int32_t y) {
    if (!dnd || !dnd->active) return;

    dnd->current_x = x;
    dnd->current_y = y;
}

void dnd_finish(dnd_context_t *dnd) {
    if (!dnd || !dnd->active) return;

    if (dnd->valid_target && dnd->target_path[0] != '\0') {
        // Perform drop operation
        const char *sources[256];
        for (uint32_t i = 0; i < dnd->source_count; i++) {
            sources[i] = dnd->source_paths[i];
        }

        file_operation_t *op;
        if (dnd->operation == DND_COPY) {
            op = operation_copy_files(sources, dnd->source_count, dnd->target_path);
        } else {
            op = operation_move_files(sources, dnd->source_count, dnd->target_path);
        }

        if (op) {
            operation_start(op);
            printf("[DnD] Started %s operation\n",
                   dnd->operation == DND_COPY ? "copy" : "move");
        }
    }

    dnd->active = false;
    dnd->operation = DND_NONE;
    dnd->source_count = 0;
}

void dnd_cancel(dnd_context_t *dnd) {
    if (!dnd) return;

    dnd->active = false;
    dnd->operation = DND_NONE;
    dnd->source_count = 0;

    printf("[DnD] Cancelled drag\n");
}

void dnd_set_target(dnd_context_t *dnd, const char *target_path) {
    if (!dnd || !target_path) return;

    strncpy(dnd->target_path, target_path, sizeof(dnd->target_path) - 1);
    dnd->valid_target = is_directory(target_path);
}

bool dnd_can_drop(dnd_context_t *dnd) {
    if (!dnd || !dnd->active) return false;
    return dnd->valid_target;
}

void dnd_render(dnd_context_t *dnd) {
    if (!dnd || !dnd->active) return;

    // TODO: Render drag image at cursor position
    // TODO: Show drop indicator if valid target
}

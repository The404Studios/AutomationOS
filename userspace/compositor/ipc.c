/**
 * Compositor IPC Interface - Implementation
 *
 * Production implementation using Agent 1's IPC subsystem.
 * Handles all compositor↔application communication.
 */

#include "ipc.h"
#include "../include/ipc_protocol.h"
#include "../include/ipc_keys.h"
#include "../libc/ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

// Need memcpy for IPC_MSG_SET_PAYLOAD
#ifndef memcpy
extern void *memcpy(void *dest, const void *src, size_t n);
#endif

// Message queue ID (global)
static int msg_queue_id = -1;

// Shared memory tracking
#define MAX_SHM_SEGMENTS 64
static struct {
    uint32_t shm_id;
    void *addr;
    size_t size;
} shm_segments[MAX_SHM_SEGMENTS];
static uint32_t shm_count = 0;

/**
 * Initialize compositor IPC
 *
 * Creates message queue for receiving commands from applications.
 */
int ipc_init_compositor(void) {
    // Create message queue with well-known key
    msg_queue_id = msgget(IPC_KEY_COMPOSITOR, IPC_CREAT | 0666);
    if (msg_queue_id == -1) {
        fprintf(stderr, "[IPC] Failed to create compositor queue: %s\n", strerror(errno));
        fprintf(stderr, "[IPC] Key used: 0x%x\n", IPC_KEY_COMPOSITOR);
        return -1;
    }

    printf("[IPC] Compositor IPC initialized\n");
    printf("[IPC]   Queue ID: %d\n", msg_queue_id);
    printf("[IPC]   Key: 0x%x\n", IPC_KEY_COMPOSITOR);

    return 0;
}

/**
 * Cleanup compositor IPC
 */
void ipc_cleanup_compositor(void) {
    // Detach all shared memory segments
    for (uint32_t i = 0; i < shm_count; i++) {
        if (shm_segments[i].addr) {
            shmdt(shm_segments[i].addr);
        }
    }

    // Remove message queue
    if (msg_queue_id >= 0) {
        msgctl(msg_queue_id, IPC_RMID, NULL);
    }

    printf("[IPC] Compositor IPC cleaned up\n");
}

/**
 * Receive IPC message (non-blocking)
 *
 * Returns 0 if message received, -1 if no message or error.
 */
int ipc_receive_message(ipc_message_t *msg) {
    if (!msg) return -1;

    if (msg_queue_id < 0) {
        return -1;
    }

    // Receive any compositor message (non-blocking)
    // mtype range: 1-50 for compositor messages
    ssize_t ret = msgrcv(msg_queue_id, msg, sizeof(ipc_message_t) - sizeof(long),
                        0, IPC_NOWAIT);
    if (ret == -1) {
        if (errno != ENOMSG && errno != -42) {
            fprintf(stderr, "[IPC] msgrcv() failed: %d\n", errno);
        }
        return -1;
    }

    return 0;
}

/**
 * Send response to client
 */
int ipc_send_response(uint32_t client_id, int32_t result) {
    // TODO: Implement response mechanism
    (void)client_id;
    (void)result;
    return 0;
}

/**
 * Attach shared memory segment
 */
static void *shm_attach(uint32_t shm_id) {
    // Check if already attached
    for (uint32_t i = 0; i < shm_count; i++) {
        if (shm_segments[i].shm_id == shm_id) {
            return shm_segments[i].addr;
        }
    }

    // Attach new segment
    void *addr = shmat(shm_id, NULL, SHM_RDONLY);
    if (addr == (void *)-1) {
        fprintf(stderr, "[IPC] shmat() failed: %s\n", strerror(errno));
        return NULL;
    }

    // Track segment
    if (shm_count < MAX_SHM_SEGMENTS) {
        shm_segments[shm_count].shm_id = shm_id;
        shm_segments[shm_count].addr = addr;
        shm_segments[shm_count].size = 0;  // TODO: get size
        shm_count++;
    }

    return addr;
}

/**
 * Handle create window request
 */
int ipc_handle_create_window(fb_compositor_t *comp, ipc_message_t *msg) {
    create_window_request_t *req = IPC_MSG_GET_PAYLOAD(msg, create_window_request_t);

    // Cap untrusted dimensions BEFORE they size the SHM segment (WINDOW_SURFACE_SIZE
    // below) and the window pixel buffer, so neither can overflow and the segment
    // capacity matches the window's recorded surface_capacity_pixels.
    if (req->width  == 0) req->width  = 1;
    if (req->height == 0) req->height = 1;
    if (req->width  > WINDOW_MAX_DIM) req->width  = WINDOW_MAX_DIM;
    if (req->height > WINDOW_MAX_DIM) req->height = WINDOW_MAX_DIM;

    // Generate window ID
    static uint32_t next_window_id = 1;
    uint32_t window_id = next_window_id++;

    // Create window
    window_t *window = window_create(window_id, req->window_type,
                                     req->x, req->y,
                                     req->width, req->height);
    if (!window) {
        return IPC_ERR_NO_MEMORY;
    }

    window_set_title(window, req->title);
    fb_compositor_add_window(comp, window);

    // Create shared memory for window surface
    size_t surface_size = WINDOW_SURFACE_SIZE(req->width, req->height);
    int shm_id = shmget(IPC_KEY_WINDOW_SURFACE(window_id), surface_size, IPC_CREAT | 0666);

    if (shm_id < 0) {
        fprintf(stderr, "[IPC] Failed to create surface shared memory\n");
        return IPC_ERR_NO_MEMORY;
    }

    // Initialize surface
    window_surface_t *surface = shmat(shm_id, NULL, 0);
    if (!surface) {
        fprintf(stderr, "[IPC] Failed to attach surface shared memory\n");
        shmctl(shm_id, IPC_RMID, NULL);
        return IPC_ERR_NO_MEMORY;
    }

    surface->width = req->width;
    surface->height = req->height;
    surface->format = 0;  // RGBA8888
    surface->stride = req->width * 4;
    surface->version = 1;
    surface->damage_count = 0;

    shmdt(surface);

    printf("[IPC] Created window %u: %s (%ux%u), shm_id=%d\n",
           window_id, req->title, req->width, req->height, shm_id);

    // Send response
    ipc_message_t response;
    IPC_MSG_INIT(&response, MSG_RESPONSE_SUCCESS, 0, msg->header.sequence);

    create_window_response_t *resp = IPC_MSG_GET_PAYLOAD(&response, create_window_response_t);
    resp->window_id = window_id;
    resp->shm_id = shm_id;
    resp->actual_x = req->x;
    resp->actual_y = req->y;
    resp->actual_w = req->width;
    resp->actual_h = req->height;

    response.header.payload_size = sizeof(create_window_response_t);

    // Send response to sender's queue
    int sender_queue = msgget(ipc_response_key(msg->header.sender_id), 0);
    if (sender_queue >= 0) {
        msgsnd(sender_queue, &response, sizeof(ipc_message_t) - sizeof(long), IPC_NOWAIT);
    }

    return IPC_SUCCESS;
}

/**
 * Handle destroy window request
 */
int ipc_handle_destroy_window(fb_compositor_t *comp, ipc_message_t *msg) {
    window_operation_t *req = IPC_MSG_GET_PAYLOAD(msg, window_operation_t);

    // Remove shared memory for window surface
    int shm_id = shmget(IPC_KEY_WINDOW_SURFACE(req->window_id), 0, 0);
    if (shm_id >= 0) {
        shmctl(shm_id, IPC_RMID, NULL);
    }

    fb_compositor_remove_window(comp, req->window_id);
    printf("[IPC] Destroyed window %u\n", req->window_id);
    return IPC_SUCCESS;
}

/**
 * Handle map window request
 */
int ipc_handle_map_window(fb_compositor_t *comp, ipc_message_t *msg) {
    window_operation_t *req = IPC_MSG_GET_PAYLOAD(msg, window_operation_t);

    window_t *window = fb_compositor_find_window(comp, req->window_id);
    if (!window) return IPC_ERR_INVALID_WINDOW;

    window->mapped = true;
    damage_add_region(&comp->damage, &window->geometry);

    printf("[IPC] Mapped window %u\n", req->window_id);
    return IPC_SUCCESS;
}

/**
 * Handle unmap window request
 */
int ipc_handle_unmap_window(fb_compositor_t *comp, ipc_message_t *msg) {
    window_operation_t *req = IPC_MSG_GET_PAYLOAD(msg, window_operation_t);

    window_t *window = fb_compositor_find_window(comp, req->window_id);
    if (!window) return IPC_ERR_INVALID_WINDOW;

    damage_add_region(&comp->damage, &window->geometry);
    window->mapped = false;

    printf("[IPC] Unmapped window %u\n", req->window_id);
    return IPC_SUCCESS;
}

/**
 * Handle update surface request
 */
int ipc_handle_update_surface(fb_compositor_t *comp, ipc_message_t *msg) {
    update_surface_request_t *req = IPC_MSG_GET_PAYLOAD(msg, update_surface_request_t);

    window_t *window = fb_compositor_find_window(comp, req->window_id);
    if (!window) return IPC_ERR_INVALID_WINDOW;

    // Attach shared memory for window surface
    int shm_id = shmget(IPC_KEY_WINDOW_SURFACE(req->window_id), 0, 0);
    if (shm_id < 0) {
        fprintf(stderr, "[IPC] Window surface not found for window %u\n", req->window_id);
        return IPC_ERR_NOT_FOUND;
    }

    window_surface_t *surface = shmat(shm_id, NULL, SHM_RDONLY);
    if (!surface) {
        fprintf(stderr, "[IPC] Failed to attach surface shared memory\n");
        return IPC_ERR_NO_MEMORY;
    }

    // Update window surface from shared memory
    // If dirty_rect is zero, update full window
    rect_t dirty = req->dirty_rect;
    if (dirty.width == 0 || dirty.height == 0) {
        dirty = (rect_t){0, 0, surface->width, surface->height};
    }

    // Copy damaged region to window buffer
    window_update_surface(window, surface->pixels, surface->width, surface->height);

    damage_add_region(&comp->damage, &window->geometry);

    shmdt(surface);

    return IPC_SUCCESS;
}

/**
 * Handle set title request
 */
int ipc_handle_set_title(fb_compositor_t *comp, ipc_message_t *msg) {
    set_title_request_t *req = IPC_MSG_GET_PAYLOAD(msg, set_title_request_t);

    window_t *window = fb_compositor_find_window(comp, req->window_id);
    if (!window) return IPC_ERR_INVALID_WINDOW;

    window_set_title(window, req->title);

    printf("[IPC] Set window %u title: %s\n", req->window_id, req->title);
    return IPC_SUCCESS;
}

/**
 * Handle move window request
 */
int ipc_handle_move_window(fb_compositor_t *comp, ipc_message_t *msg) {
    move_window_request_t *req = IPC_MSG_GET_PAYLOAD(msg, move_window_request_t);

    window_t *window = fb_compositor_find_window(comp, req->window_id);
    if (!window) return IPC_ERR_INVALID_WINDOW;

    // Mark old and new positions as damaged
    damage_add_region(&comp->damage, &window->geometry);

    window->geometry.x = req->x;
    window->geometry.y = req->y;

    damage_add_region(&comp->damage, &window->geometry);

    return IPC_SUCCESS;
}

/**
 * Handle resize window request
 */
int ipc_handle_resize_window(fb_compositor_t *comp, ipc_message_t *msg) {
    resize_window_request_t *req = IPC_MSG_GET_PAYLOAD(msg, resize_window_request_t);

    window_t *window = fb_compositor_find_window(comp, req->window_id);
    if (!window) return IPC_ERR_INVALID_WINDOW;

    // Mark old size as damaged
    damage_add_region(&comp->damage, &window->geometry);

    window->geometry.width = req->width;
    window->geometry.height = req->height;

    // Resize surface
    // TODO: Notify application to redraw
    damage_add_region(&comp->damage, &window->geometry);

    return IPC_SUCCESS;
}

/**
 * Handle focus window request (from WM)
 */
int ipc_handle_focus_window(fb_compositor_t *comp, ipc_message_t *msg) {
    wm_focus_changed_t *req = IPC_MSG_GET_PAYLOAD(msg, wm_focus_changed_t);

    // Unfocus old window
    if (req->old_window_id != 0) {
        window_t *old_window = fb_compositor_find_window(comp, req->old_window_id);
        if (old_window) {
            old_window->focused = false;
            damage_add_region(&comp->damage, &old_window->geometry);
        }
    }

    // Focus new window
    if (req->new_window_id != 0) {
        window_t *new_window = fb_compositor_find_window(comp, req->new_window_id);
        if (!new_window) return IPC_ERR_INVALID_WINDOW;

        new_window->focused = true;
        damage_add_region(&comp->damage, &new_window->geometry);
    }

    return IPC_SUCCESS;
}

/**
 * Handle raise window request
 */
int ipc_handle_raise_window(fb_compositor_t *comp, ipc_message_t *msg) {
    window_operation_t *req = IPC_MSG_GET_PAYLOAD(msg, window_operation_t);

    fb_compositor_raise_window(comp, req->window_id);
    return IPC_SUCCESS;
}

/**
 * Handle lower window request
 */
int ipc_handle_lower_window(fb_compositor_t *comp, ipc_message_t *msg) {
    window_operation_t *req = IPC_MSG_GET_PAYLOAD(msg, window_operation_t);

    fb_compositor_lower_window(comp, req->window_id);
    return IPC_SUCCESS;
}

/**
 * Dispatch IPC message to appropriate handler
 */
int ipc_dispatch_message(fb_compositor_t *comp, ipc_message_t *msg) {
    if (!comp || !msg) return -1;

    switch (msg->header.mtype) {
        case MSG_COMPOSITOR_CREATE_WINDOW:
            return ipc_handle_create_window(comp, msg);
        case MSG_COMPOSITOR_DESTROY_WINDOW:
            return ipc_handle_destroy_window(comp, msg);
        case MSG_COMPOSITOR_MAP_WINDOW:
            return ipc_handle_map_window(comp, msg);
        case MSG_COMPOSITOR_UNMAP_WINDOW:
            return ipc_handle_unmap_window(comp, msg);
        case MSG_COMPOSITOR_UPDATE_SURFACE:
            return ipc_handle_update_surface(comp, msg);
        case MSG_COMPOSITOR_SET_TITLE:
            return ipc_handle_set_title(comp, msg);
        case MSG_COMPOSITOR_MOVE_WINDOW:
            return ipc_handle_move_window(comp, msg);
        case MSG_COMPOSITOR_RESIZE_WINDOW:
            return ipc_handle_resize_window(comp, msg);
        case MSG_WM_FOCUS_CHANGED:
            return ipc_handle_focus_window(comp, msg);
        case MSG_COMPOSITOR_RAISE_WINDOW:
            return ipc_handle_raise_window(comp, msg);
        case MSG_COMPOSITOR_LOWER_WINDOW:
            return ipc_handle_lower_window(comp, msg);
        default:
            fprintf(stderr, "[IPC] Unknown message type: %ld\n", msg->header.mtype);
            return -1;
    }
}

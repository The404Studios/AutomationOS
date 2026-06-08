/**
 * IPC Window Test Client
 * ======================
 *
 * Demonstrates complete IPC protocol for creating a window,
 * rendering to shared memory, and communicating with compositor.
 */

#include "../include/ipc_protocol.h"
#include "../include/ipc_keys.h"
#include "../libc/ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Simple memcpy if not available
#ifndef memcpy
void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}
#endif

/**
 * Test: Create window and render
 */
int test_create_and_render(void) {
    printf("\n=== Test: Create Window and Render ===\n");

    // 1. Connect to compositor
    int compositor_queue = msgget(IPC_KEY_COMPOSITOR, 0);
    if (compositor_queue < 0) {
        fprintf(stderr, "[ERROR] Failed to connect to compositor queue (key=0x%x)\n", IPC_KEY_COMPOSITOR);
        return -1;
    }
    printf("[OK] Connected to compositor (queue_id=%d)\n", compositor_queue);

    // 2. Create response queue for this process
    int my_pid = getpid();
    int response_queue = msgget(ipc_response_key(my_pid), IPC_CREAT | 0666);
    if (response_queue < 0) {
        fprintf(stderr, "[ERROR] Failed to create response queue\n");
        return -1;
    }
    printf("[OK] Created response queue (queue_id=%d)\n", response_queue);

    // 3. Send create window request
    ipc_message_t msg;
    IPC_MSG_INIT(&msg, MSG_COMPOSITOR_CREATE_WINDOW, my_pid, 1);

    create_window_request_t req = {
        .window_type = 0,  // Normal window
        .x = 100,
        .y = 100,
        .width = 640,
        .height = 480,
        .parent_id = 0,
    };
    strcpy(req.title, "IPC Test Window");

    IPC_MSG_SET_PAYLOAD(&msg, &req, sizeof(req));

    printf("[SEND] Creating window: %s (%ux%u)\n", req.title, req.width, req.height);

    if (msgsnd(compositor_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0) < 0) {
        fprintf(stderr, "[ERROR] Failed to send create window request\n");
        return -1;
    }

    // 4. Wait for response
    ipc_message_t response;
    printf("[WAIT] Waiting for create window response...\n");

    if (msgrcv(response_queue, &response, sizeof(ipc_message_t) - sizeof(long),
               MSG_RESPONSE_SUCCESS, 0) < 0) {
        fprintf(stderr, "[ERROR] Failed to receive response\n");
        return -1;
    }

    if (response.header.mtype != MSG_RESPONSE_SUCCESS) {
        fprintf(stderr, "[ERROR] Received error response\n");
        return -1;
    }

    create_window_response_t *resp = IPC_MSG_GET_PAYLOAD(&response, create_window_response_t);
    printf("[RECV] Window created!\n");
    printf("       Window ID: %u\n", resp->window_id);
    printf("       SHM ID: %d\n", resp->shm_id);
    printf("       Position: (%d, %d)\n", resp->actual_x, resp->actual_y);
    printf("       Size: %ux%u\n", resp->actual_w, resp->actual_h);

    // 5. Attach to window surface
    window_surface_t *surface = shmat(resp->shm_id, NULL, 0);
    if (!surface) {
        fprintf(stderr, "[ERROR] Failed to attach to window surface\n");
        return -1;
    }
    printf("[OK] Attached to window surface @ %p\n", surface);

    // 6. Draw to surface (gradient pattern)
    printf("[DRAW] Rendering gradient...\n");
    for (uint32_t y = 0; y < surface->height; y++) {
        for (uint32_t x = 0; x < surface->width; x++) {
            uint8_t r = (x * 255) / surface->width;
            uint8_t g = (y * 255) / surface->height;
            uint8_t b = 128;
            uint8_t a = 255;
            surface->pixels[y * surface->width + x] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }

    // Mark entire surface as damaged
    surface->damage_count = 1;
    surface->damage_rects[0] = (rect_t){0, 0, surface->width, surface->height};

    // 7. Notify compositor of update
    IPC_MSG_INIT(&msg, MSG_COMPOSITOR_UPDATE_SURFACE, my_pid, 2);

    update_surface_request_t update_req = {
        .window_id = resp->window_id,
        .dirty_rect = {0, 0, surface->width, surface->height},
    };

    IPC_MSG_SET_PAYLOAD(&msg, &update_req, sizeof(update_req));

    printf("[SEND] Notifying compositor of surface update...\n");

    if (msgsnd(compositor_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0) < 0) {
        fprintf(stderr, "[ERROR] Failed to send update surface request\n");
    } else {
        printf("[OK] Compositor notified\n");
    }

    // 8. Map window (make it visible)
    IPC_MSG_INIT(&msg, MSG_COMPOSITOR_MAP_WINDOW, my_pid, 3);

    window_operation_t map_req = {
        .window_id = resp->window_id,
    };

    IPC_MSG_SET_PAYLOAD(&msg, &map_req, sizeof(map_req));

    printf("[SEND] Mapping window...\n");

    if (msgsnd(compositor_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0) < 0) {
        fprintf(stderr, "[ERROR] Failed to send map window request\n");
    } else {
        printf("[OK] Window mapped\n");
    }

    // 9. Keep window visible for a few seconds
    printf("[WAIT] Window displayed for 5 seconds...\n");
    sleep(5);

    // 10. Cleanup: Destroy window
    IPC_MSG_INIT(&msg, MSG_COMPOSITOR_DESTROY_WINDOW, my_pid, 4);

    window_operation_t destroy_req = {
        .window_id = resp->window_id,
    };

    IPC_MSG_SET_PAYLOAD(&msg, &destroy_req, sizeof(destroy_req));

    printf("[SEND] Destroying window...\n");

    if (msgsnd(compositor_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0) < 0) {
        fprintf(stderr, "[ERROR] Failed to send destroy window request\n");
    } else {
        printf("[OK] Window destroyed\n");
    }

    // Detach from surface
    shmdt(surface);

    // Remove response queue
    msgctl(response_queue, IPC_RMID, NULL);

    printf("\n=== Test: Create Window and Render - PASSED ===\n");
    return 0;
}

/**
 * Test: Multiple windows
 */
int test_multiple_windows(void) {
    printf("\n=== Test: Multiple Windows ===\n");

    int compositor_queue = msgget(IPC_KEY_COMPOSITOR, 0);
    if (compositor_queue < 0) {
        fprintf(stderr, "[ERROR] Failed to connect to compositor\n");
        return -1;
    }

    int my_pid = getpid();
    int response_queue = msgget(ipc_response_key(my_pid), IPC_CREAT | 0666);

    #define NUM_WINDOWS 3
    uint32_t window_ids[NUM_WINDOWS];
    int shm_ids[NUM_WINDOWS];

    // Create 3 windows
    for (int i = 0; i < NUM_WINDOWS; i++) {
        ipc_message_t msg;
        IPC_MSG_INIT(&msg, MSG_COMPOSITOR_CREATE_WINDOW, my_pid, i + 1);

        create_window_request_t req = {
            .window_type = 0,
            .x = 100 + (i * 50),
            .y = 100 + (i * 50),
            .width = 320,
            .height = 240,
            .parent_id = 0,
        };
        snprintf(req.title, sizeof(req.title), "Window %d", i + 1);

        IPC_MSG_SET_PAYLOAD(&msg, &req, sizeof(req));

        printf("[SEND] Creating window %d...\n", i + 1);
        msgsnd(compositor_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0);

        // Wait for response
        ipc_message_t response;
        msgrcv(response_queue, &response, sizeof(ipc_message_t) - sizeof(long),
               MSG_RESPONSE_SUCCESS, 0);

        create_window_response_t *resp = IPC_MSG_GET_PAYLOAD(&response, create_window_response_t);
        window_ids[i] = resp->window_id;
        shm_ids[i] = resp->shm_id;

        printf("[OK] Window %d created (ID=%u, SHM=%d)\n", i + 1, window_ids[i], shm_ids[i]);

        // Map window
        IPC_MSG_INIT(&msg, MSG_COMPOSITOR_MAP_WINDOW, my_pid, i + 10);
        window_operation_t map_req = { .window_id = window_ids[i] };
        IPC_MSG_SET_PAYLOAD(&msg, &map_req, sizeof(map_req));
        msgsnd(compositor_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0);
    }

    printf("[WAIT] Displaying windows for 3 seconds...\n");
    sleep(3);

    // Destroy all windows
    for (int i = 0; i < NUM_WINDOWS; i++) {
        ipc_message_t msg;
        IPC_MSG_INIT(&msg, MSG_COMPOSITOR_DESTROY_WINDOW, my_pid, i + 20);

        window_operation_t destroy_req = { .window_id = window_ids[i] };
        IPC_MSG_SET_PAYLOAD(&msg, &destroy_req, sizeof(destroy_req));

        printf("[SEND] Destroying window %d...\n", i + 1);
        msgsnd(compositor_queue, &msg, sizeof(ipc_message_t) - sizeof(long), 0);
    }

    msgctl(response_queue, IPC_RMID, NULL);

    printf("\n=== Test: Multiple Windows - PASSED ===\n");
    return 0;
}

/**
 * Main test suite
 */
int main(int argc, char **argv) {
    printf("========================================\n");
    printf("  IPC Window Test Client\n");
    printf("========================================\n");

    int result = 0;

    // Test 1: Create and render
    if (test_create_and_render() != 0) {
        fprintf(stderr, "[FAIL] Create and render test failed\n");
        result = -1;
    }

    // Test 2: Multiple windows
    if (test_multiple_windows() != 0) {
        fprintf(stderr, "[FAIL] Multiple windows test failed\n");
        result = -1;
    }

    if (result == 0) {
        printf("\n========================================\n");
        printf("  ALL TESTS PASSED\n");
        printf("========================================\n");
    } else {
        printf("\n========================================\n");
        printf("  SOME TESTS FAILED\n");
        printf("========================================\n");
    }

    return result;
}

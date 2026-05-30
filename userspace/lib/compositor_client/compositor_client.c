/**
 * Compositor Client Library - Implementation
 */

#include "compositor_client.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <errno.h>

// IPC message types (must match compositor/ipc.h)
typedef enum {
    MSG_CREATE_WINDOW = 0,
    MSG_DESTROY_WINDOW,
    MSG_MAP_WINDOW,
    MSG_UNMAP_WINDOW,
    MSG_UPDATE_SURFACE,
    MSG_SET_TITLE,
    MSG_MOVE_WINDOW,
    MSG_RESIZE_WINDOW,
    MSG_FOCUS_WINDOW,
    MSG_RAISE_WINDOW,
    MSG_LOWER_WINDOW,
} ipc_msg_type_t;

#define IPC_MAX_PAYLOAD 256

typedef struct {
    uint32_t client_id;
    ipc_msg_type_t type;
    uint32_t window_id;
    uint8_t payload[IPC_MAX_PAYLOAD];
} ipc_message_t;

struct comp_client {
    int socket_fd;
    uint32_t client_id;
    comp_window_t *windows[64];
    uint32_t window_count;
};

// ============================================================================
// CLIENT LIFECYCLE
// ============================================================================

comp_client_t *comp_client_init(const char *socket_path) {
    if (!socket_path) {
        socket_path = "/run/compositor.sock";
    }

    comp_client_t *client = calloc(1, sizeof(comp_client_t));
    if (!client) {
        fprintf(stderr, "[CompClient] Failed to allocate client\n");
        return NULL;
    }

    // Create UNIX socket
    client->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client->socket_fd < 0) {
        perror("[CompClient] socket");
        free(client);
        return NULL;
    }

    // Connect to compositor
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(client->socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[CompClient] connect");
        close(client->socket_fd);
        free(client);
        return NULL;
    }

    // Get client ID from compositor
    client->client_id = (uint32_t)getpid();

    printf("[CompClient] Connected to compositor (fd=%d, id=%u)\n",
           client->socket_fd, client->client_id);

    return client;
}

void comp_client_cleanup(comp_client_t *client) {
    if (!client) return;

    // Destroy all windows
    for (uint32_t i = 0; i < client->window_count; i++) {
        if (client->windows[i]) {
            comp_destroy_window(client, client->windows[i]);
        }
    }

    close(client->socket_fd);
    free(client);
}

// ============================================================================
// WINDOW MANAGEMENT
// ============================================================================

comp_window_t *comp_create_window(comp_client_t *client, comp_window_type_t type,
                                   int32_t x, int32_t y, uint32_t width, uint32_t height,
                                   const char *title) {
    if (!client || width == 0 || height == 0) {
        return NULL;
    }

    comp_window_t *window = calloc(1, sizeof(comp_window_t));
    if (!window) {
        fprintf(stderr, "[CompClient] Failed to allocate window\n");
        return NULL;
    }

    window->type = type;
    window->x = x;
    window->y = y;
    window->width = width;
    window->height = height;
    window->mapped = false;

    if (title) {
        strncpy(window->title, title, sizeof(window->title) - 1);
    }

    // Allocate pixel buffer
    size_t buffer_size = width * height * sizeof(uint32_t);
    window->pixels = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (window->pixels == MAP_FAILED) {
        perror("[CompClient] mmap");
        free(window);
        return NULL;
    }

    // Clear to transparent
    memset(window->pixels, 0, buffer_size);

    // Send create window message to compositor
    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.client_id = client->client_id;
    msg.type = MSG_CREATE_WINDOW;

    struct {
        comp_window_type_t type;
        int32_t x, y;
        uint32_t width, height;
        char title[128];
    } *create_req = (void *)msg.payload;

    create_req->type = type;
    create_req->x = x;
    create_req->y = y;
    create_req->width = width;
    create_req->height = height;
    if (title) {
        strncpy(create_req->title, title, sizeof(create_req->title) - 1);
    }

    if (send(client->socket_fd, &msg, sizeof(msg), 0) < 0) {
        perror("[CompClient] send");
        munmap(window->pixels, buffer_size);
        free(window);
        return NULL;
    }

    // Receive window ID response
    uint32_t window_id;
    if (recv(client->socket_fd, &window_id, sizeof(window_id), 0) < 0) {
        perror("[CompClient] recv");
        munmap(window->pixels, buffer_size);
        free(window);
        return NULL;
    }

    window->id = window_id;

    // Add to client window list
    if (client->window_count < 64) {
        client->windows[client->window_count++] = window;
    }

    printf("[CompClient] Created window %u (%ux%u) '%s'\n",
           window->id, width, height, title ? title : "(no title)");

    return window;
}

void comp_destroy_window(comp_client_t *client, comp_window_t *window) {
    if (!client || !window) return;

    // Send destroy message
    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.client_id = client->client_id;
    msg.type = MSG_DESTROY_WINDOW;
    msg.window_id = window->id;

    send(client->socket_fd, &msg, sizeof(msg), 0);

    // Free pixel buffer
    size_t buffer_size = window->width * window->height * sizeof(uint32_t);
    munmap(window->pixels, buffer_size);

    // Remove from client window list
    for (uint32_t i = 0; i < client->window_count; i++) {
        if (client->windows[i] == window) {
            for (uint32_t j = i; j < client->window_count - 1; j++) {
                client->windows[j] = client->windows[j + 1];
            }
            client->window_count--;
            break;
        }
    }

    free(window);
}

int comp_map_window(comp_client_t *client, comp_window_t *window) {
    if (!client || !window) return -1;

    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.client_id = client->client_id;
    msg.type = MSG_MAP_WINDOW;
    msg.window_id = window->id;

    if (send(client->socket_fd, &msg, sizeof(msg), 0) < 0) {
        perror("[CompClient] send");
        return -1;
    }

    window->mapped = true;
    return 0;
}

int comp_unmap_window(comp_client_t *client, comp_window_t *window) {
    if (!client || !window) return -1;

    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.client_id = client->client_id;
    msg.type = MSG_UNMAP_WINDOW;
    msg.window_id = window->id;

    if (send(client->socket_fd, &msg, sizeof(msg), 0) < 0) {
        perror("[CompClient] send");
        return -1;
    }

    window->mapped = false;
    return 0;
}

int comp_update_surface(comp_client_t *client, comp_window_t *window) {
    if (!client || !window) return -1;

    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.client_id = client->client_id;
    msg.type = MSG_UPDATE_SURFACE;
    msg.window_id = window->id;

    // TODO: Use shared memory for efficiency
    // For now, just notify compositor to re-read window buffer

    if (send(client->socket_fd, &msg, sizeof(msg), 0) < 0) {
        perror("[CompClient] send");
        return -1;
    }

    return 0;
}

int comp_set_title(comp_client_t *client, comp_window_t *window, const char *title) {
    if (!client || !window || !title) return -1;

    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.client_id = client->client_id;
    msg.type = MSG_SET_TITLE;
    msg.window_id = window->id;

    strncpy((char *)msg.payload, title, IPC_MAX_PAYLOAD - 1);
    strncpy(window->title, title, sizeof(window->title) - 1);

    if (send(client->socket_fd, &msg, sizeof(msg), 0) < 0) {
        perror("[CompClient] send");
        return -1;
    }

    return 0;
}

int comp_move_window(comp_client_t *client, comp_window_t *window, int32_t x, int32_t y) {
    if (!client || !window) return -1;

    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.client_id = client->client_id;
    msg.type = MSG_MOVE_WINDOW;
    msg.window_id = window->id;

    struct { int32_t x, y; } *move_req = (void *)msg.payload;
    move_req->x = x;
    move_req->y = y;

    window->x = x;
    window->y = y;

    if (send(client->socket_fd, &msg, sizeof(msg), 0) < 0) {
        perror("[CompClient] send");
        return -1;
    }

    return 0;
}

int comp_resize_window(comp_client_t *client, comp_window_t *window, uint32_t width, uint32_t height) {
    if (!client || !window || width == 0 || height == 0) return -1;

    // TODO: Reallocate pixel buffer
    (void)width;
    (void)height;

    fprintf(stderr, "[CompClient] WARNING: comp_resize_window not fully implemented\n");
    return -1;
}

int comp_raise_window(comp_client_t *client, comp_window_t *window) {
    if (!client || !window) return -1;

    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.client_id = client->client_id;
    msg.type = MSG_RAISE_WINDOW;
    msg.window_id = window->id;

    if (send(client->socket_fd, &msg, sizeof(msg), 0) < 0) {
        perror("[CompClient] send");
        return -1;
    }

    return 0;
}

uint32_t *comp_get_pixels(comp_window_t *window) {
    return window ? window->pixels : NULL;
}

int comp_flush(comp_client_t *client) {
    if (!client) return -1;
    // For now, all operations are synchronous
    return 0;
}

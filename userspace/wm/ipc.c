/**
 * AutomationOS Window Manager - IPC Communication
 *
 * Handles communication between window manager, compositor, and applications
 */

#include "window_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <fcntl.h>

#define MAX_CLIENTS 64
#define IPC_BUFFER_SIZE 4096

/**
 * IPC message types
 */
typedef enum {
    IPC_CREATE_WINDOW = 1,
    IPC_DESTROY_WINDOW,
    IPC_UPDATE_WINDOW,
    IPC_MOVE_WINDOW,
    IPC_RESIZE_WINDOW,
    IPC_MAP_WINDOW,
    IPC_UNMAP_WINDOW,
    IPC_FOCUS_WINDOW,
    IPC_RAISE_WINDOW,
    IPC_MINIMIZE_WINDOW,
    IPC_MAXIMIZE_WINDOW,
    IPC_CLOSE_WINDOW,
    IPC_KEYBOARD_EVENT,
    IPC_MOUSE_EVENT,
    IPC_CONFIGURE_EVENT,
    IPC_DAMAGE_EVENT,
} ipc_message_type_t;

/**
 * IPC message header
 */
typedef struct {
    uint32_t type;
    uint32_t window_id;
    uint32_t data_len;
} ipc_message_header_t;

/**
 * IPC client connection
 */
typedef struct {
    int fd;
    uint32_t app_id;
    bool active;
} ipc_client_t;

// Global IPC state
static int g_wm_socket = -1;
static int g_compositor_socket = -1;
static ipc_client_t g_clients[MAX_CLIENTS];
static uint32_t g_client_count = 0;

/**
 * Initialize IPC system
 */
int wm_ipc_init(window_manager_t *wm, const char *socket_path) {
    if (!wm || !socket_path) return -1;

    // Remove stale socket
    unlink(socket_path);

    // Create Unix domain socket
    g_wm_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_wm_socket < 0) {
        fprintf(stderr, "[IPC] Failed to create socket: %s\n", strerror(errno));
        return -1;
    }

    // Set non-blocking
    int flags = fcntl(g_wm_socket, F_GETFL, 0);
    fcntl(g_wm_socket, F_SETFL, flags | O_NONBLOCK);

    // Bind socket
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(g_wm_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[IPC] Failed to bind socket: %s\n", strerror(errno));
        close(g_wm_socket);
        g_wm_socket = -1;
        return -1;
    }

    // Listen for connections
    if (listen(g_wm_socket, 5) < 0) {
        fprintf(stderr, "[IPC] Failed to listen: %s\n", strerror(errno));
        close(g_wm_socket);
        unlink(socket_path);
        g_wm_socket = -1;
        return -1;
    }

    printf("[IPC] Listening on %s (fd=%d)\n", socket_path, g_wm_socket);
    return 0;
}

/**
 * Cleanup IPC system
 */
void wm_ipc_cleanup(void) {
    // Close all client connections
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) {
            close(g_clients[i].fd);
            g_clients[i].active = false;
        }
    }

    // Close sockets
    if (g_wm_socket >= 0) {
        close(g_wm_socket);
        g_wm_socket = -1;
    }

    if (g_compositor_socket >= 0) {
        close(g_compositor_socket);
        g_compositor_socket = -1;
    }

    printf("[IPC] Cleaned up\n");
}

/**
 * Connect to compositor
 */
int wm_ipc_connect_compositor(const char *socket_path) {
    if (!socket_path) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[IPC] Failed to create compositor socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    // Try to connect with retries
    int retries = 10;
    while (retries > 0) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            // Set non-blocking
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            g_compositor_socket = fd;
            printf("[IPC] Connected to compositor (fd=%d)\n", fd);
            return fd;
        }

        fprintf(stderr, "[IPC] Waiting for compositor... (%d retries left)\n", retries);
        sleep(1);
        retries--;
    }

    fprintf(stderr, "[IPC] Failed to connect to compositor: %s\n", strerror(errno));
    close(fd);
    return -1;
}

/**
 * Accept new client connection
 */
static void accept_client(void) {
    struct sockaddr_un client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(g_wm_socket, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[IPC] Failed to accept client: %s\n", strerror(errno));
        }
        return;
    }

    // Set non-blocking
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    // Find free slot
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].active) {
            g_clients[i].fd = client_fd;
            g_clients[i].app_id = i + 1;
            g_clients[i].active = true;
            g_client_count++;
            printf("[IPC] Client %u connected (fd=%d)\n", i, client_fd);
            return;
        }
    }

    // No free slots
    fprintf(stderr, "[IPC] Too many clients, rejecting connection\n");
    close(client_fd);
}

/**
 * Handle message from client
 */
static void handle_client_message(window_manager_t *wm, ipc_client_t *client,
                                  const ipc_message_header_t *header, const void *data) {
    printf("[IPC] Message from client: type=%u, window=%u, len=%u\n",
           header->type, header->window_id, header->data_len);

    switch (header->type) {
        case IPC_CREATE_WINDOW: {
            // data contains: type (4 bytes), width (4 bytes), height (4 bytes), title (variable)
            const uint32_t *params = data;
            window_type_t type = params[0];
            uint32_t width = params[1];
            uint32_t height = params[2];
            const char *title = (const char *)&params[3];

            window_t *window = wm_create_window(wm, type, width, height, title);
            if (window) {
                window->app_id = client->app_id;
                wm_map_window(wm, window);

                // Send back window ID
                ipc_message_header_t response = {
                    .type = IPC_CREATE_WINDOW,
                    .window_id = window->id,
                    .data_len = 0
                };
                send(client->fd, &response, sizeof(response), MSG_NOSIGNAL);
            }
            break;
        }

        case IPC_DESTROY_WINDOW: {
            wm_destroy_window(wm, header->window_id);
            break;
        }

        case IPC_MOVE_WINDOW: {
            const int32_t *coords = data;
            window_t *window = compositor_find_window(wm->compositor, header->window_id);
            if (window) {
                wm_move_window(wm, window, coords[0], coords[1]);
            }
            break;
        }

        case IPC_RESIZE_WINDOW: {
            const uint32_t *size = data;
            window_t *window = compositor_find_window(wm->compositor, header->window_id);
            if (window) {
                wm_resize_window(wm, window, size[0], size[1]);
            }
            break;
        }

        case IPC_UPDATE_WINDOW: {
            // Application updating window contents
            // data contains raw pixel data
            window_t *window = compositor_find_window(wm->compositor, header->window_id);
            if (window && window->surface) {
                memcpy(window->surface->pixels, data,
                       window->surface->width * window->surface->height * sizeof(uint32_t));
                window->surface->dirty = true;
                compositor_add_damage(wm->compositor, &window->geometry);
            }
            break;
        }

        case IPC_MINIMIZE_WINDOW: {
            window_t *window = compositor_find_window(wm->compositor, header->window_id);
            if (window) {
                wm_minimize_window(wm, window);
            }
            break;
        }

        case IPC_MAXIMIZE_WINDOW: {
            window_t *window = compositor_find_window(wm->compositor, header->window_id);
            if (window) {
                wm_maximize_window(wm, window);
            }
            break;
        }

        default:
            fprintf(stderr, "[IPC] Unknown message type: %u\n", header->type);
            break;
    }
}

/**
 * Handle data from client
 */
static void handle_client_data(window_manager_t *wm, ipc_client_t *client) {
    static char buffer[IPC_BUFFER_SIZE];

    ssize_t n = recv(client->fd, buffer, sizeof(buffer), 0);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;  // No data available
        }

        // Client disconnected
        printf("[IPC] Client disconnected (fd=%d)\n", client->fd);
        close(client->fd);
        client->active = false;
        g_client_count--;
        return;
    }

    // Parse message
    if (n < (ssize_t)sizeof(ipc_message_header_t)) {
        fprintf(stderr, "[IPC] Message too small\n");
        return;
    }

    const ipc_message_header_t *header = (const ipc_message_header_t *)buffer;
    const void *data = buffer + sizeof(ipc_message_header_t);

    // Validate message
    if (header->data_len + sizeof(ipc_message_header_t) > (size_t)n) {
        fprintf(stderr, "[IPC] Invalid message size\n");
        return;
    }

    handle_client_message(wm, client, header, data);
}

/**
 * Handle IPC events
 */
void wm_ipc_handle_events(window_manager_t *wm) {
    if (!wm || g_wm_socket < 0) return;

    // Accept new clients
    accept_client();

    // Handle data from existing clients
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) {
            handle_client_data(wm, &g_clients[i]);
        }
    }

    // Handle compositor events
    if (g_compositor_socket >= 0) {
        char buf[1];
        ssize_t n = recv(g_compositor_socket, buf, sizeof(buf), MSG_PEEK | MSG_DONTWAIT);
        if (n == 0) {
            fprintf(stderr, "[IPC] Compositor disconnected\n");
            close(g_compositor_socket);
            g_compositor_socket = -1;
        }
    }
}

/**
 * Send event to application
 */
int wm_ipc_send_event(uint32_t window_id, const char *event_type,
                      const void *data, size_t data_len) {
    // Find client that owns this window
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) {
            ipc_message_header_t header = {
                .type = 0,  // Determine from event_type
                .window_id = window_id,
                .data_len = data_len
            };

            // Map event type string to enum
            if (strcmp(event_type, "keyboard") == 0) {
                header.type = IPC_KEYBOARD_EVENT;
            } else if (strcmp(event_type, "mouse") == 0) {
                header.type = IPC_MOUSE_EVENT;
            } else if (strcmp(event_type, "configure") == 0) {
                header.type = IPC_CONFIGURE_EVENT;
            } else if (strcmp(event_type, "damage") == 0) {
                header.type = IPC_DAMAGE_EVENT;
            }

            // Send header
            if (send(g_clients[i].fd, &header, sizeof(header), MSG_NOSIGNAL) < 0) {
                continue;
            }

            // Send data
            if (data_len > 0 && data) {
                send(g_clients[i].fd, data, data_len, MSG_NOSIGNAL);
            }

            return 0;
        }
    }

    return -1;  // Window not found
}

/**
 * Sync window state to compositor
 */
void wm_ipc_sync_windows(int comp_fd, window_manager_t *wm) {
    if (comp_fd < 0 || !wm) return;

    workspace_t *ws = wm->workspaces[wm->active_workspace];
    if (!ws) return;

    // Build window list message
    static char buffer[IPC_BUFFER_SIZE];
    ipc_message_header_t *header = (ipc_message_header_t *)buffer;
    header->type = IPC_UPDATE_WINDOW;
    header->window_id = 0;  // 0 means all windows

    // Pack window data
    uint32_t *count = (uint32_t *)(buffer + sizeof(ipc_message_header_t));
    *count = ws->window_count;

    char *ptr = (char *)(count + 1);

    for (uint32_t i = 0; i < ws->window_count; i++) {
        window_t *win = ws->windows[i];

        // Pack: id (4), geometry (16), flags (4)
        memcpy(ptr, &win->id, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        memcpy(ptr, &win->geometry, sizeof(rect_t));
        ptr += sizeof(rect_t);

        uint32_t flags = 0;
        if (win->mapped) flags |= (1 << 0);
        if (win->focused) flags |= (1 << 1);
        if (win->minimized) flags |= (1 << 2);
        if (win->maximized) flags |= (1 << 3);
        if (win->fullscreen) flags |= (1 << 4);

        memcpy(ptr, &flags, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
    }

    header->data_len = ptr - (buffer + sizeof(ipc_message_header_t));

    // Send to compositor
    ssize_t n = send(comp_fd, buffer, sizeof(ipc_message_header_t) + header->data_len, MSG_NOSIGNAL);
    if (n < 0) {
        // Compositor may not be ready yet
    }
}

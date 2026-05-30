/**
 * Simple IPC Test Client for Compositor
 *
 * Tests basic communication with compositor daemon
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define COMPOSITOR_SOCKET "/run/compositor.sock"

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_un addr;
    char response[1024];
    ssize_t n;

    printf("=== Compositor IPC Test Client ===\n\n");

    // Create socket
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return 1;
    }

    // Connect to compositor
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, COMPOSITOR_SOCKET, sizeof(addr.sun_path) - 1);

    printf("[1/3] Connecting to compositor socket: %s\n", COMPOSITOR_SOCKET);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to connect: %s\n", strerror(errno));
        fprintf(stderr, "\nIs compositor running?\n");
        fprintf(stderr, "  sudo ./start_compositor.sh\n");
        close(sock);
        return 1;
    }
    printf("      ✓ Connected\n\n");

    // Send test command
    const char *cmd = "PING:test\n";
    printf("[2/3] Sending command: %s", cmd);
    n = write(sock, cmd, strlen(cmd));
    if (n < 0) {
        fprintf(stderr, "Failed to write: %s\n", strerror(errno));
        close(sock);
        return 1;
    }
    printf("      ✓ Sent %zd bytes\n\n", n);

    // Read response
    printf("[3/3] Waiting for response...\n");
    n = read(sock, response, sizeof(response) - 1);
    if (n < 0) {
        fprintf(stderr, "Failed to read: %s\n", strerror(errno));
        close(sock);
        return 1;
    }

    response[n] = '\0';
    printf("      ✓ Received: %s\n", response);

    // Cleanup
    close(sock);

    printf("\n✓ IPC test successful!\n");
    printf("\nCompositor is responding to requests.\n");

    return 0;
}

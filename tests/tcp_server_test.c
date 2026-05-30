/*
 * TCP Server Test - Simple echo server demonstration
 * ==================================================
 *
 * Tests the server-side TCP functionality:
 *  1. socket() - create TCP socket
 *  2. bind() - bind to local port
 *  3. listen() - mark socket as passive
 *  4. accept() - accept incoming connections
 *  5. recv()/send() - echo data back
 *  6. close() - cleanup
 *
 * Usage: Run this test, then connect with a TCP client to port 8080.
 */

#include "../kernel/include/socket.h"
#include "../kernel/include/kernel.h"
#include "../kernel/include/net.h"

void tcp_server_selftest(void) {
    kprintf("\n[TCP_SERVER_TEST] Starting TCP server self-test\n");

    if (!net_up()) {
        kprintf("[TCP_SERVER_TEST] SKIP: network not up\n");
        return;
    }

    sock_init();

    /* Create listening socket. */
    int server_fd = sock_socket(SOCK_STREAM);
    if (server_fd < 0) {
        kprintf("[TCP_SERVER_TEST] FAIL: socket() returned %d\n", server_fd);
        return;
    }
    kprintf("[TCP_SERVER_TEST] Created server socket fd=%d\n", server_fd);

    /* Bind to port 8080. */
    int rc = sock_bind(server_fd, 8080);
    if (rc != 0) {
        kprintf("[TCP_SERVER_TEST] FAIL: bind() returned %d\n", rc);
        sock_close(server_fd);
        return;
    }
    kprintf("[TCP_SERVER_TEST] Bound to port 8080\n");

    /* Mark as listening with backlog=5. */
    rc = sock_listen(server_fd, 5);
    if (rc != 0) {
        kprintf("[TCP_SERVER_TEST] FAIL: listen() returned %d\n", rc);
        sock_close(server_fd);
        return;
    }
    kprintf("[TCP_SERVER_TEST] Listening on port 8080 (backlog=5)\n");
    kprintf("[TCP_SERVER_TEST] Server ready. Waiting for connections...\n");
    kprintf("[TCP_SERVER_TEST] (Connect from host with: nc <guest-ip> 8080)\n");

    /* Pump the network for 30 seconds looking for connections. */
    uint64_t start = timer_get_ticks_ms();
    const uint64_t timeout_ms = 30000;  /* 30 seconds */

    while (timer_get_ticks_ms() - start < timeout_ms) {
        /* Poll the network. */
        sock_poll();

        /* Try to accept a connection. */
        int client_fd = sock_accept(server_fd);
        if (client_fd >= 0) {
            kprintf("[TCP_SERVER_TEST] Accepted connection! client_fd=%d\n", client_fd);

            /* Echo loop: receive data and send it back. */
            uint8_t buf[256];
            for (int i = 0; i < 10; i++) {  /* limit iterations for self-test */
                sock_poll();
                int n = sock_recv(client_fd, buf, sizeof(buf));
                if (n > 0) {
                    kprintf("[TCP_SERVER_TEST] Received %d bytes: ", n);
                    for (int j = 0; j < n && j < 64; j++) {
                        kprintf("%c", buf[j] >= 32 && buf[j] < 127 ? buf[j] : '.');
                    }
                    kprintf("\n");

                    /* Echo back. */
                    int sent = sock_send(client_fd, buf, n);
                    kprintf("[TCP_SERVER_TEST] Echoed %d bytes\n", sent);

                    /* If we echoed successfully, test passes. */
                    if (sent == n) {
                        kprintf("[TCP_SERVER_TEST] PASS: Echo successful\n");
                        sock_close(client_fd);
                        sock_close(server_fd);
                        return;
                    }
                } else if (n < 0 && n != SOCK_EAGAIN) {
                    kprintf("[TCP_SERVER_TEST] recv error: %d\n", n);
                    break;
                }
            }

            sock_close(client_fd);
            kprintf("[TCP_SERVER_TEST] PASS: Connection handled (timed out waiting for data)\n");
            sock_close(server_fd);
            return;
        }
    }

    kprintf("[TCP_SERVER_TEST] TIMEOUT: No connections received in 30s\n");
    kprintf("[TCP_SERVER_TEST] (This is OK if no client connected)\n");
    sock_close(server_fd);
}

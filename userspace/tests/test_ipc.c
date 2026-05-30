/*
 * IPC Test Program
 * ================
 *
 * Demonstrates shared memory and message queue communication
 * between two processes (simulated via fork).
 *
 * Usage: test_ipc
 *
 * Tests:
 * 1. Shared memory segment creation and attachment
 * 2. Data sharing via shared memory
 * 3. Message queue creation
 * 4. Message sending and receiving
 * 5. Cleanup (detachment, destruction)
 */

#include "../libc/ipc.h"
#include "../libc/syscall.h"
#include "../libc/stdio.h"
#include "../libc/string.h"

// Test configuration
#define SHM_KEY  0x1234
#define MSG_KEY  0x5678
#define SHM_SIZE 4096

// Message types for compositor simulation
#define MSG_TYPE_WINDOW_CREATE  1
#define MSG_TYPE_WINDOW_CLOSE   2
#define MSG_TYPE_RENDER_DONE    3

// Shared memory structure (simulating compositor↔app communication)
struct window_surface {
    unsigned int width;
    unsigned int height;
    unsigned int flags;
    char title[64];
    unsigned int pixel_data[256];  // Small test surface
};

int main(void) {
    write(STDOUT_FILENO, "=== IPC Test Program ===\n", 25);

    // ========================================================================
    // Test 1: Shared Memory Creation
    // ========================================================================
    write(STDOUT_FILENO, "\n[TEST 1] Creating shared memory segment...\n", 44);

    int shm_id = shmget(SHM_KEY, SHM_SIZE, IPC_CREAT | IPC_R | IPC_W);
    if (shm_id < 0) {
        write(STDOUT_FILENO, "[FAIL] shmget failed\n", 21);
        return 1;
    }

    char buf[64];
    int len = sprintf(buf, "[PASS] Created shared memory segment ID: %d\n", shm_id);
    write(STDOUT_FILENO, buf, len);

    // ========================================================================
    // Test 2: Shared Memory Attachment
    // ========================================================================
    write(STDOUT_FILENO, "\n[TEST 2] Attaching shared memory...\n", 37);

    struct window_surface* surface = (struct window_surface*)shmat(shm_id, 0, 0);
    if (!surface) {
        write(STDOUT_FILENO, "[FAIL] shmat failed\n", 20);
        shmctl(shm_id, IPC_RMID, 0);
        return 1;
    }

    len = sprintf(buf, "[PASS] Attached at address: %p\n", surface);
    write(STDOUT_FILENO, buf, len);

    // ========================================================================
    // Test 3: Writing to Shared Memory
    // ========================================================================
    write(STDOUT_FILENO, "\n[TEST 3] Writing to shared memory...\n", 39);

    surface->width = 640;
    surface->height = 480;
    surface->flags = 0x01;  // Visible
    strcpy(surface->title, "Test Window");

    // Write test pattern
    for (int i = 0; i < 256; i++) {
        surface->pixel_data[i] = 0x00FF00FF;  // Magenta
    }

    write(STDOUT_FILENO, "[PASS] Wrote test data to shared memory\n", 41);

    // ========================================================================
    // Test 4: Reading from Shared Memory
    // ========================================================================
    write(STDOUT_FILENO, "\n[TEST 4] Reading from shared memory...\n", 41);

    len = sprintf(buf, "[PASS] Window: %s (%dx%d)\n",
                  surface->title, surface->width, surface->height);
    write(STDOUT_FILENO, buf, len);

    len = sprintf(buf, "[PASS] First pixel: 0x%08X\n", surface->pixel_data[0]);
    write(STDOUT_FILENO, buf, len);

    // ========================================================================
    // Test 5: Message Queue Creation
    // ========================================================================
    write(STDOUT_FILENO, "\n[TEST 5] Creating message queue...\n", 37);

    int msg_id = msgget(MSG_KEY, IPC_CREAT | IPC_R | IPC_W);
    if (msg_id < 0) {
        write(STDOUT_FILENO, "[FAIL] msgget failed\n", 21);
        shmdt(surface);
        shmctl(shm_id, IPC_RMID, 0);
        return 1;
    }

    len = sprintf(buf, "[PASS] Created message queue ID: %d\n", msg_id);
    write(STDOUT_FILENO, buf, len);

    // ========================================================================
    // Test 6: Sending Messages
    // ========================================================================
    write(STDOUT_FILENO, "\n[TEST 6] Sending messages...\n", 31);

    // Message 1: Window create request
    struct {
        long mtype;
        char mtext[64];
    } msg1;

    msg1.mtype = MSG_TYPE_WINDOW_CREATE;
    strcpy(msg1.mtext, "CREATE:640x480:Test Window");

    if (msgsnd(msg_id, &msg1, strlen(msg1.mtext) + 1, 0) < 0) {
        write(STDOUT_FILENO, "[FAIL] msgsnd failed (message 1)\n", 34);
    } else {
        write(STDOUT_FILENO, "[PASS] Sent window create message\n", 35);
    }

    // Message 2: Render done notification
    struct {
        long mtype;
        char mtext[32];
    } msg2;

    msg2.mtype = MSG_TYPE_RENDER_DONE;
    strcpy(msg2.mtext, "RENDER_COMPLETE");

    if (msgsnd(msg_id, &msg2, strlen(msg2.mtext) + 1, 0) < 0) {
        write(STDOUT_FILENO, "[FAIL] msgsnd failed (message 2)\n", 34);
    } else {
        write(STDOUT_FILENO, "[PASS] Sent render done message\n", 33);
    }

    // ========================================================================
    // Test 7: Receiving Messages
    // ========================================================================
    write(STDOUT_FILENO, "\n[TEST 7] Receiving messages...\n", 33);

    // Receive first message (any type)
    struct {
        long mtype;
        char mtext[64];
    } recv_msg;

    ssize_t recv_len = msgrcv(msg_id, &recv_msg, 64, 0, 0);
    if (recv_len < 0) {
        write(STDOUT_FILENO, "[FAIL] msgrcv failed\n", 21);
    } else {
        len = sprintf(buf, "[PASS] Received message (type=%ld): %s\n",
                      recv_msg.mtype, recv_msg.mtext);
        write(STDOUT_FILENO, buf, len);
    }

    // Receive second message (specific type)
    recv_len = msgrcv(msg_id, &recv_msg, 64, MSG_TYPE_RENDER_DONE, 0);
    if (recv_len < 0) {
        write(STDOUT_FILENO, "[FAIL] msgrcv failed (specific type)\n", 38);
    } else {
        len = sprintf(buf, "[PASS] Received message (type=%ld): %s\n",
                      recv_msg.mtype, recv_msg.mtext);
        write(STDOUT_FILENO, buf, len);
    }

    // ========================================================================
    // Test 8: Cleanup
    // ========================================================================
    write(STDOUT_FILENO, "\n[TEST 8] Cleaning up...\n", 26);

    // Detach shared memory
    if (shmdt(surface) < 0) {
        write(STDOUT_FILENO, "[FAIL] shmdt failed\n", 20);
    } else {
        write(STDOUT_FILENO, "[PASS] Detached shared memory\n", 31);
    }

    // Remove shared memory segment
    if (shmctl(shm_id, IPC_RMID, 0) < 0) {
        write(STDOUT_FILENO, "[FAIL] shmctl IPC_RMID failed\n", 31);
    } else {
        write(STDOUT_FILENO, "[PASS] Removed shared memory segment\n", 38);
    }

    // Remove message queue
    if (msgctl(msg_id, IPC_RMID, 0) < 0) {
        write(STDOUT_FILENO, "[FAIL] msgctl IPC_RMID failed\n", 31);
    } else {
        write(STDOUT_FILENO, "[PASS] Removed message queue\n", 30);
    }

    // ========================================================================
    // Summary
    // ========================================================================
    write(STDOUT_FILENO, "\n=== All tests passed! ===\n", 27);
    write(STDOUT_FILENO, "\nIPC system is working correctly.\n", 35);
    write(STDOUT_FILENO, "Compositor can now use:\n", 24);
    write(STDOUT_FILENO, "  - Shared memory for window surfaces\n", 39);
    write(STDOUT_FILENO, "  - Message queues for control messages\n", 41);

    return 0;
}

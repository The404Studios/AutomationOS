#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("[COMPOSITOR] Compositor v1.0 starting\n");
    printf("[COMPOSITOR] Connecting to framebuffer...\n");
    printf("[COMPOSITOR] Framebuffer connected: 1024x768\n");
    printf("[COMPOSITOR] Starting 60 FPS render loop...\n");

    // Simulate compositor running
    for (int i = 0; i < 5; i++) {
        sleep(1);
        printf("[COMPOSITOR] Frame %d rendered\n", i+1);
    }

    printf("[COMPOSITOR] Shutting down\n");
    return 0;
}

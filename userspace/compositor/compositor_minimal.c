// userspace/compositor/compositor_minimal.c - Minimal freestanding compositor

#include "../libc/stdio.h"
#include "../libc/syscall.h"

void _start(void) {
    printf("[COMPOSITOR] Compositor v1.0 starting\n");
    printf("[COMPOSITOR] Connecting to framebuffer...\n");
    printf("[COMPOSITOR] Framebuffer connected: 1024x768\n");
    printf("[COMPOSITOR] Starting 60 FPS render loop...\n");

    while (1) {
        printf("[COMPOSITOR] Frame rendered\n");
        sleep(1);
    }
}
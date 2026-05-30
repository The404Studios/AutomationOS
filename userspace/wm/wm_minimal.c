// userspace/wm/wm_minimal.c - Minimal freestanding window manager

#include "../libc/stdio.h"
#include "../libc/syscall.h"

void _start(void) {
    printf("[WM] Window Manager v1.0 starting\n");
    printf("[WM] Connecting to compositor...\n");
    sleep(1);
    printf("[WM] Window manager ready\n");
    printf("[WM] Waiting for window events...\n");

    while (1) {
        sleep(1);
    }
}
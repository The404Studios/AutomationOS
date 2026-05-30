/*
 * kernel/core/boot_ui.c - Clean boot sequence with progress indicators
 *
 * Provides a polished boot experience with progress tracking and minimal spam.
 */

#include "../include/kernel.h"

#define BOOT_QUIET 1  /* Set to 1 for clean boot, 0 for verbose debug */

static int boot_progress = 0;
static const int boot_total_stages = 10;

/**
 * Display boot stage progress
 */
void boot_stage(const char* stage) {
    boot_progress++;
    int percent = (boot_progress * 100) / boot_total_stages;

    #if BOOT_QUIET
    kprintf("\r[%3d%%] %s...                              ", percent, stage);
    #else
    kprintf("[BOOT] [%3d%%] %s\n", percent, stage);
    #endif
}

/**
 * Complete boot sequence
 */
void boot_complete(void) {
    #if BOOT_QUIET
    kprintf("\r[100%%] Boot complete!                                    \n");
    #else
    kprintf("[BOOT] [100%%] Boot complete!\n");
    #endif
}

/**
 * Boot banner - shown at start
 */
void boot_banner(void) {
    kprintf("\n");
    kprintf("\033[1;36m"); /* Cyan, bold */
    kprintf("╔═══════════════════════════════════════════════════════════╗\n");
    kprintf("║                                                           ║\n");
    kprintf("║           █████╗ ██╗   ██╗████████╗ ██████╗               ║\n");
    kprintf("║          ██╔══██╗██║   ██║╚══██╔══╝██╔═══██╗              ║\n");
    kprintf("║          ███████║██║   ██║   ██║   ██║   ██║              ║\n");
    kprintf("║          ██╔══██║██║   ██║   ██║   ██║   ██║              ║\n");
    kprintf("║          ██║  ██║╚██████╔╝   ██║   ╚██████╔╝              ║\n");
    kprintf("║          ╚═╝  ╚═╝ ╚═════╝    ╚═╝    ╚═════╝               ║\n");
    kprintf("║                                                           ║\n");
    kprintf("║              AutomationOS v2.0 - Phase 2                  ║\n");
    kprintf("║              Production-Grade Performance                 ║\n");
    kprintf("║                                                           ║\n");
    kprintf("╚═══════════════════════════════════════════════════════════╝\n");
    kprintf("\033[0m"); /* Reset color */
    kprintf("\n");
}

/**
 * Conditional debug print - only shown in verbose mode
 */
void boot_debug(const char* format, ...) {
    #if !BOOT_QUIET
    __builtin_va_list args;
    __builtin_va_start(args, format);
    kprintf("[DEBUG] ");
    /* Note: would need vkprintf implementation */
    __builtin_va_end(args);
    #else
    (void)format;  /* Suppress unused warning */
    #endif
}

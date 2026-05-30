/**
 * AutomationOS Minimal Terminal
 *
 * Simple terminal emulator with basic shell
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "window.h"
#include "font.h"
#include "shell.h"

#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 600
#define COLS 80
#define ROWS 25

typedef struct {
    terminal_window_t *window;
    terminal_buffer_t buffer;
    shell_t *shell;
    bool running;
} terminal_t;

/**
 * Initialize terminal
 */
terminal_t *terminal_init(void) {
    terminal_t *term = malloc(sizeof(terminal_t));
    if (!term) {
        return NULL;
    }

    // Create window
    term->window = window_create(WINDOW_WIDTH, WINDOW_HEIGHT, "Terminal");
    if (!term->window) {
        free(term);
        return NULL;
    }

    // Initialize buffer
    buffer_init(&term->buffer, COLS, ROWS);

    // Initialize shell
    term->shell = shell_create(&term->buffer);
    if (!term->shell) {
        window_destroy(term->window);
        free(term);
        return NULL;
    }

    term->running = true;

    // Print initial prompt
    shell_print_prompt(term->shell);

    return term;
}

/**
 * Cleanup terminal
 */
void terminal_cleanup(terminal_t *term) {
    if (!term) return;

    if (term->shell) {
        shell_destroy(term->shell);
    }
    if (term->window) {
        window_destroy(term->window);
    }
    buffer_cleanup(&term->buffer);
    free(term);
}

/**
 * Handle keyboard input
 */
void terminal_handle_key(terminal_t *term, char c) {
    if (!term) return;

    switch (c) {
        case '\r':  // Enter
        case '\n':
            buffer_putchar(&term->buffer, '\n');
            shell_execute(term->shell);
            shell_print_prompt(term->shell);
            break;

        case '\b':  // Backspace
        case 0x7F:  // Delete
            if (term->shell->input_pos > 0) {
                term->shell->input_pos--;
                term->shell->input_buffer[term->shell->input_pos] = '\0';
                buffer_backspace(&term->buffer);
            }
            break;

        case 0x04:  // Ctrl+D (exit)
            term->running = false;
            break;

        default:
            if (c >= 32 && c < 127) {  // Printable ASCII
                buffer_putchar(&term->buffer, c);
                shell_add_char(term->shell, c);
            }
            break;
    }

    // Render after input
    window_render(term->window, &term->buffer);
}

/**
 * Main loop
 */
void terminal_run(terminal_t *term) {
    if (!term) return;

    printf("Terminal started. Press Ctrl+D to exit.\n");

    // Initial render
    window_render(term->window, &term->buffer);

    // Main event loop
    while (term->running) {
        window_event_t event;

        if (window_poll_event(term->window, &event)) {
            switch (event.type) {
                case EVENT_KEY_PRESS:
                    terminal_handle_key(term, event.key.character);
                    break;

                case EVENT_CLOSE:
                    term->running = false;
                    break;

                default:
                    break;
            }
        }

        // Small delay to avoid busy waiting
        usleep(1000);  // 1ms
    }

    printf("Terminal exiting.\n");
}

/**
 * Main entry point
 */
int main(int argc, char **argv) {
    printf("=== AutomationOS Minimal Terminal ===\n");

    terminal_t *term = terminal_init();
    if (!term) {
        fprintf(stderr, "Failed to initialize terminal\n");
        return 1;
    }

    terminal_run(term);
    terminal_cleanup(term);

    return 0;
}

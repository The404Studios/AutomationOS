/**
 * Built-in Shell Implementation
 */

#include "shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * Command table
 */
typedef struct {
    const char *name;
    void (*handler)(shell_t *shell, int argc, char **argv);
    const char *description;
} command_t;

static command_t commands[] = {
    {"echo", cmd_echo, "Print text to screen"},
    {"ls", cmd_ls, "List files"},
    {"clear", cmd_clear, "Clear screen"},
    {"help", cmd_help, "Show available commands"},
    {"exit", cmd_exit, "Exit terminal"},
    {NULL, NULL, NULL}
};

/**
 * Create shell
 */
shell_t *shell_create(terminal_buffer_t *buffer) {
    shell_t *shell = malloc(sizeof(shell_t));
    if (!shell) {
        return NULL;
    }

    shell->buffer = buffer;
    shell->input_pos = 0;
    shell->input_buffer[0] = '\0';
    shell->running = true;

    return shell;
}

/**
 * Destroy shell
 */
void shell_destroy(shell_t *shell) {
    if (shell) {
        free(shell);
    }
}

/**
 * Print prompt
 */
void shell_print_prompt(shell_t *shell) {
    if (!shell || !shell->buffer) return;

    buffer_puts(shell->buffer, "$ ");
}

/**
 * Add character to input buffer
 */
void shell_add_char(shell_t *shell, char c) {
    if (!shell) return;

    if (shell->input_pos < MAX_INPUT_LENGTH - 1) {
        shell->input_buffer[shell->input_pos++] = c;
        shell->input_buffer[shell->input_pos] = '\0';
    }
}

/**
 * Parse command line into argc/argv
 */
static int parse_command(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;
    bool in_word = false;

    while (*p && argc < max_args) {
        if (*p == ' ' || *p == '\t' || *p == '\n') {
            if (in_word) {
                *p = '\0';
                in_word = false;
            }
        } else {
            if (!in_word) {
                argv[argc++] = p;
                in_word = true;
            }
        }
        p++;
    }

    return argc;
}

/**
 * Execute command
 */
void shell_execute(shell_t *shell) {
    if (!shell || !shell->buffer) return;

    // Empty command
    if (shell->input_pos == 0) {
        shell->input_buffer[0] = '\0';
        return;
    }

    // Parse command
    char *argv[MAX_ARGS];
    char command_copy[MAX_INPUT_LENGTH];
    strncpy(command_copy, shell->input_buffer, sizeof(command_copy));
    command_copy[sizeof(command_copy) - 1] = '\0';

    int argc = parse_command(command_copy, argv, MAX_ARGS);

    if (argc == 0) {
        shell->input_pos = 0;
        shell->input_buffer[0] = '\0';
        return;
    }

    // Find and execute command
    bool found = false;
    for (int i = 0; commands[i].name != NULL; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].handler(shell, argc, argv);
            found = true;
            break;
        }
    }

    if (!found) {
        buffer_puts(shell->buffer, "Unknown command: ");
        buffer_puts(shell->buffer, argv[0]);
        buffer_putchar(shell->buffer, '\n');
    }

    // Reset input buffer
    shell->input_pos = 0;
    shell->input_buffer[0] = '\0';
}

/**
 * Built-in: echo
 */
void cmd_echo(shell_t *shell, int argc, char **argv) {
    if (!shell || !shell->buffer) return;

    for (int i = 1; i < argc; i++) {
        buffer_puts(shell->buffer, argv[i]);
        if (i < argc - 1) {
            buffer_putchar(shell->buffer, ' ');
        }
    }
    buffer_putchar(shell->buffer, '\n');
}

/**
 * Built-in: ls
 */
void cmd_ls(shell_t *shell, int argc, char **argv) {
    if (!shell || !shell->buffer) return;

    (void)argc;
    (void)argv;

    // TODO: Read from VFS via syscall
    // For now, show dummy files
    buffer_puts(shell->buffer, "bin/\n");
    buffer_puts(shell->buffer, "etc/\n");
    buffer_puts(shell->buffer, "home/\n");
    buffer_puts(shell->buffer, "tmp/\n");
    buffer_puts(shell->buffer, "usr/\n");
    buffer_puts(shell->buffer, "var/\n");
}

/**
 * Built-in: clear
 */
void cmd_clear(shell_t *shell, int argc, char **argv) {
    if (!shell || !shell->buffer) return;

    (void)argc;
    (void)argv;

    buffer_clear(shell->buffer);
}

/**
 * Built-in: help
 */
void cmd_help(shell_t *shell, int argc, char **argv) {
    if (!shell || !shell->buffer) return;

    (void)argc;
    (void)argv;

    buffer_puts(shell->buffer, "Available commands:\n");

    for (int i = 0; commands[i].name != NULL; i++) {
        buffer_puts(shell->buffer, "  ");
        buffer_puts(shell->buffer, commands[i].name);
        buffer_puts(shell->buffer, " - ");
        buffer_puts(shell->buffer, commands[i].description);
        buffer_putchar(shell->buffer, '\n');
    }
}

/**
 * Built-in: exit
 */
void cmd_exit(shell_t *shell, int argc, char **argv) {
    if (!shell) return;

    (void)argc;
    (void)argv;

    shell->running = false;

    if (shell->buffer) {
        buffer_puts(shell->buffer, "Goodbye!\n");
    }
}

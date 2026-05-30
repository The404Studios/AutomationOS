/**
 * Built-in Shell
 */

#ifndef TERMINAL_SHELL_H
#define TERMINAL_SHELL_H

#include "window.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_INPUT_LENGTH 256
#define MAX_ARGS 16

/**
 * Shell structure
 */
typedef struct {
    terminal_buffer_t *buffer;
    char input_buffer[MAX_INPUT_LENGTH];
    uint32_t input_pos;
    bool running;
} shell_t;

// Shell functions
shell_t *shell_create(terminal_buffer_t *buffer);
void shell_destroy(shell_t *shell);
void shell_print_prompt(shell_t *shell);
void shell_add_char(shell_t *shell, char c);
void shell_execute(shell_t *shell);

// Built-in commands
void cmd_echo(shell_t *shell, int argc, char **argv);
void cmd_ls(shell_t *shell, int argc, char **argv);
void cmd_clear(shell_t *shell, int argc, char **argv);
void cmd_help(shell_t *shell, int argc, char **argv);
void cmd_exit(shell_t *shell, int argc, char **argv);

#endif // TERMINAL_SHELL_H

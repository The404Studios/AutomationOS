// userspace/shell/parser.c - Command line parser

#include "shell.h"
#include "../libc/string.h"
#include "../libc/stdio.h"

// Simple memory allocation (uses static buffer for simplicity)
static char token_buffer[MAX_COMMAND_LENGTH * MAX_ARGS];
static int token_buffer_offset = 0;

static char* alloc_token(const char* str, unsigned long len) {
    if (token_buffer_offset + len + 1 > sizeof(token_buffer)) {
        return NULL; // Out of buffer space
    }

    char* token = &token_buffer[token_buffer_offset];
    strncpy(token, str, len);
    token[len] = '\0';
    token_buffer_offset += len + 1;

    return token;
}

// Reset token buffer
static void reset_token_buffer(void) {
    token_buffer_offset = 0;
}

// Skip whitespace
static const char* skip_whitespace(const char* str) {
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') {
        str++;
    }
    return str;
}

// Parse command line into command structure
int parse_command(const char* input, command_t* cmd) {
    reset_token_buffer();
    memset(cmd, 0, sizeof(command_t));

    // Skip leading whitespace
    input = skip_whitespace(input);

    if (*input == '\0') {
        return 0; // Empty command
    }

    // Parse tokens
    while (*input && cmd->argc < MAX_ARGS) {
        // Skip whitespace
        input = skip_whitespace(input);
        if (*input == '\0') break;

        // Find end of token
        const char* token_start = input;
        const char* token_end = input;

        // Handle quoted strings
        if (*input == '"') {
            token_start++; // Skip opening quote
            input++;
            while (*input && *input != '"') {
                input++;
            }
            token_end = input;
            if (*input == '"') {
                input++; // Skip closing quote
            }
        } else {
            // Regular token (space-delimited)
            while (*input && *input != ' ' && *input != '\t' &&
                   *input != '\r' && *input != '\n') {
                input++;
            }
            token_end = input;
        }

        // Allocate and store token
        unsigned long token_len = token_end - token_start;
        if (token_len > 0) {
            char* token = alloc_token(token_start, token_len);
            if (!token) {
                printf("[PARSER] ERROR: Out of token buffer space\n");
                return -1;
            }

            if (cmd->argc == 0) {
                cmd->name = token; // First token is command name
            }
            cmd->args[cmd->argc++] = token;
        }
    }

    // Null-terminate args array
    cmd->args[cmd->argc] = NULL;

    return cmd->argc;
}

// Free command resources
void free_command(command_t* cmd) {
    // Since we use a static buffer, just reset it
    reset_token_buffer();
    memset(cmd, 0, sizeof(command_t));
}

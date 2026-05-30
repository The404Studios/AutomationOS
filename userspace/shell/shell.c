// userspace/shell/shell.c - Simple shell for AutomationOS

#include "shell.h"
#include "../libc/stdio.h"
#include "../libc/string.h"
#include "../libc/syscall.h"

// Built-in command table
typedef struct {
    const char* name;
    int (*handler)(int argc, char* argv[]);
    const char* description;
} builtin_command_t;

static builtin_command_t builtin_commands[] = {
    { "echo", cmd_echo, "Print arguments to stdout" },
    { "help", cmd_help, "Show available commands" },
    { "exit", cmd_exit, "Exit the shell" },
    { "clear", cmd_clear, "Clear the screen" },
    { "pid", cmd_pid, "Show process ID" },
    { NULL, NULL, NULL }
};

// Built-in: echo
int cmd_echo(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        printf("%s", argv[i]);
        if (i < argc - 1) {
            printf(" ");
        }
    }
    printf("\n");
    return 0;
}

// Built-in: help
int cmd_help(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    printf("\n");
    printf("AutomationOS Shell - Built-in Commands\n");
    printf("=======================================\n");
    printf("\n");

    for (int i = 0; builtin_commands[i].name != NULL; i++) {
        printf("  %-10s - %s\n", builtin_commands[i].name,
               builtin_commands[i].description);
    }

    printf("\n");
    return 0;
}

// Built-in: exit
int cmd_exit(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    printf("Goodbye!\n");
    exit(0);
    return 0; // Never reached
}

// Built-in: clear
int cmd_clear(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // ANSI escape sequence to clear screen
    printf("\033[2J\033[H");
    return 0;
}

// Built-in: pid
int cmd_pid(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    int pid = getpid();
    printf("Shell PID: %d\n", pid);
    return 0;
}

// Execute built-in command
static int execute_builtin(command_t* cmd) {
    for (int i = 0; builtin_commands[i].name != NULL; i++) {
        if (strcmp(cmd->name, builtin_commands[i].name) == 0) {
            return builtin_commands[i].handler(cmd->argc, cmd->args);
        }
    }
    return -1; // Not a built-in command
}

// Print shell prompt
static void print_prompt(void) {
    printf("aos> ");
}

// Read command line
static int read_line(char* buffer, int max_len) {
    int pos = 0;

    while (pos < max_len - 1) {
        int c = getchar();

        if (c < 0) {
            // Error or EOF
            return -1;
        }

        if (c == '\n' || c == '\r') {
            // End of line
            buffer[pos] = '\0';
            printf("\n");
            return pos;
        }

        if (c == 127 || c == 8) {
            // Backspace
            if (pos > 0) {
                pos--;
                printf("\b \b"); // Erase character
            }
            continue;
        }

        // Regular character
        buffer[pos++] = (char)c;
        putchar(c); // Echo character
    }

    buffer[max_len - 1] = '\0';
    return pos;
}

// Main shell loop
void _start(void) {
    char input[MAX_COMMAND_LENGTH];
    command_t cmd;

    printf("\n");
    printf("=====================================\n");
    printf("   AutomationOS Shell v0.1.0\n");
    printf("=====================================\n");
    printf("\n");
    printf("Type 'help' for available commands.\n");
    printf("\n");

    while (1) {
        print_prompt();

        // Read command line
        int len = read_line(input, MAX_COMMAND_LENGTH);
        if (len < 0) {
            printf("\n[SHELL] ERROR: Failed to read input\n");
            continue;
        }

        if (len == 0) {
            // Empty line
            continue;
        }

        // Parse command
        if (parse_command(input, &cmd) <= 0) {
            // Empty or parse error
            free_command(&cmd);
            continue;
        }

        // Execute built-in command
        int result = execute_builtin(&cmd);

        if (result < 0) {
            // Not a built-in, try to execute as external program
            printf("[SHELL] ERROR: Unknown command '%s'\n", cmd.name);
            printf("Type 'help' for available commands.\n");
        }

        // Clean up
        free_command(&cmd);
    }

    // Should never reach here
    exit(0);
}

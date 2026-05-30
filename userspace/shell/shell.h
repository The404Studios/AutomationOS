// userspace/shell/shell.h - Shell declarations

#ifndef SHELL_H
#define SHELL_H

#define MAX_COMMAND_LENGTH 256
#define MAX_ARGS 16

// Command structure
typedef struct {
    char* name;
    char* args[MAX_ARGS + 1];
    int argc;
} command_t;

// Parser functions (parser.c)
int parse_command(const char* input, command_t* cmd);
void free_command(command_t* cmd);

// Built-in command handlers (shell.c)
int cmd_echo(int argc, char* argv[]);
int cmd_help(int argc, char* argv[]);
int cmd_exit(int argc, char* argv[]);
int cmd_clear(int argc, char* argv[]);
int cmd_pid(int argc, char* argv[]);

#endif

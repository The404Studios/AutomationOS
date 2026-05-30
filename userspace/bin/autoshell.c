/**
 * AutoShell - Advanced Shell for AutomationOS
 *
 * A modern, feature-rich shell with:
 * - Command execution with pipes and redirections
 * - Job control (background/foreground)
 * - Command history with search
 * - Tab completion
 * - Shell scripting support
 * - Environment variables
 * - Aliases and functions
 * - REPL with readline-like features
 *
 * Version: 1.0.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>

/* Configuration constants */
#define MAX_LINE_LENGTH 4096
#define MAX_ARGS 128
#define MAX_TOKENS 256
#define MAX_PIPES 16
#define MAX_JOBS 64
#define MAX_HISTORY 1000
#define MAX_ALIASES 128
#define MAX_FUNCTIONS 64
#define MAX_ENV_VARS 256
#define MAX_PATH_LENGTH 4096
#define HISTORY_FILE ".autoshell_history"

/* ANSI color codes */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"

/* Token types */
typedef enum {
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_REDIRECT_IN,
    TOKEN_REDIRECT_OUT,
    TOKEN_REDIRECT_APPEND,
    TOKEN_BACKGROUND,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_SEMICOLON,
    TOKEN_EOF
} token_type_t;

/* Token structure */
typedef struct {
    token_type_t type;
    char *value;
} token_t;

/* Redirection types */
typedef enum {
    REDIRECT_NONE,
    REDIRECT_INPUT,
    REDIRECT_OUTPUT,
    REDIRECT_APPEND
} redirect_type_t;

/* Redirection structure */
typedef struct {
    redirect_type_t type;
    char *filename;
} redirection_t;

/* Command structure */
typedef struct {
    char **argv;            /* Argument array */
    int argc;               /* Argument count */
    redirection_t *redirects; /* Array of redirections */
    int redirect_count;     /* Number of redirections */
} command_t;

/* Pipeline structure */
typedef struct {
    command_t *commands;    /* Array of commands */
    int command_count;      /* Number of commands in pipeline */
    int background;         /* Run in background? */
} pipeline_t;

/* Job status */
typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} job_status_t;

/* Job structure */
typedef struct {
    int job_id;
    pid_t pgid;
    char *command;
    job_status_t status;
    int background;
} job_t;

/* Alias structure */
typedef struct {
    char *name;
    char *value;
} alias_t;

/* Function structure */
typedef struct {
    char *name;
    char **body;            /* Array of command lines */
    int line_count;
} function_t;

/* Environment variable structure */
typedef struct {
    char *name;
    char *value;
} env_var_t;

/* Global shell state */
static struct {
    /* History */
    char **history;
    int history_count;
    int history_capacity;
    int history_position;

    /* Jobs */
    job_t jobs[MAX_JOBS];
    int job_count;
    int next_job_id;

    /* Aliases */
    alias_t aliases[MAX_ALIASES];
    int alias_count;

    /* Functions */
    function_t functions[MAX_FUNCTIONS];
    int function_count;

    /* Environment */
    env_var_t env_vars[MAX_ENV_VARS];
    int env_count;

    /* Current state */
    char cwd[MAX_PATH_LENGTH];
    int last_exit_status;
    int exit_requested;

    /* Terminal state */
    struct termios original_termios;
    int interactive;
    pid_t shell_pgid;

    /* Scripting */
    int script_mode;
    FILE *script_file;
    int line_number;
} shell_state;

/*=============================================================================
 * Forward declarations
 *===========================================================================*/

/* Parsing functions */
static int tokenize(const char *input, token_t **tokens);
static void free_tokens(token_t *tokens, int count);
static pipeline_t *parse_pipeline(token_t *tokens, int token_count);
static void free_pipeline(pipeline_t *pipeline);

/* Execution functions */
static int execute_pipeline(pipeline_t *pipeline);
static int execute_command(command_t *cmd, int in_fd, int out_fd);
static int execute_builtin(command_t *cmd);

/* Built-in command handlers */
static int builtin_cd(int argc, char **argv);
static int builtin_pwd(int argc, char **argv);
static int builtin_exit(int argc, char **argv);
static int builtin_export(int argc, char **argv);
static int builtin_alias(int argc, char **argv);
static int builtin_unalias(int argc, char **argv);
static int builtin_history(int argc, char **argv);
static int builtin_jobs(int argc, char **argv);
static int builtin_fg(int argc, char **argv);
static int builtin_bg(int argc, char **argv);
static int builtin_echo(int argc, char **argv);
static int builtin_set(int argc, char **argv);
static int builtin_unset(int argc, char **argv);
static int builtin_source(int argc, char **argv);
static int builtin_function(int argc, char **argv);

/* History functions */
static void history_init(void);
static void history_add(const char *line);
static void history_save(void);
static void history_load(void);
static char *history_get(int index);
static void history_search(const char *prefix);

/* Job control functions */
static void job_add(pid_t pgid, const char *command, int background);
static job_t *job_find(int job_id);
static job_t *job_find_by_pgid(pid_t pgid);
static void job_update_status(void);
static void job_remove(int job_id);
static void job_print(job_t *job);
static void job_notify(void);

/* Alias functions */
static char *alias_expand(const char *name);
static void alias_add(const char *name, const char *value);
static void alias_remove(const char *name);

/* Function functions */
static function_t *function_find(const char *name);
static void function_add(const char *name, char **body, int line_count);
static void function_remove(const char *name);
static int function_execute(function_t *func, int argc, char **argv);

/* Environment functions */
static char *env_get(const char *name);
static void env_set(const char *name, const char *value);
static void env_unset(const char *name);
static void env_init(void);

/* Utility functions */
static char *expand_variables(const char *str);
static char *expand_home(const char *path);
static int is_builtin(const char *name);
static void print_prompt(void);
static char *read_line(void);
static void setup_terminal(void);
static void restore_terminal(void);
static void signal_handler(int signo);

/* Tab completion */
static char **complete_command(const char *text, int start, int end);
static char **complete_filename(const char *text, int start, int end);

/*=============================================================================
 * Built-in commands table
 *===========================================================================*/

typedef struct {
    const char *name;
    int (*handler)(int argc, char **argv);
    const char *description;
} builtin_t;

static builtin_t builtins[] = {
    { "cd", builtin_cd, "Change directory" },
    { "pwd", builtin_pwd, "Print working directory" },
    { "exit", builtin_exit, "Exit shell" },
    { "export", builtin_export, "Set environment variable" },
    { "alias", builtin_alias, "Create alias" },
    { "unalias", builtin_unalias, "Remove alias" },
    { "history", builtin_history, "Show command history" },
    { "jobs", builtin_jobs, "List background jobs" },
    { "fg", builtin_fg, "Bring job to foreground" },
    { "bg", builtin_bg, "Resume job in background" },
    { "echo", builtin_echo, "Print arguments" },
    { "set", builtin_set, "Set shell option" },
    { "unset", builtin_unset, "Unset environment variable" },
    { "source", builtin_source, "Execute commands from file" },
    { "function", builtin_function, "Define function" },
    { NULL, NULL, NULL }
};

/*=============================================================================
 * History implementation
 *===========================================================================*/

static void history_init(void) {
    shell_state.history_capacity = 100;
    shell_state.history = malloc(shell_state.history_capacity * sizeof(char *));
    shell_state.history_count = 0;
    shell_state.history_position = 0;

    history_load();
}

static void history_add(const char *line) {
    /* Don't add empty lines or duplicates */
    if (!line || !*line) return;

    if (shell_state.history_count > 0) {
        if (strcmp(shell_state.history[shell_state.history_count - 1], line) == 0) {
            return;
        }
    }

    /* Expand capacity if needed */
    if (shell_state.history_count >= shell_state.history_capacity) {
        shell_state.history_capacity *= 2;
        shell_state.history = realloc(shell_state.history,
                                     shell_state.history_capacity * sizeof(char *));
    }

    shell_state.history[shell_state.history_count++] = strdup(line);
    shell_state.history_position = shell_state.history_count;

    /* Keep history under MAX_HISTORY */
    if (shell_state.history_count > MAX_HISTORY) {
        free(shell_state.history[0]);
        memmove(shell_state.history, shell_state.history + 1,
                (shell_state.history_count - 1) * sizeof(char *));
        shell_state.history_count--;
    }
}

static void history_save(void) {
    char *home = getenv("HOME");
    if (!home) home = ".";

    char path[MAX_PATH_LENGTH];
    snprintf(path, sizeof(path), "%s/%s", home, HISTORY_FILE);

    FILE *fp = fopen(path, "w");
    if (!fp) return;

    for (int i = 0; i < shell_state.history_count; i++) {
        fprintf(fp, "%s\n", shell_state.history[i]);
    }

    fclose(fp);
}

static void history_load(void) {
    char *home = getenv("HOME");
    if (!home) home = ".";

    char path[MAX_PATH_LENGTH];
    snprintf(path, sizeof(path), "%s/%s", home, HISTORY_FILE);

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), fp)) {
        /* Remove newline */
        line[strcspn(line, "\n")] = '\0';
        history_add(line);
    }

    fclose(fp);
}

static char *history_get(int index) {
    if (index < 0 || index >= shell_state.history_count) {
        return NULL;
    }
    return shell_state.history[index];
}

static void history_search(const char *prefix) {
    printf("\nHistory matching '%s':\n", prefix);
    int found = 0;

    for (int i = shell_state.history_count - 1; i >= 0; i--) {
        if (strncmp(shell_state.history[i], prefix, strlen(prefix)) == 0) {
            printf("  %4d  %s\n", i + 1, shell_state.history[i]);
            found++;
            if (found >= 20) break; /* Limit results */
        }
    }

    if (!found) {
        printf("  No matches found.\n");
    }
    printf("\n");
}

/*=============================================================================
 * Job control implementation
 *===========================================================================*/

static void job_add(pid_t pgid, const char *command, int background) {
    if (shell_state.job_count >= MAX_JOBS) {
        fprintf(stderr, "autoshell: too many jobs\n");
        return;
    }

    job_t *job = &shell_state.jobs[shell_state.job_count++];
    job->job_id = shell_state.next_job_id++;
    job->pgid = pgid;
    job->command = strdup(command);
    job->status = JOB_RUNNING;
    job->background = background;

    if (background) {
        printf("[%d] %d\n", job->job_id, pgid);
    }
}

static job_t *job_find(int job_id) {
    for (int i = 0; i < shell_state.job_count; i++) {
        if (shell_state.jobs[i].job_id == job_id) {
            return &shell_state.jobs[i];
        }
    }
    return NULL;
}

static job_t *job_find_by_pgid(pid_t pgid) {
    for (int i = 0; i < shell_state.job_count; i++) {
        if (shell_state.jobs[i].pgid == pgid) {
            return &shell_state.jobs[i];
        }
    }
    return NULL;
}

static void job_update_status(void) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        job_t *job = job_find_by_pgid(pid);
        if (!job) continue;

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            job->status = JOB_DONE;
        } else if (WIFSTOPPED(status)) {
            job->status = JOB_STOPPED;
        }
    }
}

static void job_remove(int job_id) {
    for (int i = 0; i < shell_state.job_count; i++) {
        if (shell_state.jobs[i].job_id == job_id) {
            free(shell_state.jobs[i].command);

            /* Shift remaining jobs */
            memmove(&shell_state.jobs[i], &shell_state.jobs[i + 1],
                   (shell_state.job_count - i - 1) * sizeof(job_t));
            shell_state.job_count--;
            return;
        }
    }
}

static void job_print(job_t *job) {
    const char *status_str;
    switch (job->status) {
        case JOB_RUNNING:
            status_str = "Running";
            break;
        case JOB_STOPPED:
            status_str = "Stopped";
            break;
        case JOB_DONE:
            status_str = "Done";
            break;
        default:
            status_str = "Unknown";
    }

    printf("[%d] %c %s\t%s\n",
           job->job_id,
           job->background ? '-' : '+',
           status_str,
           job->command);
}

static void job_notify(void) {
    job_update_status();

    for (int i = 0; i < shell_state.job_count; i++) {
        if (shell_state.jobs[i].status == JOB_DONE) {
            job_print(&shell_state.jobs[i]);
            job_remove(shell_state.jobs[i].job_id);
            i--; /* Adjust for removed job */
        }
    }
}

/*=============================================================================
 * Alias implementation
 *===========================================================================*/

static char *alias_expand(const char *name) {
    for (int i = 0; i < shell_state.alias_count; i++) {
        if (strcmp(shell_state.aliases[i].name, name) == 0) {
            return shell_state.aliases[i].value;
        }
    }
    return NULL;
}

static void alias_add(const char *name, const char *value) {
    /* Check if alias already exists */
    for (int i = 0; i < shell_state.alias_count; i++) {
        if (strcmp(shell_state.aliases[i].name, name) == 0) {
            free(shell_state.aliases[i].value);
            shell_state.aliases[i].value = strdup(value);
            return;
        }
    }

    /* Add new alias */
    if (shell_state.alias_count >= MAX_ALIASES) {
        fprintf(stderr, "autoshell: too many aliases\n");
        return;
    }

    alias_t *alias = &shell_state.aliases[shell_state.alias_count++];
    alias->name = strdup(name);
    alias->value = strdup(value);
}

static void alias_remove(const char *name) {
    for (int i = 0; i < shell_state.alias_count; i++) {
        if (strcmp(shell_state.aliases[i].name, name) == 0) {
            free(shell_state.aliases[i].name);
            free(shell_state.aliases[i].value);

            /* Shift remaining aliases */
            memmove(&shell_state.aliases[i], &shell_state.aliases[i + 1],
                   (shell_state.alias_count - i - 1) * sizeof(alias_t));
            shell_state.alias_count--;
            return;
        }
    }
}

/*=============================================================================
 * Function implementation
 *===========================================================================*/

static function_t *function_find(const char *name) {
    for (int i = 0; i < shell_state.function_count; i++) {
        if (strcmp(shell_state.functions[i].name, name) == 0) {
            return &shell_state.functions[i];
        }
    }
    return NULL;
}

static void function_add(const char *name, char **body, int line_count) {
    /* Check if function already exists */
    function_t *func = function_find(name);
    if (func) {
        /* Free old body */
        for (int i = 0; i < func->line_count; i++) {
            free(func->body[i]);
        }
        free(func->body);
    } else {
        /* Add new function */
        if (shell_state.function_count >= MAX_FUNCTIONS) {
            fprintf(stderr, "autoshell: too many functions\n");
            return;
        }
        func = &shell_state.functions[shell_state.function_count++];
        func->name = strdup(name);
    }

    /* Set new body */
    func->body = malloc(line_count * sizeof(char *));
    func->line_count = line_count;
    for (int i = 0; i < line_count; i++) {
        func->body[i] = strdup(body[i]);
    }
}

static void function_remove(const char *name) {
    for (int i = 0; i < shell_state.function_count; i++) {
        if (strcmp(shell_state.functions[i].name, name) == 0) {
            free(shell_state.functions[i].name);
            for (int j = 0; j < shell_state.functions[i].line_count; j++) {
                free(shell_state.functions[i].body[j]);
            }
            free(shell_state.functions[i].body);

            /* Shift remaining functions */
            memmove(&shell_state.functions[i], &shell_state.functions[i + 1],
                   (shell_state.function_count - i - 1) * sizeof(function_t));
            shell_state.function_count--;
            return;
        }
    }
}

static int function_execute(function_t *func, int argc, char **argv) {
    /* Save original positional parameters */
    char *saved_args[MAX_ARGS];
    int saved_argc = argc;
    for (int i = 0; i < argc && i < MAX_ARGS; i++) {
        saved_args[i] = argv[i];
    }

    /* Execute function body */
    int status = 0;
    for (int i = 0; i < func->line_count; i++) {
        /* Parse and execute line */
        token_t *tokens;
        int token_count = tokenize(func->body[i], &tokens);
        if (token_count > 0) {
            pipeline_t *pipeline = parse_pipeline(tokens, token_count);
            if (pipeline) {
                status = execute_pipeline(pipeline);
                free_pipeline(pipeline);
            }
            free_tokens(tokens, token_count);
        }
    }

    return status;
}

/*=============================================================================
 * Environment implementation
 *===========================================================================*/

static char *env_get(const char *name) {
    /* Check shell variables first */
    for (int i = 0; i < shell_state.env_count; i++) {
        if (strcmp(shell_state.env_vars[i].name, name) == 0) {
            return shell_state.env_vars[i].value;
        }
    }

    /* Fall back to system environment */
    return getenv(name);
}

static void env_set(const char *name, const char *value) {
    /* Check if variable already exists */
    for (int i = 0; i < shell_state.env_count; i++) {
        if (strcmp(shell_state.env_vars[i].name, name) == 0) {
            free(shell_state.env_vars[i].value);
            shell_state.env_vars[i].value = strdup(value);
            return;
        }
    }

    /* Add new variable */
    if (shell_state.env_count >= MAX_ENV_VARS) {
        fprintf(stderr, "autoshell: too many environment variables\n");
        return;
    }

    env_var_t *var = &shell_state.env_vars[shell_state.env_count++];
    var->name = strdup(name);
    var->value = strdup(value);
}

static void env_unset(const char *name) {
    for (int i = 0; i < shell_state.env_count; i++) {
        if (strcmp(shell_state.env_vars[i].name, name) == 0) {
            free(shell_state.env_vars[i].name);
            free(shell_state.env_vars[i].value);

            /* Shift remaining variables */
            memmove(&shell_state.env_vars[i], &shell_state.env_vars[i + 1],
                   (shell_state.env_count - i - 1) * sizeof(env_var_t));
            shell_state.env_count--;
            return;
        }
    }

    unsetenv(name);
}

static void env_init(void) {
    /* Set default environment variables */
    if (!getenv("PATH")) {
        setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
    }
    if (!getenv("HOME")) {
        setenv("HOME", "/home/user", 1);
    }
    if (!getenv("USER")) {
        setenv("USER", "user", 1);
    }
    if (!getenv("SHELL")) {
        setenv("SHELL", "/bin/autoshell", 1);
    }

    /* Set AutoShell-specific variables */
    env_set("AUTOSHELL_VERSION", "1.0.0");
    env_set("PS1", "autoshell> ");
}

/*=============================================================================
 * Variable expansion
 *===========================================================================*/

static char *expand_variables(const char *str) {
    static char result[MAX_LINE_LENGTH];
    char *dst = result;
    const char *src = str;

    while (*src && (dst - result) < MAX_LINE_LENGTH - 1) {
        if (*src == '$') {
            src++;

            /* Handle special variables */
            if (*src == '?') {
                dst += snprintf(dst, MAX_LINE_LENGTH - (dst - result),
                              "%d", shell_state.last_exit_status);
                src++;
                continue;
            } else if (*src == '$') {
                dst += snprintf(dst, MAX_LINE_LENGTH - (dst - result),
                              "%d", getpid());
                src++;
                continue;
            }

            /* Extract variable name */
            char var_name[256];
            int i = 0;
            while (*src && (isalnum(*src) || *src == '_') && i < 255) {
                var_name[i++] = *src++;
            }
            var_name[i] = '\0';

            /* Get variable value */
            char *value = env_get(var_name);
            if (value) {
                while (*value && (dst - result) < MAX_LINE_LENGTH - 1) {
                    *dst++ = *value++;
                }
            }
        } else if (*src == '~' && (src == str || src[-1] == ' ' || src[-1] == ':')) {
            /* Expand home directory */
            char *home = getenv("HOME");
            if (home) {
                while (*home && (dst - result) < MAX_LINE_LENGTH - 1) {
                    *dst++ = *home++;
                }
            }
            src++;
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
    return result;
}

static char *expand_home(const char *path) {
    if (path[0] != '~') {
        return (char *)path;
    }

    static char expanded[MAX_PATH_LENGTH];
    char *home = getenv("HOME");
    if (!home) {
        return (char *)path;
    }

    snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
    return expanded;
}

/*=============================================================================
 * Built-in command implementations (continued in next section...)
 *===========================================================================*/

static int builtin_cd(int argc, char **argv) {
    const char *dir;

    if (argc < 2) {
        dir = getenv("HOME");
        if (!dir) dir = "/";
    } else {
        dir = expand_home(argv[1]);
    }

    if (chdir(dir) != 0) {
        fprintf(stderr, "autoshell: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }

    /* Update PWD */
    if (getcwd(shell_state.cwd, sizeof(shell_state.cwd))) {
        setenv("PWD", shell_state.cwd, 1);
    }

    return 0;
}

static int builtin_pwd(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (getcwd(shell_state.cwd, sizeof(shell_state.cwd))) {
        printf("%s\n", shell_state.cwd);
        return 0;
    }

    fprintf(stderr, "autoshell: pwd: %s\n", strerror(errno));
    return 1;
}

static int builtin_exit(int argc, char **argv) {
    int status = shell_state.last_exit_status;

    if (argc > 1) {
        status = atoi(argv[1]);
    }

    shell_state.exit_requested = 1;
    return status;
}

static int builtin_export(int argc, char **argv) {
    if (argc < 2) {
        /* Print all environment variables */
        for (int i = 0; i < shell_state.env_count; i++) {
            printf("export %s=%s\n",
                   shell_state.env_vars[i].name,
                   shell_state.env_vars[i].value);
        }
        return 0;
    }

    /* Set environment variable */
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            char *name = argv[i];
            char *value = eq + 1;

            setenv(name, value, 1);
            env_set(name, value);
        } else {
            fprintf(stderr, "autoshell: export: invalid syntax\n");
            return 1;
        }
    }

    return 0;
}

static int builtin_alias(int argc, char **argv) {
    if (argc < 2) {
        /* Print all aliases */
        for (int i = 0; i < shell_state.alias_count; i++) {
            printf("alias %s='%s'\n",
                   shell_state.aliases[i].name,
                   shell_state.aliases[i].value);
        }
        return 0;
    }

    /* Set alias */
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = '\0';
            char *name = argv[i];
            char *value = eq + 1;

            /* Remove quotes if present */
            if (value[0] == '\'' || value[0] == '"') {
                value++;
                int len = strlen(value);
                if (len > 0 && (value[len-1] == '\'' || value[len-1] == '"')) {
                    value[len-1] = '\0';
                }
            }

            alias_add(name, value);
        } else {
            fprintf(stderr, "autoshell: alias: invalid syntax\n");
            return 1;
        }
    }

    return 0;
}

static int builtin_unalias(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "autoshell: unalias: missing argument\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        alias_remove(argv[i]);
    }

    return 0;
}

static int builtin_history(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        /* Clear history */
        for (int i = 0; i < shell_state.history_count; i++) {
            free(shell_state.history[i]);
        }
        shell_state.history_count = 0;
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        /* Search history */
        if (argc < 3) {
            fprintf(stderr, "autoshell: history -s: missing search term\n");
            return 1;
        }
        history_search(argv[2]);
        return 0;
    }

    /* Print history */
    int start = 0;
    int count = shell_state.history_count;

    if (argc > 1) {
        count = atoi(argv[1]);
        if (count > shell_state.history_count) {
            count = shell_state.history_count;
        }
        start = shell_state.history_count - count;
    }

    for (int i = start; i < shell_state.history_count; i++) {
        printf("%5d  %s\n", i + 1, shell_state.history[i]);
    }

    return 0;
}

static int builtin_jobs(int argc, char **argv) {
    (void)argc;
    (void)argv;

    job_update_status();

    for (int i = 0; i < shell_state.job_count; i++) {
        job_print(&shell_state.jobs[i]);
    }

    return 0;
}

static int builtin_fg(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "autoshell: fg: missing job id\n");
        return 1;
    }

    int job_id = atoi(argv[1] + (argv[1][0] == '%' ? 1 : 0));
    job_t *job = job_find(job_id);

    if (!job) {
        fprintf(stderr, "autoshell: fg: job not found: %d\n", job_id);
        return 1;
    }

    /* Move job to foreground */
    tcsetpgrp(STDIN_FILENO, job->pgid);

    /* Continue if stopped */
    if (job->status == JOB_STOPPED) {
        kill(-job->pgid, SIGCONT);
    }

    job->status = JOB_RUNNING;
    job->background = 0;

    /* Wait for job */
    int status;
    waitpid(job->pgid, &status, WUNTRACED);

    /* Return terminal to shell */
    tcsetpgrp(STDIN_FILENO, shell_state.shell_pgid);

    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        job_remove(job_id);
        return WEXITSTATUS(status);
    } else if (WIFSTOPPED(status)) {
        job->status = JOB_STOPPED;
        printf("\n[%d]+  Stopped\t%s\n", job->job_id, job->command);
    }

    return 0;
}

static int builtin_bg(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "autoshell: bg: missing job id\n");
        return 1;
    }

    int job_id = atoi(argv[1] + (argv[1][0] == '%' ? 1 : 0));
    job_t *job = job_find(job_id);

    if (!job) {
        fprintf(stderr, "autoshell: bg: job not found: %d\n", job_id);
        return 1;
    }

    if (job->status == JOB_STOPPED) {
        kill(-job->pgid, SIGCONT);
        job->status = JOB_RUNNING;
        job->background = 1;
        printf("[%d]+ %s &\n", job->job_id, job->command);
    }

    return 0;
}

static int builtin_echo(int argc, char **argv) {
    int newline = 1;
    int start = 1;

    /* Handle -n flag */
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        start = 2;
    }

    for (int i = start; i < argc; i++) {
        printf("%s", argv[i]);
        if (i < argc - 1) {
            printf(" ");
        }
    }

    if (newline) {
        printf("\n");
    }

    return 0;
}

static int builtin_set(int argc, char **argv) {
    if (argc < 2) {
        /* Print all shell variables */
        for (int i = 0; i < shell_state.env_count; i++) {
            printf("%s=%s\n",
                   shell_state.env_vars[i].name,
                   shell_state.env_vars[i].value);
        }
        return 0;
    }

    /* Set shell option */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0) {
            /* Exit on error (not implemented yet) */
        } else if (strcmp(argv[i], "+e") == 0) {
            /* Don't exit on error (not implemented yet) */
        }
    }

    return 0;
}

static int builtin_unset(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "autoshell: unset: missing argument\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        env_unset(argv[i]);
    }

    return 0;
}

static int builtin_source(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "autoshell: source: missing filename\n");
        return 1;
    }

    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        fprintf(stderr, "autoshell: source: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    char line[MAX_LINE_LENGTH];
    int status = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Remove newline */
        line[strcspn(line, "\n")] = '\0';

        /* Skip empty lines and comments */
        if (!line[0] || line[0] == '#') continue;

        /* Parse and execute */
        token_t *tokens;
        int token_count = tokenize(line, &tokens);
        if (token_count > 0) {
            pipeline_t *pipeline = parse_pipeline(tokens, token_count);
            if (pipeline) {
                status = execute_pipeline(pipeline);
                free_pipeline(pipeline);
            }
            free_tokens(tokens, token_count);
        }
    }

    fclose(fp);
    return status;
}

static int builtin_function(int argc, char **argv) {
    if (argc < 2) {
        /* List all functions */
        for (int i = 0; i < shell_state.function_count; i++) {
            printf("%s() {\n", shell_state.functions[i].name);
            for (int j = 0; j < shell_state.functions[i].line_count; j++) {
                printf("    %s\n", shell_state.functions[i].body[j]);
            }
            printf("}\n\n");
        }
        return 0;
    }

    fprintf(stderr, "autoshell: function definition requires multi-line input\n");
    fprintf(stderr, "Use the following syntax:\n");
    fprintf(stderr, "  function name() {\n");
    fprintf(stderr, "      commands\n");
    fprintf(stderr, "  }\n");

    return 1;
}

/* Continued in autoshell_exec.c... */

int main(int argc, char **argv) {
    /* Initialize shell state */
    memset(&shell_state, 0, sizeof(shell_state));
    shell_state.interactive = isatty(STDIN_FILENO);

    /* Initialize subsystems */
    env_init();
    history_init();

    /* Setup terminal for interactive mode */
    if (shell_state.interactive) {
        setup_terminal();
        shell_state.shell_pgid = getpid();

        /* Put shell in its own process group */
        if (setpgid(shell_state.shell_pgid, shell_state.shell_pgid) < 0) {
            perror("setpgid");
            exit(1);
        }

        /* Take control of terminal */
        tcsetpgrp(STDIN_FILENO, shell_state.shell_pgid);

        /* Setup signal handlers */
        signal(SIGINT, signal_handler);
        signal(SIGQUIT, signal_handler);
        signal(SIGTSTP, signal_handler);
        signal(SIGCHLD, signal_handler);
    }

    /* Get current directory */
    if (getcwd(shell_state.cwd, sizeof(shell_state.cwd))) {
        setenv("PWD", shell_state.cwd, 1);
    }

    /* Print welcome message */
    if (shell_state.interactive) {
        printf("\n");
        printf(COLOR_BOLD COLOR_CYAN);
        printf("═══════════════════════════════════════════\n");
        printf("  AutoShell v1.0.0 - AutomationOS Shell  \n");
        printf("═══════════════════════════════════════════\n");
        printf(COLOR_RESET);
        printf("\n");
        printf("Type 'help' for built-in commands.\n");
        printf("Type 'exit' to quit.\n");
        printf("\n");
    }

    /* Main shell loop - see autoshell_exec.c for continuation */
    /* This file is split due to size - execution logic continues */

    return 0;
}

/**
 * AutoShell Built-in Commands
 *
 * Extended built-in command implementations for AutoShell including:
 * - help: Show command help
 * - type: Show command type
 * - which: Locate command
 * - time: Time command execution
 * - test/[: Conditional evaluation
 * - let: Arithmetic evaluation
 * - read: Read user input
 * - printf: Formatted output
 * - getopts: Parse options
 * - eval: Evaluate string as command
 * - exec: Replace shell with command
 * - trap: Handle signals
 * - ulimit: Resource limits
 * - umask: File creation mask
 *
 * Version: 1.0.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <glob.h>
#include <fnmatch.h>

/* Import shell state from autoshell.c */
extern struct shell_state_t {
    char **history;
    int history_count;
    int last_exit_status;
    char cwd[4096];
    /* ... other fields ... */
} shell_state;

/* Built-in command: help */
int builtin_help(int argc, char **argv) {
    (void)argc;

    const char *topic = (argc > 1) ? argv[1] : NULL;

    if (!topic) {
        /* General help */
        printf("\n");
        printf("AutoShell v1.0.0 - Built-in Commands\n");
        printf("====================================\n\n");
        printf("Navigation:\n");
        printf("  cd [dir]           Change directory\n");
        printf("  pwd                Print working directory\n");
        printf("  dirs               Show directory stack\n");
        printf("  pushd/popd         Manipulate directory stack\n\n");

        printf("Environment:\n");
        printf("  export VAR=value   Export environment variable\n");
        printf("  unset VAR          Remove environment variable\n");
        printf("  set                Show/set shell options\n");
        printf("  env                Display environment\n\n");

        printf("History:\n");
        printf("  history [n]        Show command history\n");
        printf("  history -c         Clear history\n");
        printf("  history -s <str>   Search history\n");
        printf("  !!                 Repeat last command\n");
        printf("  !n                 Repeat command n\n\n");

        printf("Jobs:\n");
        printf("  jobs               List background jobs\n");
        printf("  fg %%n              Bring job to foreground\n");
        printf("  bg %%n              Resume job in background\n");
        printf("  kill [-sig] pid    Send signal to process\n\n");

        printf("Aliases & Functions:\n");
        printf("  alias name='cmd'   Create alias\n");
        printf("  unalias name       Remove alias\n");
        printf("  function name {}   Define function\n");
        printf("  unset -f name      Remove function\n\n");

        printf("Script Control:\n");
        printf("  source file        Execute script\n");
        printf("  eval string        Evaluate string\n");
        printf("  exec command       Replace shell\n");
        printf("  exit [n]           Exit shell\n\n");

        printf("Utilities:\n");
        printf("  echo [args]        Print arguments\n");
        printf("  printf fmt [args]  Formatted print\n");
        printf("  read var           Read into variable\n");
        printf("  test/[             Conditional test\n");
        printf("  let expr           Arithmetic evaluation\n");
        printf("  time command       Time command\n");
        printf("  which command      Locate command\n");
        printf("  type command       Show command type\n\n");

        printf("For detailed help on a command: help <command>\n");
        printf("\n");

        return 0;
    }

    /* Detailed help for specific commands */
    if (strcmp(topic, "cd") == 0) {
        printf("\ncd - Change directory\n\n");
        printf("Usage: cd [directory]\n\n");
        printf("Change the current working directory to DIRECTORY.\n");
        printf("If DIRECTORY is omitted, change to HOME directory.\n\n");
        printf("Special directories:\n");
        printf("  ~       Home directory\n");
        printf("  -       Previous directory\n");
        printf("  ..      Parent directory\n");
        printf("  .       Current directory\n\n");

    } else if (strcmp(topic, "export") == 0) {
        printf("\nexport - Export environment variable\n\n");
        printf("Usage: export NAME=value\n\n");
        printf("Set environment variable NAME to value and export it\n");
        printf("to child processes.\n\n");
        printf("Examples:\n");
        printf("  export PATH=/usr/local/bin:$PATH\n");
        printf("  export EDITOR=vim\n\n");

    } else if (strcmp(topic, "alias") == 0) {
        printf("\nalias - Create command alias\n\n");
        printf("Usage: alias name='command'\n\n");
        printf("Create an alias NAME for COMMAND. When NAME is typed,\n");
        printf("COMMAND will be executed instead.\n\n");
        printf("Examples:\n");
        printf("  alias ll='ls -l'\n");
        printf("  alias ..='cd ..'\n");
        printf("  alias grep='grep --color=auto'\n\n");

    } else if (strcmp(topic, "history") == 0) {
        printf("\nhistory - Command history\n\n");
        printf("Usage: history [n]\n");
        printf("       history -c\n");
        printf("       history -s <search>\n\n");
        printf("Without arguments, display command history.\n");
        printf("With N, display last N commands.\n\n");
        printf("Options:\n");
        printf("  -c    Clear history\n");
        printf("  -s    Search history for pattern\n\n");
        printf("History expansion:\n");
        printf("  !!    Repeat last command\n");
        printf("  !n    Repeat command number n\n");
        printf("  !-n   Repeat nth previous command\n");
        printf("  !str  Repeat last command starting with str\n\n");

    } else if (strcmp(topic, "jobs") == 0) {
        printf("\njobs - List background jobs\n\n");
        printf("Usage: jobs\n\n");
        printf("Display status of jobs started in current shell.\n\n");
        printf("Status indicators:\n");
        printf("  +     Current job\n");
        printf("  -     Previous job\n");
        printf("  Running    Job is executing\n");
        printf("  Stopped    Job is suspended\n");
        printf("  Done       Job completed\n\n");

    } else if (strcmp(topic, "fg") == 0) {
        printf("\nfg - Foreground job\n\n");
        printf("Usage: fg %%n\n\n");
        printf("Move job N to foreground and continue it.\n");
        printf("Without argument, use current job.\n\n");
        printf("Example:\n");
        printf("  fg %%1    Foreground job 1\n\n");

    } else if (strcmp(topic, "bg") == 0) {
        printf("\nbg - Background job\n\n");
        printf("Usage: bg %%n\n\n");
        printf("Continue job N in background.\n");
        printf("Without argument, use current job.\n\n");
        printf("Example:\n");
        printf("  bg %%1    Background job 1\n\n");

    } else if (strcmp(topic, "test") == 0 || strcmp(topic, "[") == 0) {
        printf("\ntest / [ - Conditional evaluation\n\n");
        printf("Usage: test EXPRESSION\n");
        printf("       [ EXPRESSION ]\n\n");
        printf("Evaluate EXPRESSION and return 0 (true) or 1 (false).\n\n");
        printf("File tests:\n");
        printf("  -e FILE    True if file exists\n");
        printf("  -f FILE    True if regular file\n");
        printf("  -d FILE    True if directory\n");
        printf("  -r FILE    True if readable\n");
        printf("  -w FILE    True if writable\n");
        printf("  -x FILE    True if executable\n");
        printf("  -s FILE    True if file size > 0\n\n");
        printf("String tests:\n");
        printf("  -z STR     True if string is empty\n");
        printf("  -n STR     True if string is not empty\n");
        printf("  S1 = S2    True if strings equal\n");
        printf("  S1 != S2   True if strings not equal\n\n");
        printf("Number tests:\n");
        printf("  N1 -eq N2  True if equal\n");
        printf("  N1 -ne N2  True if not equal\n");
        printf("  N1 -lt N2  True if less than\n");
        printf("  N1 -le N2  True if less or equal\n");
        printf("  N1 -gt N2  True if greater than\n");
        printf("  N1 -ge N2  True if greater or equal\n\n");

    } else {
        printf("\nNo help available for '%s'\n", topic);
        printf("Type 'help' for list of commands.\n\n");
    }

    return 0;
}

/* Built-in command: type */
int builtin_type(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "type: missing argument\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        /* Check if builtin */
        extern int is_builtin(const char *);
        if (is_builtin(argv[i])) {
            printf("%s is a shell builtin\n", argv[i]);
            continue;
        }

        /* Check if alias */
        extern char *alias_expand(const char *);
        char *alias = alias_expand(argv[i]);
        if (alias) {
            printf("%s is aliased to '%s'\n", argv[i], alias);
            continue;
        }

        /* Check if function */
        /* ... function check ... */

        /* Check in PATH */
        char *path_env = getenv("PATH");
        if (path_env) {
            char path[4096];
            strncpy(path, path_env, sizeof(path) - 1);

            char *dir = strtok(path, ":");
            int found = 0;

            while (dir) {
                char full_path[4096];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir, argv[i]);

                if (access(full_path, X_OK) == 0) {
                    printf("%s is %s\n", argv[i], full_path);
                    found = 1;
                    break;
                }

                dir = strtok(NULL, ":");
            }

            if (!found) {
                printf("%s: not found\n", argv[i]);
            }
        }
    }

    return 0;
}

/* Built-in command: which */
int builtin_which(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "which: missing argument\n");
        return 1;
    }

    char *path_env = getenv("PATH");
    if (!path_env) {
        fprintf(stderr, "which: PATH not set\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        char path[4096];
        strncpy(path, path_env, sizeof(path) - 1);

        char *dir = strtok(path, ":");
        int found = 0;

        while (dir) {
            char full_path[4096];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir, argv[i]);

            if (access(full_path, X_OK) == 0) {
                printf("%s\n", full_path);
                found = 1;
                break;
            }

            dir = strtok(NULL, ":");
        }

        if (!found) {
            fprintf(stderr, "%s not found in PATH\n", argv[i]);
        }
    }

    return 0;
}

/* Built-in command: time */
int builtin_time(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "time: missing command\n");
        return 1;
    }

    struct timeval start, end;
    struct rusage usage;

    gettimeofday(&start, NULL);

    /* Execute command */
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[1], &argv[1]);
        perror("time");
        exit(1);
    } else if (pid > 0) {
        int status;
        wait4(pid, &status, 0, &usage);

        gettimeofday(&end, NULL);

        double real_time = (end.tv_sec - start.tv_sec) +
                          (end.tv_usec - start.tv_usec) / 1000000.0;
        double user_time = usage.ru_utime.tv_sec +
                          usage.ru_utime.tv_usec / 1000000.0;
        double sys_time = usage.ru_stime.tv_sec +
                         usage.ru_stime.tv_usec / 1000000.0;

        fprintf(stderr, "\nreal\t%.3fs\n", real_time);
        fprintf(stderr, "user\t%.3fs\n", user_time);
        fprintf(stderr, "sys\t%.3fs\n", sys_time);

        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }

    return 1;
}

/* Built-in command: test / [ */
int builtin_test(int argc, char **argv) {
    /* Handle [ ... ] form */
    if (strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "[: missing ']'\n");
            return 2;
        }
        argc--; /* Remove trailing ] */
    }

    if (argc < 2) {
        return 1; /* Empty test is false */
    }

    /* Single argument */
    if (argc == 2) {
        /* Non-empty string is true */
        return (argv[1][0] != '\0') ? 0 : 1;
    }

    /* Two arguments with unary operator */
    if (argc == 3) {
        const char *op = argv[1];
        const char *arg = argv[2];

        if (strcmp(op, "-z") == 0) {
            return (arg[0] == '\0') ? 0 : 1;
        } else if (strcmp(op, "-n") == 0) {
            return (arg[0] != '\0') ? 0 : 1;
        } else if (strcmp(op, "-e") == 0) {
            struct stat st;
            return (stat(arg, &st) == 0) ? 0 : 1;
        } else if (strcmp(op, "-f") == 0) {
            struct stat st;
            return (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
        } else if (strcmp(op, "-d") == 0) {
            struct stat st;
            return (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
        } else if (strcmp(op, "-r") == 0) {
            return (access(arg, R_OK) == 0) ? 0 : 1;
        } else if (strcmp(op, "-w") == 0) {
            return (access(arg, W_OK) == 0) ? 0 : 1;
        } else if (strcmp(op, "-x") == 0) {
            return (access(arg, X_OK) == 0) ? 0 : 1;
        } else if (strcmp(op, "-s") == 0) {
            struct stat st;
            return (stat(arg, &st) == 0 && st.st_size > 0) ? 0 : 1;
        }
    }

    /* Three arguments with binary operator */
    if (argc == 4) {
        const char *arg1 = argv[1];
        const char *op = argv[2];
        const char *arg2 = argv[3];

        /* String comparisons */
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            return (strcmp(arg1, arg2) == 0) ? 0 : 1;
        } else if (strcmp(op, "!=") == 0) {
            return (strcmp(arg1, arg2) != 0) ? 0 : 1;
        }

        /* Numeric comparisons */
        int n1 = atoi(arg1);
        int n2 = atoi(arg2);

        if (strcmp(op, "-eq") == 0) {
            return (n1 == n2) ? 0 : 1;
        } else if (strcmp(op, "-ne") == 0) {
            return (n1 != n2) ? 0 : 1;
        } else if (strcmp(op, "-lt") == 0) {
            return (n1 < n2) ? 0 : 1;
        } else if (strcmp(op, "-le") == 0) {
            return (n1 <= n2) ? 0 : 1;
        } else if (strcmp(op, "-gt") == 0) {
            return (n1 > n2) ? 0 : 1;
        } else if (strcmp(op, "-ge") == 0) {
            return (n1 >= n2) ? 0 : 1;
        }
    }

    fprintf(stderr, "test: invalid expression\n");
    return 2;
}

/* Built-in command: let */
int builtin_let(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "let: missing expression\n");
        return 1;
    }

    /* Simple arithmetic evaluation */
    for (int i = 1; i < argc; i++) {
        char *expr = argv[i];
        char *eq = strchr(expr, '=');

        if (eq) {
            *eq = '\0';
            char *var = expr;
            char *value_expr = eq + 1;

            /* Evaluate right side (simple evaluation) */
            int result = atoi(value_expr);

            /* Set variable */
            char result_str[32];
            snprintf(result_str, sizeof(result_str), "%d", result);
            setenv(var, result_str, 1);
        }
    }

    return 0;
}

/* Built-in command: read */
int builtin_read(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "read: missing variable name\n");
        return 1;
    }

    char buffer[4096];
    if (fgets(buffer, sizeof(buffer), stdin)) {
        buffer[strcspn(buffer, "\n")] = '\0';
        setenv(argv[1], buffer, 1);
        return 0;
    }

    return 1;
}

/* Built-in command: printf */
int builtin_printf(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "printf: missing format\n");
        return 1;
    }

    const char *format = argv[1];
    int arg_idx = 2;

    for (const char *p = format; *p; p++) {
        if (*p == '%' && *(p + 1)) {
            p++;
            switch (*p) {
                case 's':
                    if (arg_idx < argc) {
                        printf("%s", argv[arg_idx++]);
                    }
                    break;
                case 'd':
                case 'i':
                    if (arg_idx < argc) {
                        printf("%d", atoi(argv[arg_idx++]));
                    }
                    break;
                case 'x':
                    if (arg_idx < argc) {
                        printf("%x", atoi(argv[arg_idx++]));
                    }
                    break;
                case 'f':
                    if (arg_idx < argc) {
                        printf("%f", atof(argv[arg_idx++]));
                    }
                    break;
                case '%':
                    putchar('%');
                    break;
                default:
                    putchar('%');
                    putchar(*p);
            }
        } else if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': putchar('\r'); break;
                case '\\': putchar('\\'); break;
                default: putchar(*p);
            }
        } else {
            putchar(*p);
        }
    }

    return 0;
}

/* Built-in command: eval */
int builtin_eval(int argc, char **argv) {
    if (argc < 2) {
        return 0;
    }

    /* Concatenate arguments */
    char command[4096] = "";
    size_t used = 0;
    for (int i = 1; i < argc; i++) {
        size_t arg_len = strlen(argv[i]);
        size_t sep_len = (i < argc - 1) ? 1 : 0;

        if (used + arg_len + sep_len >= sizeof(command)) {
            fprintf(stderr, "eval: command too long\n");
            return 1;
        }

        memcpy(command + used, argv[i], arg_len);
        used += arg_len;
        if (i < argc - 1) {
            command[used++] = ' ';
        }
    }
    command[used] = '\0';

    /* Execute via shell */
    return system(command) >> 8;
}

/* Built-in command: kill */
int builtin_kill(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "kill: missing argument\n");
        return 1;
    }

    int sig = SIGTERM;
    int start = 1;

    /* Check for signal argument */
    if (argv[1][0] == '-') {
        if (isdigit(argv[1][1])) {
            sig = atoi(argv[1] + 1);
        } else {
            /* Named signal */
            const char *signame = argv[1] + 1;
            if (strcmp(signame, "HUP") == 0) sig = SIGHUP;
            else if (strcmp(signame, "INT") == 0) sig = SIGINT;
            else if (strcmp(signame, "QUIT") == 0) sig = SIGQUIT;
            else if (strcmp(signame, "KILL") == 0) sig = SIGKILL;
            else if (strcmp(signame, "TERM") == 0) sig = SIGTERM;
            else if (strcmp(signame, "STOP") == 0) sig = SIGSTOP;
            else if (strcmp(signame, "CONT") == 0) sig = SIGCONT;
            else {
                fprintf(stderr, "kill: unknown signal: %s\n", signame);
                return 1;
            }
        }
        start = 2;
    }

    /* Send signal to processes */
    for (int i = start; i < argc; i++) {
        pid_t pid = atoi(argv[i]);
        if (kill(pid, sig) != 0) {
            fprintf(stderr, "kill: %d: %s\n", pid, strerror(errno));
            return 1;
        }
    }

    return 0;
}

/* Built-in command: umask */
int builtin_umask(int argc, char **argv) {
    if (argc < 2) {
        /* Print current umask */
        mode_t mask = umask(0);
        umask(mask);
        printf("%04o\n", mask);
        return 0;
    }

    /* Set umask */
    mode_t mask = strtol(argv[1], NULL, 8);
    umask(mask);

    return 0;
}

/* Built-in command: ulimit */
int builtin_ulimit(int argc, char **argv) {
    if (argc < 2) {
        /* Show all limits */
        struct rlimit rlim;

        getrlimit(RLIMIT_CPU, &rlim);
        printf("cpu time (seconds):         %ld\n", rlim.rlim_cur);

        getrlimit(RLIMIT_FSIZE, &rlim);
        printf("file size (blocks):         %ld\n", rlim.rlim_cur);

        getrlimit(RLIMIT_DATA, &rlim);
        printf("data seg size (kbytes):     %ld\n", rlim.rlim_cur);

        getrlimit(RLIMIT_STACK, &rlim);
        printf("stack size (kbytes):        %ld\n", rlim.rlim_cur);

        getrlimit(RLIMIT_NOFILE, &rlim);
        printf("open files:                 %ld\n", rlim.rlim_cur);

        return 0;
    }

    /* Set limit (simplified) */
    if (strcmp(argv[1], "-n") == 0 && argc > 2) {
        struct rlimit rlim;
        rlim.rlim_cur = atoi(argv[2]);
        rlim.rlim_max = rlim.rlim_cur;
        setrlimit(RLIMIT_NOFILE, &rlim);
    }

    return 0;
}

/* Built-in command: dirs */
int builtin_dirs(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Simple implementation - just show current directory */
    extern struct shell_state_t shell_state;
    printf("%s\n", shell_state.cwd);

    return 0;
}

/* Built-in command: env */
int builtin_env(int argc, char **argv) {
    (void)argc;
    (void)argv;

    extern char **environ;
    for (char **env = environ; *env; env++) {
        printf("%s\n", *env);
    }

    return 0;
}

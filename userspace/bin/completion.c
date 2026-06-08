/**
 * AutoShell Tab Completion System
 *
 * Provides intelligent tab completion for:
 * - Commands in PATH
 * - File and directory names
 * - Command options
 * - Environment variables
 * - Aliases
 * - Function names
 * - History search
 *
 * Version: 1.0.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <glob.h>

#define MAX_COMPLETIONS 256
#define MAX_PATH_LENGTH 4096

/* Completion context */
typedef struct {
    char **matches;
    int match_count;
    int match_capacity;
} completion_t;

/* Global completion state */
static completion_t g_completion = {NULL, 0, 0};

/*=============================================================================
 * Forward declarations
 *===========================================================================*/

static void completion_init(void);
static void completion_add(const char *match);
static void completion_clear(void);
static char **completion_get_matches(int *count);

static void complete_command(const char *prefix);
static void complete_filename(const char *prefix, const char *cwd);
static void complete_variable(const char *prefix);
static void complete_alias(const char *prefix);
static void complete_builtin(const char *prefix);

/*=============================================================================
 * Completion initialization
 *===========================================================================*/

static void completion_init(void) {
    if (!g_completion.matches) {
        g_completion.match_capacity = MAX_COMPLETIONS;
        g_completion.matches = malloc(g_completion.match_capacity * sizeof(char *));
        if (!g_completion.matches) {
            g_completion.match_capacity = 0;
            return;
        }
        g_completion.match_count = 0;
    }
}

static void completion_clear(void) {
    for (int i = 0; i < g_completion.match_count; i++) {
        free(g_completion.matches[i]);
    }
    g_completion.match_count = 0;
}

static void completion_add(const char *match) {
    if (g_completion.match_count >= g_completion.match_capacity) {
        return; /* Full */
    }

    /* Check for duplicates */
    for (int i = 0; i < g_completion.match_count; i++) {
        if (strcmp(g_completion.matches[i], match) == 0) {
            return; /* Already added */
        }
    }

    char *dup = strdup(match);
    if (!dup) return;
    g_completion.matches[g_completion.match_count++] = dup;
}

static char **completion_get_matches(int *count) {
    *count = g_completion.match_count;
    return g_completion.matches;
}

/*=============================================================================
 * Main completion function
 *===========================================================================*/

/**
 * Complete the current input line
 *
 * @param line Current input line
 * @param pos Cursor position in line
 * @param matches Output array of matches (caller must free)
 * @return Number of matches
 */
int complete_line(const char *line, int pos, char ***matches) {
    completion_init();
    completion_clear();

    /* Find the word to complete */
    int word_start = pos;
    while (word_start > 0 && !isspace(line[word_start - 1])) {
        word_start--;
    }

    char word[256];
    int word_len = pos - word_start;
    if (word_len >= sizeof(word)) word_len = sizeof(word) - 1;
    strncpy(word, line + word_start, word_len);
    word[word_len] = '\0';

    /* Determine what to complete based on context */
    int is_first_word = 1;
    for (int i = 0; i < word_start; i++) {
        if (!isspace(line[i])) {
            is_first_word = 0;
            break;
        }
    }

    char cwd[MAX_PATH_LENGTH];
    getcwd(cwd, sizeof(cwd));

    if (is_first_word) {
        /* Complete command name */
        if (word[0] == '$') {
            /* Variable completion */
            complete_variable(word + 1);
        } else if (strchr(word, '/')) {
            /* Path completion */
            complete_filename(word, cwd);
        } else {
            /* Command completion */
            complete_builtin(word);
            complete_alias(word);
            complete_command(word);
        }
    } else {
        /* Complete filename/path */
        if (word[0] == '$') {
            complete_variable(word + 1);
        } else {
            complete_filename(word, cwd);
        }
    }

    /* Return matches */
    *matches = completion_get_matches(&g_completion.match_count);
    return g_completion.match_count;
}

/*=============================================================================
 * Command completion
 *===========================================================================*/

static void complete_command(const char *prefix) {
    char *path_env = getenv("PATH");
    if (!path_env) return;

    size_t prefix_len = strlen(prefix);

    /* Make a copy of PATH */
    char path[4096];
    strncpy(path, path_env, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    /* Search each directory in PATH */
    char *dir = strtok(path, ":");
    while (dir) {
        DIR *dp = opendir(dir);
        if (dp) {
            struct dirent *entry;
            while ((entry = readdir(dp)) != NULL) {
                /* Skip hidden files */
                if (entry->d_name[0] == '.') continue;

                /* Check if name matches prefix */
                if (strncmp(entry->d_name, prefix, prefix_len) == 0) {
                    /* Check if executable */
                    char full_path[MAX_PATH_LENGTH];
                    snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);

                    struct stat st;
                    if (stat(full_path, &st) == 0 && (st.st_mode & S_IXUSR)) {
                        completion_add(entry->d_name);
                    }
                }
            }
            closedir(dp);
        }

        dir = strtok(NULL, ":");
    }
}

/*=============================================================================
 * Filename completion
 *===========================================================================*/

static void complete_filename(const char *prefix, const char *cwd) {
    char pattern[MAX_PATH_LENGTH];
    char dir_path[MAX_PATH_LENGTH];
    const char *basename = prefix;

    /* Split into directory and basename */
    const char *slash = strrchr(prefix, '/');
    if (slash) {
        size_t dir_len = slash - prefix;
        if (dir_len >= sizeof(dir_path)) dir_len = sizeof(dir_path) - 1;
        strncpy(dir_path, prefix, dir_len);
        dir_path[dir_len] = '\0';
        basename = slash + 1;

        /* Handle absolute vs relative paths */
        if (prefix[0] != '/') {
            char tmp[MAX_PATH_LENGTH];
            snprintf(tmp, sizeof(tmp), "%s/%s", cwd, dir_path);
            strncpy(dir_path, tmp, sizeof(dir_path) - 1);
        }
    } else {
        /* No slash - use current directory */
        strncpy(dir_path, cwd, sizeof(dir_path) - 1);
    }

    /* Open directory */
    DIR *dp = opendir(dir_path);
    if (!dp) return;

    size_t basename_len = strlen(basename);

    /* Read entries */
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        /* Skip . and .. unless explicitly typed */
        if (basename[0] != '.' && entry->d_name[0] == '.') {
            continue;
        }

        /* Check if name matches */
        if (strncmp(entry->d_name, basename, basename_len) == 0) {
            /* Build full path for match */
            char match[MAX_PATH_LENGTH];

            if (slash) {
                /* Include directory prefix */
                size_t prefix_dir_len = slash - prefix + 1;
                strncpy(match, prefix, prefix_dir_len);
                match[prefix_dir_len] = '\0';
                strncat(match, entry->d_name, sizeof(match) - prefix_dir_len - 1);
            } else {
                strncpy(match, entry->d_name, sizeof(match) - 1);
            }

            /* Add trailing slash for directories */
            struct stat st;
            char full_path[MAX_PATH_LENGTH];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

            if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                strncat(match, "/", sizeof(match) - strlen(match) - 1);
            }

            completion_add(match);
        }
    }

    closedir(dp);
}

/*=============================================================================
 * Variable completion
 *===========================================================================*/

static void complete_variable(const char *prefix) {
    size_t prefix_len = strlen(prefix);

    /* Complete from environment */
    extern char **environ;
    for (char **env = environ; *env; env++) {
        char *eq = strchr(*env, '=');
        if (!eq) continue;

        size_t name_len = eq - *env;
        if (name_len >= prefix_len &&
            strncmp(*env, prefix, prefix_len) == 0) {

            /* Add variable name (without value) */
            char var_name[256];
            if (name_len >= sizeof(var_name)) name_len = sizeof(var_name) - 1;
            strncpy(var_name, *env, name_len);
            var_name[name_len] = '\0';

            /* Prepend $ for completion */
            char match[258];
            snprintf(match, sizeof(match), "$%s", var_name);
            completion_add(match);
        }
    }

    /* Add special variables */
    const char *special_vars[] = {
        "$?", "$0", "$1", "$2", "$3", "$4", "$5",
        "$6", "$7", "$8", "$9", "$$", "$!", "$#",
        NULL
    };

    for (int i = 0; special_vars[i]; i++) {
        if (strncmp(special_vars[i] + 1, prefix, prefix_len) == 0) {
            completion_add(special_vars[i]);
        }
    }
}

/*=============================================================================
 * Alias completion
 *===========================================================================*/

static void complete_alias(const char *prefix) {
    /* Get aliases from shell */
    extern struct {
        char *name;
        char *value;
    } aliases[];
    extern int alias_count;

    size_t prefix_len = strlen(prefix);

    for (int i = 0; i < alias_count; i++) {
        if (strncmp(aliases[i].name, prefix, prefix_len) == 0) {
            completion_add(aliases[i].name);
        }
    }
}

/*=============================================================================
 * Built-in command completion
 *===========================================================================*/

static void complete_builtin(const char *prefix) {
    const char *builtins[] = {
        "cd", "pwd", "exit", "export", "alias", "unalias",
        "history", "jobs", "fg", "bg", "echo", "set",
        "unset", "source", "function", "help", "type",
        "which", "time", "test", "[", "let", "read",
        "printf", "eval", "exec", "kill", "umask", "ulimit",
        "dirs", "pushd", "popd", "env",
        NULL
    };

    size_t prefix_len = strlen(prefix);

    for (int i = 0; builtins[i]; i++) {
        if (strncmp(builtins[i], prefix, prefix_len) == 0) {
            completion_add(builtins[i]);
        }
    }
}

/*=============================================================================
 * Display completions
 *===========================================================================*/

/**
 * Display completion matches to user
 */
void display_completions(char **matches, int count) {
    if (count == 0) {
        /* No matches - beep */
        putchar('\a');
        fflush(stdout);
        return;
    }

    if (count == 1) {
        /* Single match - already inserted by caller */
        return;
    }

    /* Multiple matches - display them */
    printf("\n");

    /* Calculate column width */
    int max_len = 0;
    for (int i = 0; i < count; i++) {
        int len = strlen(matches[i]);
        if (len > max_len) max_len = len;
    }

    int cols = 80 / (max_len + 2);
    if (cols < 1) cols = 1;

    /* Display in columns */
    for (int i = 0; i < count; i++) {
        printf("%-*s", max_len + 2, matches[i]);

        if ((i + 1) % cols == 0) {
            printf("\n");
        }
    }

    if (count % cols != 0) {
        printf("\n");
    }

    printf("\n");
}

/*=============================================================================
 * Find common prefix of matches
 *===========================================================================*/

/**
 * Find the common prefix of all matches
 *
 * @param matches Array of match strings
 * @param count Number of matches
 * @return Length of common prefix
 */
int find_common_prefix_len(char **matches, int count) {
    if (count == 0) return 0;
    if (count == 1) return strlen(matches[0]);

    int prefix_len = 0;
    while (1) {
        char c = matches[0][prefix_len];
        if (c == '\0') break;

        for (int i = 1; i < count; i++) {
            if (matches[i][prefix_len] != c) {
                return prefix_len;
            }
        }

        prefix_len++;
    }

    return prefix_len;
}

/*=============================================================================
 * Glob pattern completion
 *===========================================================================*/

/**
 * Complete using glob patterns (e.g., *.txt)
 */
int complete_glob(const char *pattern, char ***matches) {
    glob_t glob_result;

    int flags = GLOB_TILDE | GLOB_MARK;
    if (glob(pattern, flags, NULL, &glob_result) == 0) {
        /* Copy matches */
        completion_init();
        completion_clear();

        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
            completion_add(glob_result.gl_pathv[i]);
        }

        globfree(&glob_result);

        *matches = completion_get_matches(&g_completion.match_count);
        return g_completion.match_count;
    }

    *matches = NULL;
    return 0;
}

/**
 * AutoShell Script Interpreter
 *
 * Full shell scripting support including:
 * - Variables and parameter expansion
 * - Conditional statements (if/elif/else/fi)
 * - Loops (for, while, until)
 * - Functions
 * - Case statements
 * - Arithmetic expansion
 * - Command substitution
 * - Here documents
 * - Script debugging
 *
 * Version: 1.0.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <errno.h>

#define MAX_SCRIPT_LINES 10000
#define MAX_LINE_LENGTH 4096
#define MAX_VARIABLES 256
#define MAX_LOOP_DEPTH 32

/* Script execution context */
typedef struct {
    char **lines;
    int line_count;
    int current_line;
    int in_function;
    int debug_mode;
    int exit_on_error;
} script_context_t;

/* Control flow state */
typedef enum {
    FLOW_NORMAL,
    FLOW_BREAK,
    FLOW_CONTINUE,
    FLOW_RETURN
} flow_state_t;

typedef struct {
    flow_state_t state;
    int return_value;
} flow_control_t;

/* Loop context */
typedef struct {
    int start_line;
    int end_line;
    char loop_type[16]; /* "for", "while", "until" */
} loop_context_t;

static loop_context_t loop_stack[MAX_LOOP_DEPTH];
static int loop_depth = 0;

/*=============================================================================
 * Forward declarations
 *===========================================================================*/

static int execute_script_line(const char *line, flow_control_t *flow);
static int execute_if_statement(script_context_t *ctx, flow_control_t *flow);
static int execute_for_loop(script_context_t *ctx, flow_control_t *flow);
static int execute_while_loop(script_context_t *ctx, flow_control_t *flow);
static int execute_until_loop(script_context_t *ctx, flow_control_t *flow);
static int execute_case_statement(script_context_t *ctx, flow_control_t *flow);
static int execute_function_def(script_context_t *ctx);

static char *expand_variables(const char *line);
static char *expand_command_substitution(const char *line);
static int evaluate_condition(const char *condition);
static int arithmetic_eval(const char *expr);

/*=============================================================================
 * Script execution engine
 *===========================================================================*/

/**
 * Execute a shell script from file
 */
int execute_script(const char *filename, int argc, char **argv) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "autoshell: %s: %s\n", filename, strerror(errno));
        return 1;
    }

    /* Read script into memory */
    script_context_t ctx = {0};
    ctx.lines = malloc(MAX_SCRIPT_LINES * sizeof(char *));
    ctx.line_count = 0;

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), fp) && ctx.line_count < MAX_SCRIPT_LINES) {
        /* Remove newline */
        line[strcspn(line, "\n")] = '\0';

        /* Skip empty lines and comments */
        char *trimmed = line;
        while (isspace(*trimmed)) trimmed++;

        if (*trimmed == '\0' || *trimmed == '#') {
            continue;
        }

        ctx.lines[ctx.line_count++] = strdup(line);
    }

    fclose(fp);

    /* Set script arguments */
    for (int i = 0; i < argc && i < 10; i++) {
        char var[8];
        snprintf(var, sizeof(var), "%d", i);
        setenv(var, argv[i], 1);
    }

    /* Execute script */
    flow_control_t flow = { FLOW_NORMAL, 0 };
    int status = 0;

    for (ctx.current_line = 0;
         ctx.current_line < ctx.line_count && flow.state == FLOW_NORMAL;
         ctx.current_line++) {

        const char *line = ctx.lines[ctx.current_line];

        if (ctx.debug_mode) {
            fprintf(stderr, "+ %s\n", line);
        }

        /* Handle control structures */
        if (strncmp(line, "if ", 3) == 0 || strcmp(line, "if") == 0) {
            status = execute_if_statement(&ctx, &flow);
        } else if (strncmp(line, "for ", 4) == 0) {
            status = execute_for_loop(&ctx, &flow);
        } else if (strncmp(line, "while ", 6) == 0) {
            status = execute_while_loop(&ctx, &flow);
        } else if (strncmp(line, "until ", 6) == 0) {
            status = execute_until_loop(&ctx, &flow);
        } else if (strncmp(line, "case ", 5) == 0) {
            status = execute_case_statement(&ctx, &flow);
        } else if (strncmp(line, "function ", 9) == 0 ||
                   strstr(line, "() {")) {
            status = execute_function_def(&ctx);
        } else if (strcmp(line, "break") == 0) {
            flow.state = FLOW_BREAK;
            break;
        } else if (strcmp(line, "continue") == 0) {
            flow.state = FLOW_CONTINUE;
            break;
        } else if (strncmp(line, "return", 6) == 0) {
            flow.state = FLOW_RETURN;
            flow.return_value = (line[6] == ' ') ? atoi(line + 7) : 0;
            break;
        } else {
            /* Regular command */
            status = execute_script_line(line, &flow);

            if (ctx.exit_on_error && status != 0) {
                fprintf(stderr, "autoshell: line %d: command failed with status %d\n",
                       ctx.current_line + 1, status);
                break;
            }
        }
    }

    /* Cleanup */
    for (int i = 0; i < ctx.line_count; i++) {
        free(ctx.lines[i]);
    }
    free(ctx.lines);

    return (flow.state == FLOW_RETURN) ? flow.return_value : status;
}

/**
 * Execute a single script line
 */
static int execute_script_line(const char *line, flow_control_t *flow) {
    (void)flow;

    /* Expand variables */
    char *expanded = expand_variables(line);

    /* Expand command substitution */
    char *substituted = expand_command_substitution(expanded);

    /* Execute via system() for now (simplified) */
    int status = system(substituted) >> 8;

    return status;
}

/*=============================================================================
 * Control structures: if/elif/else/fi
 *===========================================================================*/

static int execute_if_statement(script_context_t *ctx, flow_control_t *flow) {
    int status = 0;
    int condition_met = 0;

    /* Parse if line */
    const char *line = ctx->lines[ctx->current_line];
    const char *condition = line + 3; /* Skip "if " */

    /* Handle "if [ ... ]" or "if test ..." */
    condition_met = evaluate_condition(condition);

    /* Find then/else/elif/fi */
    int then_line = -1;
    int else_line = -1;
    int fi_line = -1;
    int depth = 0;

    for (int i = ctx->current_line + 1; i < ctx->line_count; i++) {
        const char *l = ctx->lines[i];

        if (strncmp(l, "if ", 3) == 0) {
            depth++;
        } else if (strcmp(l, "then") == 0 && depth == 0) {
            then_line = i;
        } else if (strcmp(l, "else") == 0 && depth == 0) {
            else_line = i;
        } else if (strcmp(l, "fi") == 0) {
            if (depth == 0) {
                fi_line = i;
                break;
            }
            depth--;
        }
    }

    if (fi_line == -1) {
        fprintf(stderr, "autoshell: if: missing 'fi'\n");
        return 1;
    }

    /* Execute appropriate branch */
    int start = condition_met ? then_line + 1 : (else_line != -1 ? else_line + 1 : fi_line);
    int end = condition_met ? (else_line != -1 ? else_line : fi_line) : fi_line;

    for (int i = start; i < end && flow->state == FLOW_NORMAL; i++) {
        status = execute_script_line(ctx->lines[i], flow);
    }

    /* Skip to fi */
    ctx->current_line = fi_line;

    return status;
}

/*=============================================================================
 * Loops: for/while/until
 *===========================================================================*/

static int execute_for_loop(script_context_t *ctx, flow_control_t *flow) {
    /* Parse: for var in list; do ... done */
    const char *line = ctx->lines[ctx->current_line];

    /* Extract variable name and list */
    char var[256] = "";
    char list[4096] = "";

    sscanf(line, "for %255s in %4095[^\n]", var, list);

    /* Remove trailing "; do" if present */
    char *do_pos = strstr(list, "; do");
    if (do_pos) *do_pos = '\0';

    /* Find do...done */
    int do_line = -1;
    int done_line = -1;
    int depth = 0;

    for (int i = ctx->current_line + 1; i < ctx->line_count; i++) {
        const char *l = ctx->lines[i];

        if (strcmp(l, "do") == 0 && depth == 0) {
            do_line = i;
        } else if (strncmp(l, "for ", 4) == 0 || strncmp(l, "while ", 6) == 0) {
            depth++;
        } else if (strcmp(l, "done") == 0) {
            if (depth == 0) {
                done_line = i;
                break;
            }
            depth--;
        }
    }

    if (done_line == -1) {
        fprintf(stderr, "autoshell: for: missing 'done'\n");
        return 1;
    }

    /* Push loop context */
    if (loop_depth >= MAX_LOOP_DEPTH) {
        fprintf(stderr, "autoshell: for: loop nesting too deep\n");
        return 1;
    }

    loop_stack[loop_depth].start_line = do_line + 1;
    loop_stack[loop_depth].end_line = done_line;
    strcpy(loop_stack[loop_depth].loop_type, "for");
    loop_depth++;

    /* Iterate over list */
    char *saveptr;
    char *item = strtok_r(list, " \t", &saveptr);
    int status = 0;

    while (item) {
        /* Set loop variable */
        setenv(var, item, 1);

        /* Execute loop body */
        for (int i = do_line + 1; i < done_line && flow->state == FLOW_NORMAL; i++) {
            status = execute_script_line(ctx->lines[i], flow);

            if (flow->state == FLOW_BREAK) {
                flow->state = FLOW_NORMAL;
                goto loop_exit;
            } else if (flow->state == FLOW_CONTINUE) {
                flow->state = FLOW_NORMAL;
                break;
            }
        }

        item = strtok_r(NULL, " \t", &saveptr);
    }

loop_exit:
    /* Pop loop context */
    loop_depth--;

    /* Skip to done */
    ctx->current_line = done_line;

    return status;
}

static int execute_while_loop(script_context_t *ctx, flow_control_t *flow) {
    /* Parse: while condition; do ... done */
    const char *line = ctx->lines[ctx->current_line];
    const char *condition = line + 6; /* Skip "while " */

    /* Find do...done */
    int do_line = -1;
    int done_line = -1;
    int depth = 0;

    for (int i = ctx->current_line + 1; i < ctx->line_count; i++) {
        const char *l = ctx->lines[i];

        if (strcmp(l, "do") == 0 && depth == 0) {
            do_line = i;
        } else if (strncmp(l, "while ", 6) == 0 || strncmp(l, "for ", 4) == 0) {
            depth++;
        } else if (strcmp(l, "done") == 0) {
            if (depth == 0) {
                done_line = i;
                break;
            }
            depth--;
        }
    }

    if (done_line == -1) {
        fprintf(stderr, "autoshell: while: missing 'done'\n");
        return 1;
    }

    /* Execute loop */
    int status = 0;

    while (evaluate_condition(condition) && flow->state == FLOW_NORMAL) {
        for (int i = do_line + 1; i < done_line && flow->state == FLOW_NORMAL; i++) {
            status = execute_script_line(ctx->lines[i], flow);

            if (flow->state == FLOW_BREAK) {
                flow->state = FLOW_NORMAL;
                goto while_exit;
            } else if (flow->state == FLOW_CONTINUE) {
                flow->state = FLOW_NORMAL;
                break;
            }
        }
    }

while_exit:
    /* Skip to done */
    ctx->current_line = done_line;

    return status;
}

static int execute_until_loop(script_context_t *ctx, flow_control_t *flow) {
    /* Parse: until condition; do ... done */
    const char *line = ctx->lines[ctx->current_line];
    const char *condition = line + 6; /* Skip "until " */

    /* Find do...done */
    int do_line = -1;
    int done_line = -1;

    for (int i = ctx->current_line + 1; i < ctx->line_count; i++) {
        const char *l = ctx->lines[i];

        if (strcmp(l, "do") == 0) {
            do_line = i;
        } else if (strcmp(l, "done") == 0) {
            done_line = i;
            break;
        }
    }

    if (done_line == -1) {
        fprintf(stderr, "autoshell: until: missing 'done'\n");
        return 1;
    }

    /* Execute loop (opposite of while) */
    int status = 0;

    while (!evaluate_condition(condition) && flow->state == FLOW_NORMAL) {
        for (int i = do_line + 1; i < done_line && flow->state == FLOW_NORMAL; i++) {
            status = execute_script_line(ctx->lines[i], flow);

            if (flow->state == FLOW_BREAK) {
                flow->state = FLOW_NORMAL;
                goto until_exit;
            } else if (flow->state == FLOW_CONTINUE) {
                flow->state = FLOW_NORMAL;
                break;
            }
        }
    }

until_exit:
    ctx->current_line = done_line;
    return status;
}

/*=============================================================================
 * Case statement
 *===========================================================================*/

static int execute_case_statement(script_context_t *ctx, flow_control_t *flow) {
    /* Parse: case $var in pattern) commands ;; esac */
    const char *line = ctx->lines[ctx->current_line];

    /* Extract variable */
    char var_expr[256];
    sscanf(line, "case %255s in", var_expr);

    char *var_value = expand_variables(var_expr);

    /* Find esac */
    int esac_line = -1;
    for (int i = ctx->current_line + 1; i < ctx->line_count; i++) {
        if (strcmp(ctx->lines[i], "esac") == 0) {
            esac_line = i;
            break;
        }
    }

    if (esac_line == -1) {
        fprintf(stderr, "autoshell: case: missing 'esac'\n");
        return 1;
    }

    /* Match patterns */
    int status = 0;
    int matched = 0;

    for (int i = ctx->current_line + 1; i < esac_line && !matched; i++) {
        const char *l = ctx->lines[i];

        /* Check if this is a pattern line */
        if (strchr(l, ')')) {
            char pattern[256];
            strncpy(pattern, l, sizeof(pattern) - 1);
            pattern[sizeof(pattern) - 1] = '\0';  /* strncpy doesn't NUL-terminate on truncation */
            char *rparen = strchr(pattern, ')');
            if (rparen) {
                *rparen = '\0';

                /* Match pattern (simplified) */
                if (strcmp(pattern, var_value) == 0 || strcmp(pattern, "*") == 0) {
                    matched = 1;

                    /* Execute commands until ;; */
                    for (int j = i + 1; j < esac_line; j++) {
                        if (strstr(ctx->lines[j], ";;")) {
                            break;
                        }
                        status = execute_script_line(ctx->lines[j], flow);
                    }
                }
            }
        }
    }

    ctx->current_line = esac_line;
    return status;
}

/*=============================================================================
 * Function definition
 *===========================================================================*/

static int execute_function_def(script_context_t *ctx) {
    /* Parse: function name { ... } or name() { ... } */
    const char *line = ctx->lines[ctx->current_line];

    char func_name[256];
    if (strncmp(line, "function ", 9) == 0) {
        sscanf(line, "function %255s", func_name);
        /* Remove trailing () if present */
        char *paren = strchr(func_name, '(');
        if (paren) *paren = '\0';
    } else {
        sscanf(line, "%255[^(]", func_name);
    }

    /* Find closing brace */
    int start_line = ctx->current_line + 1;
    int end_line = -1;
    int depth = 0;

    for (int i = start_line; i < ctx->line_count; i++) {
        const char *l = ctx->lines[i];

        if (strchr(l, '{')) depth++;
        if (strchr(l, '}')) {
            if (depth == 0) {
                end_line = i;
                break;
            }
            depth--;
        }
    }

    if (end_line == -1) {
        fprintf(stderr, "autoshell: function: missing '}'\n");
        return 1;
    }

    /* Store function body */
    int body_lines = end_line - start_line;
    char **body = malloc(body_lines * sizeof(char *));

    for (int i = 0; i < body_lines; i++) {
        body[i] = strdup(ctx->lines[start_line + i]);
    }

    /* Register function */
    extern void function_add(const char *, char **, int);
    function_add(func_name, body, body_lines);

    /* Skip to end of function */
    ctx->current_line = end_line;

    return 0;
}

/*=============================================================================
 * Variable and command expansion
 *===========================================================================*/

static char *expand_variables(const char *line) {
    static char result[MAX_LINE_LENGTH];
    char *dst = result;
    const char *src = line;

    while (*src && (dst - result) < MAX_LINE_LENGTH - 1) {
        if (*src == '$') {
            src++;

            if (*src == '{') {
                /* ${var} format */
                src++;
                char var[256];
                int i = 0;
                while (*src && *src != '}' && i < 255) {
                    var[i++] = *src++;
                }
                var[i] = '\0';
                if (*src == '}') src++;

                char *value = getenv(var);
                if (value) {
                    while (*value && (dst - result) < MAX_LINE_LENGTH - 1) {
                        *dst++ = *value++;
                    }
                }
            } else {
                /* $var format */
                char var[256];
                int i = 0;
                while (*src && (isalnum(*src) || *src == '_') && i < 255) {
                    var[i++] = *src++;
                }
                var[i] = '\0';

                char *value = getenv(var);
                if (value) {
                    while (*value && (dst - result) < MAX_LINE_LENGTH - 1) {
                        *dst++ = *value++;
                    }
                }
            }
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
    return result;
}

static char *expand_command_substitution(const char *line) {
    static char result[MAX_LINE_LENGTH];
    char *dst = result;
    const char *src = line;

    while (*src && (dst - result) < MAX_LINE_LENGTH - 1) {
        if (*src == '$' && *(src + 1) == '(') {
            /* $(command) format */
            src += 2;
            char command[1024];
            int i = 0;
            int depth = 1;

            while (*src && depth > 0 && i < 1023) {
                if (*src == '(') depth++;
                if (*src == ')') depth--;
                if (depth > 0) {
                    command[i++] = *src++;
                } else {
                    src++;
                }
            }
            command[i] = '\0';

            /* Execute command and capture output */
            FILE *fp = popen(command, "r");
            if (fp) {
                int c;
                while ((c = fgetc(fp)) != EOF && (dst - result) < MAX_LINE_LENGTH - 1) {
                    if (c == '\n') c = ' '; /* Replace newlines with spaces */
                    *dst++ = c;
                }
                pclose(fp);
            }
        } else if (*src == '`') {
            /* `command` format (legacy) */
            src++;
            char command[1024];
            int i = 0;

            while (*src && *src != '`' && i < 1023) {
                command[i++] = *src++;
            }
            command[i] = '\0';
            if (*src == '`') src++;

            /* Execute command and capture output */
            FILE *fp = popen(command, "r");
            if (fp) {
                int c;
                while ((c = fgetc(fp)) != EOF && (dst - result) < MAX_LINE_LENGTH - 1) {
                    if (c == '\n') c = ' ';
                    *dst++ = c;
                }
                pclose(fp);
            }
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
    return result;
}

/*=============================================================================
 * Condition evaluation
 *===========================================================================*/

static int evaluate_condition(const char *condition) {
    /* Handle [ ... ] format */
    if (condition[0] == '[') {
        /* Use test builtin */
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "test %s", condition + 1);
        return (system(cmd) >> 8) == 0;
    }

    /* Handle test command format */
    if (strncmp(condition, "test ", 5) == 0) {
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "%s", condition);
        return (system(cmd) >> 8) == 0;
    }

    /* Otherwise execute as command */
    return (system(condition) >> 8) == 0;
}

/*=============================================================================
 * Arithmetic evaluation
 *===========================================================================*/

static int arithmetic_eval(const char *expr) {
    /* Simple arithmetic evaluation */
    /* In a full implementation, this would be a proper expression parser */

    /* Handle simple cases */
    int result = 0;
    char op = '+';
    int num = 0;

    for (const char *p = expr; *p; p++) {
        if (isdigit(*p)) {
            num = num * 10 + (*p - '0');
        } else if (*p == '+' || *p == '-' || *p == '*' || *p == '/') {
            switch (op) {
                case '+': result += num; break;
                case '-': result -= num; break;
                case '*': result *= num; break;
                case '/': if (num) result /= num; break;
            }
            op = *p;
            num = 0;
        }
    }

    /* Apply last operation */
    switch (op) {
        case '+': result += num; break;
        case '-': result -= num; break;
        case '*': result *= num; break;
        case '/': if (num) result /= num; break;
    }

    return result;
}

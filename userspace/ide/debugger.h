// AutomationOS IDE - Debugger Component
#ifndef IDE_DEBUGGER_H
#define IDE_DEBUGGER_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_BREAKPOINTS 256
#define MAX_WATCHPOINTS 64
#define MAX_STACK_FRAMES 128
#define MAX_VARIABLES 512

// Debugger states
typedef enum {
    DBG_IDLE,
    DBG_RUNNING,
    DBG_PAUSED,
    DBG_STEP,
    DBG_STOPPED,
    DBG_ERROR
} debugger_state_enum_t;

// Breakpoint types
typedef enum {
    BP_NORMAL,
    BP_CONDITIONAL,
    BP_TEMPORARY,
    BP_HARDWARE
} breakpoint_type_t;

// Breakpoint
typedef struct {
    uint32_t id;
    char *file;
    int line;
    uint64_t address;
    breakpoint_type_t type;
    char *condition;  // For conditional breakpoints
    bool enabled;
    int hit_count;
} breakpoint_t;

// Watchpoint
typedef struct {
    uint32_t id;
    char *expression;
    uint64_t address;
    size_t size;
    bool on_write;
    bool on_read;
    bool enabled;
} watchpoint_t;

// Variable value
typedef struct {
    char *name;
    char *type;
    char *value;
    uint64_t address;
    bool in_scope;
} variable_t;

// Stack frame
typedef struct {
    int level;
    char *function;
    char *file;
    int line;
    uint64_t address;
    variable_t *locals[MAX_VARIABLES];
    int local_count;
} stack_frame_t;

// Thread info
typedef struct {
    uint32_t tid;
    char *name;
    debugger_state_enum_t state;
    int current_frame;
    stack_frame_t *frames[MAX_STACK_FRAMES];
    int frame_count;
} thread_info_t;

// Debugger state
struct debugger_state {
    debugger_state_enum_t state;

    // Target process
    int target_pid;
    char *target_path;
    char **target_args;

    // Breakpoints and watchpoints
    breakpoint_t *breakpoints[MAX_BREAKPOINTS];
    watchpoint_t *watchpoints[MAX_WATCHPOINTS];
    int breakpoint_count;
    int watchpoint_count;

    // Thread information
    thread_info_t *current_thread;
    thread_info_t **threads;
    int thread_count;

    // GDB integration
    int gdb_pid;
    int gdb_input_fd;
    int gdb_output_fd;

    // Event callbacks
    void (*on_break)(struct debugger_state *dbg);
    void (*on_step)(struct debugger_state *dbg);
    void (*on_exit)(struct debugger_state *dbg, int code);
};

// Debugger control
int debugger_attach(struct debugger_state *dbg, int pid);
int debugger_detach(struct debugger_state *dbg);
int debugger_launch(struct debugger_state *dbg, const char *path, char **args);
int debugger_terminate(struct debugger_state *dbg);

// Execution control
int debugger_continue(struct debugger_state *dbg);
int debugger_pause(struct debugger_state *dbg);
int debugger_step_over(struct debugger_state *dbg);
int debugger_step_into(struct debugger_state *dbg);
int debugger_step_out(struct debugger_state *dbg);

// Breakpoints
int debugger_add_breakpoint(struct debugger_state *dbg, const char *file, int line);
int debugger_add_breakpoint_addr(struct debugger_state *dbg, uint64_t addr);
int debugger_remove_breakpoint(struct debugger_state *dbg, uint32_t id);
int debugger_enable_breakpoint(struct debugger_state *dbg, uint32_t id, bool enable);
int debugger_set_condition(struct debugger_state *dbg, uint32_t id, const char *cond);

// Watchpoints
int debugger_add_watchpoint(struct debugger_state *dbg, const char *expr);
int debugger_remove_watchpoint(struct debugger_state *dbg, uint32_t id);

// Stack and variables
int debugger_get_stack(struct debugger_state *dbg, stack_frame_t ***frames);
int debugger_select_frame(struct debugger_state *dbg, int level);
int debugger_get_locals(struct debugger_state *dbg, int frame, variable_t ***vars);
int debugger_get_global(struct debugger_state *dbg, const char *name, variable_t **var);

// Memory inspection
int debugger_read_memory(struct debugger_state *dbg, uint64_t addr, void *buf, size_t len);
int debugger_write_memory(struct debugger_state *dbg, uint64_t addr, const void *buf, size_t len);

// Expression evaluation
int debugger_evaluate(struct debugger_state *dbg, const char *expr, char **result);

// Disassembly
int debugger_disassemble(struct debugger_state *dbg, uint64_t addr, int count, char **output);

// GDB MI interface
int debugger_gdb_command(struct debugger_state *dbg, const char *cmd, char **response);

#endif // IDE_DEBUGGER_H

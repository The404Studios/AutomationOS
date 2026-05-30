// AutomationOS IDE - Main header file
#ifndef IDE_H
#define IDE_H

#include <stdint.h>
#include <stdbool.h>

#define IDE_VERSION "1.0.0"
#define IDE_NAME "AutomationOS IDE"

// Forward declarations
typedef struct ide_context ide_context_t;
typedef struct editor_state editor_state_t;
typedef struct blueprint_editor blueprint_editor_t;
typedef struct debugger_state debugger_state_t;
typedef struct project_info project_info_t;

// IDE context - main state container
struct ide_context {
    editor_state_t *editor;
    blueprint_editor_t *blueprint;
    debugger_state_t *debugger;
    project_info_t *project;

    bool running;
    char *current_file;
    char *project_path;
};

// Public API
ide_context_t* ide_init(const char *project_path);
void ide_run(ide_context_t *ctx);
void ide_shutdown(ide_context_t *ctx);

// Component initialization
int ide_editor_init(ide_context_t *ctx);
int ide_blueprint_init(ide_context_t *ctx);
int ide_debugger_init(ide_context_t *ctx);
int ide_project_init(ide_context_t *ctx, const char *path);

// Component cleanup
void ide_editor_cleanup(editor_state_t *editor);
void ide_blueprint_cleanup(blueprint_editor_t *blueprint);
void ide_debugger_cleanup(debugger_state_t *debugger);
void ide_project_cleanup(project_info_t *project);

#endif // IDE_H

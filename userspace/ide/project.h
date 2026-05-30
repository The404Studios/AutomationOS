// AutomationOS IDE - Project Management Component
#ifndef IDE_PROJECT_H
#define IDE_PROJECT_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_SOURCE_FILES 1024
#define MAX_BUILD_TARGETS 32
#define MAX_DEPENDENCIES 128

// Project types
typedef enum {
    PROJ_C_EXECUTABLE,
    PROJ_CPP_EXECUTABLE,
    PROJ_C_LIBRARY,
    PROJ_CPP_LIBRARY,
    PROJ_KERNEL_MODULE,
    PROJ_BOOTLOADER,
    PROJ_CUSTOM
} project_type_t;

// Build system
typedef enum {
    BUILD_MAKE,
    BUILD_CMAKE,
    BUILD_CUSTOM
} build_system_t;

// Build target
typedef struct {
    char *name;
    char *output;
    char **sources;
    int source_count;
    char **include_dirs;
    int include_count;
    char **libraries;
    int library_count;
    char *compiler_flags;
    char *linker_flags;
} build_target_t;

// Source file info
typedef struct {
    char *path;
    char *relative_path;
    bool is_header;
    uint64_t last_modified;
    uint64_t size;
} source_file_t;

// Project information
struct project_info {
    char *name;
    char *path;
    char *root_dir;
    project_type_t type;
    build_system_t build_system;

    // Source files
    source_file_t *files[MAX_SOURCE_FILES];
    int file_count;

    // Build targets
    build_target_t *targets[MAX_BUILD_TARGETS];
    int target_count;
    int active_target;

    // Dependencies
    char *dependencies[MAX_DEPENDENCIES];
    int dependency_count;

    // Settings
    char *compiler;
    char *debugger;
    char *version_control;
    char *build_dir;
    char *output_dir;

    // Git integration
    char *git_branch;
    int git_status;  // 0 = clean, >0 = modified files

    bool loaded;
};

// Project operations
int project_create(struct project_info *proj, const char *path, project_type_t type);
int project_load(struct project_info *proj, const char *path);
int project_save(struct project_info *proj);
int project_close(struct project_info *proj);

// File management
int project_add_file(struct project_info *proj, const char *path);
int project_remove_file(struct project_info *proj, const char *path);
int project_scan_files(struct project_info *proj);

// Build operations
int project_build(struct project_info *proj);
int project_rebuild(struct project_info *proj);
int project_clean(struct project_info *proj);
int project_run(struct project_info *proj);

// Target management
int project_add_target(struct project_info *proj, const char *name);
int project_remove_target(struct project_info *proj, const char *name);
int project_set_active_target(struct project_info *proj, int index);

// Dependencies
int project_add_dependency(struct project_info *proj, const char *dep);
int project_resolve_dependencies(struct project_info *proj);

// Version control
int project_git_init(struct project_info *proj);
int project_git_status(struct project_info *proj);
int project_git_commit(struct project_info *proj, const char *message);
int project_git_push(struct project_info *proj);
int project_git_pull(struct project_info *proj);

// Utility
void project_print_info(struct project_info *proj);
int project_get_config(struct project_info *proj, const char *key, char **value);
int project_set_config(struct project_info *proj, const char *key, const char *value);

#endif // IDE_PROJECT_H

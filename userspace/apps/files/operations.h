/**
 * File Explorer - File Operations Header
 *
 * Handles file operations with progress tracking and background execution
 */

#ifndef OPERATIONS_H
#define OPERATIONS_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// Forward declarations
typedef struct file_operation file_operation_t;
typedef struct progress_dialog progress_dialog_t;

/**
 * Operation types
 */
typedef enum {
    OP_COPY,
    OP_MOVE,
    OP_DELETE,
    OP_COMPRESS,
    OP_EXTRACT,
    OP_RENAME,
} operation_type_t;

/**
 * Compression formats
 */
typedef enum {
    COMPRESS_ZIP,
    COMPRESS_TAR_GZ,
    COMPRESS_TAR_BZ2,
    COMPRESS_7Z,
} compression_type_t;

/**
 * Operation status
 */
typedef enum {
    OP_STATUS_PENDING,
    OP_STATUS_RUNNING,
    OP_STATUS_PAUSED,
    OP_STATUS_COMPLETED,
    OP_STATUS_ERROR,
    OP_STATUS_CANCELLED,
} operation_status_t;

/**
 * Progress dialog
 */
struct progress_dialog {
    // Window
    void *window;
    bool visible;

    // Progress
    char title[128];
    char current_file[256];
    float progress;             // 0.0 to 1.0

    // Details
    uint64_t bytes_total;
    uint64_t bytes_done;
    uint64_t files_total;
    uint64_t files_done;

    // Speed
    uint64_t start_time;
    uint64_t last_update_time;
    uint64_t bytes_per_second;
    uint64_t estimated_remaining_seconds;

    // Controls
    bool cancellable;
    bool pausable;
    bool show_details;

    // Callbacks
    void (*on_cancel)(void *userdata);
    void (*on_pause)(void *userdata);
    void *userdata;
};

/**
 * File operation
 */
struct file_operation {
    uint32_t id;
    operation_type_t type;
    operation_status_t status;

    // Source and destination
    char source_paths[256][4096];
    uint32_t source_count;
    char destination[4096];

    // Compression
    compression_type_t compression_type;
    char archive_path[4096];

    // Progress tracking
    uint64_t total_bytes;
    uint64_t bytes_done;
    uint64_t files_total;
    uint64_t files_done;

    // Error handling
    char error_message[512];
    int error_code;

    // Threading
    pthread_t thread;
    pthread_mutex_t mutex;
    bool thread_running;

    // UI
    progress_dialog_t *dialog;
    bool show_progress;

    // Control
    volatile bool cancel_requested;
    volatile bool pause_requested;
    volatile bool paused;

    // Timestamps
    uint64_t start_time;
    uint64_t end_time;

    // Callbacks
    void (*on_complete)(file_operation_t *op, void *userdata);
    void (*on_error)(file_operation_t *op, void *userdata);
    void (*on_progress)(file_operation_t *op, void *userdata);
    void *userdata;
};

// Operation creation
file_operation_t* operation_create(operation_type_t type);
void operation_destroy(file_operation_t *op);

// Copy operations
file_operation_t* operation_copy_files(const char **sources, uint32_t count, const char *dest);
void operation_copy_file(const char *source, const char *dest, file_operation_t *op);
void operation_copy_directory(const char *source, const char *dest, file_operation_t *op);

// Move operations
file_operation_t* operation_move_files(const char **sources, uint32_t count, const char *dest);

// Delete operations
file_operation_t* operation_delete_files(const char **paths, uint32_t count, bool permanent);

// Compress operations
file_operation_t* operation_compress_files(const char **paths, uint32_t count,
                                           const char *archive_path,
                                           compression_type_t type);

// Extract operations
file_operation_t* operation_extract_archive(const char *archive_path, const char *dest);

// Rename operations
file_operation_t* operation_rename_file(const char *old_path, const char *new_name);

// Execution control
void operation_start(file_operation_t *op);
void operation_pause(file_operation_t *op);
void operation_resume(file_operation_t *op);
void operation_cancel(file_operation_t *op);
void operation_wait(file_operation_t *op);

// Progress tracking
float operation_get_progress(file_operation_t *op);
void operation_update_progress(file_operation_t *op, uint64_t bytes_processed);
uint64_t operation_get_speed(file_operation_t *op);
uint64_t operation_get_eta(file_operation_t *op);

// Progress dialog
progress_dialog_t* progress_dialog_create(const char *title);
void progress_dialog_destroy(progress_dialog_t *dialog);
void progress_dialog_show(progress_dialog_t *dialog);
void progress_dialog_hide(progress_dialog_t *dialog);
void progress_dialog_update(progress_dialog_t *dialog, file_operation_t *op);

// Utility functions
uint64_t get_file_size(const char *path);
uint64_t get_directory_size(const char *path);
bool is_directory(const char *path);
bool file_exists(const char *path);
void create_directory(const char *path);
void remove_file(const char *path);
void remove_directory(const char *path);

// Conflict resolution
typedef enum {
    CONFLICT_SKIP,
    CONFLICT_OVERWRITE,
    CONFLICT_RENAME,
    CONFLICT_CANCEL,
} conflict_resolution_t;

conflict_resolution_t ask_conflict_resolution(const char *path, bool *apply_to_all);

#endif // OPERATIONS_H

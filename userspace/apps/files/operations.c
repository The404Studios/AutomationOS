/**
 * File Explorer - File Operations Implementation
 *
 * Handles copy, move, delete, compress operations with progress tracking
 */

#include "operations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

#define BUFFER_SIZE (1024 * 1024)  // 1MB buffer for file copying

static uint32_t next_operation_id = 1;

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Create new file operation
 */
file_operation_t* operation_create(operation_type_t type) {
    file_operation_t *op = calloc(1, sizeof(file_operation_t));
    if (!op) return NULL;

    op->id = next_operation_id++;
    op->type = type;
    op->status = OP_STATUS_PENDING;
    op->source_count = 0;
    op->total_bytes = 0;
    op->bytes_done = 0;
    op->files_total = 0;
    op->files_done = 0;
    op->show_progress = true;
    op->cancel_requested = false;
    op->pause_requested = false;
    op->paused = false;
    op->thread_running = false;

    pthread_mutex_init(&op->mutex, NULL);

    return op;
}

/**
 * Destroy file operation
 */
void operation_destroy(file_operation_t *op) {
    if (!op) return;

    // Wait for thread to finish
    if (op->thread_running) {
        op->cancel_requested = true;
        pthread_join(op->thread, NULL);
    }

    // Destroy progress dialog
    if (op->dialog) {
        progress_dialog_destroy(op->dialog);
    }

    pthread_mutex_destroy(&op->mutex);
    free(op);
}

/**
 * Get file size
 */
uint64_t get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return st.st_size;
}

/**
 * Check if path is a directory
 */
bool is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

/**
 * Check if file exists
 */
bool file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

/**
 * Get directory size (recursive)
 */
uint64_t get_directory_size(const char *path) {
    uint64_t total = 0;
    DIR *dir = opendir(path);
    if (!dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        if (is_directory(full_path)) {
            total += get_directory_size(full_path);
        } else {
            total += get_file_size(full_path);
        }
    }

    closedir(dir);
    return total;
}

/**
 * Create directory (with parents)
 */
void create_directory(const char *path) {
    char tmp[4096];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/**
 * Remove file
 */
void remove_file(const char *path) {
    unlink(path);
}

/**
 * Remove directory (recursive)
 */
void remove_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        if (is_directory(full_path)) {
            remove_directory(full_path);
        } else {
            remove_file(full_path);
        }
    }

    closedir(dir);
    rmdir(path);
}

/**
 * Update operation progress
 */
void operation_update_progress(file_operation_t *op, uint64_t bytes_processed) {
    if (!op) return;

    pthread_mutex_lock(&op->mutex);
    op->bytes_done += bytes_processed;
    pthread_mutex_unlock(&op->mutex);

    // Update progress dialog if exists
    if (op->dialog && op->show_progress) {
        progress_dialog_update(op->dialog, op);
    }

    // Call progress callback
    if (op->on_progress) {
        op->on_progress(op, op->userdata);
    }
}

/**
 * Get operation progress (0.0 to 1.0)
 */
float operation_get_progress(file_operation_t *op) {
    if (!op || op->total_bytes == 0) return 0.0f;

    pthread_mutex_lock(&op->mutex);
    float progress = (float)op->bytes_done / (float)op->total_bytes;
    pthread_mutex_unlock(&op->mutex);

    return progress;
}

/**
 * Get operation speed (bytes per second)
 */
uint64_t operation_get_speed(file_operation_t *op) {
    if (!op || op->start_time == 0) return 0;

    uint64_t elapsed = get_time_us() - op->start_time;
    if (elapsed == 0) return 0;

    pthread_mutex_lock(&op->mutex);
    uint64_t bytes = op->bytes_done;
    pthread_mutex_unlock(&op->mutex);

    return (bytes * 1000000) / elapsed;  // bytes per second
}

/**
 * Get estimated time remaining (seconds)
 */
uint64_t operation_get_eta(file_operation_t *op) {
    if (!op) return 0;

    uint64_t speed = operation_get_speed(op);
    if (speed == 0) return 0;

    pthread_mutex_lock(&op->mutex);
    uint64_t remaining_bytes = op->total_bytes - op->bytes_done;
    pthread_mutex_unlock(&op->mutex);

    return remaining_bytes / speed;
}

/**
 * Copy single file with progress
 */
void operation_copy_file(const char *source, const char *dest, file_operation_t *op) {
    if (!source || !dest) return;

    int src_fd = open(source, O_RDONLY);
    if (src_fd < 0) {
        snprintf(op->error_message, sizeof(op->error_message),
                 "Failed to open source file: %s", source);
        op->error_code = errno;
        return;
    }

    int dest_fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dest_fd < 0) {
        snprintf(op->error_message, sizeof(op->error_message),
                 "Failed to create destination file: %s", dest);
        op->error_code = errno;
        close(src_fd);
        return;
    }

    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        snprintf(op->error_message, sizeof(op->error_message),
                 "Memory allocation failed");
        op->error_code = ENOMEM;
        close(src_fd);
        close(dest_fd);
        return;
    }

    ssize_t bytes_read, bytes_written;
    while ((bytes_read = read(src_fd, buffer, BUFFER_SIZE)) > 0) {
        // Check for cancellation
        if (op->cancel_requested) {
            close(src_fd);
            close(dest_fd);
            free(buffer);
            unlink(dest);  // Remove partial file
            return;
        }

        // Check for pause
        while (op->pause_requested) {
            op->paused = true;
            usleep(100000);  // Sleep 100ms
            if (op->cancel_requested) {
                close(src_fd);
                close(dest_fd);
                free(buffer);
                unlink(dest);
                return;
            }
        }
        op->paused = false;

        bytes_written = write(dest_fd, buffer, bytes_read);
        if (bytes_written != bytes_read) {
            snprintf(op->error_message, sizeof(op->error_message),
                     "Write error: %s", dest);
            op->error_code = errno;
            close(src_fd);
            close(dest_fd);
            free(buffer);
            unlink(dest);
            return;
        }

        operation_update_progress(op, bytes_written);
    }

    if (bytes_read < 0) {
        snprintf(op->error_message, sizeof(op->error_message),
                 "Read error: %s", source);
        op->error_code = errno;
    }

    close(src_fd);
    close(dest_fd);
    free(buffer);

    pthread_mutex_lock(&op->mutex);
    op->files_done++;
    pthread_mutex_unlock(&op->mutex);
}

/**
 * Copy directory recursively
 */
void operation_copy_directory(const char *source, const char *dest, file_operation_t *op) {
    if (!source || !dest) return;

    // Create destination directory
    create_directory(dest);

    DIR *dir = opendir(source);
    if (!dir) {
        snprintf(op->error_message, sizeof(op->error_message),
                 "Failed to open directory: %s", source);
        op->error_code = errno;
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (op->cancel_requested) {
            closedir(dir);
            return;
        }

        char src_path[4096];
        char dst_path[4096];
        snprintf(src_path, sizeof(src_path), "%s/%s", source, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dest, entry->d_name);

        if (is_directory(src_path)) {
            operation_copy_directory(src_path, dst_path, op);
        } else {
            operation_copy_file(src_path, dst_path, op);
        }
    }

    closedir(dir);
}

/**
 * Copy operation thread worker
 */
static void* copy_operation_worker(void *arg) {
    file_operation_t *op = (file_operation_t*)arg;

    op->status = OP_STATUS_RUNNING;
    op->start_time = get_time_us();

    // Process each source
    for (uint32_t i = 0; i < op->source_count; i++) {
        if (op->cancel_requested) {
            op->status = OP_STATUS_CANCELLED;
            goto cleanup;
        }

        const char *source = op->source_paths[i];

        // Build destination path
        char dest_path[4096];
        const char *filename = strrchr(source, '/');
        filename = filename ? filename + 1 : source;
        snprintf(dest_path, sizeof(dest_path), "%s/%s", op->destination, filename);

        // Copy file or directory
        if (is_directory(source)) {
            operation_copy_directory(source, dest_path, op);
        } else {
            operation_copy_file(source, dest_path, op);
        }

        if (op->error_code != 0) {
            op->status = OP_STATUS_ERROR;
            goto cleanup;
        }
    }

    op->status = OP_STATUS_COMPLETED;
    op->end_time = get_time_us();

    // Call completion callback
    if (op->on_complete) {
        op->on_complete(op, op->userdata);
    }

cleanup:
    op->thread_running = false;
    return NULL;
}

/**
 * Create copy operation
 */
file_operation_t* operation_copy_files(const char **sources, uint32_t count, const char *dest) {
    if (!sources || count == 0 || !dest) return NULL;

    file_operation_t *op = operation_create(OP_COPY);
    if (!op) return NULL;

    // Copy source paths
    op->source_count = count;
    for (uint32_t i = 0; i < count && i < 256; i++) {
        strncpy(op->source_paths[i], sources[i], sizeof(op->source_paths[i]) - 1);
    }

    strncpy(op->destination, dest, sizeof(op->destination) - 1);

    // Calculate total size and file count
    op->total_bytes = 0;
    op->files_total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (is_directory(sources[i])) {
            op->total_bytes += get_directory_size(sources[i]);
            // TODO: Count files in directory
            op->files_total += 1;
        } else {
            op->total_bytes += get_file_size(sources[i]);
            op->files_total += 1;
        }
    }

    // Create progress dialog
    if (op->show_progress) {
        op->dialog = progress_dialog_create("Copying files...");
        if (op->dialog) {
            op->dialog->bytes_total = op->total_bytes;
            op->dialog->files_total = op->files_total;
            progress_dialog_show(op->dialog);
        }
    }

    return op;
}

/**
 * Create move operation (copy then delete)
 */
file_operation_t* operation_move_files(const char **sources, uint32_t count, const char *dest) {
    if (!sources || count == 0 || !dest) return NULL;

    file_operation_t *op = operation_create(OP_MOVE);
    if (!op) return NULL;

    // Copy source paths
    op->source_count = count;
    for (uint32_t i = 0; i < count && i < 256; i++) {
        strncpy(op->source_paths[i], sources[i], sizeof(op->source_paths[i]) - 1);
    }

    strncpy(op->destination, dest, sizeof(op->destination) - 1);

    // Calculate total size
    op->total_bytes = 0;
    op->files_total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (is_directory(sources[i])) {
            op->total_bytes += get_directory_size(sources[i]);
            op->files_total += 1;
        } else {
            op->total_bytes += get_file_size(sources[i]);
            op->files_total += 1;
        }
    }

    // Double the total for copy + delete
    op->total_bytes *= 2;

    // Create progress dialog
    if (op->show_progress) {
        op->dialog = progress_dialog_create("Moving files...");
        if (op->dialog) {
            op->dialog->bytes_total = op->total_bytes;
            op->dialog->files_total = op->files_total;
            progress_dialog_show(op->dialog);
        }
    }

    return op;
}

/**
 * Delete operation worker
 */
static void* delete_operation_worker(void *arg) {
    file_operation_t *op = (file_operation_t*)arg;

    op->status = OP_STATUS_RUNNING;
    op->start_time = get_time_us();

    for (uint32_t i = 0; i < op->source_count; i++) {
        if (op->cancel_requested) {
            op->status = OP_STATUS_CANCELLED;
            goto cleanup;
        }

        const char *path = op->source_paths[i];

        if (is_directory(path)) {
            remove_directory(path);
        } else {
            remove_file(path);
        }

        pthread_mutex_lock(&op->mutex);
        op->files_done++;
        pthread_mutex_unlock(&op->mutex);
    }

    op->status = OP_STATUS_COMPLETED;
    op->end_time = get_time_us();

    if (op->on_complete) {
        op->on_complete(op, op->userdata);
    }

cleanup:
    op->thread_running = false;
    return NULL;
}

/**
 * Create delete operation
 */
file_operation_t* operation_delete_files(const char **paths, uint32_t count, bool permanent) {
    if (!paths || count == 0) return NULL;

    file_operation_t *op = operation_create(OP_DELETE);
    if (!op) return NULL;

    // Copy paths
    op->source_count = count;
    for (uint32_t i = 0; i < count && i < 256; i++) {
        strncpy(op->source_paths[i], paths[i], sizeof(op->source_paths[i]) - 1);
    }

    // Calculate total
    op->files_total = count;

    // Create progress dialog
    if (op->show_progress) {
        op->dialog = progress_dialog_create(permanent ? "Deleting files..." : "Moving to trash...");
        if (op->dialog) {
            op->dialog->files_total = op->files_total;
            progress_dialog_show(op->dialog);
        }
    }

    return op;
}

/**
 * Start operation (spawn worker thread)
 */
void operation_start(file_operation_t *op) {
    if (!op || op->thread_running) return;

    void* (*worker)(void*) = NULL;

    switch (op->type) {
        case OP_COPY:
            worker = copy_operation_worker;
            break;
        case OP_MOVE:
            worker = copy_operation_worker;  // TODO: implement move worker
            break;
        case OP_DELETE:
            worker = delete_operation_worker;
            break;
        default:
            return;
    }

    op->thread_running = true;
    pthread_create(&op->thread, NULL, worker, op);
}

/**
 * Pause operation
 */
void operation_pause(file_operation_t *op) {
    if (!op) return;
    op->pause_requested = true;
}

/**
 * Resume operation
 */
void operation_resume(file_operation_t *op) {
    if (!op) return;
    op->pause_requested = false;
}

/**
 * Cancel operation
 */
void operation_cancel(file_operation_t *op) {
    if (!op) return;
    op->cancel_requested = true;
}

/**
 * Wait for operation to complete
 */
void operation_wait(file_operation_t *op) {
    if (!op || !op->thread_running) return;
    pthread_join(op->thread, NULL);
}

/**
 * Create progress dialog
 */
progress_dialog_t* progress_dialog_create(const char *title) {
    progress_dialog_t *dialog = calloc(1, sizeof(progress_dialog_t));
    if (!dialog) return NULL;

    strncpy(dialog->title, title, sizeof(dialog->title) - 1);
    dialog->visible = false;
    dialog->progress = 0.0f;
    dialog->cancellable = true;
    dialog->pausable = true;
    dialog->show_details = false;

    return dialog;
}

/**
 * Destroy progress dialog
 */
void progress_dialog_destroy(progress_dialog_t *dialog) {
    if (!dialog) return;
    // TODO: Close window
    free(dialog);
}

/**
 * Show progress dialog
 */
void progress_dialog_show(progress_dialog_t *dialog) {
    if (!dialog) return;
    dialog->visible = true;
    dialog->start_time = get_time_us();
}

/**
 * Hide progress dialog
 */
void progress_dialog_hide(progress_dialog_t *dialog) {
    if (!dialog) return;
    dialog->visible = false;
}

/**
 * Update progress dialog
 */
void progress_dialog_update(progress_dialog_t *dialog, file_operation_t *op) {
    if (!dialog || !op) return;

    dialog->progress = operation_get_progress(op);
    dialog->bytes_done = op->bytes_done;
    dialog->bytes_total = op->total_bytes;
    dialog->files_done = op->files_done;
    dialog->files_total = op->files_total;

    dialog->bytes_per_second = operation_get_speed(op);
    dialog->estimated_remaining_seconds = operation_get_eta(op);

    dialog->last_update_time = get_time_us();
}

/**
 * Ask user for conflict resolution
 */
conflict_resolution_t ask_conflict_resolution(const char *path, bool *apply_to_all) {
    // TODO: Show dialog and get user input
    // For now, default to overwrite
    *apply_to_all = false;
    return CONFLICT_OVERWRITE;
}

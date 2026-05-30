# File Explorer API Documentation

Comprehensive API reference for File Explorer development.

## Table of Contents

1. [Core Explorer API](#core-explorer-api)
2. [File Types API](#file-types-api)
3. [Operations API](#operations-api)
4. [Search API](#search-api)
5. [Preview API](#preview-api)
6. [View Modes API](#view-modes-api)
7. [Drag and Drop API](#drag-and-drop-api)
8. [Properties API](#properties-api)

---

## Core Explorer API

### Explorer Creation

```c
file_explorer_t* explorer_create(compositor_t *compositor, 
                                 const char *initial_path);
```

Creates a new file explorer instance.

**Parameters:**
- `compositor`: Compositor context for rendering
- `initial_path`: Initial directory to display (NULL = home)

**Returns:** Explorer instance or NULL on failure

**Example:**
```c
compositor_t *comp = compositor_init("/dev/gpu0");
file_explorer_t *explorer = explorer_create(comp, "/home/user/documents");
```

### Explorer Destruction

```c
void explorer_destroy(file_explorer_t *explorer);
```

Destroys explorer and frees resources.

### Main Loop

```c
void explorer_run(file_explorer_t *explorer);
```

Runs the main event loop. Blocks until window is closed.

**Example:**
```c
explorer_run(explorer);  // Blocks here
explorer_destroy(explorer);
```

---

## Navigation API

### Navigate to Path

```c
void explorer_navigate_to(file_explorer_t *explorer, const char *path);
```

Navigates to specified directory.

**Parameters:**
- `path`: Absolute path to navigate to

**Example:**
```c
explorer_navigate_to(explorer, "/usr/local/bin");
```

### Navigation History

```c
void explorer_navigate_back(file_explorer_t *explorer);
void explorer_navigate_forward(file_explorer_t *explorer);
void explorer_navigate_parent(file_explorer_t *explorer);
void explorer_navigate_home(file_explorer_t *explorer);
```

Navigate through history or special locations.

---

## File Types API

### Type Detection

```c
file_type_t detect_file_type(const char *filename, bool is_directory);
```

Detects file type from filename and directory flag.

**Returns:** One of:
- `FILE_TYPE_FOLDER`
- `FILE_TYPE_IMAGE`
- `FILE_TYPE_VIDEO`
- `FILE_TYPE_AUDIO`
- `FILE_TYPE_TEXT`
- `FILE_TYPE_CODE`
- `FILE_TYPE_DOCUMENT`
- `FILE_TYPE_ARCHIVE`
- `FILE_TYPE_EXECUTABLE`
- `FILE_TYPE_UNKNOWN`

**Example:**
```c
file_type_t type = detect_file_type("photo.jpg", false);
// type == FILE_TYPE_IMAGE
```

### Type Information

```c
const file_type_info_t* get_file_type_info(file_type_t type);
```

Gets detailed information about file type.

**Returns:** Pointer to file type info with:
- `category_name`: Human-readable name
- `icon_name`: Icon identifier
- `color`: ARGB32 color
- `extensions`: Array of extensions

**Example:**
```c
const file_type_info_t *info = get_file_type_info(FILE_TYPE_IMAGE);
printf("Category: %s, Icon: %s\n", info->category_name, info->icon_name);
```

### File Size Formatting

```c
void format_file_size(uint64_t size, char *buf, size_t buf_size);
```

Formats byte size for display.

**Example:**
```c
char size_str[64];
format_file_size(2048576, size_str, sizeof(size_str));
// size_str = "2.0 MB"
```

### Extension Utilities

```c
const char* get_file_extension(const char *filename);
bool has_extension(const char *filename, const char *ext);
```

Get or check file extension.

**Example:**
```c
const char *ext = get_file_extension("document.pdf");
// ext = "pdf"

if (has_extension("photo.jpg", "jpg")) {
    // Handle JPEG
}
```

---

## Operations API

### Copy Files

```c
file_operation_t* operation_copy_files(const char **sources, 
                                      uint32_t count,
                                      const char *dest);
```

Creates a copy operation.

**Parameters:**
- `sources`: Array of source paths
- `count`: Number of sources
- `dest`: Destination directory

**Returns:** Operation handle

**Example:**
```c
const char *files[] = {"/home/file1.txt", "/home/file2.txt"};
file_operation_t *op = operation_copy_files(files, 2, "/tmp");

// Start operation in background
operation_start(op);

// Wait for completion
operation_wait(op);

operation_destroy(op);
```

### Move Files

```c
file_operation_t* operation_move_files(const char **sources,
                                      uint32_t count, 
                                      const char *dest);
```

Creates a move operation.

### Delete Files

```c
file_operation_t* operation_delete_files(const char **paths, 
                                        uint32_t count,
                                        bool permanent);
```

Creates a delete operation.

**Parameters:**
- `permanent`: If false, moves to trash

**Example:**
```c
const char *files[] = {"/tmp/old_file.txt"};
file_operation_t *op = operation_delete_files(files, 1, false);  // To trash
operation_start(op);
```

### Compress Files

```c
file_operation_t* operation_compress_files(const char **paths,
                                          uint32_t count,
                                          const char *archive_path,
                                          compression_type_t type);
```

Creates a compression operation.

**Compression Types:**
- `COMPRESS_ZIP`
- `COMPRESS_TAR_GZ`
- `COMPRESS_TAR_BZ2`
- `COMPRESS_7Z`

**Example:**
```c
const char *files[] = {"/home/docs/report.pdf", "/home/docs/data.xlsx"};
file_operation_t *op = operation_compress_files(
    files, 2,
    "/home/archive.zip",
    COMPRESS_ZIP
);
operation_start(op);
```

### Operation Control

```c
void operation_pause(file_operation_t *op);
void operation_resume(file_operation_t *op);
void operation_cancel(file_operation_t *op);
```

Control running operations.

### Progress Tracking

```c
float operation_get_progress(file_operation_t *op);
uint64_t operation_get_speed(file_operation_t *op);
uint64_t operation_get_eta(file_operation_t *op);
```

Get operation progress information.

**Example:**
```c
float progress = operation_get_progress(op);  // 0.0 to 1.0
printf("Progress: %.1f%%\n", progress * 100);

uint64_t speed = operation_get_speed(op);  // bytes per second
printf("Speed: %lu MB/s\n", speed / 1024 / 1024);

uint64_t eta = operation_get_eta(op);  // seconds remaining
printf("ETA: %lu seconds\n", eta);
```

---

## Search API

### Index Creation

```c
search_index_t* index_create(const char *path);
void index_destroy(search_index_t *index);
```

Creates and destroys search index.

**Example:**
```c
search_index_t *index = index_create("/home/user");
index_update(index);  // Build index

// ... use index ...

index_destroy(index);
```

### Index Update

```c
void index_update(search_index_t *index);
void index_start_background_update(search_index_t *index);
```

Updates index with current file system state.

### Query Creation

```c
search_query_t* query_create(const char *text);
void query_destroy(search_query_t *query);
```

Creates search query.

**Example:**
```c
search_query_t *query = query_create("*.jpg");
query_set_type_filter(query, FILTER_IMAGES);
query_set_date_filter(query, DATE_THIS_WEEK);
```

### Query Configuration

```c
void query_set_type_filter(search_query_t *query, file_type_filter_t filter);
void query_set_size_range(search_query_t *query, uint64_t min, uint64_t max);
void query_set_date_filter(search_query_t *query, date_filter_t filter);
void query_set_path(search_query_t *query, const char *path);
```

Configure search filters.

**Type Filters:**
- `FILTER_ALL_FILES`
- `FILTER_DOCUMENTS`
- `FILTER_IMAGES`
- `FILTER_VIDEOS`
- `FILTER_AUDIO`
- `FILTER_ARCHIVES`
- `FILTER_CODE`
- `FILTER_EXECUTABLES`

**Date Filters:**
- `DATE_ANY`
- `DATE_TODAY`
- `DATE_YESTERDAY`
- `DATE_THIS_WEEK`
- `DATE_THIS_MONTH`
- `DATE_THIS_YEAR`
- `DATE_CUSTOM`

### Execute Search

```c
search_results_t* search_execute(search_index_t *index, 
                                 const search_query_t *query);
void search_results_destroy(search_results_t *results);
```

Execute search and get results.

**Example:**
```c
search_query_t *query = query_create("report");
query_set_type_filter(query, FILTER_DOCUMENTS);
query_set_date_filter(query, DATE_THIS_MONTH);

search_results_t *results = search_execute(index, query);

printf("Found %u matches in %lu µs\n", 
       results->count, results->search_time_us);

for (uint32_t i = 0; i < results->count; i++) {
    search_result_t *result = &results->results[i];
    printf("  %s (%s, score: %u)\n",
           result->file.name,
           result->file.path,
           result->match_score);
}

search_results_destroy(results);
query_destroy(query);
```

### Result Sorting

```c
void results_sort_by_name(search_results_t *results, bool ascending);
void results_sort_by_date(search_results_t *results, bool ascending);
void results_sort_by_size(search_results_t *results, bool ascending);
void results_sort_by_relevance(search_results_t *results);
```

Sort search results.

---

## Preview API

### Preview Window

```c
preview_window_t* preview_create(void);
void preview_destroy(preview_window_t *preview);
void preview_show(preview_window_t *preview, const char *file_path);
void preview_hide(preview_window_t *preview);
```

Create and control preview window (Quick Look).

**Example:**
```c
preview_window_t *preview = preview_create();
preview_show(preview, "/home/photo.jpg");

// ... user views preview ...

preview_hide(preview);
preview_destroy(preview);
```

### Zoom and Pan

```c
void preview_zoom_in(preview_window_t *preview);
void preview_zoom_out(preview_window_t *preview);
void preview_zoom_fit(preview_window_t *preview);
void preview_zoom_actual(preview_window_t *preview);
void preview_pan(preview_window_t *preview, int32_t dx, int32_t dy);
```

Control preview display.

### Playback Control

```c
void preview_play(preview_window_t *preview);
void preview_pause(preview_window_t *preview);
void preview_stop(preview_window_t *preview);
void preview_seek(preview_window_t *preview, uint64_t position_ms);
```

Control video/audio playback.

---

## View Modes API

### Set View Mode

```c
void explorer_set_view_mode(file_explorer_t *explorer, view_mode_t mode);
```

Change view mode.

**Modes:**
- `VIEW_MODE_ICONS` - Grid of icons with thumbnails
- `VIEW_MODE_LIST` - Detailed list view
- `VIEW_MODE_COLUMNS` - Multi-column browser
- `VIEW_MODE_GALLERY` - Large thumbnails

**Example:**
```c
explorer_set_view_mode(explorer, VIEW_MODE_LIST);
```

### Sort Mode

```c
void explorer_set_sort_mode(file_explorer_t *explorer, sort_mode_t mode);
```

Change sort order.

**Sort Modes:**
- `SORT_NAME_ASC` / `SORT_NAME_DESC`
- `SORT_SIZE_ASC` / `SORT_SIZE_DESC`
- `SORT_DATE_ASC` / `SORT_DATE_DESC`
- `SORT_TYPE_ASC` / `SORT_TYPE_DESC`

---

## Drag and Drop API

### DnD Context

```c
dnd_context_t* dnd_create(void);
void dnd_destroy(dnd_context_t *dnd);
```

Create drag and drop context.

### Start Drag

```c
void dnd_start(dnd_context_t *dnd, const char **paths, uint32_t count,
               int32_t x, int32_t y);
```

Start drag operation.

**Example:**
```c
dnd_context_t *dnd = dnd_create();

const char *files[] = {"/home/file1.txt", "/home/file2.txt"};
dnd_start(dnd, files, 2, mouse_x, mouse_y);

// Update as mouse moves
dnd_update(dnd, new_x, new_y);

// Set drop target
dnd_set_target(dnd, "/tmp");

// Check if valid
if (dnd_can_drop(dnd)) {
    dnd_finish(dnd);  // Perform operation
} else {
    dnd_cancel(dnd);
}

dnd_destroy(dnd);
```

---

## Properties API

### Properties Dialog

```c
properties_dialog_t* properties_dialog_create(void);
void properties_dialog_destroy(properties_dialog_t *dialog);
void properties_dialog_show(properties_dialog_t *dialog, 
                            const file_entry_t *file);
```

Show file properties dialog.

**Example:**
```c
properties_dialog_t *props = properties_dialog_create();
properties_dialog_show(props, &file_entry);

// Dialog shows:
// - General (size, dates, type)
// - Permissions (rwx)
// - Metadata (EXIF, ID3, etc.)

properties_dialog_destroy(props);
```

---

## Data Structures

### file_entry_t

```c
typedef struct {
    char name[256];
    char path[4096];
    uint64_t size;
    uint64_t modified_time;
    uint64_t created_time;
    uint64_t accessed_time;
    uint32_t permissions;
    uint32_t uid;
    uint32_t gid;
    bool is_directory;
    bool is_hidden;
    bool is_symlink;
    bool is_readonly;
    file_type_t type;
} file_entry_t;
```

Complete file information.

### rect_t

```c
typedef struct {
    int32_t x, y;
    int32_t w, h;
} rect_t;
```

Rectangle for UI layout.

---

## Integration Example

Complete example integrating multiple APIs:

```c
#include "explorer.h"
#include "search.h"
#include "operations.h"

int main(void) {
    // Initialize
    compositor_t *comp = compositor_init("/dev/gpu0");
    file_explorer_t *explorer = explorer_create(comp, "/home/user");
    
    // Create search index
    search_index_t *index = index_create("/home/user");
    index_start_background_update(index);
    
    // Search for large images
    search_query_t *query = query_create("*.jpg");
    query_set_type_filter(query, FILTER_IMAGES);
    query_set_size_range(query, 1024*1024, UINT64_MAX);  // > 1MB
    
    search_results_t *results = search_execute(index, query);
    printf("Found %u large images\n", results->count);
    
    // Copy found images to new folder
    const char *paths[results->count];
    for (uint32_t i = 0; i < results->count; i++) {
        paths[i] = results->results[i].file.path;
    }
    
    file_operation_t *op = operation_copy_files(paths, results->count, 
                                                "/home/user/large_photos");
    operation_start(op);
    
    // Monitor progress
    while (op->status == OP_STATUS_RUNNING) {
        printf("\rProgress: %.1f%%", operation_get_progress(op) * 100);
        usleep(100000);
    }
    
    // Cleanup
    operation_destroy(op);
    search_results_destroy(results);
    query_destroy(query);
    index_destroy(index);
    
    // Run explorer UI
    explorer_run(explorer);
    
    explorer_destroy(explorer);
    compositor_cleanup(comp);
    
    return 0;
}
```

---

## Performance Considerations

### Thumbnail Generation

- **Async generation**: Use background threads
- **Caching**: Cache to `~/.cache/thumbnails/`
- **Size limits**: Don't generate for files > 50MB

### Search Indexing

- **Background**: Index on startup, don't block UI
- **Incremental**: Only re-index changed files
- **Statistics**: Track index size and time

### File Operations

- **Threading**: All operations run in background threads
- **Cancellation**: Check `cancel_requested` frequently
- **Progress**: Update every 1MB or 100ms

---

**Version**: 1.0.0  
**Last Updated**: 2026-05-26

/**
 * File Explorer - File Types and Icons
 *
 * Defines file type detection and icon mapping for the file explorer.
 */

#ifndef FILE_TYPES_H
#define FILE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * File type categories
 */
typedef enum {
    FILE_TYPE_UNKNOWN,
    FILE_TYPE_FOLDER,
    FILE_TYPE_TEXT,
    FILE_TYPE_IMAGE,
    FILE_TYPE_VIDEO,
    FILE_TYPE_AUDIO,
    FILE_TYPE_DOCUMENT,
    FILE_TYPE_ARCHIVE,
    FILE_TYPE_EXECUTABLE,
    FILE_TYPE_CODE,
    FILE_TYPE_SPREADSHEET,
    FILE_TYPE_PRESENTATION,
    FILE_TYPE_PDF,
    FILE_TYPE_DISK_IMAGE,
    FILE_TYPE_DATABASE,
    FILE_TYPE_FONT,
    FILE_TYPE_SYSTEM,
    FILE_TYPE_COUNT
} file_type_t;

/**
 * File type information
 */
typedef struct {
    file_type_t type;
    const char *category_name;
    const char *icon_name;      // Icon identifier
    uint32_t color;             // ARGB color for icon/highlight
    const char **extensions;    // Null-terminated array of extensions
} file_type_info_t;

/**
 * File metadata
 */
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

/**
 * Thumbnail cache entry
 */
typedef struct {
    char path[4096];
    uint32_t *pixels;           // ARGB32 format
    uint32_t width;
    uint32_t height;
    uint64_t timestamp;         // File modification time
    bool valid;
} thumbnail_t;

// File type detection
file_type_t detect_file_type(const char *filename, bool is_directory);
const file_type_info_t* get_file_type_info(file_type_t type);
const char* get_file_icon(const char *filename, bool is_directory);
uint32_t get_file_color(file_type_t type);

// File size formatting
void format_file_size(uint64_t size, char *buf, size_t buf_size);

// Extension utilities
const char* get_file_extension(const char *filename);
bool has_extension(const char *filename, const char *ext);

// MIME type detection
const char* get_mime_type(const char *filename);

// Preview support
bool can_preview_file(file_type_t type);
bool can_generate_thumbnail(file_type_t type);

// Icon names (for UI rendering)
#define ICON_FOLDER          "folder"
#define ICON_FILE_TEXT       "file-text"
#define ICON_FILE_IMAGE      "file-image"
#define ICON_FILE_VIDEO      "file-video"
#define ICON_FILE_AUDIO      "file-audio"
#define ICON_FILE_CODE       "file-code"
#define ICON_FILE_PDF        "file-pdf"
#define ICON_FILE_ARCHIVE    "file-archive"
#define ICON_FILE_BINARY     "file-binary"
#define ICON_FILE_UNKNOWN    "file-unknown"

// Colors (ARGB32)
#define COLOR_FOLDER         0xFF4A90E2  // Blue
#define COLOR_TEXT           0xFF90A4AE  // Gray
#define COLOR_IMAGE          0xFF66BB6A  // Green
#define COLOR_VIDEO          0xFFE57373  // Red
#define COLOR_AUDIO          0xFF9575CD  // Purple
#define COLOR_CODE           0xFFFF9800  // Orange
#define COLOR_DOCUMENT       0xFF42A5F5  // Light blue
#define COLOR_ARCHIVE        0xFF8D6E63  // Brown
#define COLOR_EXECUTABLE     0xFF26C6DA  // Cyan
#define COLOR_UNKNOWN        0xFFBDBDBD  // Light gray

#endif // FILE_TYPES_H

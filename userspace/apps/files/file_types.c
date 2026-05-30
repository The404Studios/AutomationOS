/**
 * File Explorer - File Types and Icons Implementation
 */

#include "file_types.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// Extension arrays for each type
static const char *text_extensions[] = {
    "txt", "md", "log", "conf", "cfg", "ini", "json", "xml", "yaml", "yml", NULL
};

static const char *image_extensions[] = {
    "png", "jpg", "jpeg", "gif", "bmp", "ico", "svg", "webp", "tiff", "tif", NULL
};

static const char *video_extensions[] = {
    "mp4", "avi", "mkv", "mov", "wmv", "flv", "webm", "m4v", "mpeg", "mpg", NULL
};

static const char *audio_extensions[] = {
    "mp3", "wav", "ogg", "flac", "aac", "m4a", "wma", "opus", NULL
};

static const char *document_extensions[] = {
    "doc", "docx", "odt", "rtf", NULL
};

static const char *archive_extensions[] = {
    "zip", "tar", "gz", "bz2", "7z", "rar", "xz", "tgz", NULL
};

static const char *executable_extensions[] = {
    "exe", "bin", "elf", "app", "sh", "bat", "cmd", NULL
};

static const char *code_extensions[] = {
    "c", "cpp", "h", "hpp", "java", "py", "js", "ts", "go", "rs", "rb", "php",
    "cs", "swift", "kt", "scala", "lua", "pl", "asm", "s", NULL
};

static const char *spreadsheet_extensions[] = {
    "xls", "xlsx", "ods", "csv", NULL
};

static const char *presentation_extensions[] = {
    "ppt", "pptx", "odp", NULL
};

static const char *disk_image_extensions[] = {
    "iso", "img", "dmg", "vdi", "vmdk", NULL
};

static const char *database_extensions[] = {
    "db", "sqlite", "sqlite3", "sql", "mdb", NULL
};

static const char *font_extensions[] = {
    "ttf", "otf", "woff", "woff2", "eot", NULL
};

// File type info table
static const file_type_info_t file_type_table[FILE_TYPE_COUNT] = {
    [FILE_TYPE_UNKNOWN] = {
        .type = FILE_TYPE_UNKNOWN,
        .category_name = "Unknown",
        .icon_name = ICON_FILE_UNKNOWN,
        .color = COLOR_UNKNOWN,
        .extensions = NULL
    },
    [FILE_TYPE_FOLDER] = {
        .type = FILE_TYPE_FOLDER,
        .category_name = "Folder",
        .icon_name = ICON_FOLDER,
        .color = COLOR_FOLDER,
        .extensions = NULL
    },
    [FILE_TYPE_TEXT] = {
        .type = FILE_TYPE_TEXT,
        .category_name = "Text",
        .icon_name = ICON_FILE_TEXT,
        .color = COLOR_TEXT,
        .extensions = text_extensions
    },
    [FILE_TYPE_IMAGE] = {
        .type = FILE_TYPE_IMAGE,
        .category_name = "Image",
        .icon_name = ICON_FILE_IMAGE,
        .color = COLOR_IMAGE,
        .extensions = image_extensions
    },
    [FILE_TYPE_VIDEO] = {
        .type = FILE_TYPE_VIDEO,
        .category_name = "Video",
        .icon_name = ICON_FILE_VIDEO,
        .color = COLOR_VIDEO,
        .extensions = video_extensions
    },
    [FILE_TYPE_AUDIO] = {
        .type = FILE_TYPE_AUDIO,
        .category_name = "Audio",
        .icon_name = ICON_FILE_AUDIO,
        .color = COLOR_AUDIO,
        .extensions = audio_extensions
    },
    [FILE_TYPE_DOCUMENT] = {
        .type = FILE_TYPE_DOCUMENT,
        .category_name = "Document",
        .icon_name = ICON_FILE_TEXT,
        .color = COLOR_DOCUMENT,
        .extensions = document_extensions
    },
    [FILE_TYPE_ARCHIVE] = {
        .type = FILE_TYPE_ARCHIVE,
        .category_name = "Archive",
        .icon_name = ICON_FILE_ARCHIVE,
        .color = COLOR_ARCHIVE,
        .extensions = archive_extensions
    },
    [FILE_TYPE_EXECUTABLE] = {
        .type = FILE_TYPE_EXECUTABLE,
        .category_name = "Executable",
        .icon_name = ICON_FILE_BINARY,
        .color = COLOR_EXECUTABLE,
        .extensions = executable_extensions
    },
    [FILE_TYPE_CODE] = {
        .type = FILE_TYPE_CODE,
        .category_name = "Source Code",
        .icon_name = ICON_FILE_CODE,
        .color = COLOR_CODE,
        .extensions = code_extensions
    },
    [FILE_TYPE_SPREADSHEET] = {
        .type = FILE_TYPE_SPREADSHEET,
        .category_name = "Spreadsheet",
        .icon_name = ICON_FILE_TEXT,
        .color = COLOR_DOCUMENT,
        .extensions = spreadsheet_extensions
    },
    [FILE_TYPE_PRESENTATION] = {
        .type = FILE_TYPE_PRESENTATION,
        .category_name = "Presentation",
        .icon_name = ICON_FILE_TEXT,
        .color = COLOR_DOCUMENT,
        .extensions = presentation_extensions
    },
    [FILE_TYPE_PDF] = {
        .type = FILE_TYPE_PDF,
        .category_name = "PDF Document",
        .icon_name = ICON_FILE_PDF,
        .color = COLOR_DOCUMENT,
        .extensions = (const char*[]){"pdf", NULL}
    },
    [FILE_TYPE_DISK_IMAGE] = {
        .type = FILE_TYPE_DISK_IMAGE,
        .category_name = "Disk Image",
        .icon_name = ICON_FILE_BINARY,
        .color = COLOR_ARCHIVE,
        .extensions = disk_image_extensions
    },
    [FILE_TYPE_DATABASE] = {
        .type = FILE_TYPE_DATABASE,
        .category_name = "Database",
        .icon_name = ICON_FILE_BINARY,
        .color = COLOR_DOCUMENT,
        .extensions = database_extensions
    },
    [FILE_TYPE_FONT] = {
        .type = FILE_TYPE_FONT,
        .category_name = "Font",
        .icon_name = ICON_FILE_BINARY,
        .color = COLOR_TEXT,
        .extensions = font_extensions
    },
    [FILE_TYPE_SYSTEM] = {
        .type = FILE_TYPE_SYSTEM,
        .category_name = "System File",
        .icon_name = ICON_FILE_BINARY,
        .color = COLOR_UNKNOWN,
        .extensions = (const char*[]){"sys", "dll", "so", "dylib", NULL}
    }
};

/**
 * Get file extension (returns pointer to extension or empty string)
 */
const char* get_file_extension(const char *filename) {
    if (!filename) return "";

    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "";

    return dot + 1;
}

/**
 * Check if file has specific extension (case-insensitive)
 */
bool has_extension(const char *filename, const char *ext) {
    if (!filename || !ext) return false;

    const char *file_ext = get_file_extension(filename);
    if (!*file_ext) return false;

    return strcasecmp(file_ext, ext) == 0;
}

/**
 * Detect file type from filename and directory flag
 */
file_type_t detect_file_type(const char *filename, bool is_directory) {
    if (!filename) return FILE_TYPE_UNKNOWN;

    if (is_directory) {
        return FILE_TYPE_FOLDER;
    }

    const char *ext = get_file_extension(filename);
    if (!*ext) return FILE_TYPE_UNKNOWN;

    // Check each file type
    for (int i = 0; i < FILE_TYPE_COUNT; i++) {
        const file_type_info_t *info = &file_type_table[i];
        if (!info->extensions) continue;

        for (const char **e = info->extensions; *e; e++) {
            if (strcasecmp(ext, *e) == 0) {
                return info->type;
            }
        }
    }

    return FILE_TYPE_UNKNOWN;
}

/**
 * Get file type information
 */
const file_type_info_t* get_file_type_info(file_type_t type) {
    if (type >= FILE_TYPE_COUNT) {
        return &file_type_table[FILE_TYPE_UNKNOWN];
    }
    return &file_type_table[type];
}

/**
 * Get icon name for file
 */
const char* get_file_icon(const char *filename, bool is_directory) {
    file_type_t type = detect_file_type(filename, is_directory);
    const file_type_info_t *info = get_file_type_info(type);
    return info->icon_name;
}

/**
 * Get color for file type
 */
uint32_t get_file_color(file_type_t type) {
    const file_type_info_t *info = get_file_type_info(type);
    return info->color;
}

/**
 * Format file size for display
 */
void format_file_size(uint64_t size, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;

    const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int unit = 0;
    double display_size = (double)size;

    while (display_size >= 1024.0 && unit < 5) {
        display_size /= 1024.0;
        unit++;
    }

    if (unit == 0) {
        snprintf(buf, buf_size, "%llu B", (unsigned long long)size);
    } else {
        snprintf(buf, buf_size, "%.1f %s", display_size, units[unit]);
    }
}

/**
 * Get MIME type for file
 */
const char* get_mime_type(const char *filename) {
    file_type_t type = detect_file_type(filename, false);

    switch (type) {
        case FILE_TYPE_TEXT:
            return "text/plain";
        case FILE_TYPE_IMAGE:
            if (has_extension(filename, "png")) return "image/png";
            if (has_extension(filename, "jpg") || has_extension(filename, "jpeg")) return "image/jpeg";
            if (has_extension(filename, "gif")) return "image/gif";
            if (has_extension(filename, "bmp")) return "image/bmp";
            if (has_extension(filename, "svg")) return "image/svg+xml";
            return "image/*";
        case FILE_TYPE_VIDEO:
            if (has_extension(filename, "mp4")) return "video/mp4";
            if (has_extension(filename, "webm")) return "video/webm";
            return "video/*";
        case FILE_TYPE_AUDIO:
            if (has_extension(filename, "mp3")) return "audio/mpeg";
            if (has_extension(filename, "ogg")) return "audio/ogg";
            if (has_extension(filename, "wav")) return "audio/wav";
            return "audio/*";
        case FILE_TYPE_PDF:
            return "application/pdf";
        case FILE_TYPE_ARCHIVE:
            if (has_extension(filename, "zip")) return "application/zip";
            if (has_extension(filename, "tar")) return "application/x-tar";
            return "application/x-compressed";
        case FILE_TYPE_CODE:
            if (has_extension(filename, "c") || has_extension(filename, "h")) return "text/x-c";
            if (has_extension(filename, "cpp") || has_extension(filename, "hpp")) return "text/x-c++";
            if (has_extension(filename, "py")) return "text/x-python";
            if (has_extension(filename, "js")) return "text/javascript";
            return "text/plain";
        case FILE_TYPE_EXECUTABLE:
            return "application/x-executable";
        default:
            return "application/octet-stream";
    }
}

/**
 * Check if file can be previewed
 */
bool can_preview_file(file_type_t type) {
    switch (type) {
        case FILE_TYPE_TEXT:
        case FILE_TYPE_IMAGE:
        case FILE_TYPE_VIDEO:
        case FILE_TYPE_AUDIO:
        case FILE_TYPE_PDF:
        case FILE_TYPE_CODE:
            return true;
        default:
            return false;
    }
}

/**
 * Check if thumbnail can be generated
 */
bool can_generate_thumbnail(file_type_t type) {
    switch (type) {
        case FILE_TYPE_IMAGE:
        case FILE_TYPE_VIDEO:
        case FILE_TYPE_PDF:
            return true;
        default:
            return false;
    }
}

// Case-insensitive string compare (implementation for systems without it)
int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

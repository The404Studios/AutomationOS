// AutomationOS IDE - Code Editor Component
#ifndef IDE_EDITOR_H
#define IDE_EDITOR_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_LINES 10000
#define MAX_LINE_LENGTH 1024
#define MAX_OPEN_FILES 32

// Language types
typedef enum {
    LANG_NONE,
    LANG_C,
    LANG_CPP,
    LANG_ASSEMBLY,
    LANG_PYTHON,
    LANG_JAVASCRIPT,
    LANG_MAKEFILE,
    LANG_BLUEPRINT
} language_type_t;

// File buffer
typedef struct {
    char *lines[MAX_LINES];
    int line_count;
    char *path;
    language_type_t language;
    bool modified;
    bool read_only;
} file_buffer_t;

// Cursor position
typedef struct {
    int line;
    int column;
} cursor_pos_t;

// Editor state
struct editor_state {
    file_buffer_t *buffers[MAX_OPEN_FILES];
    int buffer_count;
    int active_buffer;

    cursor_pos_t cursor;
    int scroll_offset;

    // Editor settings
    int tab_size;
    bool use_spaces;
    bool show_line_numbers;
    bool syntax_highlighting;
    bool auto_indent;
};

// Editor operations
int editor_open_file(struct editor_state *ed, const char *path);
int editor_save_file(struct editor_state *ed);
int editor_save_as(struct editor_state *ed, const char *path);
int editor_close_file(struct editor_state *ed);

// Navigation
void editor_move_cursor(struct editor_state *ed, int dx, int dy);
void editor_goto_line(struct editor_state *ed, int line);
void editor_page_up(struct editor_state *ed);
void editor_page_down(struct editor_state *ed);

// Editing operations
void editor_insert_char(struct editor_state *ed, char c);
void editor_delete_char(struct editor_state *ed);
void editor_insert_line(struct editor_state *ed);
void editor_delete_line(struct editor_state *ed);

// Search and replace
int editor_find(struct editor_state *ed, const char *pattern);
int editor_replace(struct editor_state *ed, const char *old, const char *new);

// Syntax highlighting
void editor_highlight_line(struct editor_state *ed, int line, char *output);
language_type_t editor_detect_language(const char *filename);

// Utility
void editor_render(struct editor_state *ed);
void editor_status_line(struct editor_state *ed, char *output);

#endif // IDE_EDITOR_H

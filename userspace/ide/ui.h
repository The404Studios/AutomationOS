// AutomationOS IDE - UI Component
#ifndef IDE_UI_H
#define IDE_UI_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_PANELS 16
#define MAX_MENU_ITEMS 64

// Panel types
typedef enum {
    PANEL_EDITOR,
    PANEL_BLUEPRINT,
    PANEL_PROJECT,
    PANEL_CONSOLE,
    PANEL_DEBUGGER,
    PANEL_VARIABLES,
    PANEL_CALLSTACK,
    PANEL_BREAKPOINTS,
    PANEL_OUTPUT,
    PANEL_TERMINAL
} panel_type_t;

// Panel state
typedef struct {
    panel_type_t type;
    char *title;
    int x, y;
    int width, height;
    bool visible;
    bool focused;
    void *data;  // Panel-specific data
} panel_t;

// Menu item
typedef struct {
    char *label;
    char *shortcut;
    void (*callback)(void *ctx);
    bool enabled;
    bool separator;
} menu_item_t;

// Menu
typedef struct {
    char *title;
    menu_item_t *items[MAX_MENU_ITEMS];
    int item_count;
    bool visible;
} menu_t;

// UI state
typedef struct {
    // Panels
    panel_t *panels[MAX_PANELS];
    int panel_count;
    int focused_panel;

    // Menus
    menu_t *file_menu;
    menu_t *edit_menu;
    menu_t *view_menu;
    menu_t *build_menu;
    menu_t *debug_menu;
    menu_t *tools_menu;
    menu_t *help_menu;

    // Theme
    uint32_t bg_color;
    uint32_t fg_color;
    uint32_t accent_color;
    uint32_t error_color;
    uint32_t warning_color;
    uint32_t success_color;

    // Layout
    int window_width;
    int window_height;
    int status_bar_height;
    int menu_bar_height;
    int toolbar_height;

    // Input state
    int mouse_x, mouse_y;
    bool mouse_down;
    uint32_t key_modifiers;
} ui_state_t;

// UI initialization
ui_state_t* ui_init(int width, int height);
void ui_cleanup(ui_state_t *ui);

// Panel management
panel_t* ui_add_panel(ui_state_t *ui, panel_type_t type, const char *title);
void ui_remove_panel(ui_state_t *ui, panel_t *panel);
void ui_show_panel(ui_state_t *ui, panel_type_t type);
void ui_hide_panel(ui_state_t *ui, panel_type_t type);
void ui_focus_panel(ui_state_t *ui, panel_t *panel);

// Menu management
menu_item_t* ui_add_menu_item(menu_t *menu, const char *label, void (*callback)(void*));
void ui_add_menu_separator(menu_t *menu);
void ui_show_menu(ui_state_t *ui, menu_t *menu);

// Rendering
void ui_render(ui_state_t *ui);
void ui_render_menu_bar(ui_state_t *ui);
void ui_render_toolbar(ui_state_t *ui);
void ui_render_status_bar(ui_state_t *ui);
void ui_render_panel(ui_state_t *ui, panel_t *panel);

// Input handling
void ui_handle_key(ui_state_t *ui, int key, uint32_t modifiers);
void ui_handle_mouse(ui_state_t *ui, int x, int y, bool down);
void ui_handle_scroll(ui_state_t *ui, int delta);

// Dialogs
void ui_show_message(const char *title, const char *message);
bool ui_confirm(const char *title, const char *message);
char* ui_prompt(const char *title, const char *prompt, const char *default_value);
char* ui_file_dialog(bool save, const char *filter);

// Status messages
void ui_status(const char *message);
void ui_error(const char *message);
void ui_warning(const char *message);
void ui_success(const char *message);

// Theme
void ui_set_theme(ui_state_t *ui, const char *name);
void ui_set_color(uint32_t *color, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

#endif // IDE_UI_H

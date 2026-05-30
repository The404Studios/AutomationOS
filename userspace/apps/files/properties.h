/**
 * File Explorer - Properties Dialog
 *
 * Shows detailed file/folder properties
 */

#ifndef PROPERTIES_H
#define PROPERTIES_H

#include <stdint.h>
#include <stdbool.h>
#include "file_types.h"

typedef struct properties_dialog properties_dialog_t;

/**
 * Properties tabs
 */
typedef enum {
    TAB_GENERAL,
    TAB_PERMISSIONS,
    TAB_METADATA,
    TAB_PREVIEW,
} properties_tab_t;

/**
 * Properties dialog
 */
struct properties_dialog {
    void *window;
    bool visible;

    file_entry_t file;
    properties_tab_t current_tab;

    // Permissions editing
    struct {
        uint32_t owner_read :1;
        uint32_t owner_write :1;
        uint32_t owner_execute :1;
        uint32_t group_read :1;
        uint32_t group_write :1;
        uint32_t group_execute :1;
        uint32_t other_read :1;
        uint32_t other_write :1;
        uint32_t other_execute :1;
    } permissions;

    bool permissions_modified;
};

// Dialog management
properties_dialog_t* properties_dialog_create(void);
void properties_dialog_destroy(properties_dialog_t *dialog);
void properties_dialog_show(properties_dialog_t *dialog, const file_entry_t *file);
void properties_dialog_hide(properties_dialog_t *dialog);

// Tab switching
void properties_set_tab(properties_dialog_t *dialog, properties_tab_t tab);

// Permissions
void properties_update_permissions(properties_dialog_t *dialog);
void properties_apply_permissions(properties_dialog_t *dialog);

// Rendering
void properties_render(properties_dialog_t *dialog);

#endif // PROPERTIES_H

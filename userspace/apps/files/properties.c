/**
 * File Explorer - Properties Dialog Implementation
 */

#include "properties.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

properties_dialog_t* properties_dialog_create(void) {
    properties_dialog_t *dialog = calloc(1, sizeof(properties_dialog_t));
    if (!dialog) return NULL;

    dialog->visible = false;
    dialog->current_tab = TAB_GENERAL;
    dialog->permissions_modified = false;

    return dialog;
}

void properties_dialog_destroy(properties_dialog_t *dialog) {
    if (!dialog) return;
    free(dialog);
}

void properties_dialog_show(properties_dialog_t *dialog, const file_entry_t *file) {
    if (!dialog || !file) return;

    dialog->file = *file;
    dialog->visible = true;

    // Parse permissions
    properties_update_permissions(dialog);
}

void properties_dialog_hide(properties_dialog_t *dialog) {
    if (!dialog) return;
    dialog->visible = false;
}

void properties_set_tab(properties_dialog_t *dialog, properties_tab_t tab) {
    if (!dialog) return;
    dialog->current_tab = tab;
}

void properties_update_permissions(properties_dialog_t *dialog) {
    if (!dialog) return;

    uint32_t mode = dialog->file.permissions;
    dialog->permissions.owner_read = (mode & S_IRUSR) != 0;
    dialog->permissions.owner_write = (mode & S_IWUSR) != 0;
    dialog->permissions.owner_execute = (mode & S_IXUSR) != 0;
    dialog->permissions.group_read = (mode & S_IRGRP) != 0;
    dialog->permissions.group_write = (mode & S_IWGRP) != 0;
    dialog->permissions.group_execute = (mode & S_IXGRP) != 0;
    dialog->permissions.other_read = (mode & S_IROTH) != 0;
    dialog->permissions.other_write = (mode & S_IWOTH) != 0;
    dialog->permissions.other_execute = (mode & S_IXOTH) != 0;
}

void properties_apply_permissions(properties_dialog_t *dialog) {
    if (!dialog || !dialog->permissions_modified) return;

    // Build mode from permissions
    uint32_t mode = 0;
    if (dialog->permissions.owner_read) mode |= S_IRUSR;
    if (dialog->permissions.owner_write) mode |= S_IWUSR;
    if (dialog->permissions.owner_execute) mode |= S_IXUSR;
    if (dialog->permissions.group_read) mode |= S_IRGRP;
    if (dialog->permissions.group_write) mode |= S_IWGRP;
    if (dialog->permissions.group_execute) mode |= S_IXGRP;
    if (dialog->permissions.other_read) mode |= S_IROTH;
    if (dialog->permissions.other_write) mode |= S_IWOTH;
    if (dialog->permissions.other_execute) mode |= S_IXOTH;

    // Apply to file
    chmod(dialog->file.path, mode);

    dialog->permissions_modified = false;
}

void properties_render(properties_dialog_t *dialog) {
    if (!dialog || !dialog->visible) return;

    // TODO: Render properties dialog based on current tab
}

/*
 * kernel/include/boot_ui.h - Boot UI progress tracking
 */

#ifndef BOOT_UI_H
#define BOOT_UI_H

void boot_banner(void);
void boot_stage(const char* stage);
void boot_complete(void);
void boot_debug(const char* format, ...);

#endif /* BOOT_UI_H */

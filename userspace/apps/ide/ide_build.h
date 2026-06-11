/*
 * ide_build.h -- the BUILD panel + Build/Run actions.
 *
 * Build compiles a->cur_file with the native toolchain (tc_build) and shows
 * the TcResult; Run spawns the produced ELF via SYS_SPAWN. The panel renders a
 * status line, output sizes, the toolchain message, diagnostics and an asm
 * preview, all clipped to its Rect. Freestanding C: no libc/malloc/stdio, only
 * static buffers.
 */
#ifndef IDE_BUILD_H
#define IDE_BUILD_H

#include "ide.h"
#include "tc.h"

/* Compile a->cur_file with the native toolchain; stash the result. */
void ide_do_build(Ide* a);
/* Spawn the most recently built ELF (no-op message if nothing built / failed). */
void ide_do_run(Ide* a);
/* Poll for child exit (non-blocking WNOHANG). Returns 1 if state changed. */
int  ide_run_poll(void);
/* Render the BUILD panel into Rect r of cv. */
void panel_build(Ide* a, Canvas* cv, Rect r);
/* Handle a click inside the BUILD panel at (mx,my) within Rect r.
 * If a diagnostic line is clicked, jump the editor to that line. Returns 1 if consumed. */
int  panel_build_click(Ide* a, Rect r, int mx, int my);
/* Handle mouse-wheel scroll in the BUILD panel. delta>0 = scroll down. */
void panel_build_scroll(int delta);
/* 1 if there's a build result to show, else 0. */
int  ide_build_active(void);
/* IDE-CONTEXT-0: 1 if the last build succeeded (a result exists AND ok). */
int  ide_build_ok(void);
/* IDE-CONTEXT-0: diagnostic count of the last build; 0 if no build yet. */
int  ide_build_diag_count(void);
/* Query whether line `ln` (0-based) has a build error on it. Returns the
 * diagnostic severity: 0=none, 1=error, 2=warning. Used by the editor to
 * paint error-line backgrounds. */
int  ide_build_line_severity(int ln);
/* Build flash state: returns a colour to briefly tint the BUILD tab header
 * after a build completes (green=ok, red=fail). Returns 0 when no flash is
 * active. Call ide_build_tick(dt_ms) each frame to decay the flash. */
uint32_t ide_build_flash_color(void);
void     ide_build_tick(int dt_ms);
/* Return the build duration in milliseconds (0 if no build yet). */
int  ide_build_time_ms(void);
/* IDE-FORGE-0: the last Run message text (g_runmsg); empty string if none. */
const char* ide_run_msg(void);

#endif /* IDE_BUILD_H */

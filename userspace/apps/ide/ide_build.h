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
/* Render the BUILD panel into Rect r of cv. */
void panel_build(Ide* a, Canvas* cv, Rect r);
/* 1 if there's a build result to show, else 0. */
int  ide_build_active(void);

#endif /* IDE_BUILD_H */

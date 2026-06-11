/*
 * ide_marks.h -- per-symbol "marks" store (IDE-FORGE-0).
 *
 * The aphantasia north star needs ONE thing the model never kept: the user's
 * INTENT per function -- "I've handled this / I still owe this / watch this /
 * silence this." Func.closed is an IDA collapse flag and dies on every reparse;
 * marks must SURVIVE a reparse and a restart, so they live here, keyed by the
 * stable function NAME (the same identity the runtime strip uses to match flow
 * labels to functions, ide_runtime.c). The fields are plain int so the existing
 * panel_settings SET_TOGGLE widgets bind to &m->done etc. with zero widget
 * changes.
 *
 * Persistence mirrors ide_config.c EXACTLY (durable diskfs first, session file
 * fallback, key=value lines, tolerant parser). Only the key set differs: one
 * line per non-zero mark, e.g. "done.tower_tick=1".
 *
 * Freestanding: no libc/malloc. Static table, no allocation.
 */
#ifndef IDE_MARKS_H
#define IDE_MARKS_H

#include "ide_model.h"           /* M_NAME, M_MAXFUNCS, Model */

/* One function's marks. name is the stable key (function name). */
typedef struct {
    char name[M_NAME];           /* function name = the stable key            */
    int  done;                   /* 0/1  user marked this function finished   */
    int  star;                   /* 0/1  watch/pin -> always-visible chip     */
    int  isolate;                /* 0/1  focus-lock + map solo (see ide.c gate)*/
    int  mute;                   /* 0/1  suppress this fn's warnings/penalty   */
} SymMark;

/* Get the mark record for `name`, creating a zeroed one on first touch.
 * Returns NULL only if the table is full (or name is empty). The fields are int
 * so panel_settings' int*-based widgets bind to &m->done etc. unchanged. */
SymMark* marks_get(const char* name);
/* Look up `name`; returns NULL if it has no record yet (read-only paths). */
SymMark* marks_find(const char* name);

/* Number of functions in *m whose mark is done. Safe if m is NULL. */
int  marks_count_done(const Model* m);

void ide_marks_load(void);               /* call once at init (beside config) */
void ide_marks_save(void);               /* call after every mark change      */

#endif /* IDE_MARKS_H */

/*
 * ide_complete.h -- the unified autocomplete engine for the IDE editor.
 *
 * Replaces the old 40-word hardcoded keyword dictionary. Candidates are drawn,
 * in priority order, from: the focused function's parameters, the parsed Model
 * (functions, globals, prototypes, records, macros), C keywords/types, and the
 * "complex" library triggers (ide_library). Results land in the editor's
 * existing ac_matches/ac_count/ac_active/ac_sel fields; per-candidate metadata
 * (kind + snippet body for the preview pane) is queried via the accessors.
 *
 * Freestanding: no libc / malloc; bounded static scratch.
 */
#ifndef IDE_COMPLETE_H
#define IDE_COMPLETE_H

struct Ide;

/* Candidate kinds (drive the popup icon/chip color). */
enum {
    CK_PARAM = 0,
    CK_FUNC,
    CK_GLOBAL,
    CK_TYPE,
    CK_MACRO,
    CK_KEYWORD,
    CK_SNIPPET
};

/* Recompute the candidate list from the editor's current ac_prefix. Fills
 * a->editor.ac_matches/ac_count/ac_active and clamps ac_sel. */
void complete_refresh(struct Ide* a);

/* Metadata for candidate i (0..ac_count-1). */
int         complete_kind(int i);       /* CK_*                              */
const char* complete_preview(int i);    /* snippet body for the preview, or 0 */

/* Accept the selected candidate: insert the symbol suffix, or (for a library
 * complex) replace the typed trigger and expand the snippet with tab-stops. */
void complete_accept(struct Ide* a);

#endif /* IDE_COMPLETE_H */

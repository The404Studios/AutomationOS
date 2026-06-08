/*
 * ide_library.h -- the IDE's "complex" / snippet library.
 *
 * A curated, non-AI catalog of reusable code blocks ("complexes"): C idioms,
 * data/control structures, and networking blocks. Each entry has a TRIGGER
 * (typed to summon it via autocomplete), a human LABEL, a CATEGORY, and a BODY
 * (real C source with ${N:..} / $N / $0 tab-stop markers). Bodies are
 * const char* literals in .rodata (zero .bss). The table is the union of a
 * static built-in core and any *.snip files loaded from /usr/lib/snippets at
 * startup (lib_load_disk), so it scales without recompiling the IDE.
 *
 * Insertion + tab-stop expansion lives in the editor (ide_editor_insert_snippet)
 * because it needs the caret + buffer; this module is pure data + lookup.
 *
 * Freestanding: no libc / malloc.
 */
#ifndef IDE_LIBRARY_H
#define IDE_LIBRARY_H

struct Ide;

/* Category ids (drive the popup/palette icon + grouping). */
enum {
    LIBCAT_IDIOM = 0,   /* for/while/if/switch/struct/function ...   */
    LIBCAT_DATA,        /* state machine, ring buffer, pool, loop    */
    LIBCAT_NET,         /* TCP/UDP/DNS blocks                        */
    LIBCAT_OTHER,
    LIBCAT_COUNT
};

typedef struct {
    const char* trigger;    /* what the user types to summon it          */
    const char* label;      /* human-readable name shown in the list     */
    int         category;   /* LIBCAT_*                                   */
    const char* body;       /* C source with ${N:..}/$N/$0 tab-stops     */
} Snippet;

/* Total entries (built-in core + any disk-loaded). */
int            lib_count(void);
/* Entry by index, or 0 if out of range. */
const Snippet* lib_get(int idx);
/* Short category tag ("c", "data", "net") for the popup chip. */
const char*    lib_cat_tag(int category);

/* Load /usr/lib/snippets/<*.snip> and append to the table (bounded; non-fatal
 * if the dir is missing -- the built-in core always remains). Called once at
 * IDE startup. */
void           lib_load_disk(struct Ide* a);

#endif /* IDE_LIBRARY_H */

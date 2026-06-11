/*
 * ide_marks.c -- per-symbol marks store, persisted like the Settings knobs.
 *
 * A static table keyed by function name (no malloc). marks_get() linear-scans
 * (<= MARKS_CAP entries, the same cost as sem_find_global's scan) and appends a
 * zeroed row on miss. Persistence copies ide_config.c verbatim: durable diskfs
 * first (SYS_PERSIST_READ/WRITE), session file fallback, key=value lines, a
 * tolerant parser that skips unknown keys. The only difference from the config
 * file is that the key set is DYNAMIC -- one line per non-zero mark, encoded as
 * "<field>.<name>=1" (e.g. "done.tower_tick=1"). Only non-zero fields are
 * written, so the file stays tiny and forward-tolerant.
 *
 * Freestanding: no libc. IO via ide_read_file / ide_write_file (ide_sys.c).
 */
#include "ide_marks.h"
#include "ide_sys.h"      /* ide_read_file / ide_write_file / ide_streq / ide_strlen */

/* Durable diskfs syscalls (mirror ide_config.c) -- reboot-durable on a present
 * SATA disk, session file fallback otherwise. */
#define SYS_PERSIST_READ   94
#define SYS_PERSIST_WRITE  95

#define IDE_MARKS_NAME      "ide.marks"            /* diskfs flat file name      */
#define IDE_MARKS_FALLBACK  "/Desktop/.ide_marks" /* session-writable fallback  */

#define MARKS_CAP   (M_MAXFUNCS * 2)   /* room for renames within a session */
static SymMark g_marks[MARKS_CAP];
static int     g_nmarks;

/* Each line is "<field>.<name>=1\n"; MARKS_CAP*4 fields * (~60 bytes) fits here. */
#define MARKS_BUF_CAP 4096

/* Read the durable diskfs file into buf; returns bytes, or <0 if unavailable. */
static int persist_read(char* buf, int cap) {
    long r = ide_sc(SYS_PERSIST_READ, (long)IDE_MARKS_NAME, (long)buf, (long)cap, 0, 0, 0);
    return (int)r;
}
/* Write buf to the durable diskfs file; returns bytes written, or <0. */
static int persist_write(const char* buf, int len) {
    long r = ide_sc(SYS_PERSIST_WRITE, (long)IDE_MARKS_NAME, (long)buf, (long)len, 0, 0, 0);
    return (int)r;
}

/* ====================================================================== *
 *  Table accessors                                                       *
 * ====================================================================== */

SymMark* marks_find(const char* name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < g_nmarks && i < MARKS_CAP; i++)
        if (ide_streq(g_marks[i].name, name)) return &g_marks[i];
    return 0;
}

SymMark* marks_get(const char* name) {
    if (!name || !name[0]) return 0;
    SymMark* m = marks_find(name);
    if (m) return m;
    if (g_nmarks >= MARKS_CAP) return 0;           /* table full */
    m = &g_marks[g_nmarks++];
    ide_strlcpy(m->name, name, M_NAME);
    m->done = m->star = m->isolate = m->mute = 0;
    return m;
}

int marks_count_done(const Model* m) {
    if (!m) return 0;
    int c = 0, nf = m->nfuncs;
    if (nf > M_MAXFUNCS) nf = M_MAXFUNCS;
    if (nf < 0) nf = 0;
    for (int i = 0; i < nf; i++) {
        SymMark* mk = marks_find(m->funcs[i].name);
        if (mk && mk->done) c++;
    }
    return c;
}

/* ====================================================================== *
 *  Persistence -- copies ide_config.c's pattern, dynamic key set         *
 * ====================================================================== */

/* Parse a non-negative decimal at s[..]; stop at the first non-digit. */
static int marks_atoi(const char* s, int len, int* consumed) {
    int v = 0, i = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    if (consumed) *consumed = i;
    return v;
}

/* field[0..flen) == literal? */
static int marks_feq(const char* field, int flen, const char* lit) {
    if (ide_strlen(lit) != flen) return 0;
    for (int i = 0; i < flen; i++) if (field[i] != lit[i]) return 0;
    return 1;
}

/* Apply one parsed "<field>.<name>=value" record (clamped to 0/1). */
static void marks_apply_key(const char* key, int klen, int value) {
    int dot = -1;
    for (int i = 0; i < klen; i++) if (key[i] == '.') { dot = i; break; }
    if (dot <= 0) return;                          /* need a non-empty field */
    const char* field = key;
    int flen = dot;
    const char* name = key + dot + 1;
    int nlen = klen - dot - 1;
    if (nlen <= 0 || nlen >= M_NAME) return;
    char nm[M_NAME];
    for (int i = 0; i < nlen; i++) nm[i] = name[i];
    nm[nlen] = 0;
    SymMark* mk = marks_get(nm);
    if (!mk) return;
    int v = value ? 1 : 0;
    if      (marks_feq(field, flen, "done"))    mk->done    = v;
    else if (marks_feq(field, flen, "star"))    mk->star    = v;
    else if (marks_feq(field, flen, "isolate")) mk->isolate = v;
    else if (marks_feq(field, flen, "mute"))    mk->mute    = v;
    /* unknown field. : ignore (forward compatible, like cfg_apply_kv) */
}

void ide_marks_load(void) {
    static char buf[MARKS_BUF_CAP];
    int n = persist_read(buf, MARKS_BUF_CAP);            /* durable diskfs first */
    if (n <= 0) n = ide_read_file(IDE_MARKS_FALLBACK, buf, MARKS_BUF_CAP);
    if (n <= 0) return;                 /* no marks yet -> empty table */
    if (n > MARKS_BUF_CAP) n = MARKS_BUF_CAP;

    int i = 0;
    while (i < n) {
        while (i < n && (buf[i] == ' ' || buf[i] == '\t' ||
                         buf[i] == '\n' || buf[i] == '\r')) i++;
        int ks = i;
        while (i < n && buf[i] != '=' && buf[i] != '\n') i++;
        if (i >= n || buf[i] != '=') {  /* no '=' on this line: skip to EOL */
            while (i < n && buf[i] != '\n') i++;
            continue;
        }
        int klen = i - ks;
        i++;                            /* step past '=' */
        int used = 0;
        int val = marks_atoi(buf + i, n - i, &used);
        i += used;
        if (klen > 0 && used > 0) marks_apply_key(buf + ks, klen, val);
        while (i < n && buf[i] != '\n') i++;   /* to end of line */
    }
}

/* Append literal s to buf at *p (bounded by MARKS_BUF_CAP). */
static void marks_puts(char* buf, int* p, const char* s) {
    for (int j = 0; s[j] && *p < MARKS_BUF_CAP - 2; j++) buf[(*p)++] = s[j];
}

/* Emit "<field>.<name>=1\n" only when `on`. */
static void marks_emit(char* buf, int* p, const char* field,
                       const char* name, int on) {
    if (!on || *p >= MARKS_BUF_CAP - 80) return;
    marks_puts(buf, p, field);
    if (*p < MARKS_BUF_CAP - 2) buf[(*p)++] = '.';
    marks_puts(buf, p, name);
    if (*p < MARKS_BUF_CAP - 3) { buf[(*p)++] = '='; buf[(*p)++] = '1'; buf[(*p)++] = '\n'; }
}

void ide_marks_save(void) {
    static char buf[MARKS_BUF_CAP];
    int p = 0;
    for (int r = 0; r < g_nmarks && r < MARKS_CAP; r++) {
        SymMark* mk = &g_marks[r];
        if (!mk->name[0]) continue;
        marks_emit(buf, &p, "done",    mk->name, mk->done);
        marks_emit(buf, &p, "star",    mk->name, mk->star);
        marks_emit(buf, &p, "isolate", mk->name, mk->isolate);
        marks_emit(buf, &p, "mute",    mk->name, mk->mute);
    }
    /* Prefer durable diskfs; fall back to the session file if no disk present. */
    if (persist_write(buf, p) != p)
        ide_write_file(IDE_MARKS_FALLBACK, buf, p);
}

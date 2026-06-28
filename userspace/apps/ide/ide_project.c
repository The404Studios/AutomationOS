/*
 * ide_project.c -- project.json (flat key=value) IO + the current-project model.
 * Freestanding: no libc. See ide_project.h.
 */
#include "ide_project.h"
#include "ide_sys.h"      /* ide_read_file / ide_write_file / ide_strlcpy / ide_strneq */

#define PROJ_JSON_CAP 512

/* Join root + "/" + leaf into out (cap), collapsing a trailing slash on root. */
static void pj_path(char* out, int cap, const char* root, const char* leaf) {
    int n = 0;
    for (int i = 0; root[i] && n < cap - 1; i++) out[n++] = root[i];
    if (n > 0 && out[n - 1] != '/' && n < cap - 1) out[n++] = '/';
    for (int i = 0; leaf[i] && n < cap - 1; i++) out[n++] = leaf[i];
    out[n] = 0;
}

/* basename of a path (after the last '/') into out (cap). */
static void pj_basename(char* out, int cap, const char* path) {
    int last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    ide_strlcpy(out, path + last + 1, cap);
}

/* Default run_target = "build/<name>.elf" into dst (cap). */
static void pj_default_run_target(char* dst, int cap, const char* name) {
    int n = 0;
    const char* pre = "build/";
    for (int i = 0; pre[i] && n < cap - 1; i++) dst[n++] = pre[i];
    for (int i = 0; name[i] && n < cap - 5; i++) dst[n++] = name[i];
    const char* ext = ".elf";
    for (int i = 0; ext[i] && n < cap - 1; i++) dst[n++] = ext[i];
    dst[n] = 0;
}

int ide_project_write_manifest(const IdeProject* p) {
    if (!p) return -1;
    char buf[PROJ_JSON_CAP];
    int n = 0;
#define APP(s) do { const char* _s = (s); for (int _i = 0; _s[_i] && n < PROJ_JSON_CAP - 1; _i++) buf[n++] = _s[_i]; } while (0)
#define NL()   do { if (n < PROJ_JSON_CAP - 1) buf[n++] = '\n'; } while (0)
    APP("name=");       APP(p->name[0] ? p->name : "project");           NL();
    APP("lang=");       APP(p->lang[0] ? p->lang : "c");                 NL();
    APP("entry=");      APP(p->entry[0] ? p->entry : "src/main.c");      NL();
    APP("run_target="); APP(p->run_target[0] ? p->run_target : "build/main.elf"); NL();
    APP("kind=");       APP(p->kind);                                    NL();
    APP("cflags=");                                                      NL();
#undef APP
#undef NL
    char path[192];
    pj_path(path, (int)sizeof(path), p->root, "project.json");
    return ide_write_file(path, buf, n);
}

/* Copy value at s[from..) up to '\n'/end (trimmed) into dst (cap). */
static void pj_kv_value(const char* s, int from, int n, char* dst, int cap) {
    int i = from, j = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    while (i < n && s[i] != '\n' && s[i] != '\r' && j < cap - 1) dst[j++] = s[i++];
    while (j > 0 && (dst[j - 1] == ' ' || dst[j - 1] == '\t')) j--;
    dst[j] = 0;
}

int ide_project_load(IdeProject* p, const char* root) {
    if (!p || !root) return -1;

    /* defaults (always leave *p valid) */
    p->active = 1;
    ide_strlcpy(p->root, root, (int)sizeof(p->root));
    pj_basename(p->name, (int)sizeof(p->name), root);
    ide_strlcpy(p->lang, "c", (int)sizeof(p->lang));
    ide_strlcpy(p->entry, "src/main.c", (int)sizeof(p->entry));
    pj_default_run_target(p->run_target, (int)sizeof(p->run_target), p->name);
    p->kind[0] = 0;                              /* default: on-device compile      */

    char path[192];
    pj_path(path, (int)sizeof(path), root, "project.json");
    static char buf[PROJ_JSON_CAP];
    int n = ide_read_file(path, buf, PROJ_JSON_CAP);
    if (n <= 0) return -1;                        /* no manifest -> defaults stand */
    if (n > PROJ_JSON_CAP) n = PROJ_JSON_CAP;

    int i = 0;
    while (i < n) {
        while (i < n && (buf[i] == ' ' || buf[i] == '\t' ||
                         buf[i] == '\n' || buf[i] == '\r')) i++;
        int ks = i;
        while (i < n && buf[i] != '=' && buf[i] != '\n') i++;
        if (i >= n || buf[i] != '=') { while (i < n && buf[i] != '\n') i++; continue; }
        int klen = i - ks;
        i++;                                       /* step past '=' */
        char val[128];
        pj_kv_value(buf, i, n, val, (int)sizeof(val));
        if      (klen == 4  && ide_strneq(buf + ks, "name", 4)        && val[0]) ide_strlcpy(p->name, val, (int)sizeof(p->name));
        else if (klen == 4  && ide_strneq(buf + ks, "lang", 4)        && val[0]) ide_strlcpy(p->lang, val, (int)sizeof(p->lang));
        else if (klen == 5  && ide_strneq(buf + ks, "entry", 5)       && val[0]) ide_strlcpy(p->entry, val, (int)sizeof(p->entry));
        else if (klen == 10 && ide_strneq(buf + ks, "run_target", 10) && val[0]) ide_strlcpy(p->run_target, val, (int)sizeof(p->run_target));
        else if (klen == 4  && ide_strneq(buf + ks, "kind", 4)        && val[0]) ide_strlcpy(p->kind, val, (int)sizeof(p->kind));
        /* cflags: reserved, ignored in v0 */
        while (i < n && buf[i] != '\n') i++;
    }
    return 0;
}

int ide_project_is_project_dir(const char* dir) {
    if (!dir) return 0;
    char path[192];
    pj_path(path, (int)sizeof(path), dir, "project.json");
    char tmp[4];
    return ide_read_file(path, tmp, (int)sizeof(tmp)) >= 0;   /* >=0 == opened == exists */
}

int ide_project_seed_main(const char* path) {
    static const char seed[] =
        "/* main.c -- AutomationOS project starter.\n"
        "   Single-file C; sys_write(fd,buf,len) and sys_exit(code) are builtins.\n"
        "   The toolchain emits _start: it calls main() and exits with its return. */\n"
        "int main(void) {\n"
        "    char msg[] = \"Hello from AutomationOS!\\n\";\n"
        "    int n = 0;\n"
        "    while (msg[n]) n++;\n"
        "    sys_write(1, msg, n);\n"
        "    return 0;\n"
        "}\n";
    return ide_write_file(path, seed, (int)(sizeof(seed) - 1));
}

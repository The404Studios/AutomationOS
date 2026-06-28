/*
 * ide_project.h -- the IDE's current-project world model + project.json IO.
 *
 * A project is a REAL VFS directory tree under /Desktop/Projects/<Name>/:
 *     project.json          flat key=value manifest (NOT real JSON)
 *     src/main.c            the entry source
 *     build/<Name>.elf      the compiled run target
 *     res/                  resources
 *
 * Manifest paths are RELATIVE to the project root so the folder is
 * self-contained / movable; absolute paths are joined at use sites.
 */
#ifndef IDE_PROJECT_H
#define IDE_PROJECT_H

/* Project-name cap. Kept small so /Desktop/Projects/<Name>/build/<Name>.elf
 * stays under SYS_SPAWN's 127-char path limit, diskfs's ~55-char names, and the
 * desktop's 32-char icon labels. */
#define PROJECT_NAME_MAX 32

typedef struct {
    int  active;                      /* 1 = a project is open                      */
    char root[192];                   /* "/Desktop/Projects/<Name>"  (== IDE_PATH)  */
    char name[PROJECT_NAME_MAX + 1];  /* project + display name                     */
    char lang[8];                     /* "c" / "asm" (Layer-2: "cpp"/"py")          */
    char entry[64];                   /* compiled source, rel to root ("src/main.c")*/
    char run_target[96];              /* ELF Run spawns, rel ("build/<Name>.elf")   */
    char kind[16];                    /* "" / "c" = on-device compile; "prebuilt" / */
                                      /* "native" = Build skips cc and Runs the     */
                                      /* shipped run_target (a game linking wl/bf/   */
                                      /* g3d the single-file compiler can't relink). */
} IdeProject;

/* Write <p->root>/project.json from p. Returns 0 or <0. */
int  ide_project_write_manifest(const IdeProject* p);

/* Parse <root>/project.json into *p (p->root := root first). Missing/garbage keys
 * fall back to defaults (name=basename(root), lang=c, entry=src/main.c,
 * run_target=build/<name>.elf). Always leaves *p valid. Returns 0 if a manifest
 * was read, <0 if none (defaults applied). */
int  ide_project_load(IdeProject* p, const char* root);

/* True if <dir>/project.json exists (classifies a folder as a project). */
int  ide_project_is_project_dir(const char* dir);

/* Write a minimal, compilable starter main() to `path`. Returns 0 or <0. */
int  ide_project_seed_main(const char* path);

#endif /* IDE_PROJECT_H */

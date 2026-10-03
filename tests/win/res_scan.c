/*
 * res_scan.c -- read-only corpus scanner for pe_resources / pe_manifest.
 *
 *   usage: res_scan <file.exe|dll> ...        (files are only READ; nothing is written or modified)
 *
 * For each file: pe_parse -> pe_map into a private heap image -> pe_res_dir / pe_res_enum / pe_manifest_from_image /
 * pe_version_info, printing ONE tab-separated line:
 *
 *   path  size  parse  dll  resdir  enum_rc  n_res  man_rc  well_formed  elevation  ui_access  n_deps  deps  os_mask
 *         dpi  longpath  codepage  ver_rc  file_version  description  man_size  man_enc  nelem
 *
 * tests/win/res_corpus.sh feeds it a sample of C:\Windows\System32 and summarises the lines.  Run under ASan/UBSan
 * (the same build flags as the tests) so a crash or out-of-bounds read on a real file aborts loudly.
 */
#include "pe.h"
#include "pe_resources.h"
#include "pe_manifest.h"
#include "res_common.h"

static int count_cb(const pe_res_entry_t *e, void *u)
{
    (void)e;
    (*(unsigned *)u)++;
    return 0;
}

static const char *code_name(int r)
{
    switch (r) {
    case 0: return "ok";
    case 1: return "none";
    default: return NULL;
    }
}

static void print_code(int r, int manifest)
{
    const char *n = code_name(r);
    if (n) { fputs(n, stdout); return; }
    if (manifest) printf("E%d:%s", r, pe_manifest_strerror(r)); else printf("E%d:%s", r, pe_res_strerror(r));
}

/* res_scan --dump <file>: write the file's manifest resource (raw bytes) to stdout and the parse verdict to stderr */
static int dump_manifest(const char *path)
{
    size_t fl = 0;
    u8 *file = load_file(path, &fl), *img;
    pe_info_t info;
    pe_manifest_t *m = (pe_manifest_t *)calloc(1, sizeof *m);
    const u8 *d = NULL;
    unsigned sz = 0;
    int r;
    if (!file || pe_parse(file, fl, &info) != PE_OK) { fprintf(stderr, "%s: cannot parse\n", path); return 1; }
    img = (u8 *)calloc(info.size_of_image, 1);
    if (pe_map(file, fl, &info, img) != PE_OK) { fprintf(stderr, "%s: cannot map\n", path); return 1; }
    r = pe_manifest_from_image(&info, img, m);
    fprintf(stderr, "%s: rc=%d (%s) pos=%u well_formed=%d\n", path, r, r > 0 ? "none" : pe_manifest_strerror(r), m->error_pos, m->well_formed);
    if (pe_res_find(&info, img, PE_RT_MANIFEST, PE_RES_ID_ANY, 0, &d, &sz) == 0) fwrite(d, 1, sz, stdout);
    free(file); free(img); free(m);
    return 0;
}

int main(int argc, char **argv)
{
    int a;
    if (argc == 3 && strcmp(argv[1], "--dump") == 0) return dump_manifest(argv[2]);
    for (a = 1; a < argc; a++) {
        size_t fl = 0;
        u8 *file = load_file(argv[a], &fl);
        pe_info_t info;
        u8 *img = NULL;
        int pr, mr = 0, rr, er = 0, mfr = 0, vr = 0;
        unsigned n_res = 0, rva = 0, size = 0, i;
        pe_manifest_t *m = (pe_manifest_t *)calloc(1, sizeof *m);
        pe_version_t *v = (pe_version_t *)calloc(1, sizeof *v);
        const u8 *md = NULL;
        unsigned msz = 0;

        if (!file) { printf("%s\t-\tUNREADABLE\n", argv[a]); free(m); free(v); continue; }
        pr = pe_parse(file, fl, &info);
        printf("%s\t%zu\t", argv[a], fl);
        if (pr != PE_OK) {
            printf("E%d:%s\n", pr, pe_strerror(pr));
            free(file); free(m); free(v);
            continue;
        }
        img = (u8 *)calloc(info.size_of_image, 1);
        mr = pe_map(file, fl, &info, img);
        if (mr != PE_OK) { printf("MAPFAIL:%s\n", pe_strerror(mr)); free(file); free(img); free(m); free(v); continue; }
        fputs("ok\t", stdout);
        printf("%u\t", info.is_dll);
        rr = pe_res_dir(&info, img, &rva, &size);
        print_code(rr, 0);
        fputc('\t', stdout);
        er = pe_res_enum(&info, img, NULL, count_cb, &n_res, &i);
        print_code(er, 0);
        printf("\t%u\t", n_res);
        mfr = pe_manifest_from_image(&info, img, m);
        print_code(mfr, 1);
        printf("\t%d\t%d\t%d\t%u\t", m->well_formed, m->exec_level, m->ui_access, m->n_deps_total);
        for (i = 0; i < m->n_deps; i++) printf("%s%s@%s", i ? ";" : "", m->deps[i].name, m->deps[i].version);
        printf("\t%u\t%d\t%s\t%s\t", m->os_mask, pe_manifest_dpi_mode(m), m->has_long_path_aware ? m->long_path_aware : "-",
               m->has_active_code_page ? m->active_code_page : "-");
        vr = pe_version_info(&info, img, 0x0409, v);
        print_code(vr, 0);
        if (vr == 0 && v->has_fixed)
            printf("\t%u.%u.%u.%u", v->file_version[0], v->file_version[1], v->file_version[2], v->file_version[3]);
        else
            printf("\t-");
        printf("\t%s", v->have_str & PE_VER_S_DESCRIPTION ? v->file_description : "-");
        if (pe_res_find(&info, img, PE_RT_MANIFEST, PE_RES_ID_ANY, 0, &md, &msz) == 0) printf("\t%u", msz); else printf("\t-");
        printf("\t%d\t%u\n", m->encoding, m->n_elements);
        free(file); free(img); free(m); free(v);
    }
    return 0;
}

/* Web-stack fuzz harness (B11 / WEB-FUZZ-0)
 *
 * Feeds arbitrary bytes through the REAL HTML parser (html_parse -> DOM tree),
 * under AddressSanitizer, to catch memory-safety bugs (OOB, UAF, leaks) on
 * untrusted/malformed input and to lock in the ~28 parser fixes from the prior
 * hardening sweeps. Host-compiled (no kernel/QEMU); host libc supplies
 * malloc/free/str*, so the OS's syscall-using malloc.c is NOT linked.
 *
 * Two modes:
 *   - libFuzzer (coverage-guided): clang -fsanitize=fuzzer,address -DUSE_LIBFUZZER
 *   - replay (deterministic):      gcc -fsanitize=address ; pass corpus files as argv
 *
 * See build_test/web_fuzz_smoke.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "html_parse.h"   /* html_parse() + DOM types + dom_document_free (via ../dom/dom.h) */

static void run_one(const unsigned char *data, unsigned long size)
{
    struct dom_document *doc = html_parse((const char *)data, size);
    if (doc) dom_document_free(doc);   /* free so leaks/UAF are visible to ASan */
}

#ifdef USE_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    run_one(data, (unsigned long)size);
    return 0;
}
#else
int main(int argc, char **argv)
{
    static unsigned char buf[1 << 20];   /* 1 MB input cap */
    int i;
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE...\n", argv[0]);
        return 2;
    }
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        unsigned long n;
        if (!f) { fprintf(stderr, "  skip (open) %s\n", argv[i]); continue; }
        n = (unsigned long)fread(buf, 1, sizeof(buf), f);
        fclose(f);
        run_one(buf, n);
        fprintf(stderr, "  ok %s (%lu bytes)\n", argv[i], n);
    }
    fprintf(stderr, "WEBFUZZ: all inputs survived (no ASan fault)\n");
    return 0;
}
#endif

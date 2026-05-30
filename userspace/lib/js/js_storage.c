/*
 * js_storage.c -- localStorage / sessionStorage for AutomationOS JS.
 * ==================================================================
 *
 * See js_storage.h for the full API, persistence format, and limits.
 *
 * Build (NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
 *       -fno-stack-protector -fno-pic -fno-pie \
 *       -mno-red-zone -mstackrealign -O2 \
 *       -I userspace/libc \
 *       -c userspace/lib/js/js_storage.c -o js_storage.o
 *
 * Dependencies:
 *   userspace/libc/syscall.h   (sc(), SYS_*, O_* flags)
 *   userspace/libc/malloc.h    (malloc/free for the store)
 *   userspace/libc/string.h    (memcpy, memset, strcmp, strlen, strncpy)
 *   userspace/lib/js/js.h      (js_vm)
 *   userspace/lib/js/js_native.h (registration + value helpers)
 */

#include "js_storage.h"
#include "js_native.h"

/* libc from the freestanding userspace */
#include "../../libc/syscall.h"
#include "../../libc/malloc.h"
#include "../../libc/string.h"

/* ================================================================== */
/*  Tiny helpers (avoid any libc dependency beyond the headers above)  */
/* ================================================================== */

/* Safe NUL-terminated string copy into a fixed buffer.
 * Always NUL-terminates; returns number of bytes written (excl. NUL). */
static unsigned long stor_strlcpy(char *dst, const char *src, unsigned long cap)
{
    unsigned long i = 0;
    if (!cap) return 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* Write exactly `n` bytes to `fd`, looping on short writes.
 * Returns 0 on success, -1 on error. */
static int stor_write_all(int fd, const void *buf, unsigned long n)
{
    const char *p = (const char *)buf;
    unsigned long remaining = n;
    while (remaining) {
        long r = write(fd, p, remaining);
        if (r <= 0) return -1;
        p         += (unsigned long)r;
        remaining -= (unsigned long)r;
    }
    return 0;
}

/* Read exactly `n` bytes from `fd`.
 * Returns 0 on success, 1 on EOF before n bytes, -1 on error. */
static int stor_read_exact(int fd, void *buf, unsigned long n)
{
    char *p = (char *)buf;
    unsigned long remaining = n;
    while (remaining) {
        long r = read(fd, p, remaining);
        if (r == 0) return 1;  /* EOF */
        if (r  < 0) return -1; /* error */
        p         += (unsigned long)r;
        remaining -= (unsigned long)r;
    }
    return 0;
}

/* ================================================================== */
/*  In-memory key/value store                                          */
/* ================================================================== */

typedef struct {
    char key  [JS_STORAGE_MAX_KEY   + 1];
    char value[JS_STORAGE_MAX_VALUE + 1];
    int  used;   /* 1 = live entry */
} stor_entry;

/* We use two separate flat arrays: one for localStorage, one for
 * sessionStorage.  Each lives in BSS (static).  That avoids any
 * heap dependency for the store itself and makes the lifetime crystal
 * clear.  Total BSS cost: 2 × 256 × (128+4096+1+4) = ~2.2 MB.    */

static stor_entry g_local  [JS_STORAGE_MAX_ENTRIES];
static int        g_local_count;   /* number of live entries */

static stor_entry g_session[JS_STORAGE_MAX_ENTRIES];
static int        g_session_count;

/* ------------------------------------------------------------------ */
/*  Generic store operations (operate on a (stor_entry *, int *) pair) */
/* ------------------------------------------------------------------ */

/* Find the index of `key` in the store, or -1. */
static int store_find(stor_entry *store, int count_unused, const char *key)
{
    (void)count_unused;
    for (int i = 0; i < JS_STORAGE_MAX_ENTRIES; i++) {
        if (store[i].used && strcmp(store[i].key, key) == 0)
            return i;
    }
    return -1;
}

/* Find a free slot, or -1 if the store is full. */
static int store_free_slot(stor_entry *store, int *countp)
{
    if (*countp >= JS_STORAGE_MAX_ENTRIES) return -1;
    for (int i = 0; i < JS_STORAGE_MAX_ENTRIES; i++) {
        if (!store[i].used) return i;
    }
    return -1;
}

/*
 * store_set -- insert or update a key/value pair.
 * Returns  0 on success.
 *         -1 if key or value exceeds the size limit.
 *         -2 if the store is full (quota exceeded).
 */
static int store_set(stor_entry *store, int *countp,
                     const char *key, const char *value)
{
    unsigned long klen = strlen(key);
    unsigned long vlen = strlen(value);
    if (klen > JS_STORAGE_MAX_KEY)   return -1;
    if (vlen > JS_STORAGE_MAX_VALUE) return -1;

    int idx = store_find(store, 0, key);
    if (idx < 0) {
        idx = store_free_slot(store, countp);
        if (idx < 0) return -2;
        stor_strlcpy(store[idx].key, key, sizeof(store[idx].key));
        store[idx].used = 1;
        (*countp)++;
    }
    stor_strlcpy(store[idx].value, value, sizeof(store[idx].value));
    return 0;
}

/*
 * store_get -- look up `key`; writes result pointer into *out.
 * Returns 1 if found (pointing into store memory), 0 if not found.
 */
static int store_get(stor_entry *store, const char *key, const char **out)
{
    int idx = store_find(store, 0, key);
    if (idx < 0) return 0;
    *out = store[idx].value;
    return 1;
}

/* store_remove -- remove `key`; returns 1 if it existed. */
static int store_remove(stor_entry *store, int *countp, const char *key)
{
    int idx = store_find(store, 0, key);
    if (idx < 0) return 0;
    memset(&store[idx], 0, sizeof(stor_entry));
    (*countp)--;
    return 1;
}

/* store_clear -- remove all entries. */
static void store_clear(stor_entry *store, int *countp)
{
    memset(store, 0, sizeof(stor_entry) * JS_STORAGE_MAX_ENTRIES);
    *countp = 0;
}

/*
 * store_key_at -- return the key at insertion-order position `index`.
 * We approximate insertion order by iterating through slots in array
 * order (entries keep their array position; no re-packing on remove).
 * Returns NULL if index >= number of live entries.
 */
static const char *store_key_at(stor_entry *store, int index)
{
    int seen = 0;
    for (int i = 0; i < JS_STORAGE_MAX_ENTRIES; i++) {
        if (store[i].used) {
            if (seen == index) return store[i].key;
            seen++;
        }
    }
    return (void *)0;
}

/* ================================================================== */
/*  Disk I/O for localStorage                                          */
/* ================================================================== */

/*
 * Record on-disk layout:
 *   [magic:4][keylen:4][valuelen:4][key bytes][value bytes]
 * All multi-byte integers are stored as little-endian uint32_t.
 */

typedef struct __attribute__((packed)) {
    unsigned int magic;
    unsigned int keylen;
    unsigned int valuelen;
} stor_hdr;

/*
 * AOS syscall number for SYS_MKDIR.  Defined in kernel/include/syscall.h
 * but not yet exposed in userspace/libc/syscall.h; declare locally here.
 * MUST stay in sync with kernel/include/syscall.h #define SYS_MKDIR 67.
 */
#ifndef SYS_MKDIR
#define SYS_MKDIR 67
#endif

/* Ensure /home exists; ignore errors (may already exist). */
static void ensure_home_dir(void)
{
    /* SYS_MKDIR = 67; mode 0755 = 0493 decimal */
    register long r10 asm("r10") = 0;
    register long r8  asm("r8")  = 0;
    register long r9  asm("r9")  = 0;
    long ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_MKDIR), "D"((long)"/home"), "S"((long)0755),
          "d"((long)0), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    (void)ret; /* silently ignore EEXIST */
}

int js_storage_save_to_disk(void)
{
    ensure_home_dir();

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    int fd = open(JS_LOCALSTORAGE_PATH, flags, 0644);
    if (fd < 0) return -1;

    for (int i = 0; i < JS_STORAGE_MAX_ENTRIES; i++) {
        if (!g_local[i].used) continue;

        unsigned long klen = strlen(g_local[i].key);
        unsigned long vlen = strlen(g_local[i].value);

        stor_hdr hdr;
        hdr.magic    = JS_STORAGE_MAGIC;
        hdr.keylen   = (unsigned int)klen;
        hdr.valuelen = (unsigned int)vlen;

        if (stor_write_all(fd, &hdr, sizeof(hdr))              != 0) goto err;
        if (stor_write_all(fd, g_local[i].key,   klen)         != 0) goto err;
        if (stor_write_all(fd, g_local[i].value, vlen)         != 0) goto err;
    }
    close(fd);
    return 0;
err:
    close(fd);
    return -1;
}

int js_storage_load_from_disk(void)
{
    int fd = open(JS_LOCALSTORAGE_PATH, O_RDONLY, 0);
    if (fd < 0) return 0;  /* file absent == empty store; not an error */

    store_clear(g_local, &g_local_count);

    stor_hdr hdr;
    /*
     * Cap the number of records we consume from disk to
     * JS_STORAGE_MAX_ENTRIES.  A file with more records than that is
     * either corrupt or from a previous build with a higher limit; in
     * either case, stop loading rather than looping forever.
     */
    int records_loaded = 0;
    for (;;) {
        if (records_loaded >= JS_STORAGE_MAX_ENTRIES) break; /* hard cap */

        int rc = stor_read_exact(fd, &hdr, sizeof(hdr));
        if (rc == 1) break;   /* clean EOF */
        if (rc < 0) goto err;

        if (hdr.magic != JS_STORAGE_MAGIC) goto err;
        if (hdr.keylen   > JS_STORAGE_MAX_KEY)    goto err;
        if (hdr.valuelen > JS_STORAGE_MAX_VALUE)  goto err;

        char key  [JS_STORAGE_MAX_KEY   + 1];
        char value[JS_STORAGE_MAX_VALUE + 1];

        /* Read key bytes (hdr.keylen may be 0 for an empty-string key). */
        if (hdr.keylen > 0) {
            rc = stor_read_exact(fd, key, hdr.keylen);
            if (rc) goto err;
        }
        key[hdr.keylen] = '\0';

        /* Read value bytes (hdr.valuelen == 0 is valid -- empty string). */
        if (hdr.valuelen > 0) {
            rc = stor_read_exact(fd, value, hdr.valuelen);
            if (rc) goto err;
        }
        value[hdr.valuelen] = '\0';

        /* Silently skip if store is full (shouldn't happen given the cap). */
        if (store_set(g_local, &g_local_count, key, value) == 0)
            records_loaded++;
    }
    close(fd);
    return 0;
err:
    close(fd);
    return -1;
}

/* ================================================================== */
/*  JS native method implementations                                  */
/* ================================================================== */

/* We encode which store to use in the void *self_ptr:
 *   (void *)0  -> localStorage  (g_local  / g_local_count)
 *   (void *)1  -> sessionStorage (g_session / g_session_count)
 */

static stor_entry *store_from_ptr(void *p, int **countp)
{
    if ((unsigned long)p == 0) { if (countp) *countp = &g_local_count;   return g_local;   }
    else                       { if (countp) *countp = &g_session_count; return g_session; }
}

/* setItem(key, value) */
static js_value stor_setItem(js_vm *vm, void *self, int argc, js_value *argv)
{
    if (argc < 2) { js_throw_str(vm, "setItem: 2 arguments required"); return js_native_make_undefined(); }
    const char *key = js_native_to_cstr(vm, argv[0]);
    const char *val = js_native_to_cstr(vm, argv[1]);
    if (!key || !val) { js_throw_str(vm, "setItem: OOM"); return js_native_make_undefined(); }

    int *countp;
    stor_entry *store = store_from_ptr(self, &countp);
    int rc = store_set(store, countp, key, val);
    if (rc == -1) { js_throw_str(vm, "setItem: key or value too large"); return js_native_make_undefined(); }
    if (rc == -2) { js_throw_str(vm, "QuotaExceededError: storage full"); return js_native_make_undefined(); }
    return js_native_make_undefined();
}

/* getItem(key) -> string | null */
static js_value stor_getItem(js_vm *vm, void *self, int argc, js_value *argv)
{
    if (argc < 1) { js_throw_str(vm, "getItem: 1 argument required"); return js_native_make_undefined(); }
    const char *key = js_native_to_cstr(vm, argv[0]);
    if (!key) return js_native_make_null();

    stor_entry *store = store_from_ptr(self, (void *)0);
    const char *found = (void *)0;
    if (!store_get(store, key, &found))
        return js_native_make_null();
    return js_native_make_string(vm, found);
}

/* removeItem(key) */
static js_value stor_removeItem(js_vm *vm, void *self, int argc, js_value *argv)
{
    if (argc < 1) { js_throw_str(vm, "removeItem: 1 argument required"); return js_native_make_undefined(); }
    const char *key = js_native_to_cstr(vm, argv[0]);
    if (!key) return js_native_make_undefined();

    int *countp;
    stor_entry *store = store_from_ptr(self, &countp);
    store_remove(store, countp, key);
    return js_native_make_undefined();
}

/* clear() */
static js_value stor_clear(js_vm *vm, void *self, int argc, js_value *argv)
{
    (void)vm; (void)argc; (void)argv;
    int *countp;
    stor_entry *store = store_from_ptr(self, &countp);
    store_clear(store, countp);
    return js_native_make_undefined();
}

/* key(index) -> string | null */
static js_value stor_key(js_vm *vm, void *self, int argc, js_value *argv)
{
    if (argc < 1) { js_throw_str(vm, "key: 1 argument required"); return js_native_make_undefined(); }
    int idx = 0;
    if (!js_native_to_int(argv[0], &idx))
        return js_native_make_null();

    stor_entry *store = store_from_ptr(self, (void *)0);
    const char *k = store_key_at(store, idx);
    if (!k) return js_native_make_null();
    return js_native_make_string(vm, k);
}

/* ------------------------------------------------------------------ */
/*  Property getter: `length`                                          */
/* ------------------------------------------------------------------ */
static js_value stor_get(js_vm *vm, void *self, const char *prop)
{
    if (strcmp(prop, "length") == 0) {
        int *countp;
        store_from_ptr(self, &countp);
        return js_native_make_number(vm, (double)*countp);
    }
    return js_native_make_undefined();
}

/* ------------------------------------------------------------------ */
/*  Class descriptors (one shared layout, two instances)              */
/* ------------------------------------------------------------------ */

/* The js_native_class.methods field uses an inline anonymous struct type.
 * We cannot assign a separately-declared array of any other struct type
 * in a static initializer even with identical layout.  Instead, we use
 * js_native_method_entry (same layout per the js_native.h comment) and
 * cast via void* at install time (the same cast js_native.c itself does
 * on line: `const method_entry_t *m = (const method_entry_t *)cls->methods`).
 *
 * Strategy: leave .methods == NULL in the static class literals, then
 * patch the pointer in js_storage_install() before handing the class to
 * js_native_register_class().  We use a non-const copy on the stack/static
 * at install time.                                                        */

static const js_native_method_entry stor_methods[] = {
    { "setItem",    stor_setItem    },
    { "getItem",    stor_getItem    },
    { "removeItem", stor_removeItem },
    { "clear",      stor_clear      },
    { "key",        stor_key        },
    { (void *)0,    (void *)0       }
};

/* Base descriptors with methods == NULL (patched at install time). */
static const js_native_class g_local_class_base = {
    "Storage",
    stor_get,      /* getter */
    (void *)0,     /* no setter -- use setItem/removeItem */
    (void *)0      /* methods patched in js_storage_install */
};

static const js_native_class g_session_class_base = {
    "Storage",
    stor_get,
    (void *)0,
    (void *)0
};

/* ================================================================== */
/*  Installation                                                       */
/* ================================================================== */

void js_storage_install(js_vm *vm)
{
    /* Build concrete class descriptors with the methods pointer patched in.
     * We cast js_native_method_entry* via void* to satisfy the anonymous-
     * struct pointer field type (identical layout; same cast used in
     * js_native.c line "const method_entry_t *m = (const method_entry_t *)
     * cls->methods").                                                     */
    typedef struct { const char *name; js_native_method fn; } _anon_me;
    const _anon_me *mptr = (const _anon_me *)(const void *)stor_methods;

    js_native_class local_cls   = g_local_class_base;
    js_native_class session_cls = g_session_class_base;
    local_cls.methods   = (const void *)mptr;
    session_cls.methods = (const void *)mptr;

    /* Register classes (idempotent per js_new() lifetime). */
    int lid = js_native_register_class(vm, &local_cls);
    int sid = js_native_register_class(vm, &session_cls);

    if (lid < 0 || sid < 0) return;  /* registry full -- should not happen */

    /* Wrap the two C backing stores as JS objects.
     * self_ptr == (void*)0 --> localStorage
     * self_ptr == (void*)1 --> sessionStorage                          */
    js_value lv = js_native_wrap(vm, lid, (void *)0);
    js_value sv = js_native_wrap(vm, sid, (void *)1);

    js_native_register_global_value(vm, "localStorage",   lv);
    js_native_register_global_value(vm, "sessionStorage", sv);
}

/* ================================================================== */
/*  Self-test (no JS engine required)                                  */
/* ================================================================== */

/*
 * Minimal write to fd=1 (stdout) for test diagnostics.
 * We cannot use printf (freestanding), so we roll our own tiny print.
 */
static void test_puts(const char *s)
{
    unsigned long n = strlen(s);
    write(1, s, n);
}

static void test_putnum(int n)
{
    if (n < 0) { write(1, "-", 1); n = -n; }
    if (n == 0) { write(1, "0", 1); return; }
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    while (n && i > 0) { buf[--i] = (char)('0' + n % 10); n /= 10; }
    test_puts(buf + i);
}

static int g_fail_count;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { \
        test_puts("[FAIL] "); test_puts(msg); test_puts("\n"); \
        g_fail_count++; \
    } else { \
        test_puts("[PASS] "); test_puts(msg); test_puts("\n"); \
    } \
} while (0)

int js_storage_selftest(void)
{
    g_fail_count = 0;
    test_puts("=== js_storage_selftest begin ===\n");

    /* --- test target: g_local store --------------------------------- */
    stor_entry *S = g_local;
    int        *C = &g_local_count;
    store_clear(S, C);

    /* 1. Basic set + get */
    store_set(S, C, "alpha", "1");
    store_set(S, C, "beta",  "hello world");
    EXPECT(*C == 2, "count after 2 insertions");

    const char *v = (void *)0;
    EXPECT(store_get(S, "alpha", &v) && strcmp(v, "1") == 0,   "get alpha == '1'");
    EXPECT(store_get(S, "beta",  &v) && strcmp(v, "hello world") == 0, "get beta");
    EXPECT(!store_get(S, "missing", &v),  "get missing returns 0");

    /* 2. Update existing */
    store_set(S, C, "alpha", "updated");
    EXPECT(*C == 2, "count unchanged after update");
    EXPECT(store_get(S, "alpha", &v) && strcmp(v, "updated") == 0, "updated value");

    /* 3. Remove */
    store_remove(S, C, "alpha");
    EXPECT(*C == 1, "count after remove");
    EXPECT(!store_get(S, "alpha", &v), "removed key absent");

    /* 4. key(index)
     * At this point the store has: beta (slot 0), key0 (slot 1), key1 (slot 2).
     * key_at(0..2) are valid; key_at(3) is out of range. */
    store_set(S, C, "key0", "v0");
    store_set(S, C, "key1", "v1");
    const char *k0 = store_key_at(S, 0);
    const char *k1 = store_key_at(S, 1);
    const char *k2 = store_key_at(S, 2);   /* third live entry: key1 */
    const char *k3 = store_key_at(S, 3);   /* out of range -> NULL */
    EXPECT(k0 != (void *)0, "key(0) non-null");
    EXPECT(k1 != (void *)0, "key(1) non-null");
    EXPECT(k2 != (void *)0, "key(2) non-null (third live entry)");
    EXPECT(k3 == (void *)0, "key(3) null (out of range)");

    /* 5. Clear */
    store_clear(S, C);
    EXPECT(*C == 0, "count after clear");
    EXPECT(!store_get(S, "beta", &v), "all entries cleared");

    /* 6. Key-length limit */
    {
        char bigkey[JS_STORAGE_MAX_KEY + 10];
        memset(bigkey, 'x', sizeof(bigkey) - 1);
        bigkey[sizeof(bigkey) - 1] = '\0';
        int rc = store_set(S, C, bigkey, "val");
        EXPECT(rc == -1, "oversized key rejected");
    }

    /* 7. Value-length limit */
    {
        char *bigval = (char *)malloc(JS_STORAGE_MAX_VALUE + 10);
        if (bigval) {
            memset(bigval, 'v', JS_STORAGE_MAX_VALUE + 9);
            bigval[JS_STORAGE_MAX_VALUE + 9] = '\0';
            int rc = store_set(S, C, "k", bigval);
            EXPECT(rc == -1, "oversized value rejected");
            free(bigval);
        }
    }

    /* 8. Quota limit: fill the store */
    {
        store_clear(S, C);
        int i;
        for (i = 0; i < JS_STORAGE_MAX_ENTRIES; i++) {
            char k[16];
            /* hand-roll itoa to keep freestanding */
            int tmp = i;
            int pos = 14;
            k[15] = '\0';
            if (tmp == 0) { k[14] = '0'; pos = 14; }
            else while (tmp && pos >= 0) { k[pos--] = (char)('0' + tmp % 10); tmp /= 10; }
            int rc = store_set(S, C, k + pos + 1, "v");
            if (rc != 0) break;
        }
        EXPECT(i == JS_STORAGE_MAX_ENTRIES, "filled store to max");
        int rc = store_set(S, C, "overflow", "x");
        EXPECT(rc == -2, "overflow key rejected with quota error");
        store_clear(S, C);
    }

    /* 9. sessionStorage is independent */
    {
        stor_entry *SS = g_session;
        int        *SC = &g_session_count;
        store_clear(SS, SC);
        store_set(S,  C,  "shared_key", "local");
        store_set(SS, SC, "shared_key", "session");
        const char *lv2 = (void *)0, *sv2 = (void *)0;
        store_get(S,  "shared_key", &lv2);
        store_get(SS, "shared_key", &sv2);
        EXPECT(strcmp(lv2, "local")   == 0, "localStorage independent of sessionStorage");
        EXPECT(strcmp(sv2, "session") == 0, "sessionStorage independent of localStorage");
        store_clear(S,  C);
        store_clear(SS, SC);
    }

    /* 10. Disk round-trip ------------------------------------------- */
    {
        store_clear(S, C);
        store_set(S, C, "persist_a", "value_a");
        store_set(S, C, "persist_b", "value_b_with_spaces and more!");
        store_set(S, C, "persist_c", "");   /* empty value */

        int wrc = js_storage_save_to_disk();
        EXPECT(wrc == 0, "save_to_disk returns 0");

        /* Corrupt in-memory state so we know the load is real */
        store_clear(S, C);
        EXPECT(*C == 0, "store cleared before reload");

        int rrc = js_storage_load_from_disk();
        EXPECT(rrc == 0, "load_from_disk returns 0");
        EXPECT(*C == 3, "count after reload == 3");

        const char *ra = (void *)0, *rb = (void *)0, *rc2 = (void *)0;
        store_get(S, "persist_a", &ra);
        store_get(S, "persist_b", &rb);
        store_get(S, "persist_c", &rc2);
        EXPECT(ra  && strcmp(ra,  "value_a")                    == 0, "persist_a round-trip");
        EXPECT(rb  && strcmp(rb,  "value_b_with_spaces and more!") == 0, "persist_b round-trip");
        EXPECT(rc2 && strcmp(rc2, "")                            == 0, "persist_c empty value round-trip");

        /* Clean up the test file by wiping store and saving empty file. */
        store_clear(S, C);
        js_storage_save_to_disk();
    }

    /* --- summary ----------------------------------------------------- */
    test_puts("=== js_storage_selftest: ");
    test_putnum(g_fail_count);
    test_puts(" failure(s) ===\n");
    return g_fail_count;
}

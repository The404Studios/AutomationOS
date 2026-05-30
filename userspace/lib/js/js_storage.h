/*
 * js_storage.h -- localStorage / sessionStorage API for AutomationOS JS.
 * =======================================================================
 *
 * OVERVIEW
 * --------
 * Exposes the Web Storage API (localStorage + sessionStorage) to the JS
 * engine via the js_native embedding interface.
 *
 * localStorage  -- persisted to /home/.localstorage across reboots.
 * sessionStorage -- in-memory only; cleared when the engine is torn down.
 *
 * PERSISTENCE FILE FORMAT (localStorage)
 * ----------------------------------------
 * File: /home/.localstorage
 * Encoding: a sequence of fixed-header records, one per key/value pair:
 *
 *   struct {
 *       uint32_t magic;       // 0x4C535430 ("LST0") sanity sentinel
 *       uint32_t keylen;      // byte length of key string (NOT NUL)
 *       uint32_t valuelen;    // byte length of value string (NOT NUL)
 *       char     key[keylen]; // UTF-8 key, no NUL terminator in file
 *       char     val[valuelen];
 *   }
 *
 * The file is entirely rewritten on every js_storage_save_to_disk() call
 * (open+O_TRUNC, write all live entries, close).  Reading scans forward
 * consuming records until EOF.
 *
 * Tombstoned (removed) entries are NOT written; the save is a clean snapshot.
 *
 * LIMITS
 * ------
 *   JS_STORAGE_MAX_ENTRIES  256   -- max simultaneous key/value pairs
 *   JS_STORAGE_MAX_KEY      128   -- max key bytes (excl. NUL)
 *   JS_STORAGE_MAX_VALUE   4096   -- max value bytes (excl. NUL)
 *
 * JS SURFACE
 * ----------
 * Both localStorage and sessionStorage expose:
 *   .setItem(key, value)   -- store; throws QuotaExceededError if full
 *   .getItem(key)          -- returns string or null
 *   .removeItem(key)       -- remove (no-op if absent)
 *   .clear()               -- remove all entries
 *   .length                -- (property) number of stored entries
 *   .key(index)            -- returns the n-th key in insertion order
 *
 * Direct property access (`localStorage["foo"] = "bar"`) is NOT
 * supported because the native-set callback would conflict with the
 * five named methods.  Use setItem/getItem.
 *
 * BUILD FLAGS  (must remain freestanding, no fs:0x28 canary)
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 */

#ifndef JS_STORAGE_H
#define JS_STORAGE_H

#include "js.h"

/* ------------------------------------------------------------------ */
/*  Limits                                                             */
/* ------------------------------------------------------------------ */
#define JS_STORAGE_MAX_ENTRIES  256
#define JS_STORAGE_MAX_KEY      128
#define JS_STORAGE_MAX_VALUE    4096

/* Magic sentinel in each on-disk record header. */
#define JS_STORAGE_MAGIC  0x4C535430u   /* "LST0" */

/* Path to the persistent backing file.  /home is created on first use. */
#define JS_LOCALSTORAGE_PATH  "/home/.localstorage"

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/*
 * js_storage_install -- register localStorage and sessionStorage as
 * global variables in `vm`.  Must be called after every js_new() for
 * which the storage objects should be visible (the native-class registry
 * and the global env are reset by js_new / each js_eval).
 */
void js_storage_install(js_vm *vm);

/*
 * js_storage_save_to_disk -- flush in-memory localStorage to
 * /home/.localstorage.  Returns 0 on success, <0 on I/O error.
 * Call on page-unload, browser shutdown, or any explicit sync point.
 */
int js_storage_save_to_disk(void);

/*
 * js_storage_load_from_disk -- read /home/.localstorage into the
 * in-memory localStorage store.  Silently succeeds if the file does
 * not exist (first boot).  Returns 0 on success, <0 on I/O error.
 * Call once during engine startup.
 */
int js_storage_load_from_disk(void);

/*
 * js_storage_selftest -- exercise the key-value store and disk
 * round-trip WITHOUT needing a running JS engine.
 * Returns 0 on full pass, or a positive failure count.
 */
int js_storage_selftest(void);

#endif /* JS_STORAGE_H */

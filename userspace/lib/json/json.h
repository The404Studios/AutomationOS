/*
 * json.h -- freestanding, allocation-free DOM JSON parser (RFC 8259).
 * =======================================================================
 *
 * A pure userspace (ring 3) JSON parser for consuming web-API responses
 * from the from-scratch x86_64 OS. FREESTANDING in the strict sense:
 *
 *   - NO libc, NO stdio, NO malloc, NO standard headers.
 *   - NO dynamic allocation of any kind. The caller hands in a FIXED node
 *     pool (an array of json_node) and the parser fills it. If the document
 *     needs more nodes than the pool holds, parsing fails cleanly.
 *   - All string/number/char helpers (length, compare, digit math, double
 *     assembly) are implemented internally in json.c -- nothing external.
 *
 * Build (objdump must show NO `fs:0x28` stack canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
 *       -c userspace/lib/json/json.c -o json.o
 *
 * ---------------------------------------------------------------------------
 *  DOM model
 * ---------------------------------------------------------------------------
 * The parser produces a tree of json_node values laid out flat inside the
 * caller's pool. Container children are a singly linked list:
 *
 *      ARRAY/OBJECT node --first_child--> child0 --next_sibling--> child1 ...
 *
 * Each node index is its position in the pool. -1 means "none". The root
 * node index returned by json_parse() is the entry point.
 *
 * ---------------------------------------------------------------------------
 *  String storage policy  (IMPORTANT)
 * ---------------------------------------------------------------------------
 * To avoid any allocation, STRING values and OBJECT member keys are stored
 * as a (pointer,len) slice that points DIRECTLY INTO the caller's source
 * `text` buffer. These slices are therefore NOT null-terminated (the source
 * buffer is the source of truth and must outlive the json_doc) and they
 * still contain the RAW JSON escape sequences (e.g. backslash-n, \uXXXX).
 *
 * If you need a decoded, NUL-terminated value, call json_unescape() to copy
 * one slice into your own buffer with all RFC 8259 escapes resolved
 * (backslash-quote backslash-/ backslash-b backslash-f backslash-n
 *  backslash-r backslash-t and \uXXXX decoded to UTF-8, surrogate pairs
 * combined). The string lookups/compares in this library compare against
 * decoded content too, so json_object_get("a/b") matches a key written as
 * "a\/b" in the source.
 */

#ifndef JSON_H
#define JSON_H

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type;

typedef struct json_node {
    json_type type;

    /* STRING: raw slice into the source buffer (NOT NUL-terminated; may
     *         contain JSON escape sequences -- use json_unescape() to decode).
     * Unused for other types. */
    const char   *str;
    unsigned long  slen;

    /* NUMBER: both representations are filled. inum is the value truncated
     *         toward zero (valid for integers); dnum is the full double. */
    long long      inum;
    double         dnum;

    /* ARRAY/OBJECT: head of the child linked list, -1 if empty.
     * BOOL: 0 / 1 is stored in inum. */
    int            first_child;
    int            next_sibling; /* next element/member, -1 if last */

    /* OBJECT members only: raw slice of the member name (same policy as str). */
    const char    *key;
    unsigned long   klen;
} json_node;

typedef struct {
    json_node    *nodes;  /* caller-provided node pool                */
    int           cap;    /* capacity of the pool (in nodes)          */
    int           used;   /* number of nodes consumed by last parse   */
    const char   *err;    /* NULL on success, else a static message   */
} json_doc;

/*
 * Parse text[0..len) into the caller's node pool.
 *   doc       -- out: filled with nodes/cap/used/err (err==NULL on success).
 *   pool      -- caller-owned array of >= pool_cap json_node.
 *   pool_cap  -- number of nodes available.
 *   text,len  -- the JSON document. MUST outlive any use of the doc, because
 *                STRING/key slices point into it.
 * Returns the root node index (>= 0) on success, or -1 on error
 * (doc->err is set to a human-readable static string).
 */
int json_parse(json_doc *doc, json_node *pool, int pool_cap,
               const char *text, unsigned long len);

/* Object member lookup by key. key is a normal NUL-terminated C string and is
 * compared against the DECODED member name. Returns the child node index, or
 * -1 if obj_index is not an object or the key is absent. */
int json_object_get(const json_doc *doc, int obj_index, const char *key);

/* Array element by zero-based position. Returns the element node index, or -1
 * if arr_index is not an array or i is out of range. */
int json_array_get(const json_doc *doc, int arr_index, int i);

/* Number of elements in an array (0 if empty, -1 if not an array). */
int json_array_len(const json_doc *doc, int arr_index);

/*
 * Decode a STRING node's raw slice into out[0..out_cap), resolving all JSON
 * escapes and writing a trailing NUL. Returns the number of bytes written
 * (excluding the NUL), or -1 on error (bad escape / not a string / would
 * overflow out_cap). out_cap must include room for the NUL.
 */
int json_unescape(const json_doc *doc, int str_index,
                  char *out, unsigned long out_cap);

/* Self-contained offline parse test. Returns 0 iff every check passes. */
int json_selftest(void);

#endif /* JSON_H */

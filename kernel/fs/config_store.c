/*
 * config_store.c -- CONFIG-STORE: a durable, namespaced key/value config store.
 * ==========================================================================
 *
 * The keystone for system persistence (settings/theme/services/terminal): a
 * flat namespaced K/V table ("namespace.key" -> bytes) held in RAM and written
 * THROUGH to a single diskfs flat file ("config.db") so values survive a reboot.
 *
 * Original-by-construction (ORIGINALITY_5X_CHARTER): NOT a journaled KV/DB and
 * NOT an LMDB/leveldb/redis wire format -- it is a flat serialize-the-whole-table
 * write matching the diskfs house style, with the namespace folded into a flat
 * key. RAM-fallback when diskless makes the round-trip provable in the default
 * 43/43 smoke; durability is exercised by smoke_persist (DISK_PERSIST + a disk).
 *
 * Lazy-initialized (no boot-order wiring): the table is allocated + loaded from
 * disk on first cfg_set/cfg_get, exactly like the socket table.
 *
 * Syscalls (Wave0 SYSCALL-LEDGER): SYS_CFG_GET=133, SYS_CFG_SET=134.
 * The agent tool rail can read/write config under policy -> the automation-native
 * introspection surface this brick contributes.
 *
 * Scope: kernel/fs/config_store.c (new).
 */
#include "../include/types.h"
#include "../include/mem.h"     /* kmalloc/kfree, copy_from_user/copy_to_user/copy_user_string, COPY_SUCCESS */
#include "../include/string.h"
#include "../include/kernel.h"  /* kprintf */
#include "../include/errno.h"   /* canonical negative errno */

#define CFG_MAX_ENTRIES 48
#define CFG_KEY_MAX     48      /* "namespace.key", NUL-terminated */
#define CFG_VAL_MAX     192
#define CFG_DB_NAME     "config.db"

typedef struct {
    uint8_t  used;
    char     key[CFG_KEY_MAX];
    uint16_t len;
    uint8_t  val[CFG_VAL_MAX];
} cfg_entry_t;

/* Heap-backed (kmalloc), not .bss -- ~11 KB table; mirrors the socket-table
 * discipline that keeps kernel .bss clear of the GRUB-placed initrd. */
static cfg_entry_t* g_cfg = (cfg_entry_t*)0;
static bool g_cfg_inited = false;

/* diskfs flat-file API (kernel/fs/diskfs.c) -- the durable backing store. */
extern bool diskfs_ready(void);
extern int  diskfs_create(const char* name);
extern int  diskfs_open(const char* name);
extern long diskfs_read (int ino, unsigned long off, void* buf, unsigned long len);
extern long diskfs_write(int ino, unsigned long off, const void* buf, unsigned long len);
extern long diskfs_size (int ino);
extern int  diskfs_unlink(const char* name);

static int cfg_keylen(const char* k) {
    int n = 0;
    while (n < CFG_KEY_MAX && k[n] != '\0') n++;
    return n;
}

static cfg_entry_t* cfg_find(const char* key) {
    for (int i = 0; i < CFG_MAX_ENTRIES; i++)
        if (g_cfg[i].used && strcmp(g_cfg[i].key, key) == 0) return &g_cfg[i];
    return (cfg_entry_t*)0;
}

/* On-disk format: [u32 count] then per entry [u8 keylen][key][u16 vallen][val]. */
static void cfg_load_from_disk(void) {
    if (!diskfs_ready()) return;
    int ino = diskfs_open(CFG_DB_NAME);
    if (ino < 0) return;
    long sz = diskfs_size(ino);
    if (sz < 4) return;
    uint8_t* buf = (uint8_t*)kmalloc((unsigned long)sz);
    if (!buf) return;
    long r = diskfs_read(ino, 0, buf, (unsigned long)sz);
    if (r >= 4) {
        uint32_t count = 0; memcpy(&count, buf, 4);
        unsigned long off = 4, rl = (unsigned long)r;
        for (uint32_t i = 0; i < count && off < rl; i++) {
            uint8_t kl = buf[off++];
            if (kl == 0 || kl >= CFG_KEY_MAX || off + (unsigned long)kl + 2 > rl) break;
            char key[CFG_KEY_MAX];
            memcpy(key, buf + off, kl); key[kl] = '\0'; off += kl;
            uint16_t vl = 0; memcpy(&vl, buf + off, 2); off += 2;
            if (vl > CFG_VAL_MAX || off + (unsigned long)vl > rl) break;
            for (int s = 0; s < CFG_MAX_ENTRIES; s++) {
                if (!g_cfg[s].used) {
                    g_cfg[s].used = 1;
                    memcpy(g_cfg[s].key, key, (unsigned long)kl + 1);
                    g_cfg[s].len = vl;
                    if (vl) memcpy(g_cfg[s].val, buf + off, vl);
                    break;
                }
            }
            off += vl;
        }
    }
    kfree(buf);
}

static void cfg_ensure_init(void) {
    if (g_cfg_inited) return;
    if (!g_cfg) {
        g_cfg = (cfg_entry_t*)kmalloc(sizeof(cfg_entry_t) * CFG_MAX_ENTRIES);
        if (!g_cfg) return;
    }
    memset(g_cfg, 0, sizeof(cfg_entry_t) * CFG_MAX_ENTRIES);
    g_cfg_inited = true;
    cfg_load_from_disk();
}

/* Flat write-through: serialize the whole table and rewrite config.db (diskfs
 * house style, no journaling). No-op when no disk -- the RAM table still works. */
static void cfg_write_through(void) {
    if (!diskfs_ready()) return;
    unsigned long cap = 4 + (unsigned long)CFG_MAX_ENTRIES * (1 + CFG_KEY_MAX + 2 + CFG_VAL_MAX);
    uint8_t* buf = (uint8_t*)kmalloc(cap);
    if (!buf) return;
    unsigned long off = 4;
    uint32_t count = 0;
    for (int i = 0; i < CFG_MAX_ENTRIES; i++) {
        if (!g_cfg[i].used) continue;
        int kl = cfg_keylen(g_cfg[i].key);
        buf[off++] = (uint8_t)kl;
        memcpy(buf + off, g_cfg[i].key, kl); off += kl;
        uint16_t vl = g_cfg[i].len;
        memcpy(buf + off, &vl, 2); off += 2;
        if (vl) { memcpy(buf + off, g_cfg[i].val, vl); off += vl; }
        count++;
    }
    memcpy(buf, &count, 4);
    diskfs_unlink(CFG_DB_NAME);
    int ino = diskfs_create(CFG_DB_NAME);
    if (ino >= 0) diskfs_write(ino, 0, buf, off);
    kfree(buf);
}

/* Set key -> val (len bytes). RAM + write-through. 0 on success, -1 on error. */
int cfg_set(const char* key, const void* val, uint16_t len) {
    cfg_ensure_init();
    if (!g_cfg || !key) return -1;
    int kl = cfg_keylen(key);
    if (kl == 0 || kl >= CFG_KEY_MAX) return -1;
    if (len > CFG_VAL_MAX) return -1;
    cfg_entry_t* e = cfg_find(key);
    if (!e) {
        for (int i = 0; i < CFG_MAX_ENTRIES; i++) {
            if (!g_cfg[i].used) { e = &g_cfg[i]; e->used = 1; memcpy(e->key, key, (unsigned long)kl + 1); break; }
        }
    }
    if (!e) return -1;   /* table full */
    e->len = len;
    if (len) memcpy(e->val, val, len);
    cfg_write_through();
    return 0;
}

/* Get key into buf (up to cap). Returns value length copied, or -1 if absent. */
int cfg_get(const char* key, void* buf, uint16_t cap) {
    cfg_ensure_init();
    if (!g_cfg || !key) return -1;
    cfg_entry_t* e = cfg_find(key);
    if (!e) return -1;
    uint16_t n = (e->len < cap) ? e->len : cap;
    if (n) memcpy(buf, e->val, n);
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* Syscalls: SYS_CFG_SET=134, SYS_CFG_GET=133 (Wave0 ledger).          */
/* ------------------------------------------------------------------ */
int64_t sys_cfg_set(uint64_t key, uint64_t val, uint64_t len,
                    uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!key) return EFAULT;
    if (len > CFG_VAL_MAX) return EINVAL;
    char kkey[CFG_KEY_MAX];
    if (copy_user_string(kkey, (const void*)key, CFG_KEY_MAX) != COPY_SUCCESS) return EFAULT;
    kkey[CFG_KEY_MAX - 1] = '\0';
    uint8_t kval[CFG_VAL_MAX];
    if (len) {
        if (!val) return EFAULT;
        if (copy_from_user(kval, (const void*)val, (size_t)len) != COPY_SUCCESS) return EFAULT;
    }
    return (cfg_set(kkey, kval, (uint16_t)len) == 0) ? 0 : EINVAL;
}

int64_t sys_cfg_get(uint64_t key, uint64_t buf, uint64_t cap,
                    uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    if (!key || !buf) return EFAULT;
    char kkey[CFG_KEY_MAX];
    if (copy_user_string(kkey, (const void*)key, CFG_KEY_MAX) != COPY_SUCCESS) return EFAULT;
    kkey[CFG_KEY_MAX - 1] = '\0';
    uint16_t ccap = (cap > CFG_VAL_MAX) ? CFG_VAL_MAX : (uint16_t)cap;
    uint8_t kbuf[CFG_VAL_MAX];
    int n = cfg_get(kkey, kbuf, ccap);
    if (n < 0) return ENOENT;
    if (n > 0 && copy_to_user((void*)buf, kbuf, (size_t)n) != COPY_SUCCESS) return EFAULT;
    return n;
}

#ifdef NET_SELFTEST
/* CFGRIG: prove the RAM round-trip with zero hardware (disk durability is
 * covered by smoke_persist with DISK_PERSIST). */
void cfg_selftest(void) {
    int set_ok = 0, get_ok = 0, ns_iso = 0, overwrite = 0, oversize_rej = 0, missing = 0;
    uint8_t out[CFG_VAL_MAX];
    int n;

    set_ok = (cfg_set("theme.accent", "blue", 4) == 0) ? 1 : 0;
    n = cfg_get("theme.accent", out, sizeof(out));
    get_ok = (n == 4 && memcmp(out, "blue", 4) == 0) ? 1 : 0;

    /* namespace isolation: the same key tail in another namespace is distinct. */
    cfg_set("sound.accent", "loud", 4);
    n = cfg_get("theme.accent", out, sizeof(out));
    ns_iso = (n == 4 && memcmp(out, "blue", 4) == 0) ? 1 : 0;

    cfg_set("theme.accent", "red", 3);     /* overwrite */
    n = cfg_get("theme.accent", out, sizeof(out));
    overwrite = (n == 3 && memcmp(out, "red", 3) == 0) ? 1 : 0;

    static uint8_t big[CFG_VAL_MAX + 8];
    oversize_rej = (cfg_set("x.big", big, CFG_VAL_MAX + 8) != 0) ? 1 : 0;

    missing = (cfg_get("no.such", out, sizeof(out)) < 0) ? 1 : 0;

    kprintf("CFGRIG: %s set=%d get=%d ns_iso=%d overwrite=%d oversize_rej=%d missing=%d\n",
            (set_ok && get_ok && ns_iso && overwrite && oversize_rej && missing) ? "PASS" : "FAIL",
            set_ok, get_ok, ns_iso, overwrite, oversize_rej, missing);
}

/* Cross-reboot durability probe: read a boot counter from config.db, increment,
 * write it back. On a DISK_PERSIST build with a disk, boot #2 must read back the
 * value boot #1 wrote (was=N-1) -- proving the K/V table reaches durable storage.
 * Without a disk (ready=0) this degrades to the RAM-fallback (bootn=1 each boot). */
void cfg_persist_selftest(void) {
    uint8_t buf[8];
    int n = cfg_get("sys.bootn", buf, sizeof(buf));
    uint32_t cnt = 0;
    if (n == 4) memcpy(&cnt, buf, 4);
    uint32_t next = cnt + 1;
    cfg_set("sys.bootn", &next, 4);
    kprintf("CFGPERSIST: bootn=%u was=%u ready=%d\n",
            (unsigned)next, (unsigned)cnt, (int)diskfs_ready());
}
#endif

/*
 * diskfs.c -- persistent on-disk state + a simple real filesystem over AHCI.
 * ========================================================================
 *
 * The OS is otherwise entirely RAM-backed: the initrd is unpacked into kmalloc
 * memory and every change is lost at power-off. This is the persistence layer --
 * a durable superblock PLUS a tiny inode/data-block filesystem written through
 * the AHCI block driver (ahci_read/ahci_write) that SURVIVES A REBOOT.
 *
 * ----------------------------------------------------------------------------
 * Layer 1: durable SUPERBLOCK (unchanged contract)
 * ----------------------------------------------------------------------------
 *   - One 512-byte superblock at a fixed LBA carrying a magic, a format
 *     version, a monotonically-increasing BOOT COUNTER, a checksum, and (new in
 *     v2) the filesystem region layout.
 *   - diskfs_init(): on boot, read the superblock; if absent/invalid/old, format
 *     a fresh one (and the fs regions); otherwise bump the boot counter; write
 *     it back; then read it back and byte-compare (read-after-write verify). The
 *     boot counter going 2,3,4... across reboots of the SAME disk image is the
 *     proof of durable persistence (see scripts/smoke_persist.sh, a 2-boot
 *     harness). The strings "[DISKFS] mounted: boot #N (...)" and
 *     "[DISKFS] SELFTEST: PASS" are part of that contract and are preserved.
 *
 *   Gate-safety: with no SATA disk attached, ahci_present() is false and
 *   diskfs_init() is a clean no-op -- the default diskless smoke stays green.
 *
 * ----------------------------------------------------------------------------
 * Layer 2: a SIMPLE filesystem (new)
 * ----------------------------------------------------------------------------
 * A flat (single-directory) filesystem laid out in fixed LBA regions ABOVE the
 * superblock. All metadata is cached in kmalloc'd memory and written THROUGH to
 * the disk on every mutation (simple + safe -- no write-back, no journal).
 *
 *   On-disk layout (LBAs; sector == 512 bytes; the smoke disk is 16 MB ==
 *   32768 sectors). Chosen clear of LBA 0 (MBR), the AHCI sector-0 selftest,
 *   LBA 64 (the superblock), and LBA 2048 (the `blk` userspace test sector):
 *
 *     LBA 64           superblock                    (1 sector)
 *     LBA 96           free/allocation BITMAP        (1 sector  == 4096 bits)
 *     LBA 128 .. 143   inode table (64 inodes)       (16 sectors, 128 B/inode)
 *     LBA 4096 ..      data region (4096-byte blocks, 8 sectors each)
 *
 *   The exact region starts + counts are persisted INTO the superblock so the
 *   layout is self-describing and can evolve. On a missing/old/mismatched
 *   superblock the fs regions are (re)formatted from scratch.
 *
 *   Inodes are fixed 128-byte records: {used, type, size, name[56], 12 direct
 *   4K block pointers, 1 single-indirect block pointer}. A single-indirect
 *   block (4096 bytes) holds 1024 u32 block pointers, so the maximum file size
 *   is 12*4K + 1024*4K == ~4.05 MB. The "root directory" is simply the set of
 *   used, named inodes -- no on-disk directory blocks are needed for v1.
 *
 *   Block 0 of the data region is reserved as the NULL/sentinel block so that a
 *   zeroed (freshly formatted) inode's block pointers (== 0) unambiguously mean
 *   "unallocated".
 *
 * Public file API (the VFS integrator calls these; we do NOT edit vfs.c):
 *     int  diskfs_format(void);
 *     int  diskfs_create(const char* name);
 *     int  diskfs_open(const char* name);
 *     long diskfs_read (int ino, unsigned long off, void* buf, unsigned long len);
 *     long diskfs_write(int ino, unsigned long off, const void* buf, unsigned long len);
 *     long diskfs_size (int ino);
 *     int  diskfs_unlink(const char* name);
 *     int  diskfs_list (char names[][64], int max);
 */

#include "../include/kernel.h"   /* kprintf                              */
#include "../include/string.h"   /* memset / memcmp / memcpy / str*      */
#include "../include/ahci.h"     /* ahci_present / ahci_read / ahci_write */
#include "../include/mem.h"      /* kmalloc / kfree                      */

#define DISKFS_MAGIC    0x4B534644u   /* 'D','F','S','K' on disk (LE) */
#define DISKFS_VERSION  2u            /* bumped: superblock now carries fs layout */
#define DISKFS_SB_LBA   64u           /* superblock sector (clear of MBR/blk) */
#define DISKFS_SECTOR   512u

/* ----- filesystem geometry (all in LBAs / sector counts) ----- */
#define DFS_BITMAP_LBA      96u       /* allocation bitmap (1 sector)        */
#define DFS_BITMAP_SECTORS  1u        /* 512 B == 4096 tracked blocks        */
#define DFS_INODE_LBA       128u      /* inode table start                   */
#define DFS_INODE_COUNT     64u       /* fixed number of inodes              */
#define DFS_INODE_SIZE      128u      /* on-disk inode record size           */
#define DFS_INODES_PER_SEC  (DISKFS_SECTOR / DFS_INODE_SIZE)        /* 4    */
#define DFS_INODE_SECTORS   (DFS_INODE_COUNT / DFS_INODES_PER_SEC)  /* 16   */
#define DFS_DATA_LBA        4096u     /* data region start                   */
#define DFS_BLOCK_SIZE      4096u     /* logical fs block                    */
#define DFS_SECS_PER_BLOCK  (DFS_BLOCK_SIZE / DISKFS_SECTOR)        /* 8    */

#define DFS_DISK_SECTORS    32768u    /* 16 MB smoke disk (stay within this) */

/* Number of 4K data blocks that fit between DFS_DATA_LBA and end-of-disk,
 * also bounded by the bitmap capacity (DFS_BITMAP_SECTORS * 512 * 8 bits). */
#define DFS_DATA_SECTORS    (DFS_DISK_SECTORS - DFS_DATA_LBA)        /* 28672 */
#define DFS_BLOCKS_BY_DISK  (DFS_DATA_SECTORS / DFS_SECS_PER_BLOCK)  /* 3584  */
#define DFS_BITMAP_BITS     (DFS_BITMAP_SECTORS * DISKFS_SECTOR * 8) /* 4096  */
#define DFS_BLOCK_COUNT     (DFS_BLOCKS_BY_DISK < DFS_BITMAP_BITS ? \
                             DFS_BLOCKS_BY_DISK : DFS_BITMAP_BITS)   /* 3584  */

/* Inode on-disk layout */
#define DFS_NAME_MAX        56u       /* incl. NUL terminator               */
#define DFS_DIRECT          12u       /* direct block pointers              */
#define DFS_PTRS_PER_BLOCK  (DFS_BLOCK_SIZE / sizeof(uint32_t))     /* 1024 */
#define DFS_INODE_TYPE_FREE 0u
#define DFS_INODE_TYPE_FILE 1u

/* Superblock -- lives in the first bytes of sector DISKFS_SB_LBA. The on-disk
 * sector is 512 bytes; the tail is reserved zero. v2 appends the fs layout
 * after the original five words; old (v1) superblocks lack these fields and are
 * treated as a format mismatch (-> reformat the fs regions). */
typedef struct {
    uint32_t magic;          /* DISKFS_MAGIC when formatted          */
    uint32_t version;        /* DISKFS_VERSION                       */
    uint32_t boot_count;     /* incremented once per boot            */
    uint32_t write_count;    /* total superblock writes (wear/debug) */
    uint32_t checksum;       /* sum of all meaningful words below     */

    /* --- v2: self-describing filesystem layout --- */
    uint32_t bitmap_lba;     /* allocation bitmap start LBA          */
    uint32_t bitmap_sectors; /* allocation bitmap length (sectors)   */
    uint32_t inode_lba;      /* inode table start LBA                */
    uint32_t inode_count;    /* number of inodes                     */
    uint32_t inode_size;     /* bytes per on-disk inode              */
    uint32_t data_lba;       /* data region start LBA                */
    uint32_t block_size;     /* fs block size (bytes)                */
    uint32_t block_count;    /* number of data blocks                */
    uint32_t fs_formatted;   /* 1 once the fs regions are laid out   */
} diskfs_superblock_t;

/* On-disk inode record (exactly DFS_INODE_SIZE bytes). */
typedef struct {
    uint32_t type;                  /* DFS_INODE_TYPE_*                    */
    uint32_t size;                  /* file size in bytes                  */
    char     name[DFS_NAME_MAX];    /* NUL-terminated file name            */
    uint32_t direct[DFS_DIRECT];    /* direct data-block indices (0=none)  */
    uint32_t indirect;              /* single-indirect block index (0=none)*/
    uint32_t reserved[3];           /* pad to exactly DFS_INODE_SIZE bytes */
} diskfs_inode_t;

/* Lock the on-disk inode record to exactly DFS_INODE_SIZE so DFS_INODES_PER_SEC
 * slots tile a 512-byte sector perfectly and the table geometry stays correct. */
_Static_assert(sizeof(diskfs_inode_t) == DFS_INODE_SIZE,
               "diskfs_inode_t must be exactly DFS_INODE_SIZE bytes");

/* ------------------------------------------------------------------------- */
/* Module state                                                              */
/* ------------------------------------------------------------------------- */

static bool     g_diskfs_ready = false;
static uint32_t g_boot_count   = 0;

/* Cached layout (mirrors the superblock once mounted). */
static struct {
    bool     mounted;
    uint32_t bitmap_lba;
    uint32_t bitmap_sectors;
    uint32_t inode_lba;
    uint32_t inode_count;
    uint32_t inode_size;
    uint32_t data_lba;
    uint32_t block_size;
    uint32_t block_count;
} g_fs;

/* In-memory cache of the allocation bitmap (write-through). Sized to the bitmap
 * region (DFS_BITMAP_SECTORS * 512 bytes). */
static uint8_t* g_bitmap = NULL;

/* ------------------------------------------------------------------------- */
/* Forward declarations of the public file API                               */
/* ------------------------------------------------------------------------- */
int  diskfs_format(void);
int  diskfs_create(const char* name);
int  diskfs_open(const char* name);
long diskfs_read (int ino, unsigned long off, void* buf, unsigned long len);
long diskfs_write(int ino, unsigned long off, const void* buf, unsigned long len);
long diskfs_size (int ino);
int  diskfs_unlink(const char* name);
int  diskfs_list (char names[][64], int max);

/* ------------------------------------------------------------------------- */
/* Superblock helpers                                                        */
/* ------------------------------------------------------------------------- */

static uint32_t diskfs_csum(const diskfs_superblock_t* sb)
{
    /* Additive checksum over every meaningful word (all fields except the
     * checksum slot itself). */
    return sb->magic + sb->version + sb->boot_count + sb->write_count +
           sb->bitmap_lba + sb->bitmap_sectors +
           sb->inode_lba + sb->inode_count + sb->inode_size +
           sb->data_lba + sb->block_size + sb->block_count + sb->fs_formatted;
}

bool     diskfs_ready(void)      { return g_diskfs_ready; }
uint32_t diskfs_boot_count(void) { return g_boot_count; }

/* ------------------------------------------------------------------------- */
/* Low-level block I/O helpers (all bounded to the fs regions)               */
/* ------------------------------------------------------------------------- */

/* Read inode #ino from disk into *out. Returns 0 on success, <0 on error. */
static int dfs_read_inode(uint32_t ino, diskfs_inode_t* out)
{
    if (ino >= g_fs.inode_count) return -1;
    uint32_t sec   = g_fs.inode_lba + (ino / DFS_INODES_PER_SEC);
    uint32_t slot  = ino % DFS_INODES_PER_SEC;
    uint8_t  buf[DISKFS_SECTOR];
    if (ahci_read(sec, 1, buf) != 0) return -1;
    memcpy(out, buf + (slot * DFS_INODE_SIZE), sizeof(diskfs_inode_t));
    return 0;
}

/* Write inode #ino through to disk (read-modify-write its sector). */
static int dfs_write_inode(uint32_t ino, const diskfs_inode_t* in)
{
    if (ino >= g_fs.inode_count) return -1;
    uint32_t sec  = g_fs.inode_lba + (ino / DFS_INODES_PER_SEC);
    uint32_t slot = ino % DFS_INODES_PER_SEC;
    uint8_t  buf[DISKFS_SECTOR];
    if (ahci_read(sec, 1, buf) != 0) return -1;     /* preserve neighbours */
    memcpy(buf + (slot * DFS_INODE_SIZE), in, sizeof(diskfs_inode_t));
    if (ahci_write(sec, 1, buf) != 0) return -1;
    return 0;
}

/* Persist the whole in-memory bitmap to disk (write-through). */
static int dfs_flush_bitmap(void)
{
    if (!g_bitmap) return -1;
    return ahci_write(g_fs.bitmap_lba, g_fs.bitmap_sectors, g_bitmap) == 0 ? 0 : -1;
}

static bool dfs_bit_test(uint32_t blk)
{
    if (!g_bitmap || blk >= g_fs.block_count) return true; /* treat OOB as used */
    return (g_bitmap[blk >> 3] >> (blk & 7)) & 1u;
}
static void dfs_bit_set(uint32_t blk)
{
    if (g_bitmap && blk < g_fs.block_count) g_bitmap[blk >> 3] |= (uint8_t)(1u << (blk & 7));
}
static void dfs_bit_clear(uint32_t blk)
{
    if (g_bitmap && blk < g_fs.block_count) g_bitmap[blk >> 3] &= (uint8_t)~(1u << (blk & 7));
}

/* Allocate a free data block. Returns block index (>=1) or 0 on full/error.
 * Block 0 is permanently reserved as the NULL sentinel. Zeroes the block on
 * disk and write-throughs the bitmap. */
static uint32_t dfs_block_alloc(void)
{
    if (!g_bitmap) return 0;
    for (uint32_t b = 1; b < g_fs.block_count; b++) {
        if (!dfs_bit_test(b)) {
            dfs_bit_set(b);
            if (dfs_flush_bitmap() != 0) { dfs_bit_clear(b); return 0; }
            /* Zero the freshly allocated block on disk. */
            uint8_t zero[DISKFS_SECTOR];
            memset(zero, 0, sizeof(zero));
            uint32_t base = g_fs.data_lba + b * DFS_SECS_PER_BLOCK;
            for (uint32_t s = 0; s < DFS_SECS_PER_BLOCK; s++) {
                if (ahci_write(base + s, 1, zero) != 0) {
                    dfs_bit_clear(b); (void)dfs_flush_bitmap();
                    return 0;
                }
            }
            return b;
        }
    }
    return 0; /* full */
}

/* Free a data block (clears bit, write-throughs bitmap). */
static void dfs_block_free(uint32_t blk)
{
    if (blk == 0 || blk >= g_fs.block_count) return;
    dfs_bit_clear(blk);
    (void)dfs_flush_bitmap();
}

/* Read 4096-byte block `blk` into buf (must be >= DFS_BLOCK_SIZE). */
static int dfs_block_read(uint32_t blk, void* buf)
{
    if (blk == 0 || blk >= g_fs.block_count) return -1;
    uint32_t base = g_fs.data_lba + blk * DFS_SECS_PER_BLOCK;
    return ahci_read(base, DFS_SECS_PER_BLOCK, buf) == 0 ? 0 : -1;
}

/* Write 4096-byte block `blk` from buf. */
static int dfs_block_write(uint32_t blk, const void* buf)
{
    if (blk == 0 || blk >= g_fs.block_count) return -1;
    uint32_t base = g_fs.data_lba + blk * DFS_SECS_PER_BLOCK;
    return ahci_write(base, DFS_SECS_PER_BLOCK, buf) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* Block-pointer mapping (file logical block index -> data block index)      */
/* ------------------------------------------------------------------------- */

/* Maximum number of logical blocks a file may have. */
#define DFS_MAX_FILE_BLOCKS (DFS_DIRECT + DFS_PTRS_PER_BLOCK)

/* The single-indirect block holds DFS_PTRS_PER_BLOCK u32 pointers spread over
 * DFS_SECS_PER_BLOCK sectors. To keep stack usage tiny we touch the indirect
 * block ONE 512-byte sector at a time instead of buffering the whole 4 KB. */
#define DFS_PTRS_PER_SEC  (DISKFS_SECTOR / sizeof(uint32_t))   /* 128 */

/* Read pointer slot `idx` (0..DFS_PTRS_PER_BLOCK-1) from indirect block `iblk`.
 * Returns the stored data-block index, or 0 on hole/error. */
static uint32_t dfs_iptr_get(uint32_t iblk, uint32_t idx)
{
    // Bound `iblk` (an on-disk inode's `indirect` field, copied verbatim from an
    // untrusted image) against the volume size, exactly as dfs_block_read/write do.
    // Without it, iblk * DFS_SECS_PER_BLOCK overflows uint32 and the wrapped value
    // becomes an arbitrary in-bounds device LBA -> read of an arbitrary sector
    // (superblock/inode table/bitmap/other files) interpreted as a block pointer.
    if (iblk == 0 || iblk >= g_fs.block_count || idx >= DFS_PTRS_PER_BLOCK) return 0;
    uint32_t sec  = g_fs.data_lba + iblk * DFS_SECS_PER_BLOCK + (idx / DFS_PTRS_PER_SEC);
    uint32_t slot = idx % DFS_PTRS_PER_SEC;
    uint32_t buf[DFS_PTRS_PER_SEC];
    if (ahci_read(sec, 1, buf) != 0) return 0;
    return buf[slot];
}

/* Set pointer slot `idx` of indirect block `iblk` to `val` (read-modify-write
 * of the single 512-byte sector). Returns 0 on success, <0 on I/O error. */
static int dfs_iptr_set(uint32_t iblk, uint32_t idx, uint32_t val)
{
    // Same untrusted-iblk bound as dfs_iptr_get, but return -1 (error) here, NOT 0:
    // dfs_iptr_set's contract is 0==success, so returning 0 on a bad iblk would
    // falsely report a successful pointer write to dfs_map_block. Without the bound,
    // iblk * DFS_SECS_PER_BLOCK overflows to an arbitrary in-bounds LBA -> a
    // read-modify-WRITE of an arbitrary device sector (superblock/inode/bitmap/other
    // files), defeating the per-block bounds checks the rest of the FS enforces.
    if (iblk == 0 || iblk >= g_fs.block_count || idx >= DFS_PTRS_PER_BLOCK) return -1;
    uint32_t sec  = g_fs.data_lba + iblk * DFS_SECS_PER_BLOCK + (idx / DFS_PTRS_PER_SEC);
    uint32_t slot = idx % DFS_PTRS_PER_SEC;
    uint32_t buf[DFS_PTRS_PER_SEC];
    if (ahci_read(sec, 1, buf) != 0) return -1;
    buf[slot] = val;
    if (ahci_write(sec, 1, buf) != 0) return -1;
    return 0;
}

/* Resolve logical block `lbn` of inode `in` to a data-block index. If `alloc`
 * and the block (or the indirect block it lives in) is unallocated, allocate
 * it and persist the inode/indirect block. Returns the data-block index, or 0
 * on "absent and not allocated", or 0 on allocation failure (callers that pass
 * alloc=1 must treat 0 as ENOSPC). `*inode_dirty` is set true if the in-memory
 * inode was modified (caller must persist it). */
static uint32_t dfs_map_block(uint32_t ino, diskfs_inode_t* in, uint32_t lbn,
                              bool alloc, bool* inode_dirty)
{
    (void)ino;
    if (lbn >= DFS_MAX_FILE_BLOCKS) return 0;

    if (lbn < DFS_DIRECT) {
        if (in->direct[lbn] == 0 && alloc) {
            uint32_t nb = dfs_block_alloc();
            if (nb == 0) return 0;
            in->direct[lbn] = nb;
            if (inode_dirty) *inode_dirty = true;
        }
        return in->direct[lbn];
    }

    /* Single-indirect region. */
    uint32_t idx = lbn - DFS_DIRECT;            /* slot within indirect block */
    if (in->indirect == 0) {
        if (!alloc) return 0;
        uint32_t nb = dfs_block_alloc();         /* indirect block is zeroed   */
        if (nb == 0) return 0;
        in->indirect = nb;
        if (inode_dirty) *inode_dirty = true;
    }

    uint32_t blk = dfs_iptr_get(in->indirect, idx);
    if (blk == 0 && alloc) {
        uint32_t nb = dfs_block_alloc();
        if (nb == 0) return 0;
        if (dfs_iptr_set(in->indirect, idx, nb) != 0) {
            dfs_block_free(nb);
            return 0;
        }
        blk = nb;
    }
    return blk;
}

/* Free every data block (direct + indirect + the indirect block itself)
 * referenced by an inode and zero its pointers. */
static void dfs_free_inode_blocks(diskfs_inode_t* in)
{
    for (uint32_t i = 0; i < DFS_DIRECT; i++) {
        if (in->direct[i]) { dfs_block_free(in->direct[i]); in->direct[i] = 0; }
    }
    if (in->indirect) {
        for (uint32_t i = 0; i < DFS_PTRS_PER_BLOCK; i++) {
            uint32_t blk = dfs_iptr_get(in->indirect, i);
            if (blk) dfs_block_free(blk);
        }
        dfs_block_free(in->indirect);
        in->indirect = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Name / inode lookup helpers                                               */
/* ------------------------------------------------------------------------- */

static bool dfs_name_ok(const char* name)
{
    if (!name || name[0] == '\0') return false;
    size_t n = strnlen(name, DFS_NAME_MAX);
    return n < DFS_NAME_MAX;  /* must fit incl. NUL */
}

/* Find the inode number of a file by name, or -1. */
static int dfs_find_by_name(const char* name)
{
    if (!g_fs.mounted || !dfs_name_ok(name)) return -1;
    diskfs_inode_t in;
    for (uint32_t i = 0; i < g_fs.inode_count; i++) {
        if (dfs_read_inode(i, &in) != 0) return -1;
        if (in.type == DFS_INODE_TYPE_FILE &&
            strncmp(in.name, name, DFS_NAME_MAX) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Find the first free inode slot, or -1. */
static int dfs_find_free_inode(void)
{
    diskfs_inode_t in;
    for (uint32_t i = 0; i < g_fs.inode_count; i++) {
        if (dfs_read_inode(i, &in) != 0) return -1;
        if (in.type == DFS_INODE_TYPE_FREE) return (int)i;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Mount / format                                                            */
/* ------------------------------------------------------------------------- */

/* Populate g_fs from a (validated) superblock and load the bitmap into RAM.
 *
 * DEFENSIVE: although the superblock already passed magic+version+checksum, the
 * self-described layout fields drive a kmalloc() and a multi-sector ahci_read().
 * On a REAL disk (the T410 target) we never want a malformed/foreign/future
 * superblock to request an enormous allocation or read, so every field is
 * sanity-bounded here. Anything out of range fails the mount cleanly -- and a
 * failed mount is non-fatal (the OS roots on RAM ramfs), so boot continues. */
static int dfs_mount_from_sb(const diskfs_superblock_t* sb)
{
    /* Reject implausible geometry rather than trusting it blindly. The bitmap
     * sector count is the dangerous one (it sizes the allocation + the read);
     * cap it to the compile-time region size. The rest must match what this
     * build lays out, since the on-disk format is fixed for v2. */
    if (sb->bitmap_sectors == 0 || sb->bitmap_sectors > DFS_BITMAP_SECTORS ||
        sb->inode_size != DFS_INODE_SIZE ||
        sb->inode_count == 0 || sb->inode_count > DFS_INODE_COUNT ||
        sb->block_size != DFS_BLOCK_SIZE ||
        sb->block_count > DFS_BLOCK_COUNT ||   /* AUDIT FIX (gap-org): bound by real capacity (3584), not bitmap bits (4096) */
        sb->bitmap_lba == 0 || sb->inode_lba == 0 || sb->data_lba == 0) {
        kprintf("[DISKFS] superblock layout out of range -- refusing mount\n");
        return -1;
    }

    g_fs.bitmap_lba     = sb->bitmap_lba;
    g_fs.bitmap_sectors = sb->bitmap_sectors;
    g_fs.inode_lba      = sb->inode_lba;
    g_fs.inode_count    = sb->inode_count;
    g_fs.inode_size     = sb->inode_size;
    g_fs.data_lba       = sb->data_lba;
    g_fs.block_size     = sb->block_size;
    g_fs.block_count    = sb->block_count;

    if (!g_bitmap) {
        g_bitmap = (uint8_t*)kmalloc(g_fs.bitmap_sectors * DISKFS_SECTOR);
        if (!g_bitmap) return -1;
    }
    if (ahci_read(g_fs.bitmap_lba, g_fs.bitmap_sectors, g_bitmap) != 0) return -1;
    g_fs.mounted = true;
    return 0;
}

/*
 * diskfs_format -- (re)initialize the inode table + allocation bitmap on disk.
 * Lays out the fixed regions, zeroes the inode table, marks block 0 reserved,
 * and write-throughs everything. Leaves g_fs mounted on success.
 * Returns 0 on success, <0 on I/O error.
 *
 * Note: this does NOT touch the superblock itself -- diskfs_init owns the
 * superblock and sets fs_formatted/layout there. Callers that want the layout
 * persisted to the superblock should go through diskfs_init's format path.
 */
int diskfs_format(void)
{
    if (!ahci_present()) return -1;

    /* Establish the canonical (compile-time) layout in g_fs. */
    g_fs.bitmap_lba     = DFS_BITMAP_LBA;
    g_fs.bitmap_sectors = DFS_BITMAP_SECTORS;
    g_fs.inode_lba      = DFS_INODE_LBA;
    g_fs.inode_count    = DFS_INODE_COUNT;
    g_fs.inode_size     = DFS_INODE_SIZE;
    g_fs.data_lba       = DFS_DATA_LBA;
    g_fs.block_size     = DFS_BLOCK_SIZE;
    g_fs.block_count    = DFS_BLOCK_COUNT;

    /* Allocate / clear the in-memory bitmap. */
    if (!g_bitmap) {
        g_bitmap = (uint8_t*)kmalloc(g_fs.bitmap_sectors * DISKFS_SECTOR);
        if (!g_bitmap) return -1;
    }
    memset(g_bitmap, 0, g_fs.bitmap_sectors * DISKFS_SECTOR);

    /* Reserve block 0 (NULL sentinel) and any bitmap bits beyond block_count
     * so the allocator never hands them out. */
    dfs_bit_set(0);
    for (uint32_t b = g_fs.block_count; b < DFS_BITMAP_BITS; b++) {
        g_bitmap[b >> 3] |= (uint8_t)(1u << (b & 7));
    }
    if (dfs_flush_bitmap() != 0) return -1;

    /* Zero the entire inode table on disk. */
    uint8_t zero[DISKFS_SECTOR];
    memset(zero, 0, sizeof(zero));
    for (uint32_t s = 0; s < DFS_INODE_SECTORS; s++) {
        if (ahci_write(g_fs.inode_lba + s, 1, zero) != 0) return -1;
    }

    g_fs.mounted = true;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Public file API                                                           */
/* ------------------------------------------------------------------------- */

/* Create an empty file. Returns its inode number (>=0) or <0 on error
 * (-1 bad name / not mounted, -2 already exists, -3 no free inode, -4 I/O). */
int diskfs_create(const char* name)
{
    if (!g_fs.mounted || !dfs_name_ok(name)) return -1;
    if (dfs_find_by_name(name) >= 0) return -2;

    int ino = dfs_find_free_inode();
    if (ino < 0) return -3;

    diskfs_inode_t in;
    memset(&in, 0, sizeof(in));
    in.type = DFS_INODE_TYPE_FILE;
    in.size = 0;
    strncpy(in.name, name, DFS_NAME_MAX - 1);
    in.name[DFS_NAME_MAX - 1] = '\0';

    if (dfs_write_inode((uint32_t)ino, &in) != 0) return -4;
    return ino;
}

/* Look up a file by name. Returns its inode number or <0 if not found. */
int diskfs_open(const char* name)
{
    if (!g_fs.mounted) return -1;
    return dfs_find_by_name(name);
}

/* Size of a file in bytes, or <0 on bad inode. */
long diskfs_size(int ino)
{
    if (!g_fs.mounted || ino < 0 || (uint32_t)ino >= g_fs.inode_count) return -1;
    diskfs_inode_t in;
    if (dfs_read_inode((uint32_t)ino, &in) != 0) return -1;
    if (in.type != DFS_INODE_TYPE_FILE) return -1;
    return (long)in.size;
}

/* Read up to `len` bytes at offset `off`. Returns bytes read (>=0) or <0. */
long diskfs_read(int ino, unsigned long off, void* buf, unsigned long len)
{
    if (!g_fs.mounted || ino < 0 || (uint32_t)ino >= g_fs.inode_count) return -1;
    if (!buf) return -1;

    diskfs_inode_t in;
    if (dfs_read_inode((uint32_t)ino, &in) != 0) return -1;
    if (in.type != DFS_INODE_TYPE_FILE) return -1;

    if (off >= in.size) return 0;
    unsigned long avail = (unsigned long)in.size - off;
    if (len > avail) len = avail;
    if (len == 0) return 0;

    uint8_t  block[DFS_BLOCK_SIZE];
    uint8_t* out  = (uint8_t*)buf;
    unsigned long done = 0;
    bool dummy = false;

    while (done < len) {
        unsigned long pos  = off + done;
        uint32_t      lbn  = (uint32_t)(pos / DFS_BLOCK_SIZE);
        uint32_t      boff = (uint32_t)(pos % DFS_BLOCK_SIZE);
        unsigned long chunk = DFS_BLOCK_SIZE - boff;
        if (chunk > (len - done)) chunk = len - done;

        uint32_t blk = dfs_map_block((uint32_t)ino, &in, lbn, false, &dummy);
        if (blk == 0) {
            /* Sparse hole (shouldn't normally happen): return zeros. */
            memset(out + done, 0, chunk);
        } else {
            if (dfs_block_read(blk, block) != 0) return (done > 0) ? (long)done : -1;
            memcpy(out + done, block + boff, chunk);
        }
        done += chunk;
    }
    return (long)done;
}

/* Write `len` bytes at offset `off`, allocating blocks as needed and growing
 * the file. Returns bytes written (>=0) or <0. */
long diskfs_write(int ino, unsigned long off, const void* buf, unsigned long len)
{
    if (!g_fs.mounted || ino < 0 || (uint32_t)ino >= g_fs.inode_count) return -1;
    if (!buf) return -1;

    diskfs_inode_t in;
    if (dfs_read_inode((uint32_t)ino, &in) != 0) return -1;
    if (in.type != DFS_INODE_TYPE_FILE) return -1;

    /* Bound the write to the maximum addressable file size. */
    unsigned long max_bytes = (unsigned long)DFS_MAX_FILE_BLOCKS * DFS_BLOCK_SIZE;
    if (off >= max_bytes) return -1;
    if (len > max_bytes - off) len = max_bytes - off;
    if (len == 0) return 0;

    uint8_t  block[DFS_BLOCK_SIZE];
    const uint8_t* src = (const uint8_t*)buf;
    unsigned long done = 0;
    bool inode_dirty = false;

    while (done < len) {
        unsigned long pos  = off + done;
        uint32_t      lbn  = (uint32_t)(pos / DFS_BLOCK_SIZE);
        uint32_t      boff = (uint32_t)(pos % DFS_BLOCK_SIZE);
        unsigned long chunk = DFS_BLOCK_SIZE - boff;
        if (chunk > (len - done)) chunk = len - done;

        uint32_t blk = dfs_map_block((uint32_t)ino, &in, lbn, true, &inode_dirty);
        if (blk == 0) break;  /* ENOSPC: stop, report partial */

        if (chunk == DFS_BLOCK_SIZE) {
            /* Full-block overwrite: no read-modify-write needed. */
            memcpy(block, src + done, DFS_BLOCK_SIZE);
        } else {
            if (dfs_block_read(blk, block) != 0) break;
            memcpy(block + boff, src + done, chunk);
        }
        if (dfs_block_write(blk, block) != 0) break;
        done += chunk;
    }

    /* Grow file size if we extended past the old EOF. */
    if (off + done > in.size) {
        in.size = (uint32_t)(off + done);
        inode_dirty = true;
    }
    if (inode_dirty) {
        if (dfs_write_inode((uint32_t)ino, &in) != 0)
            return (done > 0) ? (long)done : -1;
    }
    return (long)done;
}

/* Remove a file by name, freeing its blocks + inode. Returns 0 or <0. */
int diskfs_unlink(const char* name)
{
    if (!g_fs.mounted) return -1;
    int ino = dfs_find_by_name(name);
    if (ino < 0) return -1;

    diskfs_inode_t in;
    if (dfs_read_inode((uint32_t)ino, &in) != 0) return -1;

    dfs_free_inode_blocks(&in);
    memset(&in, 0, sizeof(in));      /* type = FREE */
    if (dfs_write_inode((uint32_t)ino, &in) != 0) return -1;
    return 0;
}

/* Fill `names` with the names of existing files (up to `max`). Returns count. */
int diskfs_list(char names[][64], int max)
{
    if (!g_fs.mounted || !names || max <= 0) return 0;
    int count = 0;
    diskfs_inode_t in;
    for (uint32_t i = 0; i < g_fs.inode_count && count < max; i++) {
        if (dfs_read_inode(i, &in) != 0) break;
        if (in.type == DFS_INODE_TYPE_FILE) {
            strncpy(names[count], in.name, 63);
            names[count][63] = '\0';
            count++;
        }
    }
    return count;
}

/* ------------------------------------------------------------------------- */
/* Filesystem self-test (run once on a fresh format)                         */
/* ------------------------------------------------------------------------- */

static bool diskfs_fs_selftest(void)
{
    static const char* kName = "boot.log";
    static const char* kMsg  = "AutomationOS diskfs file layer alive\n";
    size_t mlen = strlen(kMsg);

    int ino = diskfs_create(kName);
    if (ino < 0) { kprintf("[DISKFS] FS SELFTEST: create failed (%d)\n", ino); return false; }

    long w = diskfs_write(ino, 0, kMsg, mlen);
    if (w != (long)mlen) { kprintf("[DISKFS] FS SELFTEST: write %ld != %lu\n", w, (unsigned long)mlen); return false; }

    /* Read back via name lookup to exercise open() + read(). */
    int ino2 = diskfs_open(kName);
    if (ino2 != ino) { kprintf("[DISKFS] FS SELFTEST: open mismatch (%d vs %d)\n", ino2, ino); return false; }

    char rb[64];
    memset(rb, 0, sizeof(rb));
    long r = diskfs_read(ino2, 0, rb, mlen);
    if (r != (long)mlen || memcmp(rb, kMsg, mlen) != 0) {
        kprintf("[DISKFS] FS SELFTEST: read-back mismatch (r=%ld)\n", r);
        return false;
    }
    if (diskfs_size(ino2) != (long)mlen) {
        kprintf("[DISKFS] FS SELFTEST: size mismatch\n");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Boot-time init (superblock durability + fs mount/format)                  */
/* ------------------------------------------------------------------------- */

void diskfs_init(void)
{
    if (!ahci_present()) {
        kprintf("[DISKFS] no SATA disk -- persistence disabled\n");
        return;
    }

    /* 512-byte sector buffer on the kernel stack (rsp0). */
    uint8_t sec[DISKFS_SECTOR];
    memset(sec, 0, sizeof(sec));

    if (ahci_read(DISKFS_SB_LBA, 1, sec) != 0) {
        kprintf("[DISKFS] superblock read failed (LBA %u)\n", DISKFS_SB_LBA);
        return;
    }

    diskfs_superblock_t* sb = (diskfs_superblock_t*)sec;
    bool valid = (sb->magic == DISKFS_MAGIC &&
                  sb->version == DISKFS_VERSION &&
                  sb->checksum == diskfs_csum(sb));

    bool fresh_format = false;

    if (!valid) {
        /* Fresh / corrupt / old-version disk: format a new superblock AND lay
         * out the filesystem regions from scratch. */
        memset(sec, 0, sizeof(sec));
        sb->magic       = DISKFS_MAGIC;
        sb->version     = DISKFS_VERSION;
        sb->boot_count  = 0;
        sb->write_count = 0;

        /* Record the (canonical) fs layout into the superblock. */
        sb->bitmap_lba     = DFS_BITMAP_LBA;
        sb->bitmap_sectors = DFS_BITMAP_SECTORS;
        sb->inode_lba      = DFS_INODE_LBA;
        sb->inode_count    = DFS_INODE_COUNT;
        sb->inode_size     = DFS_INODE_SIZE;
        sb->data_lba       = DFS_DATA_LBA;
        sb->block_size     = DFS_BLOCK_SIZE;
        sb->block_count    = DFS_BLOCK_COUNT;
        sb->fs_formatted   = 1;

        /* Lay out inode table + bitmap on disk. */
        if (diskfs_format() != 0) {
            kprintf("[DISKFS] FS format failed -- filesystem unavailable\n");
            /* fall through: still try to keep the superblock durable */
            sb->fs_formatted = 0;
        } else {
            fresh_format = true;
        }
        kprintf("[DISKFS] no valid superblock -- formatting fresh disk\n");
    }

    sb->boot_count  += 1;
    sb->write_count += 1;
    sb->checksum     = diskfs_csum(sb);

    if (ahci_write(DISKFS_SB_LBA, 1, sec) != 0) {
        kprintf("[DISKFS] superblock write failed (LBA %u)\n", DISKFS_SB_LBA);
        return;
    }

    /* Read-after-write verify: prove the bytes actually hit the medium. */
    uint8_t verify[DISKFS_SECTOR];
    memset(verify, 0, sizeof(verify));
    if (ahci_read(DISKFS_SB_LBA, 1, verify) != 0 ||
        memcmp(verify, sec, DISKFS_SECTOR) != 0) {
        kprintf("[DISKFS] WRITE-VERIFY FAILED -- persistence NOT reliable\n");
        return;
    }

    /* On an existing (valid) superblock, mount the fs regions it describes. */
    if (!fresh_format && sb->fs_formatted) {
        if (dfs_mount_from_sb(sb) != 0) {
            kprintf("[DISKFS] FS mount failed -- filesystem unavailable\n");
        }
    }

    g_boot_count   = sb->boot_count;
    g_diskfs_ready = true;
    kprintf("[DISKFS] mounted: boot #%u (%s, write-verify OK)\n",
            sb->boot_count, valid ? "existing fs" : "freshly formatted");
    kprintf("[DISKFS] SELFTEST: PASS (durable superblock, boot #%u)\n",
            sb->boot_count);

    /* File-layer self-test: only on a fresh format (when the fs is empty and we
     * just laid it out). Proves the create/write/read/verify path end-to-end. */
    if (fresh_format) {
        if (diskfs_fs_selftest())
            kprintf("[DISKFS] FS SELFTEST: PASS (boot.log create/write/read OK)\n");
        else
            kprintf("[DISKFS] FS SELFTEST: FAIL\n");
    } else if (g_fs.mounted) {
        kprintf("[DISKFS] FS mounted (existing, %u inodes / %u blocks)\n",
                g_fs.inode_count, g_fs.block_count);
    }
}

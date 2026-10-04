/*
 * filesystem.c - RockFS ("RFS1") for ROK / RockOS
 *
 * ON-DISK LAYOUT (all LBAs absolute, FS_START_LBA = first sector)
 *   +0            superblock
 *   +1  .. +8     allocation bitmap (1 bit per sector, 1 = used)
 *   +9  .. +40    inode table (256 inodes x 64 bytes)
 *   +41 .. end    data sectors
 *   FS_RESERVED_LBA (32767) is marked "used" in the bitmap AND every
 *   sector access to it is rejected before it reaches your storage layer.
 *
 * Inode: 12 direct sectors + 1 single-indirect sector (128 entries)
 *        => max file size 71,680 bytes. Directories use the same layout.
 * Directory entry: 32 bytes (used, type, inode, 27-char name) = 16 per sector.
 * Inode 0 is the root directory.
 *
 * Static RAM used: about 3 KiB of sector buffers. No malloc/free.
 * No journaling: a power cut in the middle of an operation can leak
 * sectors, but never makes the directory tree point at garbage.
 */

#include "filesystem.h"

/* ================================================================== */
/* ===== ROK STORAGE ADAPTER ======================================== */
/* ================================================================== */
/*
 * This is the ONLY place that talks to your existing storage layer.
 * Replace the bodies of the functions below (see MANUAL INTEGRATION).
 * The filesystem already blocks LBA 32767 and out-of-range LBAs BEFORE
 * calling these, so they can stay dumb.
 */

#include "storage.h"
#include "display.h"

/* Read ONE 512-byte sector at 'lba' into 'buf'. Return 0 on success, non-zero on failure. */
static int filesystem_storage_read(uint32_t lba, void *buf)
{
    const block_device_t *device = storage_get_device();
    if (!device || !buf) return FS_ERR_IO;
    return block_read(device, lba, 1, buf) == BLOCK_RESULT_OK ? 0 : FS_ERR_IO;
}

/* Write ONE 512-byte sector from 'buf' to 'lba'. Return 0 on success, non-zero on failure. */
static int filesystem_storage_write(uint32_t lba, const void *buf)
{
    const block_device_t *device = storage_get_device();
    if (!device || !buf) return FS_ERR_IO;
    return block_write(device, lba, 1, buf) == BLOCK_RESULT_OK ? 0 : FS_ERR_IO;
}

/* Print one line of text to your screen/debug output. */
static void filesystem_log(const char *msg)
{
    if (msg) display_puts(msg);
    display_putchar('\n');
}

/* OPTIONAL: timestamp stored in each file's mtime (e.g. your PIT tick count). 0 is fine. */
static uint32_t filesystem_get_time(void)
{
    return 0;    /* OPTIONAL REPLACE: e.g. return (uint32_t)your_tick_counter; */
}

/* ================================================================== */
/* ===== END ROK STORAGE ADAPTER ==================================== */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Layout constants                                                    */
/* ------------------------------------------------------------------ */
#define FS_MAGIC             0x31534652u   /* bytes 'R','F','S','1' */
#define FS_VERSION           1u

#define FS_REGION_SECTORS    (FS_DISK_SECTORS - FS_START_LBA)
#define FS_BITS_PER_SECTOR   (FS_SECTOR_SIZE * 8u)
#define FS_BITMAP_SECTORS    ((FS_REGION_SECTORS + FS_BITS_PER_SECTOR - 1u) / FS_BITS_PER_SECTOR)

#define FS_INODE_SIZE        64u
#define FS_INODE_COUNT       256u
#define FS_INODES_PER_SECTOR (FS_SECTOR_SIZE / FS_INODE_SIZE)
#define FS_INODE_SECTORS     (FS_INODE_COUNT / FS_INODES_PER_SECTOR)

#define FS_BITMAP_LBA        (FS_START_LBA + 1u)
#define FS_INODE_LBA         (FS_BITMAP_LBA + FS_BITMAP_SECTORS)
#define FS_DATA_LBA          (FS_INODE_LBA + FS_INODE_SECTORS)

#define FS_DIRECT            12u
#define FS_INDIRECT_ENTRIES  (FS_SECTOR_SIZE / 4u)
#define FS_MAX_FILE_BYTES    ((FS_DIRECT + FS_INDIRECT_ENTRIES) * FS_SECTOR_SIZE)

#define FS_DIRENT_SIZE       32u
#define FS_DIRENTS_PER_SEC   (FS_SECTOR_SIZE / FS_DIRENT_SIZE)

#define FS_ROOT_INODE        0u

#if (FS_DATA_LBA >= FS_RESERVED_LBA)
#error "RockFS metadata does not fit before FS_RESERVED_LBA"
#endif
#if (FS_RESERVED_LBA >= FS_DISK_SECTORS)
#error "FS_RESERVED_LBA must be inside the disk"
#endif

/* ------------------------------------------------------------------ */
/* On-disk structures                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t sector_size;
    uint32_t total_sectors;
    uint32_t start_lba;
    uint32_t reserved_lba;
    uint32_t bitmap_start;
    uint32_t bitmap_sectors;
    uint32_t inode_start;
    uint32_t inode_sectors;
    uint32_t inode_count;
    uint32_t data_start;
    uint32_t mount_count;
    uint32_t checksum;        /* must stay LAST */
} fs_super_t;

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t parent;          /* parent directory inode (used for "..") */
    uint32_t size;
    uint32_t mtime;
    uint32_t direct[FS_DIRECT];
    uint32_t indirect;
} fs_inode_t;

typedef struct {
    uint8_t  used;
    uint8_t  type;
    uint16_t inode;
    char     name[FS_NAME_MAX + 1];   /* 28 bytes, always NUL-terminated */
} fs_dirent_t;

_Static_assert(sizeof(fs_super_t) == 52, "fs_super_t size");
_Static_assert(sizeof(fs_inode_t) == FS_INODE_SIZE, "fs_inode_t size");
_Static_assert(sizeof(fs_dirent_t) == FS_DIRENT_SIZE, "fs_dirent_t size");

/* ------------------------------------------------------------------ */
/* Static state / buffers (no malloc)                                  */
/* ------------------------------------------------------------------ */
static fs_super_t  g_sb;
static int         g_mounted;
static uint16_t    g_cwd;
static uint32_t    g_io_reads;
static uint32_t    g_io_writes;
static uint32_t    g_alloc_hint;      /* bitmap sector to start searching in */

static uint8_t     g_bm_buf[FS_SECTOR_SIZE]   __attribute__((aligned(4)));  /* bitmap sector    */
static fs_inode_t  g_ino_tbl[FS_INODES_PER_SECTOR];                          /* inode sector     */
static fs_dirent_t g_dir_tbl[FS_DIRENTS_PER_SEC];                            /* directory sector */
static uint32_t    g_ind_tbl[FS_INDIRECT_ENTRIES];                           /* indirect sector  */
static uint8_t     g_data_buf[FS_SECTOR_SIZE] __attribute__((aligned(4)));  /* file data/super  */
static uint8_t     g_zero[FS_SECTOR_SIZE]     __attribute__((aligned(4)));  /* always all zero  */

/* ------------------------------------------------------------------ */
/* Tiny helpers (no libc dependency)                                   */
/* ------------------------------------------------------------------ */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void fs_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void fs_memset(void *dst, uint8_t v, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}

static uint32_t fs_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int fs_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint32_t fs_checksum(const void *p, uint32_t n)   /* FNV-1a */
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 0x811C9DC5u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* ------------------------------------------------------------------ */
/* Guarded sector I/O - the ONLY path to the adapter                   */
/* ------------------------------------------------------------------ */
static int sec_check(uint32_t lba)
{
    if (lba == FS_RESERVED_LBA) return FS_ERR_RESERVED;
    if ((uint32_t)(lba - FS_START_LBA) >= FS_REGION_SECTORS) return FS_ERR_RANGE;   /* below start or past end */
    return FS_OK;
}

static int sec_read(uint32_t lba, void *buf)
{
    int r = sec_check(lba);
    if (r) return r;
    if (filesystem_storage_read(lba, buf) != 0) return FS_ERR_IO;
    g_io_reads++;
    return FS_OK;
}

static int sec_write(uint32_t lba, const void *buf)
{
    int r = sec_check(lba);
    if (r) return r;
    if (filesystem_storage_write(lba, buf) != 0) return FS_ERR_IO;
    g_io_writes++;
    return FS_OK;
}

/* ------------------------------------------------------------------ */
/* Allocation bitmap                                                   */
/* ------------------------------------------------------------------ */
static int bm_alloc(uint32_t *out_lba)
{
    uint32_t n, b, byte, bit, idx, lba;
    int r;

    for (n = 0; n < FS_BITMAP_SECTORS; n++) {
        b = (g_alloc_hint + n) % FS_BITMAP_SECTORS;
        r = sec_read(FS_BITMAP_LBA + b, g_bm_buf);
        if (r) return r;
        for (byte = 0; byte < FS_SECTOR_SIZE; byte++) {
            if (g_bm_buf[byte] == 0xFF) continue;
            for (bit = 0; bit < 8; bit++) {
                if (g_bm_buf[byte] & (1u << bit)) continue;
                idx = b * FS_BITS_PER_SECTOR + byte * 8u + bit;
                lba = FS_START_LBA + idx;
                if (idx >= FS_REGION_SECTORS || lba < FS_DATA_LBA || lba == FS_RESERVED_LBA)
                    continue;                      /* never hand these out */
                g_bm_buf[byte] |= (uint8_t)(1u << bit);
                r = sec_write(FS_BITMAP_LBA + b, g_bm_buf);
                if (r) return r;
                g_alloc_hint = b;
                *out_lba = lba;
                return FS_OK;
            }
        }
    }
    return FS_ERR_FULL;
}

static int bm_free(uint32_t lba)
{
    uint32_t idx, b, byte, bit;
    int r;

    if (lba < FS_DATA_LBA || lba >= FS_DISK_SECTORS || lba == FS_RESERVED_LBA)
        return FS_ERR_CORRUPT;
    idx  = lba - FS_START_LBA;
    b    = idx / FS_BITS_PER_SECTOR;
    byte = (idx % FS_BITS_PER_SECTOR) / 8u;
    bit  = idx % 8u;
    r = sec_read(FS_BITMAP_LBA + b, g_bm_buf);
    if (r) return r;
    g_bm_buf[byte] &= (uint8_t)~(1u << bit);
    return sec_write(FS_BITMAP_LBA + b, g_bm_buf);
}

/* allocate a data sector and fill it with zeros */
static int alloc_zeroed_block(uint32_t *out_lba)
{
    uint32_t lba;
    int r = bm_alloc(&lba);
    if (r) return r;
    r = sec_write(lba, g_zero);
    if (r) { bm_free(lba); return r; }
    *out_lba = lba;
    return FS_OK;
}

/* ------------------------------------------------------------------ */
/* Inodes                                                              */
/* ------------------------------------------------------------------ */
static int inode_read(uint16_t ino, fs_inode_t *out)
{
    int r;
    if (ino >= FS_INODE_COUNT) return FS_ERR_CORRUPT;
    r = sec_read(FS_INODE_LBA + ino / FS_INODES_PER_SECTOR, g_ino_tbl);
    if (r) return r;
    fs_memcpy(out, &g_ino_tbl[ino % FS_INODES_PER_SECTOR], FS_INODE_SIZE);
    return FS_OK;
}

static int inode_write(uint16_t ino, const fs_inode_t *in)
{
    uint32_t lba;
    int r;
    if (ino >= FS_INODE_COUNT) return FS_ERR_CORRUPT;
    lba = FS_INODE_LBA + ino / FS_INODES_PER_SECTOR;
    r = sec_read(lba, g_ino_tbl);
    if (r) return r;
    fs_memcpy(&g_ino_tbl[ino % FS_INODES_PER_SECTOR], in, FS_INODE_SIZE);
    return sec_write(lba, g_ino_tbl);
}

static int inode_alloc(uint16_t *out)
{
    uint32_t s, i;
    int r;
    for (s = 0; s < FS_INODE_SECTORS; s++) {
        r = sec_read(FS_INODE_LBA + s, g_ino_tbl);
        if (r) return r;
        for (i = 0; i < FS_INODES_PER_SECTOR; i++) {
            if (g_ino_tbl[i].type == FS_TYPE_NONE) {
                *out = (uint16_t)(s * FS_INODES_PER_SECTOR + i);
                return FS_OK;
            }
        }
    }
    return FS_ERR_NOINODE;
}

/*
 * Map file block index 'bi' to an absolute LBA.
 * alloc=1: allocate (zeroed) sectors as needed - the caller MUST write the
 * inode back afterwards. alloc=0: returns FS_ERR_NOENT for a hole.
 */
static int bmap(fs_inode_t *in, uint32_t bi, int alloc, uint32_t *out)
{
    uint32_t lba;
    int r;

    if (bi < FS_DIRECT) {
        if (in->direct[bi] == 0) {
            if (!alloc) return FS_ERR_NOENT;
            r = alloc_zeroed_block(&lba);
            if (r) return r;
            in->direct[bi] = lba;
        }
        *out = in->direct[bi];
        return FS_OK;
    }

    bi -= FS_DIRECT;
    if (bi >= FS_INDIRECT_ENTRIES) return FS_ERR_TOOBIG;

    if (in->indirect == 0) {
        if (!alloc) return FS_ERR_NOENT;
        r = alloc_zeroed_block(&lba);
        if (r) return r;
        in->indirect = lba;
    }
    r = sec_read(in->indirect, g_ind_tbl);
    if (r) return r;
    if (g_ind_tbl[bi] == 0) {
        if (!alloc) return FS_ERR_NOENT;
        r = alloc_zeroed_block(&lba);
        if (r) return r;
        g_ind_tbl[bi] = lba;
        r = sec_write(in->indirect, g_ind_tbl);
        if (r) { bm_free(lba); return r; }
    }
    *out = g_ind_tbl[bi];
    return FS_OK;
}

/* free every sector owned by an inode (in memory struct is cleared, caller writes it) */
static int inode_free_blocks(fs_inode_t *in)
{
    uint32_t i;
    int r, rr = FS_OK;

    for (i = 0; i < FS_DIRECT; i++) {
        if (in->direct[i]) {
            r = bm_free(in->direct[i]);
            if (r) rr = r;
            in->direct[i] = 0;
        }
    }
    if (in->indirect) {
        r = sec_read(in->indirect, g_ind_tbl);
        if (r) {
            rr = r;
        } else {
            for (i = 0; i < FS_INDIRECT_ENTRIES; i++) {
                if (g_ind_tbl[i]) {
                    r = bm_free(g_ind_tbl[i]);
                    if (r) rr = r;
                }
            }
        }
        r = bm_free(in->indirect);
        if (r) rr = r;
        in->indirect = 0;
    }
    in->size = 0;
    return rr;
}

/* ------------------------------------------------------------------ */
/* File data                                                           */
/* ------------------------------------------------------------------ */
static int file_write_at(uint16_t ino, uint32_t off, const void *data, uint32_t len)
{
    fs_inode_t in;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t done = 0, pos, bi, bo, n, lba;
    int r, err = FS_OK, wr;

    if (len == 0) return FS_OK;
    r = inode_read(ino, &in);
    if (r) return r;
    if (in.type != FS_TYPE_FILE) return FS_ERR_ISDIR;
    if (off > in.size) return FS_ERR_INVAL;                 /* no holes */
    if (len > FS_MAX_FILE_BYTES - off) return FS_ERR_TOOBIG;

    while (done < len) {
        pos = off + done;
        bi  = pos / FS_SECTOR_SIZE;
        bo  = pos % FS_SECTOR_SIZE;
        n   = FS_SECTOR_SIZE - bo;
        if (n > len - done) n = len - done;

        r = bmap(&in, bi, 1, &lba);
        if (r) { err = r; break; }

        if (n == FS_SECTOR_SIZE) {
            r = sec_write(lba, src + done);
        } else {
            r = sec_read(lba, g_data_buf);
            if (r) { err = r; break; }
            fs_memcpy(g_data_buf + bo, src + done, n);
            r = sec_write(lba, g_data_buf);
        }
        if (r) { err = r; break; }

        done += n;
        if (off + done > in.size) in.size = off + done;
    }

    in.mtime = filesystem_get_time();
    wr = inode_write(ino, &in);            /* always persist what we did */
    return err ? err : wr;
}

static int file_read_at(uint16_t ino, uint32_t off, void *buf, uint32_t len, uint32_t *out)
{
    fs_inode_t in;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0, pos, bi, bo, n, lba;
    int r;

    *out = 0;
    r = inode_read(ino, &in);
    if (r) return r;
    if (in.type != FS_TYPE_FILE) return FS_ERR_ISDIR;
    if (off >= in.size) return FS_OK;
    if (len > in.size - off) len = in.size - off;

    while (done < len) {
        pos = off + done;
        bi  = pos / FS_SECTOR_SIZE;
        bo  = pos % FS_SECTOR_SIZE;
        n   = FS_SECTOR_SIZE - bo;
        if (n > len - done) n = len - done;

        r = bmap(&in, bi, 0, &lba);
        if (r == FS_ERR_NOENT) {
            fs_memset(dst + done, 0, n);
        } else if (r) {
            *out = done;
            return r;
        } else {
            r = sec_read(lba, g_data_buf);
            if (r) { *out = done; return r; }
            fs_memcpy(dst + done, g_data_buf + bo, n);
        }
        done += n;
    }
    *out = done;
    return FS_OK;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */
static int name_ok(const char *name)
{
    uint32_t i, len = fs_strlen(name);
    if (len == 0 || len > FS_NAME_MAX) return 0;
    if (fs_streq(name, ".") || fs_streq(name, "..")) return 0;
    for (i = 0; i < len; i++) {
        if (name[i] == '/' || (uint8_t)name[i] < 0x20 || (uint8_t)name[i] > 0x7E) return 0;
    }
    return 1;
}

/* read directory slot 'idx' into *e (fresh sector read each call) */
static int dir_get(fs_inode_t *d, uint32_t idx, fs_dirent_t *e)
{
    uint32_t lba;
    int r = bmap(d, idx / FS_DIRENTS_PER_SEC, 0, &lba);
    if (r == FS_ERR_NOENT) { fs_memset(e, 0, FS_DIRENT_SIZE); return FS_OK; }
    if (r) return r;
    r = sec_read(lba, g_dir_tbl);
    if (r) return r;
    fs_memcpy(e, &g_dir_tbl[idx % FS_DIRENTS_PER_SEC], FS_DIRENT_SIZE);
    return FS_OK;
}

static int dir_find(fs_inode_t *d, const char *name, uint16_t *ino, uint8_t *type, uint32_t *slot)
{
    uint32_t nslots = d->size / FS_DIRENT_SIZE;
    uint32_t blocks = (nslots + FS_DIRENTS_PER_SEC - 1u) / FS_DIRENTS_PER_SEC;
    uint32_t b, i, s, lba;
    int r;

    for (b = 0; b < blocks; b++) {
        r = bmap(d, b, 0, &lba);
        if (r == FS_ERR_NOENT) continue;
        if (r) return r;
        r = sec_read(lba, g_dir_tbl);
        if (r) return r;
        for (i = 0; i < FS_DIRENTS_PER_SEC; i++) {
            s = b * FS_DIRENTS_PER_SEC + i;
            if (s >= nslots) break;
            if (g_dir_tbl[i].used && fs_streq(g_dir_tbl[i].name, name)) {
                if (ino)  *ino  = g_dir_tbl[i].inode;
                if (type) *type = g_dir_tbl[i].type;
                if (slot) *slot = s;
                return FS_OK;
            }
        }
    }
    return FS_ERR_NOENT;
}

/* find the name of child inode 'child' inside directory d */
static int dir_name_of(fs_inode_t *d, uint16_t child, char *out)
{
    uint32_t nslots = d->size / FS_DIRENT_SIZE;
    uint32_t blocks = (nslots + FS_DIRENTS_PER_SEC - 1u) / FS_DIRENTS_PER_SEC;
    uint32_t b, i, s, lba, k;
    int r;

    for (b = 0; b < blocks; b++) {
        r = bmap(d, b, 0, &lba);
        if (r == FS_ERR_NOENT) continue;
        if (r) return r;
        r = sec_read(lba, g_dir_tbl);
        if (r) return r;
        for (i = 0; i < FS_DIRENTS_PER_SEC; i++) {
            s = b * FS_DIRENTS_PER_SEC + i;
            if (s >= nslots) break;
            if (g_dir_tbl[i].used && g_dir_tbl[i].inode == child) {
                for (k = 0; k <= FS_NAME_MAX; k++) out[k] = g_dir_tbl[i].name[k];
                return FS_OK;
            }
        }
    }
    return FS_ERR_NOENT;
}

static int dir_is_empty(fs_inode_t *d, int *empty)
{
    uint32_t nslots = d->size / FS_DIRENT_SIZE;
    uint32_t blocks = (nslots + FS_DIRENTS_PER_SEC - 1u) / FS_DIRENTS_PER_SEC;
    uint32_t b, i, s, lba;
    int r;

    *empty = 1;
    for (b = 0; b < blocks; b++) {
        r = bmap(d, b, 0, &lba);
        if (r == FS_ERR_NOENT) continue;
        if (r) return r;
        r = sec_read(lba, g_dir_tbl);
        if (r) return r;
        for (i = 0; i < FS_DIRENTS_PER_SEC; i++) {
            s = b * FS_DIRENTS_PER_SEC + i;
            if (s >= nslots) break;
            if (g_dir_tbl[i].used) { *empty = 0; return FS_OK; }
        }
    }
    return FS_OK;
}

/* add an entry to directory inode 'dino' (reuses a free slot, else grows) */
static int dir_add(uint16_t dino, const char *name, uint8_t type, uint16_t ino)
{
    fs_inode_t d;
    uint32_t nslots, blocks, b, i, s, lba, k;
    int r, wr, found = 0;

    r = inode_read(dino, &d);
    if (r) return r;

    nslots = d.size / FS_DIRENT_SIZE;
    blocks = (nslots + FS_DIRENTS_PER_SEC - 1u) / FS_DIRENTS_PER_SEC;
    s = nslots;                                    /* default: append */

    for (b = 0; b < blocks && !found; b++) {
        r = bmap(&d, b, 0, &lba);
        if (r == FS_ERR_NOENT) continue;
        if (r) return r;
        r = sec_read(lba, g_dir_tbl);
        if (r) return r;
        for (i = 0; i < FS_DIRENTS_PER_SEC; i++) {
            if (b * FS_DIRENTS_PER_SEC + i >= nslots) break;
            if (!g_dir_tbl[i].used) { s = b * FS_DIRENTS_PER_SEC + i; found = 1; break; }
        }
    }

    r = bmap(&d, s / FS_DIRENTS_PER_SEC, 1, &lba);
    if (r == FS_OK) r = sec_read(lba, g_dir_tbl);
    if (r == FS_OK) {
        fs_dirent_t *e = &g_dir_tbl[s % FS_DIRENTS_PER_SEC];
        fs_memset(e, 0, FS_DIRENT_SIZE);
        e->used  = 1;
        e->type  = type;
        e->inode = ino;
        for (k = 0; k < FS_NAME_MAX && name[k]; k++) e->name[k] = name[k];
        r = sec_write(lba, g_dir_tbl);
        if (r == FS_OK && s == nslots) d.size += FS_DIRENT_SIZE;
    }
    wr = inode_write(dino, &d);                   /* persist any block allocation */
    return r ? r : wr;
}

static int dir_remove_slot(uint16_t dino, uint32_t slot)
{
    fs_inode_t d;
    uint32_t lba;
    int r = inode_read(dino, &d);
    if (r) return r;
    r = bmap(&d, slot / FS_DIRENTS_PER_SEC, 0, &lba);
    if (r) return r;
    r = sec_read(lba, g_dir_tbl);
    if (r) return r;
    g_dir_tbl[slot % FS_DIRENTS_PER_SEC].used = 0;
    return sec_write(lba, g_dir_tbl);
}

/* ------------------------------------------------------------------ */
/* Path walking                                                        */
/* ------------------------------------------------------------------ */
/*
 * stop_at_parent = 0: resolve the whole path to an inode (*ino_out).
 * stop_at_parent = 1: resolve everything but the last component;
 *                     *ino_out = parent dir inode, leaf = last name.
 * Supports "/", ".", "..", repeated and trailing slashes.
 */
static int path_walk(const char *path, int stop_at_parent, uint16_t *ino_out, char *leaf)
{
    uint16_t cur;
    const char *p;
    char comp[FS_NAME_MAX + 1];
    fs_inode_t d;
    uint32_t len, k;
    uint16_t next;
    int r, last;

    if (!path) return FS_ERR_INVAL;
    cur = (path[0] == '/') ? (uint16_t)FS_ROOT_INODE : g_cwd;
    p = path;
    while (*p == '/') p++;

    if (*p == 0) {
        if (stop_at_parent) return FS_ERR_INVAL;
        *ino_out = cur;
        return FS_OK;
    }

    for (;;) {
        len = 0;
        while (*p && *p != '/') {
            if (len >= FS_NAME_MAX) return FS_ERR_NAME;
            comp[len++] = *p++;
        }
        comp[len] = 0;
        while (*p == '/') p++;
        last = (*p == 0);

        r = inode_read(cur, &d);
        if (r) return r;
        if (d.type != FS_TYPE_DIR) return FS_ERR_NOTDIR;

        if (last && stop_at_parent) {
            for (k = 0; k <= len; k++) leaf[k] = comp[k];
            *ino_out = cur;
            return FS_OK;
        }

        if (fs_streq(comp, ".")) {
            /* stay */
        } else if (fs_streq(comp, "..")) {
            cur = d.parent;
        } else {
            r = dir_find(&d, comp, &next, 0, 0);
            if (r) return r;
            cur = next;
        }
        if (last) { *ino_out = cur; return FS_OK; }
    }
}

static void last_component(const char *path, char *out)
{
    uint32_t len = fs_strlen(path), end = len, start, k = 0;
    while (end > 0 && path[end - 1] == '/') end--;
    if (end == 0) { out[0] = '/'; out[1] = 0; return; }
    start = end;
    while (start > 0 && path[start - 1] != '/') start--;
    while (start < end && k < FS_NAME_MAX) out[k++] = path[start++];
    out[k] = 0;
}

/* ------------------------------------------------------------------ */
/* Node create / remove                                                */
/* ------------------------------------------------------------------ */
static int create_node(const char *path, uint8_t type, uint16_t *out_ino)
{
    char leaf[FS_NAME_MAX + 1];
    fs_inode_t pd, n;
    uint16_t parent, ino;
    int r;

    leaf[0] = 0;
    r = path_walk(path, 1, &parent, leaf);
    if (r) return r;
    if (!name_ok(leaf)) return FS_ERR_NAME;

    r = inode_read(parent, &pd);
    if (r) return r;
    if (pd.type != FS_TYPE_DIR) return FS_ERR_NOTDIR;

    r = dir_find(&pd, leaf, 0, 0, 0);
    if (r == FS_OK) return FS_ERR_EXIST;
    if (r != FS_ERR_NOENT) return r;

    r = inode_alloc(&ino);
    if (r) return r;

    fs_memset(&n, 0, FS_INODE_SIZE);
    n.type   = type;
    n.parent = parent;
    n.mtime  = filesystem_get_time();
    r = inode_write(ino, &n);
    if (r) return r;

    r = dir_add(parent, leaf, type, ino);
    if (r) {
        fs_memset(&n, 0, FS_INODE_SIZE);      /* roll back: free the inode */
        inode_write(ino, &n);
        return r;
    }
    if (out_ino) *out_ino = ino;
    return FS_OK;
}

static int remove_node(const char *path, int want_dir)
{
    char leaf[FS_NAME_MAX + 1];
    fs_inode_t pd, in;
    uint16_t parent, ino;
    uint8_t type;
    uint32_t slot;
    int r, empty, rr;

    leaf[0] = 0;
    r = path_walk(path, 1, &parent, leaf);
    if (r) return r;
    if (!name_ok(leaf)) return FS_ERR_NAME;

    r = inode_read(parent, &pd);
    if (r) return r;
    r = dir_find(&pd, leaf, &ino, &type, &slot);
    if (r) return r;
    r = inode_read(ino, &in);
    if (r) return r;

    if (want_dir) {
        if (in.type != FS_TYPE_DIR) return FS_ERR_NOTDIR;
        r = dir_is_empty(&in, &empty);
        if (r) return r;
        if (!empty) return FS_ERR_NOTEMPTY;
    } else {
        if (in.type == FS_TYPE_DIR) return FS_ERR_ISDIR;
    }

    /* unlink first, then free: a crash can only leak, never dangle */
    r = dir_remove_slot(parent, slot);
    if (r) return r;

    rr = inode_free_blocks(&in);
    in.type = FS_TYPE_NONE;
    in.flags = 0;
    in.parent = 0;
    in.mtime = 0;
    r = inode_write(ino, &in);
    if (g_cwd == ino) g_cwd = (uint16_t)FS_ROOT_INODE;
    return r ? r : rr;
}

/* ------------------------------------------------------------------ */
/* Mount / format                                                      */
/* ------------------------------------------------------------------ */
static void sb_store(void)
{
    g_sb.checksum = fs_checksum(&g_sb, (uint32_t)(sizeof(g_sb) - sizeof(g_sb.checksum)));
    fs_memset(g_data_buf, 0, FS_SECTOR_SIZE);
    fs_memcpy(g_data_buf, &g_sb, sizeof(g_sb));
}

static int region_is_blank(int *blank)
{
    uint32_t lba, i;
    int r;
    *blank = 1;
    for (lba = FS_START_LBA; lba < FS_DATA_LBA; lba++) {
        r = sec_read(lba, g_data_buf);
        if (r) return r;
        for (i = 0; i < FS_SECTOR_SIZE; i++) {
            if (g_data_buf[i]) { *blank = 0; return FS_OK; }
        }
    }
    return FS_OK;
}

int filesystem_format(void)
{
    fs_super_t sb;
    fs_inode_t root;
    uint32_t b, bit, idx, lba;
    int r;

    g_mounted = 0;

    /* 1. allocation bitmap: metadata + reserved LBA + out-of-region bits = used */
    for (b = 0; b < FS_BITMAP_SECTORS; b++) {
        fs_memset(g_bm_buf, 0, FS_SECTOR_SIZE);
        for (bit = 0; bit < FS_BITS_PER_SECTOR; bit++) {
            idx = b * FS_BITS_PER_SECTOR + bit;
            lba = FS_START_LBA + idx;
            if (idx >= FS_REGION_SECTORS || lba < FS_DATA_LBA || lba == FS_RESERVED_LBA)
                g_bm_buf[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
        }
        r = sec_write(FS_BITMAP_LBA + b, g_bm_buf);
        if (r) return r;
    }

    /* 2. empty inode table */
    for (b = 0; b < FS_INODE_SECTORS; b++) {
        r = sec_write(FS_INODE_LBA + b, g_zero);
        if (r) return r;
    }

    /* 3. root directory = inode 0 */
    fs_memset(&root, 0, FS_INODE_SIZE);
    root.type   = FS_TYPE_DIR;
    root.parent = (uint16_t)FS_ROOT_INODE;
    root.mtime  = filesystem_get_time();
    r = inode_write((uint16_t)FS_ROOT_INODE, &root);
    if (r) return r;

    /* 4. superblock LAST: a crash mid-format leaves the disk "unformatted" */
    fs_memset(&sb, 0, sizeof(sb));
    sb.magic          = FS_MAGIC;
    sb.version        = (uint16_t)FS_VERSION;
    sb.sector_size    = (uint16_t)FS_SECTOR_SIZE;
    sb.total_sectors  = FS_DISK_SECTORS;
    sb.start_lba      = FS_START_LBA;
    sb.reserved_lba   = FS_RESERVED_LBA;
    sb.bitmap_start   = FS_BITMAP_LBA;
    sb.bitmap_sectors = FS_BITMAP_SECTORS;
    sb.inode_start    = FS_INODE_LBA;
    sb.inode_sectors  = FS_INODE_SECTORS;
    sb.inode_count    = FS_INODE_COUNT;
    sb.data_start     = FS_DATA_LBA;
    sb.mount_count    = 1;
    g_sb = sb;
    sb_store();
    r = sec_write(FS_START_LBA, g_data_buf);
    if (r) return r;

    g_alloc_hint = 0;
    g_cwd = (uint16_t)FS_ROOT_INODE;
    g_mounted = 1;
    return FS_OK;
}

static int sb_valid(const fs_super_t *sb)
{
    return sb->magic == FS_MAGIC &&
           sb->version == FS_VERSION &&
           sb->sector_size == FS_SECTOR_SIZE &&
           sb->total_sectors == FS_DISK_SECTORS &&
           sb->start_lba == FS_START_LBA &&
           sb->reserved_lba == FS_RESERVED_LBA &&
           sb->bitmap_start == FS_BITMAP_LBA &&
           sb->bitmap_sectors == FS_BITMAP_SECTORS &&
           sb->inode_start == FS_INODE_LBA &&
           sb->inode_sectors == FS_INODE_SECTORS &&
           sb->inode_count == FS_INODE_COUNT &&
           sb->data_start == FS_DATA_LBA &&
           sb->checksum == fs_checksum(sb, (uint32_t)(sizeof(*sb) - sizeof(sb->checksum)));
}

int filesystem_init(void)
{
    fs_super_t sb;
    fs_inode_t root;
    int r, blank;

    g_mounted = 0;
    g_cwd = (uint16_t)FS_ROOT_INODE;
    g_alloc_hint = 0;

    r = sec_read(FS_START_LBA, g_data_buf);
    if (r) {
        filesystem_log("FS: cannot read superblock sector - is the ROK STORAGE ADAPTER connected?");
        return r;
    }
    fs_memcpy(&sb, g_data_buf, sizeof(sb));

    if (sb.magic == FS_MAGIC) {
        /* Looks like RockFS. Verify it; NEVER format on a mismatch. */
        if (!sb_valid(&sb)) {
            filesystem_log("FS: RockFS header found but invalid/incompatible - NOT formatting");
            return FS_ERR_CORRUPT;
        }
        g_sb = sb;
        r = inode_read((uint16_t)FS_ROOT_INODE, &root);
        if (r) return r;
        if (root.type != FS_TYPE_DIR) {
            filesystem_log("FS: root directory missing - NOT formatting");
            return FS_ERR_CORRUPT;
        }
        g_sb.mount_count++;
        sb_store();
        r = sec_write(FS_START_LBA, g_data_buf);
        if (r) return r;
        g_mounted = 1;
        filesystem_log("FS: mounted existing RockFS");
        return FS_OK;
    }

    /* Not RockFS. Only format if the metadata area is completely blank. */
    r = region_is_blank(&blank);
    if (r) return r;
    if (blank || FS_AUTO_FORMAT_UNRECOGNIZED) {
        filesystem_log("FS: no filesystem found, formatting");
        r = filesystem_format();
        if (r == FS_OK) filesystem_log("FS: format complete, mounted");
        return r;
    }
    filesystem_log("FS: disk has other data and no RockFS - refusing to format");
    filesystem_log("FS: change FS_START_LBA, or call filesystem_format() on purpose");
    return FS_ERR_NOT_FORMATTED;
}

int filesystem_is_mounted(void)
{
    return g_mounted;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
#define REQUIRE_MOUNTED() do { if (!g_mounted) return FS_ERR_NOT_MOUNTED; } while (0)

int filesystem_create(const char *path)
{
    REQUIRE_MOUNTED();
    return create_node(path, FS_TYPE_FILE, 0);
}

int filesystem_mkdir(const char *path)
{
    REQUIRE_MOUNTED();
    return create_node(path, FS_TYPE_DIR, 0);
}

int filesystem_delete(const char *path)
{
    REQUIRE_MOUNTED();
    return remove_node(path, 0);
}

int filesystem_rmdir(const char *path)
{
    REQUIRE_MOUNTED();
    return remove_node(path, 1);
}

int filesystem_write(const char *path, const void *data, uint32_t len)
{
    fs_inode_t in;
    uint16_t ino;
    int r;

    REQUIRE_MOUNTED();
    if (!data && len) return FS_ERR_INVAL;

    r = path_walk(path, 0, &ino, 0);
    if (r == FS_ERR_NOENT) {
        r = create_node(path, FS_TYPE_FILE, &ino);
        if (r) return r;
    } else if (r) {
        return r;
    } else {
        r = inode_read(ino, &in);
        if (r) return r;
        if (in.type == FS_TYPE_DIR) return FS_ERR_ISDIR;
        r = inode_free_blocks(&in);              /* replace: drop old contents */
        in.mtime = filesystem_get_time();
        if (r) { inode_write(ino, &in); return r; }
        r = inode_write(ino, &in);
        if (r) return r;
    }
    return file_write_at(ino, 0, data, len);
}

int filesystem_append(const char *path, const void *data, uint32_t len)
{
    fs_inode_t in;
    uint16_t ino;
    int r;

    REQUIRE_MOUNTED();
    if (!data && len) return FS_ERR_INVAL;

    r = path_walk(path, 0, &ino, 0);
    if (r == FS_ERR_NOENT) {
        r = create_node(path, FS_TYPE_FILE, &ino);
        if (r) return r;
    } else if (r) {
        return r;
    }
    r = inode_read(ino, &in);
    if (r) return r;
    if (in.type == FS_TYPE_DIR) return FS_ERR_ISDIR;
    return file_write_at(ino, in.size, data, len);
}

int filesystem_read_at(const char *path, uint32_t offset, void *buf, uint32_t len, uint32_t *bytes_read)
{
    uint16_t ino;
    uint32_t got = 0;
    int r;

    REQUIRE_MOUNTED();
    if (!buf && len) return FS_ERR_INVAL;
    r = path_walk(path, 0, &ino, 0);
    if (r) { if (bytes_read) *bytes_read = 0; return r; }
    r = file_read_at(ino, offset, buf, len, &got);
    if (bytes_read) *bytes_read = got;
    return r;
}

int filesystem_read(const char *path, void *buf, uint32_t max_len, uint32_t *bytes_read)
{
    return filesystem_read_at(path, 0, buf, max_len, bytes_read);
}

int filesystem_list(const char *path, filesystem_list_cb cb, void *ctx)
{
    fs_inode_t d, child;
    fs_dirent_t e;
    filesystem_info_t info;
    uint16_t dino;
    uint32_t s, nslots, k;
    int r;

    REQUIRE_MOUNTED();
    if (!cb) return FS_ERR_INVAL;
    r = path_walk(path, 0, &dino, 0);
    if (r) return r;
    r = inode_read(dino, &d);
    if (r) return r;
    if (d.type != FS_TYPE_DIR) return FS_ERR_NOTDIR;

    nslots = d.size / FS_DIRENT_SIZE;
    for (s = 0; s < nslots; s++) {
        r = dir_get(&d, s, &e);
        if (r) return r;
        if (!e.used) continue;
        r = inode_read(e.inode, &child);
        if (r) return r;

        for (k = 0; k <= FS_NAME_MAX; k++) info.name[k] = e.name[k];
        info.name[FS_NAME_MAX] = 0;
        info.type  = child.type;
        info.inode = e.inode;
        info.size  = child.size;
        info.mtime = child.mtime;
        if (cb(&info, ctx)) break;
    }
    return FS_OK;
}

int filesystem_stat(const char *path, filesystem_info_t *info)
{
    fs_inode_t in;
    uint16_t ino;
    int r;

    REQUIRE_MOUNTED();
    if (!info) return FS_ERR_INVAL;
    r = path_walk(path, 0, &ino, 0);
    if (r) return r;
    r = inode_read(ino, &in);
    if (r) return r;
    last_component(path, info->name);
    if (info->name[0] == 0) { info->name[0] = '/'; info->name[1] = 0; }
    info->type  = in.type;
    info->inode = ino;
    info->size  = in.size;
    info->mtime = in.mtime;
    return FS_OK;
}

int filesystem_exists(const char *path)
{
    filesystem_info_t info;
    return filesystem_stat(path, &info) == FS_OK;
}

int filesystem_chdir(const char *path)
{
    fs_inode_t in;
    uint16_t ino;
    int r;

    REQUIRE_MOUNTED();
    r = path_walk(path, 0, &ino, 0);
    if (r) return r;
    r = inode_read(ino, &in);
    if (r) return r;
    if (in.type != FS_TYPE_DIR) return FS_ERR_NOTDIR;
    g_cwd = ino;
    return FS_OK;
}

int filesystem_getcwd(char *buf, uint32_t size)
{
    char tmp[FS_PATH_MAX];
    char name[FS_NAME_MAX + 1];
    fs_inode_t in, pd;
    uint16_t cur;
    uint32_t pos = FS_PATH_MAX - 1u, len, k;
    int r;

    REQUIRE_MOUNTED();
    if (!buf || size < 2) return FS_ERR_INVAL;
    tmp[pos] = 0;
    cur = g_cwd;

    while (cur != FS_ROOT_INODE) {
        r = inode_read(cur, &in);
        if (r) return r;
        r = inode_read(in.parent, &pd);
        if (r) return r;
        r = dir_name_of(&pd, cur, name);
        if (r) return r;
        len = fs_strlen(name);
        if (pos < len + 1u) return FS_ERR_TOOBIG;
        pos -= len;
        for (k = 0; k < len; k++) tmp[pos + k] = name[k];
        tmp[--pos] = '/';
        cur = in.parent;
    }
    if (pos == FS_PATH_MAX - 1u) tmp[--pos] = '/';

    len = fs_strlen(&tmp[pos]);
    if (len + 1u > size) return FS_ERR_TOOBIG;
    for (k = 0; k <= len; k++) buf[k] = tmp[pos + k];
    return FS_OK;
}

int filesystem_get_stats(filesystem_stats_t *st)
{
    uint32_t b, bit, idx, lba, s, i, free_secs = 0, free_inodes = 0;
    int r;

    REQUIRE_MOUNTED();
    if (!st) return FS_ERR_INVAL;

    for (b = 0; b < FS_BITMAP_SECTORS; b++) {
        r = sec_read(FS_BITMAP_LBA + b, g_bm_buf);
        if (r) return r;
        for (bit = 0; bit < FS_BITS_PER_SECTOR; bit++) {
            if (g_bm_buf[bit / 8u] & (1u << (bit % 8u))) continue;
            idx = b * FS_BITS_PER_SECTOR + bit;
            lba = FS_START_LBA + idx;
            if (idx < FS_REGION_SECTORS && lba >= FS_DATA_LBA && lba != FS_RESERVED_LBA)
                free_secs++;
        }
    }
    for (s = 0; s < FS_INODE_SECTORS; s++) {
        r = sec_read(FS_INODE_LBA + s, g_ino_tbl);
        if (r) return r;
        for (i = 0; i < FS_INODES_PER_SECTOR; i++)
            if (g_ino_tbl[i].type == FS_TYPE_NONE) free_inodes++;
    }

    st->total_sectors = FS_DISK_SECTORS;
    st->free_sectors  = free_secs;
    st->inode_total   = FS_INODE_COUNT;
    st->inode_free    = free_inodes;
    st->mount_count   = g_sb.mount_count;
    st->io_reads      = g_io_reads;
    st->io_writes     = g_io_writes;
    return FS_OK;
}

const char *filesystem_strerror(int err)
{
    switch (err) {
    case FS_OK:               return "ok";
    case FS_ERR_IO:           return "storage I/O error (adapter not connected?)";
    case FS_ERR_NOT_MOUNTED:  return "filesystem not mounted";
    case FS_ERR_NOT_FORMATTED:return "disk not formatted (and not safe to auto-format)";
    case FS_ERR_CORRUPT:      return "filesystem corrupt / check failed";
    case FS_ERR_NOENT:        return "no such file or directory";
    case FS_ERR_EXIST:        return "already exists";
    case FS_ERR_NOTDIR:       return "not a directory";
    case FS_ERR_ISDIR:        return "is a directory";
    case FS_ERR_NOTEMPTY:     return "directory not empty";
    case FS_ERR_NAME:         return "bad or too-long name";
    case FS_ERR_FULL:         return "disk full";
    case FS_ERR_NOINODE:      return "out of inodes";
    case FS_ERR_TOOBIG:       return "file too big";
    case FS_ERR_INVAL:        return "invalid argument";
    case FS_ERR_RESERVED:     return "reserved sector blocked";
    case FS_ERR_RANGE:        return "sector out of range";
    default:                  return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t count; uint32_t bad; } st_list_ctx_t;

static int st_list_cb(const filesystem_info_t *info, void *ctx)
{
    st_list_ctx_t *c = (st_list_ctx_t *)ctx;
    c->count++;
    if (!fs_streq(info->name, "hello.txt") && !fs_streq(info->name, "data.bin")) c->bad++;
    return 0;
}

static uint8_t st_pat(uint32_t i)
{
    return (uint8_t)((i * 7u + 3u) & 0xFFu);
}

#define ST_TRY(expr, msg) do { r = (expr); if (r) { \
        filesystem_log("FS SELFTEST FAIL: " msg); filesystem_log(filesystem_strerror(r)); return r; } } while (0)
#define ST_CHECK(cond, msg) do { if (!(cond)) { \
        filesystem_log("FS SELFTEST FAIL: " msg); return FS_ERR_CORRUPT; } } while (0)

int filesystem_selftest(void)
{
    static const char DIR_[]  = "/rfs_selftest";
    static const char F_TXT[] = "/rfs_selftest/hello.txt";
    static const char F_BIN[] = "/rfs_selftest/data.bin";
    static const char MSG[]   = "RockFS selftest OK";
    filesystem_stats_t st0, st1;
    filesystem_info_t inf;
    st_list_ctx_t lc;
    uint8_t chunk[100], back[100], big[32];
    uint32_t i, j, got, w0, r0;
    int r;

    filesystem_log("FS selftest: start");
    if (!g_mounted) { filesystem_log("FS SELFTEST FAIL: not mounted"); return FS_ERR_NOT_MOUNTED; }

    /* 1. prove we can talk to the storage layer: superblock must read back as RFS1 */
    r0 = g_io_reads;
    ST_TRY(sec_read(FS_START_LBA, g_data_buf), "raw superblock read through adapter");
    ST_CHECK(g_io_reads == r0 + 1, "adapter read counter did not move");
    ST_CHECK(g_data_buf[0] == 'R' && g_data_buf[1] == 'F' && g_data_buf[2] == 'S' && g_data_buf[3] == '1',
             "superblock magic mismatch");
    filesystem_log("FS selftest: storage link OK (superblock read back)");

    /* 2. reserved LBA 32767 must be marked used AND blocked */
    ST_TRY(sec_read(FS_BITMAP_LBA + (FS_RESERVED_LBA - FS_START_LBA) / FS_BITS_PER_SECTOR, g_bm_buf),
           "bitmap read");
    i = (FS_RESERVED_LBA - FS_START_LBA) % FS_BITS_PER_SECTOR;
    ST_CHECK(g_bm_buf[i / 8u] & (1u << (i % 8u)), "reserved LBA not marked used in bitmap");
    r0 = g_io_reads; w0 = g_io_writes;
    ST_CHECK(sec_write(FS_RESERVED_LBA, g_zero) == FS_ERR_RESERVED, "write to reserved LBA was not blocked");
    ST_CHECK(sec_read(FS_RESERVED_LBA, g_data_buf) == FS_ERR_RESERVED, "read of reserved LBA was not blocked");
    ST_CHECK(g_io_reads == r0 && g_io_writes == w0, "reserved LBA access reached the adapter");
    filesystem_log("FS selftest: reserved LBA 32767 protected");

    /* 3. clean up leftovers from a previous interrupted run, then warm up root dir */
    filesystem_delete(F_TXT);
    filesystem_delete(F_BIN);
    filesystem_rmdir(DIR_);
    ST_TRY(filesystem_mkdir(DIR_), "warm-up mkdir");
    ST_TRY(filesystem_rmdir(DIR_), "warm-up rmdir");
    ST_TRY(filesystem_get_stats(&st0), "stats");

    /* 4. directory + small file round trip */
    ST_TRY(filesystem_mkdir(DIR_), "mkdir");
    ST_TRY(filesystem_write(F_TXT, MSG, (uint32_t)sizeof(MSG) - 1u), "write hello.txt");
    fs_memset(big, 0, sizeof(big));
    ST_TRY(filesystem_read(F_TXT, big, sizeof(big), &got), "read hello.txt");
    ST_CHECK(got == sizeof(MSG) - 1u, "hello.txt wrong length");
    for (i = 0; i < got; i++) ST_CHECK(big[i] == (uint8_t)MSG[i], "hello.txt content mismatch");
    filesystem_log("FS selftest: small file write/read OK");

    /* 5. big file: 70 x 100-byte appends (7000 bytes) crosses sectors AND the indirect block */
    ST_TRY(filesystem_create(F_BIN), "create data.bin");
    for (i = 0; i < 70; i++) {
        for (j = 0; j < 100; j++) chunk[j] = st_pat(i * 100u + j);
        ST_TRY(filesystem_append(F_BIN, chunk, 100), "append data.bin");
    }
    ST_TRY(filesystem_stat(F_BIN, &inf), "stat data.bin");
    ST_CHECK(inf.size == 7000 && inf.type == FS_TYPE_FILE, "data.bin wrong size/type");
    for (i = 0; i < 70; i++) {
        ST_TRY(filesystem_read_at(F_BIN, i * 100u, back, 100, &got), "read_at data.bin");
        ST_CHECK(got == 100, "read_at short");
        for (j = 0; j < 100; j++) ST_CHECK(back[j] == st_pat(i * 100u + j), "data.bin content mismatch");
    }
    filesystem_log("FS selftest: multi-sector file OK");

    /* 6. listing */
    lc.count = 0; lc.bad = 0;
    ST_TRY(filesystem_list(DIR_, st_list_cb, &lc), "list");
    ST_CHECK(lc.count == 2 && lc.bad == 0, "directory listing wrong");
    filesystem_log("FS selftest: directory listing OK");

    /* 7. delete everything and make sure no sectors/inodes leaked */
    ST_TRY(filesystem_delete(F_TXT), "delete hello.txt");
    ST_TRY(filesystem_delete(F_BIN), "delete data.bin");
    ST_TRY(filesystem_rmdir(DIR_), "rmdir");
    ST_CHECK(!filesystem_exists(F_TXT) && !filesystem_exists(DIR_), "test files still exist");
    ST_TRY(filesystem_get_stats(&st1), "stats");
    ST_CHECK(st1.free_sectors == st0.free_sectors, "leaked data sectors");
    ST_CHECK(st1.inode_free == st0.inode_free, "leaked inodes");
    filesystem_log("FS selftest: delete + no-leak check OK");

    filesystem_log("FS selftest: PASS");
    return FS_OK;
}
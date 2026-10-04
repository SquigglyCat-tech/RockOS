/*
 * filesystem.h - RockFS ("RFS1"), the first filesystem layer for ROK / RockOS
 *
 * Simple, lightweight, no malloc/free, 512-byte sectors only.
 * Sits on top of an existing block/sector storage layer through the
 * "ROK STORAGE ADAPTER" section at the top of filesystem.c.
 *
 * NOT thread-safe: call from one task only (or wrap calls in your own lock).
 */
#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */
#define FS_SECTOR_SIZE      512u
#define FS_DISK_SECTORS     32768u   /* 16 MiB / 512 -> LBA 0..32767            */
#define FS_RESERVED_LBA     32767u   /* storage self-test sector: NEVER touched */
#define FS_START_LBA        0u       /* first sector the filesystem may use     */

/*
 * 0 = if the disk is not blank and not RockFS, refuse to format (safe).
 * 1 = format anything unrecognized (DESTRUCTIVE - only for one-off use).
 */
#define FS_AUTO_FORMAT_UNRECOGNIZED 0

#define FS_NAME_MAX         27       /* max chars in one file/dir name          */
#define FS_PATH_MAX         128      /* max path length getcwd can return       */

/* Node types */
#define FS_TYPE_NONE        0
#define FS_TYPE_FILE        1
#define FS_TYPE_DIR         2

/* ------------------------------------------------------------------ */
/* Return codes (0 = success, negative = error)                        */
/* ------------------------------------------------------------------ */
#define FS_OK                0
#define FS_ERR_IO           (-1)   /* storage adapter failed / not connected */
#define FS_ERR_NOT_MOUNTED  (-2)
#define FS_ERR_NOT_FORMATTED (-3)  /* disk unrecognized and not safe to format */
#define FS_ERR_CORRUPT      (-4)
#define FS_ERR_NOENT        (-5)   /* no such file or directory              */
#define FS_ERR_EXIST        (-6)   /* already exists                         */
#define FS_ERR_NOTDIR       (-7)
#define FS_ERR_ISDIR        (-8)
#define FS_ERR_NOTEMPTY     (-9)
#define FS_ERR_NAME         (-10)  /* bad or too-long name                   */
#define FS_ERR_FULL         (-11)  /* out of data sectors                    */
#define FS_ERR_NOINODE      (-12)  /* out of inodes                          */
#define FS_ERR_TOOBIG       (-13)  /* file/dir would exceed max size         */
#define FS_ERR_INVAL        (-14)
#define FS_ERR_RESERVED     (-15)  /* attempt to touch the reserved LBA      */
#define FS_ERR_RANGE        (-16)  /* LBA outside filesystem region          */

/* ------------------------------------------------------------------ */
/* Public types                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char     name[FS_NAME_MAX + 1];
    uint8_t  type;      /* FS_TYPE_FILE or FS_TYPE_DIR */
    uint16_t inode;
    uint32_t size;      /* bytes (for a dir: bytes of directory slots) */
    uint32_t mtime;     /* whatever filesystem_get_time() returned     */
} filesystem_info_t;

typedef struct {
    uint32_t total_sectors;
    uint32_t free_sectors;    /* free data sectors                 */
    uint32_t inode_total;
    uint32_t inode_free;
    uint32_t mount_count;     /* increments every successful mount */
    uint32_t io_reads;        /* sectors read via the adapter      */
    uint32_t io_writes;       /* sectors written via the adapter   */
} filesystem_stats_t;

/*
 * Directory listing callback. Return 0 to continue, non-zero to stop.
 * Do NOT create/write/delete inside the callback (reading is fine).
 */
typedef int (*filesystem_list_cb)(const filesystem_info_t *info, void *ctx);

/* ------------------------------------------------------------------ */
/* Mount / format                                                      */
/* ------------------------------------------------------------------ */
int  filesystem_init(void);        /* mount; formats ONLY a blank disk        */
int  filesystem_format(void);      /* DESTROYS the filesystem, then mounts    */
int  filesystem_is_mounted(void);

/* ------------------------------------------------------------------ */
/* Files  (paths: "/a/b.txt" absolute, or relative to the current dir)  */
/* ------------------------------------------------------------------ */
int  filesystem_create(const char *path);                                  /* new empty file, EXIST if present */
int  filesystem_write(const char *path, const void *data, uint32_t len);   /* create-or-replace whole file     */
int  filesystem_append(const char *path, const void *data, uint32_t len);  /* create-if-missing, add to end    */
int  filesystem_read(const char *path, void *buf, uint32_t max_len, uint32_t *bytes_read);
int  filesystem_read_at(const char *path, uint32_t offset, void *buf, uint32_t len, uint32_t *bytes_read);
int  filesystem_delete(const char *path);                                  /* files only */

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */
int  filesystem_mkdir(const char *path);
int  filesystem_rmdir(const char *path);                                   /* empty dirs only */
int  filesystem_list(const char *path, filesystem_list_cb cb, void *ctx);
int  filesystem_chdir(const char *path);
int  filesystem_getcwd(char *buf, uint32_t size);

/* ------------------------------------------------------------------ */
/* Info                                                                */
/* ------------------------------------------------------------------ */
int  filesystem_stat(const char *path, filesystem_info_t *info);
int  filesystem_exists(const char *path);          /* 1 = yes, 0 = no */
int  filesystem_get_stats(filesystem_stats_t *st);
const char *filesystem_strerror(int err);

/* ------------------------------------------------------------------ */
/* Self-test: returns 0 on PASS, negative FS_ERR_* on FAIL.             */
/* Only touches /rfs_selftest/ and cleans up after itself.              */
/* ------------------------------------------------------------------ */
int  filesystem_selftest(void);

#endif /* FILESYSTEM_H */
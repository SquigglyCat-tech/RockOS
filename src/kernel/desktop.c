/*
 * RockOS V.2 desktop
 *
 * Wallpaper, desktop icons, taskbar, start menu and a small window manager
 * (drag, focus, minimize, close, z-order) with four built-in kernel-side apps:
 *
 *   File Manager  - RockFS browser (own path, never touches the shell's cwd)
 *   Terminal      - mini command line on the RockFS API
 *   System Info   - display / memory / RockFS / uptime
 *   About         - RockOS V.2
 *
 * Rendering: drawn into the display layer's shadow buffer only when something
 * changed (event, or once per second for the clock). display_present() is
 * called by the kernel main loop.
 *
 * Input: ALL hit-testing goes through hit_test(). Hover highlighting and
 * clicks use the same function, so what you see is what you click.
 *
 * NOTE: no memcpy/memset/struct-copy tricks on purpose; the kernel is built
 * with -ffreestanding -fno-builtin and has no libc.
 */

#include "desktop.h"

#include "display.h"
#include "filesystem.h"
#include "pmm.h"
#include "shell.h"
#include "timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ====================================================================== */
/* ======================== DESKTOP ADAPTER ============================= */
/* ====================================================================== */

/* PIT frequency passed to timer_init() in kernel.c (timer_init(100)). */
#ifndef DESKTOP_TIMER_HZ
#define DESKTOP_TIMER_HZ 100U
#endif

/* 1 = draw the RockOS logo as a watermark in the middle of the desktop. */
#ifndef DESKTOP_SHOW_LOGO
#define DESKTOP_SHOW_LOGO 1
#endif

#define SC_ESC       0x01
#define SC_ENTER     0x1C
#define SC_Q         0x10
#define SC_D         0x20
#define SC_F4        0x3E
#define SC_UP        0x48
#define SC_DOWN      0x50

/* Called when the desktop closes: repaint the shell screen. */
static void adapter_on_exit(void)
{
    shell_redraw();
}

/* ====================================================================== */
/* ============================ Constants =============================== */
/* ====================================================================== */

#define TASKBAR_H       40
#define START_BTN_W     48
#define TASK_BTN_W      150
#define TASK_BTN_H      32
#define TITLEBAR_H      30
#define CAPTION_BTN_W   30
#define MENU_W          240
#define MENU_ITEM_H     36
#define MENU_ITEMS      5
#define MENU_PAD        4
#define ICON_TILE_W     80
#define ICON_TILE_H     84
#define ICON_COUNT      4
#define DBLCLICK_TICKS  ((DESKTOP_TIMER_HZ * 4U) / 10U)

#define FM_MAX          64
#define FM_ROW_H        24
#define FM_ROWS         11
#define FM_LIST_TOP     (TITLEBAR_H + 38)

#define TERM_COLS       60
#define TERM_HIST       40
#define TERM_ROWS       13
#define TERM_ROW_H      22
#define TERM_CMD_MAX    56

/* Monochrome RockOS palette (0xRRGGBB). */
#define C_WALLPAPER     0x181818
#define C_TASKBAR       0x2B2B2B
#define C_TASKBAR_LINE  0x4A4A4A
#define C_BTN_HOVER     0x3C3C3C
#define C_BTN_ACTIVE    0x4A4A4A
#define C_WIN_BODY      0x222222
#define C_WIN_BORDER_A  0x707070
#define C_WIN_BORDER_I  0x404040
#define C_TITLE_ACTIVE  0x4A4A4A
#define C_TITLE_IDLE    0x2E2E2E
#define C_CAPTION_HOVER 0x5E5E5E
#define C_CLOSE_HOVER   0xA33A3A
#define C_MENU_BG       0x2B2B2B
#define C_MENU_LINE     0x4A4A4A
#define C_TOOLBAR       0x2A2A2A
#define C_ROW_HOVER     0x2C2C2C
#define C_TERM_BG       0x141414

enum { W_FILES = 0, W_TERM, W_SYSINFO, W_ABOUT, W_TYPES };

enum {
    R_NONE = 0,
    R_ICON,
    R_START,
    R_TASKBTN,
    R_MENUITEM,
    R_WTITLE,
    R_WCLOSE,
    R_WMIN,
    R_WBODY,
    R_FM_UP,
    R_FM_ROW
};

typedef struct {
    uint8_t kind;
    int8_t idx;
    int8_t sub;
} region_t;

typedef struct {
    bool open;
    bool minimized;
    int32_t x, y;
} dwin_t;

typedef struct {
    char name[FS_NAME_MAX + 1];
    uint8_t type;
    uint32_t size;
} fm_entry_t;

static const char* const win_title[W_TYPES] = {
    "File Manager", "Terminal", "System Info", "About RockOS"
};
static const int32_t win_w[W_TYPES] = { 560, 600, 440, 520 };
static const int32_t win_h[W_TYPES] = { 380, 380, 260, 260 };
static const uint8_t icon_win[ICON_COUNT] = {
    W_FILES, W_TERM, W_SYSINFO, W_ABOUT
};
static const char* const menu_label[MENU_ITEMS] = {
    "File Manager", "Terminal", "System Info", "About RockOS",
    "Exit to shell"
};

/* ====================================================================== */
/* =============================== State ================================ */
/* ====================================================================== */

static bool active;
static bool dirty;
static int32_t sw, sh, tb_y;

static dwin_t wins[W_TYPES];
static uint8_t zorder[W_TYPES];   /* bottom -> top */
static int zcount;
static int focused = -1;

static bool menu_open;
static int sel_icon = -1;
static uint64_t last_click_tick;
static int last_click_icon = -1;

static int32_t mx, my;
static bool prev_left;
static region_t hover;
static int drag_win = -1;
static int32_t drag_dx, drag_dy;

static uint64_t last_sec = UINT64_MAX;

/* File Manager */
static char fm_path[FS_PATH_MAX];
static fm_entry_t fm_ents[FM_MAX];
static int fm_count;
static int fm_sel;
static int fm_scroll;
static int fm_err;
static int fm_last_click_entry = -1;
static uint64_t fm_last_click_tick;

/* Terminal */
static char term_lines[TERM_HIST][TERM_COLS + 1];
static int term_count;
static char term_cwd[FS_PATH_MAX];
static char term_cmd[TERM_CMD_MAX + 1];
static int term_len;

/* ====================================================================== */
/* ============================== Helpers =============================== */
/* ====================================================================== */

static size_t slen(const char* s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static bool streq(const char* a, const char* b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static void copy_str(char* d, const char* s, size_t cap)
{
    size_t n = 0;
    while (s[n] && n + 1 < cap) { d[n] = s[n]; n++; }
    d[n] = '\0';
}

static char* put_str(char* d, const char* s)
{
    while (*s) *d++ = *s++;
    *d = 0;
    return d;
}

static char* put_u(char* d, uint64_t v)
{
    char t[21];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *d++ = t[--n];
    *d = 0;
    return d;
}

static char* put_2(char* d, uint64_t v)
{
    *d++ = (char)('0' + (v / 10) % 10);
    *d++ = (char)('0' + v % 10);
    *d = 0;
    return d;
}

static char* put_uptime(char* d, uint64_t ticks)
{
    uint64_t total = ticks / DESKTOP_TIMER_HZ;
    d = put_u(d, total / 3600);
    *d++ = ':';
    d = put_2(d, (total / 60) % 60);
    *d++ = ':';
    d = put_2(d, total % 60);
    *d = 0;
    return d;
}

static char* put_size(char* d, uint32_t bytes)
{
    if (bytes < 1024) {
        d = put_u(d, bytes);
        return put_str(d, " B");
    }
    d = put_u(d, bytes / 1024);
    return put_str(d, " KiB");
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    if (hi < lo) hi = lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Clipped RGB rect: never hand the display layer anything off-screen. */
static void rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb)
{
    int32_t x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x1 <= x || y1 <= y) return;
    display_fill_rect_rgb((uint32_t)x, (uint32_t)y, (uint32_t)(x1 - x),
        (uint32_t)(y1 - y), (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8),
        (uint8_t)rgb);
}

static void text(int32_t x, int32_t y, const char* s, uint8_t color)
{
    if (x < 0 || y < 0 || x >= sw || y >= sh) return;
    display_draw_text_ui((uint32_t)x, (uint32_t)y, s, color);
}

static bool inside(int32_t px, int32_t py, int32_t x, int32_t y,
    int32_t w, int32_t h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

/*
 * Resolve `in` (absolute or relative to `cwd`) into a normalized absolute
 * path in `out`. Handles "." and "..". `cap` must be >= 2.
 */
static void path_resolve(const char* cwd, const char* in, char* out,
    size_t cap)
{
    char comb[2 * FS_PATH_MAX];
    size_t starts[32];
    size_t n = 0, len = 0, i = 0, depth = 0;
    const char* s;

    if (in[0] != '/') {
        s = cwd;
        while (*s && n < sizeof(comb) - 2) comb[n++] = *s++;
        comb[n++] = '/';
    }
    s = in;
    while (*s && n < sizeof(comb) - 1) comb[n++] = *s++;
    comb[n] = '\0';

    out[0] = '\0';
    while (comb[i]) {
        size_t st, cl, k;
        while (comb[i] == '/') i++;
        if (!comb[i]) break;
        st = i;
        while (comb[i] && comb[i] != '/') i++;
        cl = i - st;
        if (cl == 1 && comb[st] == '.') continue;
        if (cl == 2 && comb[st] == '.' && comb[st + 1] == '.') {
            if (depth > 0) {
                depth--;
                len = starts[depth];
                out[len] = '\0';
            }
            continue;
        }
        if (depth >= 32 || len + 1 + cl + 1 > cap) break;
        starts[depth++] = len;
        out[len++] = '/';
        for (k = 0; k < cl; k++) out[len++] = comb[st + k];
        out[len] = '\0';
    }
    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
}

/* ====================================================================== */
/* ============================ File Manager ============================ */
/* ====================================================================== */

static int fm_cb(const filesystem_info_t* info, void* ctx)
{
    fm_entry_t* e;
    (void)ctx;
    if (info->name[0] == '.' && (info->name[1] == '\0' ||
        (info->name[1] == '.' && info->name[2] == '\0'))) {
        return 0;
    }
    if (fm_count >= FM_MAX) return 1;
    e = &fm_ents[fm_count++];
    copy_str(e->name, info->name, sizeof(e->name));
    e->type = info->type;
    e->size = info->size;
    return 0;
}

static void fm_copy_entry(fm_entry_t* d, const fm_entry_t* s)
{
    copy_str(d->name, s->name, sizeof(d->name));
    d->type = s->type;
    d->size = s->size;
}

static void fm_refresh(void)
{
    int rc, i, j;
    fm_count = 0;
    fm_sel = 0;
    fm_scroll = 0;
    fm_last_click_entry = -1;

    if (!filesystem_is_mounted()) {
        fm_err = FS_ERR_NOT_MOUNTED;
        return;
    }
    rc = filesystem_list(fm_path, fm_cb, NULL);
    fm_err = (rc == FS_OK || fm_count >= FM_MAX) ? FS_OK : rc;

    /* Stable insertion sort: directories first. */
    for (i = 1; i < fm_count; i++) {
        fm_entry_t key;
        fm_copy_entry(&key, &fm_ents[i]);
        j = i - 1;
        while (j >= 0 && key.type == FS_TYPE_DIR &&
            fm_ents[j].type != FS_TYPE_DIR) {
            fm_copy_entry(&fm_ents[j + 1], &fm_ents[j]);
            j--;
        }
        fm_copy_entry(&fm_ents[j + 1], &key);
    }
}

static void fm_ensure_visible(void)
{
    if (fm_sel < fm_scroll) fm_scroll = fm_sel;
    else if (fm_sel >= fm_scroll + FM_ROWS) fm_scroll = fm_sel - FM_ROWS + 1;
}

static void fm_up(void)
{
    char tmp[FS_PATH_MAX];
    path_resolve(fm_path, "..", tmp, sizeof(tmp));
    copy_str(fm_path, tmp, sizeof(fm_path));
    fm_refresh();
}

static void fm_open_selected(void)
{
    char tmp[FS_PATH_MAX];
    if (fm_sel < 0 || fm_sel >= fm_count) return;
    if (fm_ents[fm_sel].type != FS_TYPE_DIR) return;
    path_resolve(fm_path, fm_ents[fm_sel].name, tmp, sizeof(tmp));
    copy_str(fm_path, tmp, sizeof(fm_path));
    fm_refresh();
}

static void fm_key(const input_key_event_t* e)
{
    if (e->is_extended && e->scancode == SC_UP) {
        if (fm_sel > 0) fm_sel--;
        fm_ensure_visible();
        dirty = true;
    } else if (e->is_extended && e->scancode == SC_DOWN) {
        if (fm_sel + 1 < fm_count) fm_sel++;
        fm_ensure_visible();
        dirty = true;
    } else if (e->ascii == '\n' || e->ascii == '\r' ||
        (!e->is_extended && e->scancode == SC_ENTER)) {
        fm_open_selected();
        dirty = true;
    } else if (e->ascii == '\b') {
        fm_up();
        dirty = true;
    }
}

/* ====================================================================== */
/* ============================== Terminal ============================== */
/* ====================================================================== */

static void term_add_line(const char* s)
{
    int i;
    if (term_count == TERM_HIST) {
        for (i = 1; i < TERM_HIST; i++) {
            copy_str(term_lines[i - 1], term_lines[i], TERM_COLS + 1);
        }
        term_count--;
    }
    copy_str(term_lines[term_count++], s, TERM_COLS + 1);
}

/* Adds text, wrapping at TERM_COLS and splitting on '\n'. */
static void term_push(const char* s)
{
    for (;;) {
        char line[TERM_COLS + 1];
        size_t n = 0;
        while (*s && *s != '\n' && n < TERM_COLS) line[n++] = *s++;
        line[n] = '\0';
        term_add_line(line);
        if (*s == '\n') s++;
        else if (*s == '\0') break;
    }
}

static void term_error(int rc)
{
    char line[TERM_COLS + 1];
    char* p = put_str(line, "error: ");
    copy_str(p, filesystem_strerror(rc), (size_t)(TERM_COLS + 1) -
        (size_t)(p - line));
    term_add_line(line);
}

static int term_dir_cb(const filesystem_info_t* info, void* ctx)
{
    char line[TERM_COLS + 1];
    char* p;
    (void)ctx;
    if (info->name[0] == '.' && (info->name[1] == '\0' ||
        (info->name[1] == '.' && info->name[2] == '\0'))) {
        return 0;
    }
    p = put_str(line, info->type == FS_TYPE_DIR ? "[DIR]  " : "[FILE] ");
    p = put_str(p, info->name);
    if (info->type != FS_TYPE_DIR) {
        p = put_str(p, "  ");
        put_size(p, info->size);
    }
    term_add_line(line);
    return 0;
}

/* Splits on spaces; the last (max-th) token keeps the rest of the line. */
static int split_args(char* s, char** argv, int max)
{
    int n = 0;
    while (*s == ' ') s++;
    while (*s && n < max) {
        argv[n++] = s;
        if (n == max) break;
        while (*s && *s != ' ') s++;
        if (*s) {
            *s++ = '\0';
            while (*s == ' ') s++;
        }
    }
    return n;
}

static void win_close(int w);

static void term_exec(char* cmd)
{
    char* argv[3];
    char path[FS_PATH_MAX];
    char line[TERM_COLS + 1];
    char* p;
    int argc = split_args(cmd, argv, 3);
    int rc;

    if (argc == 0) return;

    if (streq(argv[0], "help")) {
        term_push("help clear pwd cd dir mkdir create read write");
        term_push("delete mem uptime version exit shell");
        term_push("write <path> <text>   read <path>   cd <path>");
    } else if (streq(argv[0], "clear")) {
        term_count = 0;
    } else if (streq(argv[0], "pwd")) {
        term_push(term_cwd);
    } else if (streq(argv[0], "cd")) {
        filesystem_info_t info;
        if (argc < 2) { term_push("usage: cd <path>"); return; }
        path_resolve(term_cwd, argv[1], path, sizeof(path));
        rc = filesystem_stat(path, &info);
        if (rc != FS_OK) { term_error(rc); return; }
        if (info.type != FS_TYPE_DIR) { term_push("error: not a directory"); return; }
        copy_str(term_cwd, path, sizeof(term_cwd));
    } else if (streq(argv[0], "dir")) {
        path_resolve(term_cwd, argc > 1 ? argv[1] : ".", path, sizeof(path));
        rc = filesystem_list(path, term_dir_cb, NULL);
        if (rc != FS_OK) term_error(rc);
    } else if (streq(argv[0], "mkdir")) {
        if (argc < 2) { term_push("usage: mkdir <path>"); return; }
        path_resolve(term_cwd, argv[1], path, sizeof(path));
        rc = filesystem_mkdir(path);
        if (rc != FS_OK) term_error(rc);
    } else if (streq(argv[0], "create")) {
        if (argc < 2) { term_push("usage: create <path>"); return; }
        path_resolve(term_cwd, argv[1], path, sizeof(path));
        rc = filesystem_create(path);
        if (rc != FS_OK) term_error(rc);
    } else if (streq(argv[0], "delete")) {
        if (argc < 2) { term_push("usage: delete <path>"); return; }
        path_resolve(term_cwd, argv[1], path, sizeof(path));
        rc = filesystem_delete(path);
        if (rc != FS_OK) term_error(rc);
    } else if (streq(argv[0], "write")) {
        uint32_t n = 0;
        if (argc < 3) { term_push("usage: write <path> <text>"); return; }
        path_resolve(term_cwd, argv[1], path, sizeof(path));
        while (argv[2][n]) n++;
        rc = filesystem_write(path, argv[2], n);
        if (rc != FS_OK) term_error(rc);
    } else if (streq(argv[0], "read")) {
        char buf[513];
        uint32_t got = 0, i;
        if (argc < 2) { term_push("usage: read <path>"); return; }
        path_resolve(term_cwd, argv[1], path, sizeof(path));
        rc = filesystem_read(path, buf, sizeof(buf) - 1, &got);
        if (rc != FS_OK) { term_error(rc); return; }
        for (i = 0; i < got; i++) {
            if (buf[i] != '\n' && (buf[i] < 0x20 || buf[i] > 0x7E)) buf[i] = '.';
        }
        buf[got] = '\0';
        term_push(buf);
    } else if (streq(argv[0], "mem")) {
        pmm_stats_t ps;
        pmm_get_stats(&ps);
        p = put_str(line, "total: ");
        p = put_u(p, ps.total_bytes >> 20);
        put_str(p, " MiB");
        term_add_line(line);
        p = put_str(line, "free:  ");
        p = put_u(p, ps.free_bytes >> 20);
        put_str(p, " MiB");
        term_add_line(line);
    } else if (streq(argv[0], "uptime")) {
        p = put_str(line, "up ");
        put_uptime(p, timer_get_ticks());
        term_add_line(line);
    } else if (streq(argv[0], "version")) {
        term_push("RockOS V.2 | ROK freestanding x86_64 kernel");
    } else if (streq(argv[0], "exit")) {
        win_close(W_TERM);
    } else if (streq(argv[0], "shell")) {
        desktop_stop();
    } else {
        p = put_str(line, "unknown command: ");
        copy_str(p, argv[0], (size_t)(TERM_COLS + 1) - (size_t)(p - line));
        term_add_line(line);
    }
}

static void term_key(const input_key_event_t* e)
{
    if (e->ascii == '\n' || e->ascii == '\r') {
        char echo[TERM_COLS + 1];
        char work[TERM_CMD_MAX + 1];
        char* p = put_str(echo, "> ");
        copy_str(p, term_cmd, (size_t)(TERM_COLS + 1) - (size_t)(p - echo));
        term_add_line(echo);
        copy_str(work, term_cmd, sizeof(work));
        term_len = 0;
        term_cmd[0] = '\0';
        term_exec(work);
        dirty = true;
    } else if (e->ascii == '\b') {
        if (term_len > 0) term_cmd[--term_len] = '\0';
        dirty = true;
    } else if (e->ascii >= 0x20 && e->ascii <= 0x7E && term_len < TERM_CMD_MAX) {
        term_cmd[term_len++] = e->ascii;
        term_cmd[term_len] = '\0';
        dirty = true;
    }
}

/* ====================================================================== */
/* ========================== Window management ========================= */
/* ====================================================================== */

static void z_remove(int w)
{
    int i;
    for (i = 0; i < zcount && zorder[i] != w; i++) { }
    if (i == zcount) return;
    for (; i < zcount - 1; i++) zorder[i] = zorder[i + 1];
    zcount--;
}

static void z_raise(int w)
{
    z_remove(w);
    zorder[zcount++] = (uint8_t)w;
}

static void focus_top(void)
{
    int i;
    focused = -1;
    for (i = zcount - 1; i >= 0; i--) {
        if (!wins[zorder[i]].minimized) { focused = zorder[i]; return; }
    }
}

static void win_focus(int w)
{
    wins[w].minimized = false;
    z_raise(w);
    focused = w;
}

static void win_place(int w)
{
    int32_t x = 90 + w * 36, y = 40 + w * 36;
    wins[w].x = clampi(x, 0, sw - win_w[w]);
    wins[w].y = clampi(y, 0, tb_y - win_h[w]);
}

static void win_open(int w)
{
    if (!wins[w].open) {
        wins[w].open = true;
        win_place(w);
        if (w == W_FILES) fm_refresh();
        if (w == W_TERM && term_count == 0) {
            term_push("RockOS V.2 terminal. Type help for commands.");
        }
    }
    win_focus(w);
}

static void win_close(int w)
{
    wins[w].open = false;
    wins[w].minimized = false;
    z_remove(w);
    if (focused == w) focus_top();
}

static void win_minimize(int w)
{
    wins[w].minimized = true;
    if (focused == w) focus_top();
}

/* ====================================================================== */
/* ============================== Hit test ============================== */
/* ====================================================================== */

static int32_t menu_h(void) { return MENU_ITEMS * MENU_ITEM_H + 2 * MENU_PAD; }
static int32_t menu_y(void) { return tb_y - menu_h(); }

static int32_t icon_x(void) { return 24; }
static int32_t icon_y(int i) { return 24 + i * 96; }

static int32_t taskbtn_x(int k) { return START_BTN_W + 8 + k * (TASK_BTN_W + 4); }

static region_t hit_test(int32_t x, int32_t y)
{
    region_t r = { R_NONE, 0, 0 };
    int i, k;

    if (menu_open) {
        int32_t my0 = menu_y();
        if (inside(x, y, 0, my0, MENU_W, menu_h())) {
            int32_t item = (y - my0 - MENU_PAD) / MENU_ITEM_H;
            r.kind = R_MENUITEM;
            r.idx = (y - my0 - MENU_PAD >= 0 && item < MENU_ITEMS)
                ? (int8_t)item : (int8_t)-1;
            return r;
        }
    }

    if (y >= tb_y) {
        if (x < START_BTN_W) { r.kind = R_START; return r; }
        k = 0;
        for (i = 0; i < W_TYPES; i++) {
            if (!wins[i].open) continue;
            if (inside(x, y, taskbtn_x(k), tb_y + 4, TASK_BTN_W, TASK_BTN_H)) {
                r.kind = R_TASKBTN;
                r.idx = (int8_t)i;
                return r;
            }
            k++;
        }
        return r;
    }

    for (i = zcount - 1; i >= 0; i--) {
        int w = zorder[i];
        int32_t wx = wins[w].x, wy = wins[w].y;
        if (wins[w].minimized) continue;
        if (!inside(x, y, wx, wy, win_w[w], win_h[w])) continue;
        r.idx = (int8_t)w;
        if (y < wy + TITLEBAR_H) {
            if (x >= wx + win_w[w] - CAPTION_BTN_W) r.kind = R_WCLOSE;
            else if (x >= wx + win_w[w] - 2 * CAPTION_BTN_W) r.kind = R_WMIN;
            else r.kind = R_WTITLE;
        } else {
            int32_t lx = x - wx, ly = y - wy;
            r.kind = R_WBODY;
            if (w == W_FILES) {
                if (inside(lx, ly, 8, TITLEBAR_H + 4, 56, 26)) {
                    r.kind = R_FM_UP;
                } else if (ly >= FM_LIST_TOP &&
                    ly < FM_LIST_TOP + FM_ROWS * FM_ROW_H) {
                    int row = (ly - FM_LIST_TOP) / FM_ROW_H;
                    if (fm_scroll + row < fm_count) {
                        r.kind = R_FM_ROW;
                        r.sub = (int8_t)row;
                    }
                }
            }
        }
        return r;
    }

    for (i = 0; i < ICON_COUNT; i++) {
        if (inside(x, y, icon_x(), icon_y(i), ICON_TILE_W, ICON_TILE_H)) {
            r.kind = R_ICON;
            r.idx = (int8_t)i;
            return r;
        }
    }
    return r;
}

static bool region_eq(region_t a, region_t b)
{
    return a.kind == b.kind && a.idx == b.idx && a.sub == b.sub;
}

/* ====================================================================== */
/* =============================== Drawing ============================== */
/* ====================================================================== */

static void draw_icon_art(int type, int32_t ix, int32_t iy)
{
    if (type == W_FILES) {
        rect(ix + 4, iy + 10, 20, 8, 0xA0A0A0);
        rect(ix + 4, iy + 16, 40, 28, 0xC8C8C8);
        rect(ix + 4, iy + 22, 40, 2, 0x9A9A9A);
    } else if (type == W_TERM) {
        rect(ix + 3, iy + 6, 42, 36, 0xC8C8C8);
        rect(ix + 6, iy + 14, 36, 25, 0x1A1A1A);
        rect(ix + 10, iy + 19, 8, 2, 0xE0E0E0);
        rect(ix + 14, iy + 21, 2, 2, 0xE0E0E0);
        rect(ix + 10, iy + 23, 8, 2, 0xE0E0E0);
        rect(ix + 22, iy + 28, 10, 2, 0xE0E0E0);
    } else if (type == W_SYSINFO) {
        rect(ix + 6, iy + 6, 36, 36, 0xC8C8C8);
        rect(ix + 22, iy + 13, 4, 4, 0x2A2A2A);
        rect(ix + 22, iy + 20, 4, 14, 0x2A2A2A);
    } else {
        display_fill_triangle((uint32_t)(ix + 24), (uint32_t)(iy + 4),
            (uint32_t)(ix + 6), (uint32_t)(iy + 22),
            (uint32_t)(ix + 42), (uint32_t)(iy + 22), COLOR_LIGHT_GRAY);
        display_fill_triangle((uint32_t)(ix + 6), (uint32_t)(iy + 22),
            (uint32_t)(ix + 42), (uint32_t)(iy + 22),
            (uint32_t)(ix + 34), (uint32_t)(iy + 44), COLOR_DARK_GRAY);
    }
}

static void draw_icons(void)
{
    int i;
    for (i = 0; i < ICON_COUNT; i++) {
        int32_t x = icon_x(), y = icon_y(i);
        bool hot = hover.kind == R_ICON && hover.idx == i;
        if (sel_icon == i) {
            display_blend_rect_rgb((uint32_t)x, (uint32_t)y, ICON_TILE_W,
                ICON_TILE_H, 255, 255, 255, 60);
        } else if (hot) {
            display_blend_rect_rgb((uint32_t)x, (uint32_t)y, ICON_TILE_W,
                ICON_TILE_H, 255, 255, 255, 24);
        }
        draw_icon_art(icon_win[i], x + 16, y + 6);
        text(x + 6, y + 58, win_title[icon_win[i]], COLOR_WHITE);
    }
}

static void draw_files(int32_t x, int32_t y)
{
    int32_t by = y + TITLEBAR_H;
    int32_t w = win_w[W_FILES];
    char buf[64];
    char* p;
    int i;
    bool up_hot = hover.kind == R_FM_UP;

    rect(x, by, w, 34, C_TOOLBAR);
    rect(x + 8, by + 4, 56, 26, up_hot ? 0x5A5A5A : 0x3A3A3A);
    text(x + 22, by + 7, "Up", COLOR_WHITE);
    text(x + 76, by + 7, fm_path, COLOR_LIGHT_GRAY);

    for (i = 0; i < FM_ROWS; i++) {
        int idx = fm_scroll + i;
        int32_t ry = y + FM_LIST_TOP + i * FM_ROW_H;
        bool sel, hot;
        if (idx >= fm_count) break;
        sel = (idx == fm_sel);
        hot = hover.kind == R_FM_ROW && hover.sub == i;
        if (sel) rect(x + 4, ry, w - 8, FM_ROW_H, C_BTN_ACTIVE);
        else if (hot) rect(x + 4, ry, w - 8, FM_ROW_H, C_ROW_HOVER);

        if (fm_ents[idx].type == FS_TYPE_DIR) {
            rect(x + 12, ry + 5, 8, 3, 0xA0A0A0);
            rect(x + 12, ry + 7, 16, 12, 0xC8C8C8);
        } else {
            rect(x + 15, ry + 4, 11, 16, 0xBDBDBD);
        }
        text(x + 40, ry + 2, fm_ents[idx].name, COLOR_WHITE);
        if (fm_ents[idx].type == FS_TYPE_DIR) {
            text(x + w - 150, ry + 2, "folder", COLOR_LIGHT_GRAY);
        } else {
            put_size(buf, fm_ents[idx].size);
            text(x + w - 150, ry + 2, buf, COLOR_LIGHT_GRAY);
        }
    }

    rect(x, y + win_h[W_FILES] - 28, w, 28, C_TOOLBAR);
    if (fm_err != FS_OK) {
        p = put_str(buf, "error: ");
        copy_str(p, filesystem_strerror(fm_err), sizeof(buf) - 7);
        text(x + 12, y + win_h[W_FILES] - 24, buf, COLOR_LIGHT_RED);
    } else {
        p = put_u(buf, (uint64_t)fm_count);
        put_str(p, fm_count == 1 ? " item" : " items");
        text(x + 12, y + win_h[W_FILES] - 24, buf, COLOR_LIGHT_GRAY);
        text(x + 140, y + win_h[W_FILES] - 24,
            "Up/Down: select   Enter: open   Backspace: up",
            COLOR_LIGHT_GRAY);
    }
}

static void draw_terminal(int32_t x, int32_t y)
{
    int32_t by = y + TITLEBAR_H;
    int32_t w = win_w[W_TERM];
    int first = term_count > TERM_ROWS ? term_count - TERM_ROWS : 0;
    int i;
    char prompt[FS_PATH_MAX + TERM_CMD_MAX + 8];
    char* p;

    rect(x, by, w, win_h[W_TERM] - TITLEBAR_H, C_TERM_BG);
    for (i = first; i < term_count; i++) {
        text(x + 12, by + 8 + (i - first) * TERM_ROW_H, term_lines[i],
            COLOR_LIGHT_GRAY);
    }
    rect(x, y + win_h[W_TERM] - 38, w, 1, C_TASKBAR_LINE);
    p = put_str(prompt, term_cwd);
    p = put_str(p, "> ");
    p = put_str(p, term_cmd);
    put_str(p, "_");
    text(x + 12, y + win_h[W_TERM] - 30, prompt, COLOR_WHITE);
}

static void draw_window_body(int w, int32_t x, int32_t y, uint64_t ticks)
{
    int32_t bx = x + 16, by = y + TITLEBAR_H + 14;
    char line[96];
    char* p;

    if (w == W_FILES) {
        draw_files(x, y);
    } else if (w == W_TERM) {
        draw_terminal(x, y);
    } else if (w == W_SYSINFO) {
        uint32_t fw = 0, fh = 0;
        pmm_stats_t ps;
        filesystem_stats_t fs;
        display_get_framebuffer_size(&fw, &fh);
        pmm_get_stats(&ps);

        text(bx, by, "RockOS V.2  (ROK kernel)", COLOR_WHITE);

        p = put_str(line, "Display: ");
        p = put_u(p, fw);
        p = put_str(p, " x ");
        put_u(p, fh);
        text(bx, by + 24, line, COLOR_LIGHT_GRAY);

        p = put_str(line, "Framebuffer: ");
        copy_str(p, display_framebuffer_status(), sizeof(line) - 13);
        text(bx, by + 48, line, COLOR_LIGHT_GRAY);

        p = put_str(line, "Memory: ");
        p = put_u(p, ps.total_bytes >> 20);
        p = put_str(p, " MiB total, ");
        p = put_u(p, ps.free_bytes >> 20);
        put_str(p, " MiB free");
        text(bx, by + 72, line, COLOR_LIGHT_GRAY);

        if (filesystem_is_mounted() && filesystem_get_stats(&fs) == FS_OK) {
            p = put_str(line, "RockFS: ");
            p = put_u(p, fs.free_sectors);
            p = put_str(p, " of ");
            p = put_u(p, fs.total_sectors);
            put_str(p, " sectors free");
        } else {
            put_str(line, "RockFS: not mounted");
        }
        text(bx, by + 96, line, COLOR_LIGHT_GRAY);

        p = put_str(line, "Uptime: ");
        put_uptime(p, ticks);
        text(bx, by + 120, line, COLOR_LIGHT_GRAY);

        p = put_str(line, "Timer ticks: ");
        put_u(p, ticks);
        text(bx, by + 144, line, COLOR_LIGHT_GRAY);
    } else {
        if (x + 20 >= 0 && y + TITLEBAR_H + 14 >= 0) {
            display_draw_rockos_logo((uint32_t)(x + 20),
                (uint32_t)(y + TITLEBAR_H + 14));
        }
        text(bx, by + 128, "RockOS V.2 desktop", COLOR_WHITE);
        text(bx, by + 152, "Ctrl+Alt+D: toggle desktop   Ctrl+Alt+Q: exit",
            COLOR_LIGHT_GRAY);
    }
}

static void draw_window(int w, uint64_t ticks)
{
    int32_t x = wins[w].x, y = wins[w].y;
    bool act = (focused == w);
    int32_t bx = x + win_w[w] - 2 * CAPTION_BTN_W;
    uint32_t border = act ? C_WIN_BORDER_A : C_WIN_BORDER_I;
    bool hmin = hover.kind == R_WMIN && hover.idx == w;
    bool hclose = hover.kind == R_WCLOSE && hover.idx == w;
    int i;

    rect(x - 1, y - 1, win_w[w] + 2, win_h[w] + 2, border);
    rect(x, y, win_w[w], win_h[w], C_WIN_BODY);
    rect(x, y, win_w[w], TITLEBAR_H, act ? C_TITLE_ACTIVE : C_TITLE_IDLE);

    text(x + 12, y + (TITLEBAR_H - (int32_t)DISPLAY_UI_TEXT_HEIGHT) / 2,
        win_title[w], act ? COLOR_WHITE : COLOR_LIGHT_GRAY);

    if (hmin) rect(bx, y, CAPTION_BTN_W, TITLEBAR_H, C_CAPTION_HOVER);
    rect(bx + 9, y + 19, 12, 2, 0xE0E0E0);

    if (hclose) {
        rect(bx + CAPTION_BTN_W, y, CAPTION_BTN_W, TITLEBAR_H, C_CLOSE_HOVER);
    }
    for (i = 0; i < 10; i++) {
        rect(bx + CAPTION_BTN_W + 10 + i, y + 10 + i, 2, 2, 0xE0E0E0);
        rect(bx + CAPTION_BTN_W + 19 - i, y + 10 + i, 2, 2, 0xE0E0E0);
    }

    draw_window_body(w, x, y, ticks);
}

static void draw_taskbar(uint64_t ticks)
{
    int i, k = 0;
    char buf[32];
    char* p;
    bool start_hot = (hover.kind == R_START) || menu_open;
    int32_t ox = 14, oy = tb_y + 10;

    rect(0, tb_y, sw, TASKBAR_H, C_TASKBAR);
    rect(0, tb_y, sw, 1, C_TASKBAR_LINE);

    if (start_hot) rect(0, tb_y + 1, START_BTN_W, TASKBAR_H - 1, C_BTN_HOVER);
    display_fill_triangle((uint32_t)(ox + 10), (uint32_t)oy,
        (uint32_t)ox, (uint32_t)(oy + 10),
        (uint32_t)(ox + 20), (uint32_t)(oy + 10), COLOR_LIGHT_GRAY);
    display_fill_triangle((uint32_t)ox, (uint32_t)(oy + 10),
        (uint32_t)(ox + 20), (uint32_t)(oy + 10),
        (uint32_t)(ox + 15), (uint32_t)(oy + 20), COLOR_DARK_GRAY);

    for (i = 0; i < W_TYPES; i++) {
        int32_t x;
        bool is_active, hot;
        if (!wins[i].open) continue;
        x = taskbtn_x(k++);
        is_active = (focused == i && !wins[i].minimized);
        hot = hover.kind == R_TASKBTN && hover.idx == i;
        rect(x, tb_y + 4, TASK_BTN_W, TASK_BTN_H,
            is_active ? C_BTN_ACTIVE : (hot ? C_BTN_HOVER : C_TASKBAR));
        if (is_active) rect(x, tb_y + 4 + TASK_BTN_H - 2, TASK_BTN_W, 2, 0xC8C8C8);
        text(x + 10, tb_y + 4 + (TASK_BTN_H - (int32_t)DISPLAY_UI_TEXT_HEIGHT) / 2,
            win_title[i], wins[i].minimized ? COLOR_LIGHT_GRAY : COLOR_WHITE);
    }

    p = put_str(buf, "up ");
    put_uptime(p, ticks);
    text(sw - 20 - (int32_t)(slen(buf) * 9), tb_y + 10, buf, COLOR_LIGHT_GRAY);
}

static void draw_menu(void)
{
    int i;
    int32_t y0 = menu_y();
    int32_t h = menu_h();

    rect(0, y0 - 1, MENU_W + 1, h + 1, C_MENU_LINE);
    rect(0, y0, MENU_W, h, C_MENU_BG);

    for (i = 0; i < MENU_ITEMS; i++) {
        int32_t iy = y0 + MENU_PAD + i * MENU_ITEM_H;
        bool hot = hover.kind == R_MENUITEM && hover.idx == i;
        if (hot) rect(MENU_PAD, iy, MENU_W - 2 * MENU_PAD, MENU_ITEM_H, C_BTN_ACTIVE);
        if (i == MENU_ITEMS - 1) rect(MENU_PAD, iy, MENU_W - 2 * MENU_PAD, 1, C_MENU_LINE);
        text(16, iy + (MENU_ITEM_H - (int32_t)DISPLAY_UI_TEXT_HEIGHT) / 2,
            menu_label[i], COLOR_WHITE);
    }
}

static void draw_all(uint64_t ticks)
{
    int i;

    rect(0, 0, sw, tb_y, C_WALLPAPER);

#if DESKTOP_SHOW_LOGO
    if (sw >= (int32_t)DISPLAY_ROCKOS_LOGO_WIDTH &&
        tb_y >= (int32_t)DISPLAY_ROCKOS_LOGO_HEIGHT) {
        display_draw_rockos_logo(
            (uint32_t)((sw - (int32_t)DISPLAY_ROCKOS_LOGO_WIDTH) / 2),
            (uint32_t)((tb_y - (int32_t)DISPLAY_ROCKOS_LOGO_HEIGHT) / 2));
    }
#endif

    draw_icons();
    for (i = 0; i < zcount; i++) {
        if (!wins[zorder[i]].minimized) draw_window(zorder[i], ticks);
    }
    draw_taskbar(ticks);
    if (menu_open) draw_menu();
}

/* ====================================================================== */
/* ============================ Input handling ========================== */
/* ====================================================================== */

static void on_press(int32_t x, int32_t y)
{
    region_t r = hit_test(x, y);
    bool was_menu = menu_open;
    uint64_t now = timer_get_ticks();

    if (r.kind != R_START && r.kind != R_MENUITEM) menu_open = false;

    switch (r.kind) {
    case R_START:
        menu_open = !was_menu;
        break;
    case R_MENUITEM:
        if (r.idx >= 0 && r.idx < W_TYPES) win_open(r.idx);
        if (r.idx == W_TYPES) { desktop_stop(); return; }
        if (r.idx >= 0) menu_open = false;
        break;
    case R_TASKBTN:
        if (wins[r.idx].minimized) win_focus(r.idx);
        else if (focused == r.idx) win_minimize(r.idx);
        else win_focus(r.idx);
        break;
    case R_ICON:
        if (last_click_icon == r.idx &&
            (now - last_click_tick) <= DBLCLICK_TICKS) {
            win_open(icon_win[r.idx]);
            last_click_icon = -1;
        } else {
            last_click_icon = r.idx;
            last_click_tick = now;
        }
        sel_icon = r.idx;
        break;
    case R_WTITLE:
        win_focus(r.idx);
        drag_win = r.idx;
        drag_dx = x - wins[r.idx].x;
        drag_dy = y - wins[r.idx].y;
        break;
    case R_WBODY:
        win_focus(r.idx);
        break;
    case R_FM_UP:
        win_focus(r.idx);
        fm_up();
        break;
    case R_FM_ROW: {
        int entry = fm_scroll + r.sub;
        win_focus(r.idx);
        if (fm_last_click_entry == entry &&
            (now - fm_last_click_tick) <= DBLCLICK_TICKS) {
            fm_sel = entry;
            fm_open_selected();
        } else {
            fm_sel = entry;
            fm_last_click_entry = entry;
            fm_last_click_tick = now;
        }
        break;
    }
    case R_WCLOSE:
        win_close(r.idx);
        break;
    case R_WMIN:
        win_minimize(r.idx);
        break;
    default:
        if (y < tb_y) sel_icon = -1;
        break;
    }

    hover = hit_test(x, y);
    dirty = true;
}

/* ====================================================================== */
/* =============================== Public API =========================== */
/* ====================================================================== */

bool desktop_is_active(void)
{
    return active;
}

void desktop_start(void)
{
    uint32_t w = 0, h = 0;
    int i;

    if (active) return;
    if (!display_get_framebuffer_size(&w, &h) || w < 640 || h < 400) return;

    sw = (int32_t)w;
    sh = (int32_t)h;
    tb_y = sh - TASKBAR_H;

    for (i = 0; i < W_TYPES; i++) {
        wins[i].open = false;
        wins[i].minimized = false;
    }
    zcount = 0;
    focused = -1;
    menu_open = false;
    sel_icon = -1;
    last_click_icon = -1;
    drag_win = -1;
    prev_left = false;
    mx = sw / 2;
    my = sh / 2;
    hover.kind = R_NONE;
    hover.idx = 0;
    hover.sub = 0;
    last_sec = UINT64_MAX;

    copy_str(fm_path, "/", sizeof(fm_path));
    fm_count = 0;
    fm_sel = 0;
    fm_scroll = 0;
    fm_err = FS_OK;
    term_count = 0;
    term_len = 0;
    term_cmd[0] = '\0';
    copy_str(term_cwd, "/", sizeof(term_cwd));

    win_open(W_ABOUT);

    active = true;
    dirty = true;
}

void desktop_stop(void)
{
    if (!active) return;
    active = false;
    adapter_on_exit();
}

void desktop_handle_mouse(int32_t x, int32_t y, bool left_down)
{
    if (!active) return;

    x = clampi(x, 0, sw - 1);
    y = clampi(y, 0, sh - 1);

    if (x != mx || y != my) {
        mx = x;
        my = y;
        if (drag_win >= 0 && left_down) {
            wins[drag_win].x = clampi(x - drag_dx, 0, sw - win_w[drag_win]);
            wins[drag_win].y = clampi(y - drag_dy, 0, tb_y - TITLEBAR_H);
            dirty = true;
        } else {
            region_t hv = hit_test(x, y);
            if (!region_eq(hv, hover)) {
                hover = hv;
                dirty = true;
            }
        }
    }

    if (left_down && !prev_left) {
        on_press(x, y);
        if (!active) { prev_left = left_down; return; }
    }
    if (!left_down && prev_left) drag_win = -1;
    prev_left = left_down;
}

void desktop_handle_key(const input_key_event_t* e)
{
    bool ctrl_alt;

    if (!active || !e || !e->is_pressed) return;

    ctrl_alt = (e->modifiers & (MOD_CTRL | MOD_ALT)) == (MOD_CTRL | MOD_ALT);

    /* Ctrl+Alt+D toggles; Ctrl+Alt+Q is the always-works safety exit. */
    if (desktop_is_launch_hotkey(e) ||
        (ctrl_alt && !e->is_extended &&
            (e->ascii == 'q' || e->ascii == 'Q' || e->scancode == SC_Q))) {
        desktop_stop();
        return;
    }

    if (e->scancode == SC_ESC && !e->is_extended && menu_open) {
        menu_open = false;
        dirty = true;
        return;
    }

    if ((e->modifiers & MOD_ALT) && e->scancode == SC_F4 && focused >= 0) {
        win_close(focused);
        dirty = true;
        return;
    }

    if (e->modifiers & (MOD_CTRL | MOD_ALT)) return;

    if (focused == W_TERM && !e->is_extended) term_key(e);
    else if (focused == W_FILES) fm_key(e);
}

void desktop_update(uint64_t ticks)
{
    uint64_t sec;

    if (!active) return;

    sec = ticks / DESKTOP_TIMER_HZ;
    if (sec != last_sec) {
        last_sec = sec;
        dirty = true;   /* taskbar clock + System Info refresh */
    }
    if (!dirty) return;
    dirty = false;
    draw_all(ticks);
}

bool desktop_is_launch_hotkey(const input_key_event_t* e)
{
    return e && e->is_pressed && !e->is_extended &&
        (e->modifiers & (MOD_CTRL | MOD_ALT)) == (MOD_CTRL | MOD_ALT) &&
        (e->ascii == 'd' || e->ascii == 'D' || e->scancode == SC_D);
}

/*
 * RockOS V.2 desktop  -  "Classic" look
 *
 * Matches the RockOS reference sheet: raised/sunken 3D bevels, gradient title
 * bars, a Start menu with a dark side banner and cascading sub-menus, a
 * recessed system tray with a real clock, and the RockOS logo as wallpaper.
 *
 * Built-in kernel-side apps:
 *   My Computer          drive list + RockFS folder browser (menu bar,
 *                        toolbar with Back/Forward/Up, address bar, status bar)
 *   Command Prompt       mini shell on the RockFS API
 *   System Information   display / memory / RockFS / uptime
 *   Display Properties   pick a colour scheme and a wallpaper
 *   About RockOS, Network Neighborhood, Recycle Bin (info windows)
 *
 * Rendering: everything is drawn into the display layer's shadow buffer.
 * Only the part of the screen that changed is redrawn ("damage" boxes), and
 * every widget that touches the damaged area is redrawn COMPLETELY, because
 * text is alpha-blended over what is already there: drawing the same text
 * twice over an unrepainted background would make it look bolder each time.
 *
 * Input: ALL hit-testing goes through hit_test(). Hover highlighting and
 * clicks use the same function, so what you see is what you click.
 *
 * NOTE: no memcpy/memset/struct-copy tricks on purpose; the kernel is built
 * with -ffreestanding -fno-builtin and has no libc.
 */

#include "desktop.h"

#include "acpi.h"
#include "display.h"
#include "filesystem.h"
#include "io.h"
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

#define SC_ESC       0x01
#define SC_ENTER     0x1C
#define SC_Q         0x10
#define SC_D         0x20
#define SC_T         0x14
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

#define FRAME           4     /* window border thickness */
#define TITLE_H         26
#define CAP_W           22
#define CAP_H           20
#define TASKBAR_H       40
#define START_W         92
#define TRAY_W          156
#define TASK_BTN_MAX    172

#define MENU_W          282
#define BANNER_W        36
#define MENU_ITEM_H     34
#define MENU_SEP_H      10
#define MENU_PAD        3
#define SUB_W           252

#define ICON_TILE_W     108
#define ICON_TILE_H     92
#define ICON_COUNT      5

#define DBLCLICK_TICKS  ((DESKTOP_TIMER_HZ * 4U) / 10U)

/* Window-local origin of a window's client area. */
#define C0X             FRAME
#define C0Y             (FRAME + TITLE_H + 2)

/* My Computer layout */
#define FM_MENU_H       26
#define FM_TOOL_H       44
#define FM_ADDR_H       32
#define FM_STATUS_H     26
#define FM_ROW_H        26
#define FM_MAX          64
#define HIST_MAX        8

#define TERM_COLS       70
#define TERM_HIST       60
#define TERM_ROW_H      22
#define TERM_CMD_MAX    62

/* Display Properties layout (window-local) */
#define SET_GX          (C0X + 10)
#define SET_G1Y         (C0Y + 8)
#define SET_G2Y         (C0Y + 8 + 116 + 12)
#define SET_GH          116
#define SET_CARD_W      150
#define SET_CARD_H      74
#define SET_STEP        162

#define THEME_COUNT     3
#define WALL_STYLES     3

enum {
    W_COMPUTER = 0, W_TERM, W_SYSINFO, W_DISPLAY, W_ABOUT, W_NETWORK,
    W_RECYCLE, W_TYPES
};

enum {
    A_NONE = 0, A_COMPUTER, A_DOCS, A_TERM, A_SYSINFO, A_DISPLAY, A_ABOUT,
    A_NETWORK, A_RECYCLE, A_LOGOFF, A_SHUTDOWN
};

enum {
    R_NONE = 0,
    R_ICON,
    R_START,
    R_TASKBTN,
    R_MENUITEM,
    R_SUBITEM,
    R_WTITLE,
    R_WCLOSE,
    R_WMAX,
    R_WMIN,
    R_WBODY,
    R_FM_MENU,
    R_FM_BTN,
    R_FM_DRIVE,
    R_FM_ROW,
    R_SET_THEME,
    R_SET_WALL,
    R_OK
};

enum {
    IC_COMPUTER = 0, IC_DOCS, IC_NETWORK, IC_RECYCLE, IC_CMD, IC_DISK,
    IC_SYSINFO, IC_DISPLAY, IC_ABOUT, IC_PROGRAMS, IC_SETTINGS, IC_SEARCH,
    IC_HELP, IC_RUN, IC_LOGOFF, IC_SHUTDOWN, IC_SPEAKER, IC_FOLDER, IC_FILE,
    IC_BACK, IC_FORWARD, IC_UP, IC_VIEWS
};

typedef struct {
    uint8_t kind;
    int8_t idx;
    int8_t sub;
} region_t;

typedef struct {
    bool open;
    bool minimized;
    bool maximized;
    int32_t x, y, w, h;
    int32_t rx, ry, rw, rh;   /* restore geometry */
} dwin_t;

typedef struct {
    char name[FS_NAME_MAX + 1];
    uint8_t type;
    uint32_t size;
} fm_entry_t;

typedef struct {
    int32_t x0, y0, x1, y1;   /* x1/y1 exclusive */
} box_t;

typedef struct {
    int32_t menu_y, tool_y, addr_y, list_y, list_h, status_y;
    int rows;
} fmg_t;

/* ====================================================================== */
/* =============================== Themes =============================== */
/* ====================================================================== */

typedef struct {
    const char* name;
    uint32_t desktop, desk_hi;
    uint32_t face, hilite, light, shadow, dkshadow;
    uint32_t title_a0, title_a1, title_i0, title_i1;
    uint32_t select_bg, window_bg, glyph;
    uint32_t banner0, banner1;
    uint8_t t_face, t_dim, t_title, t_title_idle, t_sel, t_win, t_desk;
} theme_t;

static const theme_t themes[THEME_COUNT] = {
    {
        .name = "Rock Gray",
        .desktop = 0x4A4A4A, .desk_hi = 0x6E6E6E,
        .face = 0xCCCCCC, .hilite = 0xFFFFFF, .light = 0xE6E6E6,
        .shadow = 0x808080, .dkshadow = 0x303030,
        .title_a0 = 0x262626, .title_a1 = 0x959595,
        .title_i0 = 0x6A6A6A, .title_i1 = 0xB8B8B8,
        .select_bg = 0x303030, .window_bg = 0xFFFFFF, .glyph = 0x000000,
        .banner0 = 0x2A2A2A, .banner1 = 0x5E5E5E,
        .t_face = COLOR_BLACK, .t_dim = COLOR_DARK_GRAY,
        .t_title = COLOR_WHITE, .t_title_idle = COLOR_LIGHT_GRAY,
        .t_sel = COLOR_WHITE, .t_win = COLOR_BLACK, .t_desk = COLOR_WHITE
    },
    {
        .name = "Graphite",
        .desktop = 0x1E1E1E, .desk_hi = 0x3C3C3C,
        .face = 0x4A4A4A, .hilite = 0x8A8A8A, .light = 0x6A6A6A,
        .shadow = 0x2E2E2E, .dkshadow = 0x0E0E0E,
        .title_a0 = 0x0E0E0E, .title_a1 = 0x666666,
        .title_i0 = 0x2E2E2E, .title_i1 = 0x505050,
        .select_bg = 0x8C8C8C, .window_bg = 0x2B2B2B, .glyph = 0xEAEAEA,
        .banner0 = 0x0E0E0E, .banner1 = 0x3C3C3C,
        .t_face = COLOR_WHITE, .t_dim = COLOR_LIGHT_GRAY,
        .t_title = COLOR_WHITE, .t_title_idle = COLOR_LIGHT_GRAY,
        .t_sel = COLOR_WHITE, .t_win = COLOR_WHITE, .t_desk = COLOR_WHITE
    },
    {
        .name = "Slate",
        .desktop = 0x56687A, .desk_hi = 0x7E93A8,
        .face = 0xC9D1D8, .hilite = 0xFFFFFF, .light = 0xE9EDF0,
        .shadow = 0x7F8C98, .dkshadow = 0x353F49,
        .title_a0 = 0x22364A, .title_a1 = 0x90AAC4,
        .title_i0 = 0x6E8294, .title_i1 = 0xBAC8D5,
        .select_bg = 0x2F4A66, .window_bg = 0xFFFFFF, .glyph = 0x000000,
        .banner0 = 0x1E3146, .banner1 = 0x4F6E8E,
        .t_face = COLOR_BLACK, .t_dim = COLOR_DARK_GRAY,
        .t_title = COLOR_WHITE, .t_title_idle = COLOR_LIGHT_GRAY,
        .t_sel = COLOR_WHITE, .t_win = COLOR_BLACK, .t_desk = COLOR_WHITE
    }
};

static const char* const wall_names[WALL_STYLES] = {
    "RockOS Logo", "Solid", "Gradient"
};

/* These survive closing and re-opening the desktop. */
static int theme_index = 0;
static int wall_style = 0;

#define TH (themes[theme_index])

/* Icon greys (shared by every scheme). */
#define IC_LINE 0x1E1E1E
#define IC_DK   0x5C5C5C
#define IC_MD   0x9C9C9C
#define IC_LT   0xD4D4D4
#define IC_WH   0xF6F6F6

/* ====================================================================== */
/* ============================== Tables ================================ */
/* ====================================================================== */

static const char* const win_title[W_TYPES] = {
    "My Computer", "Command Prompt", "System Information",
    "Display Properties", "About RockOS", "Network Neighborhood",
    "Recycle Bin"
};
static const uint8_t win_icon[W_TYPES] = {
    IC_COMPUTER, IC_CMD, IC_SYSINFO, IC_DISPLAY, IC_ABOUT, IC_NETWORK,
    IC_RECYCLE
};
static const int32_t win_def_w[W_TYPES] = { 660, 640, 480, 540, 480, 480, 440 };
static const int32_t win_def_h[W_TYPES] = { 440, 420, 340, 420, 290, 250, 230 };

static const uint8_t desk_icon_ic[ICON_COUNT] = {
    IC_COMPUTER, IC_DOCS, IC_NETWORK, IC_RECYCLE, IC_CMD
};
static const uint8_t desk_icon_act[ICON_COUNT] = {
    A_COMPUTER, A_DOCS, A_NETWORK, A_RECYCLE, A_TERM
};
static const char* const desk_label1[ICON_COUNT] = {
    "My Computer", "My Documents", "Network", "Recycle Bin", "Command"
};
static const char* const desk_label2[ICON_COUNT] = {
    "", "", "Neighborhood", "", "Prompt"
};

typedef struct {
    const char* label;
    uint8_t action;
} sub_item_t;

typedef struct {
    const char* label;        /* NULL = separator */
    uint8_t icon;
    uint8_t action;           /* used when sub_count == 0 */
    uint8_t sub_count;
    const sub_item_t* subs;
} menu_item_t;

static const sub_item_t subs_programs[] = {
    { "My Computer", A_COMPUTER },
    { "Command Prompt", A_TERM },
    { "System Information", A_SYSINFO }
};
static const sub_item_t subs_documents[] = {
    { "My Documents", A_DOCS },
    { "My Computer", A_COMPUTER }
};
static const sub_item_t subs_settings[] = {
    { "Display Properties", A_DISPLAY },
    { "System Information", A_SYSINFO }
};
static const sub_item_t subs_search[] = {
    { "For Files or Folders...", A_COMPUTER }
};

#define MENU_COUNT 9
static const menu_item_t menu_items[MENU_COUNT] = {
    { "Programs", IC_PROGRAMS, A_NONE, 3, subs_programs },
    { "Documents", IC_DOCS, A_NONE, 2, subs_documents },
    { "Settings", IC_SETTINGS, A_NONE, 2, subs_settings },
    { "Search", IC_SEARCH, A_NONE, 1, subs_search },
    { "Help and Support", IC_HELP, A_ABOUT, 0, NULL },
    { "Run...", IC_RUN, A_TERM, 0, NULL },
    { NULL, 0, A_NONE, 0, NULL },
    { "Log Off User...", IC_LOGOFF, A_LOGOFF, 0, NULL },
    { "Shut Down...", IC_SHUTDOWN, A_SHUTDOWN, 0, NULL }
};

static const char* const fm_menu_labels[6] = {
    "File", "Edit", "View", "Favorites", "Tools", "Help"
};
static const int32_t fm_menu_widths[6] = { 50, 50, 54, 94, 62, 56 };

/* ====================================================================== */
/* =============================== State ================================ */
/* ====================================================================== */

static bool active;
static int32_t sw, sh, tb_y;

/* damage = what must be repainted, clip = what we may paint right now */
static bool dmg_on;
static box_t dmg;
static box_t clip;

static dwin_t wins[W_TYPES];
static uint8_t zorder[W_TYPES];   /* bottom -> top */
static int zcount;
static int focused = -1;

static bool menu_open;
static int sub_open = -1;
static int sel_icon = -1;
static uint64_t last_click_tick;
static int last_click_icon = -1;
static uint64_t last_title_tick;
static int last_title_win = -1;

static int32_t mx, my;
static bool prev_left;
static region_t hover;
static int drag_win = -1;
static int32_t drag_dx, drag_dy;

static uint64_t last_sec = UINT64_MAX;

/* clock (CMOS RTC) */
static bool clock_valid;
static int clock_h12, clock_min;
static bool clock_pm;

/* My Computer */
static bool fm_computer = true;       /* true = drive list, false = folder */
static char fm_path[FS_PATH_MAX];
static fm_entry_t fm_ents[FM_MAX];
static int fm_count;
static int fm_sel;
static int fm_scroll;
static bool fm_drive_sel;
static int fm_err;
static int fm_last_click_entry = -1;
static uint64_t fm_last_click_tick;
static bool hist_c[HIST_MAX];
static char hist_p[HIST_MAX][FS_PATH_MAX];
static int hist_len;
static int hist_pos = -1;

/* Command Prompt */
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

/* "123.4 MB" / "1.25 GB" from a byte count. */
static char* put_capacity(char* d, uint64_t bytes)
{
    if (bytes >= (1ULL << 30)) {
        uint64_t hundredths = ((bytes >> 20) * 100) >> 10;
        d = put_u(d, hundredths / 100);
        *d++ = '.';
        d = put_2(d, hundredths % 100);
        return put_str(d, " GB");
    }
    {
        uint64_t tenths = (bytes * 10) >> 20;
        d = put_u(d, tenths / 10);
        *d++ = '.';
        *d++ = (char)('0' + tenths % 10);
        *d = 0;
        return put_str(d, " MB");
    }
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    if (hi < lo) hi = lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool inside(int32_t px, int32_t py, int32_t x, int32_t y,
    int32_t w, int32_t h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

static int32_t isqrt_i(int32_t v)
{
    int32_t r = 0;
    if (v <= 0) return 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* Estimated pixel width of a UI string (the font cannot be measured). */
static int32_t tw(const char* s)
{
    return (int32_t)slen(s) * 9;
}

/* Blend two 0xRRGGBB colours: pct_b percent of b, the rest of a. */
static uint32_t mix_rgb(uint32_t a, uint32_t b, int32_t pct_b)
{
    int32_t ar = (int32_t)((a >> 16) & 255), ag = (int32_t)((a >> 8) & 255);
    int32_t ab = (int32_t)(a & 255);
    int32_t br = (int32_t)((b >> 16) & 255), bg = (int32_t)((b >> 8) & 255);
    int32_t bb = (int32_t)(b & 255);
    int32_t r = (ar * (100 - pct_b) + br * pct_b) / 100;
    int32_t g = (ag * (100 - pct_b) + bg * pct_b) / 100;
    int32_t bl = (ab * (100 - pct_b) + bb * pct_b) / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
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
/* ================================ Clock =============================== */
/* ====================================================================== */

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

/* Reads the RTC. Returns true when the displayed time changed. */
static bool clock_poll(void)
{
    uint8_t status_a = cmos_read(0x0A);
    uint8_t mn, hr, b, h;
    bool pm_flag;
    int h24, old_h = clock_h12, old_m = clock_min;
    bool old_pm = clock_pm, old_valid = clock_valid;

    if (status_a & 0x80) return false;   /* update in progress */
    mn = cmos_read(0x02);
    hr = cmos_read(0x04);
    b = cmos_read(0x0B);

    pm_flag = (hr & 0x80) != 0;
    h = (uint8_t)(hr & 0x7F);
    if (!(b & 0x04)) {
        h = (uint8_t)((h & 0x0F) + (h >> 4) * 10);
        mn = (uint8_t)((mn & 0x0F) + (mn >> 4) * 10);
    }
    h24 = h;
    if (!(b & 0x02)) {
        h24 = (h % 12) + (pm_flag ? 12 : 0);
    }
    if (h24 > 23 || mn > 59) return false;

    clock_pm = h24 >= 12;
    clock_h12 = h24 % 12;
    if (clock_h12 == 0) clock_h12 = 12;
    clock_min = mn;
    clock_valid = true;
    return !old_valid || old_h != clock_h12 || old_m != clock_min ||
        old_pm != clock_pm;
}

static void clock_text(char* out)
{
    char* p;
    if (!clock_valid) {
        put_str(out, "--:--");
        return;
    }
    p = put_u(out, (uint64_t)clock_h12);
    *p++ = ':';
    p = put_2(p, (uint64_t)clock_min);
    *p++ = ' ';
    *p = '\0';
    put_str(p, clock_pm ? "PM" : "AM");
}

/* ====================================================================== */
/* =========================== Clipped drawing ========================== */
/* ====================================================================== */

/* Clipped RGB rect: never paints outside the current clip box. */
static void rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb)
{
    int32_t x1 = x + w, y1 = y + h;
    if (x < clip.x0) x = clip.x0;
    if (y < clip.y0) y = clip.y0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (y1 > clip.y1) y1 = clip.y1;
    if (x1 <= x || y1 <= y) return;
    display_fill_rect_rgb((uint32_t)x, (uint32_t)y, (uint32_t)(x1 - x),
        (uint32_t)(y1 - y), (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8),
        (uint8_t)rgb);
}

/* Text is NOT clipped (the font renderer cannot): callers only draw text for
 * widgets that lie completely inside the clip box. */
static void text(int32_t x, int32_t y, const char* s, uint8_t color)
{
    if (x < 0 || y < 0 || x >= sw || y >= sh) return;
    display_draw_text_ui((uint32_t)x, (uint32_t)y, s, color);
}

static void circle(int32_t cx, int32_t cy, int32_t r, uint32_t c)
{
    for (int32_t dy = -r; dy <= r; dy++) {
        int32_t dx = isqrt_i(r * r - dy * dy);
        rect(cx - dx, cy + dy, dx * 2 + 1, 1, c);
    }
}

static void rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
    uint32_t c)
{
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) {
        rect(x, y, w, h, c);
        return;
    }
    rect(x, y + r, w, h - 2 * r, c);
    for (int32_t i = 0; i < r; i++) {
        int32_t dy2 = 2 * (r - i) - 1;
        int32_t half2 = isqrt_i(4 * r * r - dy2 * dy2);
        int32_t inset = r - (half2 + 1) / 2;
        if (inset < 0) inset = 0;
        rect(x + inset, y + i, w - 2 * inset, 1, c);
        rect(x + inset, y + h - 1 - i, w - 2 * inset, 1, c);
    }
}

/* RGB triangle by scanlines (display_fill_triangle only knows 16 colours). */
static void tri_rgb(int32_t ax, int32_t ay, int32_t bx, int32_t by,
    int32_t cx, int32_t cy, uint32_t col)
{
    int32_t t;
    if (ay > by) { t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; }
    if (ay > cy) { t = ax; ax = cx; cx = t; t = ay; ay = cy; cy = t; }
    if (by > cy) { t = bx; bx = cx; cx = t; t = by; by = cy; cy = t; }
    if (cy == ay) return;
    for (int32_t y = ay; y <= cy; y++) {
        int32_t xl = ax + (cx - ax) * (y - ay) / (cy - ay);
        int32_t xr;
        if (y < by) {
            xr = (by == ay) ? bx : ax + (bx - ax) * (y - ay) / (by - ay);
        } else {
            xr = (cy == by) ? bx : bx + (cx - bx) * (y - by) / (cy - by);
        }
        if (xl > xr) { t = xl; xl = xr; xr = t; }
        rect(xl, y, xr - xl + 1, 1, col);
    }
}

/* ---- classic 3D frames ---- */

static void frame3d(int32_t x, int32_t y, int32_t w, int32_t h,
    uint32_t otl, uint32_t obr, uint32_t itl, uint32_t ibr)
{
    if (w < 4 || h < 4) return;
    rect(x, y, w - 1, 1, otl);
    rect(x, y, 1, h - 1, otl);
    rect(x + w - 1, y, 1, h, obr);
    rect(x, y + h - 1, w, 1, obr);
    rect(x + 1, y + 1, w - 3, 1, itl);
    rect(x + 1, y + 1, 1, h - 3, itl);
    rect(x + w - 2, y + 1, 1, h - 2, ibr);
    rect(x + 1, y + h - 2, w - 2, 1, ibr);
}

static void bevel_raised(int32_t x, int32_t y, int32_t w, int32_t h)
{
    frame3d(x, y, w, h, TH.hilite, TH.dkshadow, TH.light, TH.shadow);
}

static void bevel_sunken(int32_t x, int32_t y, int32_t w, int32_t h)
{
    frame3d(x, y, w, h, TH.shadow, TH.hilite, TH.dkshadow, TH.light);
}

static void button_box(int32_t x, int32_t y, int32_t w, int32_t h,
    bool pressed)
{
    rect(x, y, w, h, TH.face);
    if (pressed) bevel_sunken(x, y, w, h);
    else bevel_raised(x, y, w, h);
}

static void gradient_h(int32_t x, int32_t y, int32_t w, int32_t h,
    uint32_t c0, uint32_t c1)
{
    int32_t i;
    if (w <= 0) return;
    for (i = 0; i < w; i += 6) {
        int32_t bw = (i + 6 <= w) ? 6 : w - i;
        rect(x + i, y, bw, h, mix_rgb(c0, c1, (i * 100) / w));
    }
}

static void gradient_v(int32_t x, int32_t y, int32_t w, int32_t h,
    uint32_t c0, uint32_t c1)
{
    int32_t i;
    if (h <= 0) return;
    for (i = 0; i < h; i += 4) {
        int32_t bh = (i + 4 <= h) ? 4 : h - i;
        rect(x, y + i, w, bh, mix_rgb(c0, c1, (i * 100) / h));
    }
}

/* Etched group box with a caption. */
static void groupbox(int32_t x, int32_t y, int32_t w, int32_t h,
    const char* label)
{
    rect(x, y + 9, w, 1, TH.shadow);
    rect(x, y + 9, 1, h - 9, TH.shadow);
    rect(x, y + h - 1, w, 1, TH.shadow);
    rect(x + w - 1, y + 9, 1, h - 9, TH.shadow);
    rect(x + 1, y + 10, w - 2, 1, TH.hilite);
    rect(x + 1, y + 10, 1, h - 10, TH.hilite);
    rect(x + 1, y + h, w, 1, TH.hilite);
    rect(x + w, y + 9, 1, h - 8, TH.hilite);
    rect(x + 10, y, tw(label) + 8, 20, TH.face);
    text(x + 14, y, label, TH.t_face);
}

/* ====================================================================== */
/* ============================ Damage boxes ============================= */
/* ====================================================================== */

static bool box_touch(const box_t* a, int32_t x, int32_t y, int32_t w,
    int32_t h)
{
    return x < a->x1 && x + w > a->x0 && y < a->y1 && y + h > a->y0;
}

static bool box_covers(const box_t* a, int32_t x, int32_t y, int32_t w,
    int32_t h)
{
    return x >= a->x0 && y >= a->y0 && x + w <= a->x1 && y + h <= a->y1;
}

static void damage_add(int32_t x, int32_t y, int32_t w, int32_t h)
{
    int32_t x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x1 <= x || y1 <= y) return;
    if (!dmg_on) {
        dmg.x0 = x; dmg.y0 = y; dmg.x1 = x1; dmg.y1 = y1;
        dmg_on = true;
        return;
    }
    if (x < dmg.x0) dmg.x0 = x;
    if (y < dmg.y0) dmg.y0 = y;
    if (x1 > dmg.x1) dmg.x1 = x1;
    if (y1 > dmg.y1) dmg.y1 = y1;
}

static void damage_all(void)
{
    damage_add(0, 0, sw, sh);
}

/* ---- widget geometry ---- */

static int32_t icon_x(void) { return 10; }
static int32_t icon_y(int i) { return 10 + i * ICON_TILE_H; }

static int32_t menu_item_h(int i)
{
    return menu_items[i].label ? MENU_ITEM_H : MENU_SEP_H;
}

static int32_t menu_total_h(void)
{
    int32_t h = 2 * MENU_PAD + 4;
    int i;
    for (i = 0; i < MENU_COUNT; i++) h += menu_item_h(i);
    return h;
}

static int32_t menu_top(void) { return tb_y - menu_total_h(); }

static int32_t menu_row_y(int idx)
{
    int32_t y = menu_top() + 2 + MENU_PAD;
    int i;
    for (i = 0; i < idx; i++) y += menu_item_h(i);
    return y;
}

static void sub_geom(int parent, int32_t* x, int32_t* y, int32_t* w,
    int32_t* h)
{
    *w = SUB_W;
    *h = menu_items[parent].sub_count * MENU_ITEM_H + 8;
    *x = MENU_W - 4;
    *y = menu_row_y(parent) - 4;
    if (*y + *h > tb_y) *y = tb_y - *h;
}

static void menu_bbox(box_t* b)
{
    b->x0 = 0;
    b->y0 = menu_top();
    b->x1 = MENU_W;
    b->y1 = tb_y;
    if (sub_open >= 0) {
        int32_t sx, sy, swd, shg;
        sub_geom(sub_open, &sx, &sy, &swd, &shg);
        if (sx + swd > b->x1) b->x1 = sx + swd;
        if (sy < b->y0) b->y0 = sy;
    }
}

static int open_window_count(void)
{
    int i, n = 0;
    for (i = 0; i < W_TYPES; i++) if (wins[i].open) n++;
    return n;
}

static int32_t task_btn_w(void)
{
    int n = open_window_count();
    int32_t avail = sw - (START_W + 16) - TRAY_W - 12;
    int32_t w = TASK_BTN_MAX;
    if (n > 0 && avail / n < w) w = avail / n;
    if (w < 60) w = 60;
    return w;
}

static int32_t task_btn_x(int k)
{
    return START_W + 14 + k * (task_btn_w() + 4);
}

static bool logo_visible(void)
{
    return wall_style == 0 &&
        sw >= (int32_t)DISPLAY_ROCKOS_LOGO_WIDTH + 140 &&
        tb_y >= (int32_t)DISPLAY_ROCKOS_LOGO_HEIGHT;
}

static int32_t logo_x(void)
{
    return (sw - (int32_t)DISPLAY_ROCKOS_LOGO_WIDTH) / 2 + 60;
}

static int32_t logo_y(void)
{
    return (tb_y * 66) / 100 - (int32_t)DISPLAY_ROCKOS_LOGO_HEIGHT / 2;
}

static int widget_boxes(box_t* out)
{
    int n = 0;
    int i;
    if (logo_visible()) {
        out[n].x0 = logo_x(); out[n].y0 = logo_y();
        out[n].x1 = logo_x() + (int32_t)DISPLAY_ROCKOS_LOGO_WIDTH;
        out[n].y1 = logo_y() + (int32_t)DISPLAY_ROCKOS_LOGO_HEIGHT;
        n++;
    }
    for (i = 0; i < ICON_COUNT; i++) {
        out[n].x0 = icon_x(); out[n].y0 = icon_y(i);
        out[n].x1 = icon_x() + ICON_TILE_W; out[n].y1 = icon_y(i) + ICON_TILE_H;
        n++;
    }
    for (i = 0; i < zcount; i++) {
        int w = zorder[i];
        if (wins[w].minimized) continue;
        out[n].x0 = wins[w].x; out[n].y0 = wins[w].y;
        out[n].x1 = wins[w].x + wins[w].w; out[n].y1 = wins[w].y + wins[w].h;
        n++;
    }
    out[n].x0 = 0; out[n].y0 = tb_y; out[n].x1 = sw; out[n].y1 = sh;
    n++;
    if (menu_open) {
        menu_bbox(&out[n]);
        n++;
    }
    return n;
}

/* Grow the damage box until every widget it touches lies completely inside. */
static void damage_close(void)
{
    box_t w[20];
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 20) {
        int n = widget_boxes(w);
        changed = false;
        for (int i = 0; i < n; i++) {
            int32_t x = w[i].x0, y = w[i].y0;
            int32_t ww = w[i].x1 - w[i].x0, hh = w[i].y1 - w[i].y0;
            if (box_touch(&dmg, x, y, ww, hh) &&
                !box_covers(&dmg, x, y, ww, hh)) {
                damage_add(x, y, ww, hh);
                changed = true;
            }
        }
    }
}

static void damage_window(int w)
{
    damage_add(wins[w].x, wins[w].y, wins[w].w, wins[w].h);
}

static void damage_taskbar(void)
{
    damage_add(0, tb_y, sw, TASKBAR_H);
}

static void damage_menu(void)
{
    box_t b;
    menu_bbox(&b);
    damage_add(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
}

/* Only regions whose look changes on hover need a repaint. */
static void damage_region(region_t r)
{
    switch (r.kind) {
    case R_MENUITEM:
    case R_SUBITEM:
        if (menu_open) damage_menu();
        break;
    case R_FM_BTN:
    case R_FM_MENU:
        if (r.idx >= 0 && r.idx < W_TYPES) damage_window(r.idx);
        break;
    default:
        break;
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

static void fm_geom(int w, fmg_t* g)
{
    int32_t ch = wins[w].h - C0Y - FRAME;
    g->menu_y = C0Y;
    g->tool_y = C0Y + FM_MENU_H;
    g->addr_y = g->tool_y + FM_TOOL_H;
    g->list_y = g->addr_y + FM_ADDR_H + 2;
    g->status_y = C0Y + ch - FM_STATUS_H;
    g->list_h = g->status_y - g->list_y - 2;
    g->rows = (g->list_h - 6) / FM_ROW_H;
    if (g->rows < 1) g->rows = 1;
    if (g->rows > FM_MAX) g->rows = FM_MAX;
}

static void fm_ensure_visible(int rows)
{
    if (fm_sel < fm_scroll) fm_scroll = fm_sel;
    else if (fm_sel >= fm_scroll + rows) fm_scroll = fm_sel - rows + 1;
}

/* Show a location without touching the history. */
static void fm_apply(bool computer, const char* path)
{
    fm_computer = computer;
    if (!computer) {
        copy_str(fm_path, path, sizeof(fm_path));
        fm_refresh();
    }
    fm_sel = 0;
    fm_scroll = 0;
    fm_drive_sel = false;
}

static void hist_push(bool computer, const char* path)
{
    int i;
    hist_len = hist_pos + 1;
    if (hist_len >= HIST_MAX) {
        for (i = 0; i < HIST_MAX - 1; i++) {
            hist_c[i] = hist_c[i + 1];
            copy_str(hist_p[i], hist_p[i + 1], sizeof(hist_p[i]));
        }
        hist_len = HIST_MAX - 1;
    }
    hist_c[hist_len] = computer;
    copy_str(hist_p[hist_len], path, sizeof(hist_p[hist_len]));
    hist_len++;
    hist_pos = hist_len - 1;
}

static void fm_navigate(bool computer, const char* path)
{
    hist_push(computer, path);
    fm_apply(computer, path);
}

static void fm_back(void)
{
    if (hist_pos > 0) {
        hist_pos--;
        fm_apply(hist_c[hist_pos], hist_p[hist_pos]);
    }
}

static void fm_forward(void)
{
    if (hist_pos + 1 < hist_len) {
        hist_pos++;
        fm_apply(hist_c[hist_pos], hist_p[hist_pos]);
    }
}

static void fm_up(void)
{
    char tmp[FS_PATH_MAX];
    if (fm_computer) return;
    if (streq(fm_path, "/")) {
        fm_navigate(true, "/");
        return;
    }
    path_resolve(fm_path, "..", tmp, sizeof(tmp));
    fm_navigate(false, tmp);
}

static void fm_open_selected(void)
{
    char tmp[FS_PATH_MAX];
    if (fm_computer) {
        if (fm_drive_sel) fm_navigate(false, "/");
        return;
    }
    if (fm_sel < 0 || fm_sel >= fm_count) return;
    if (fm_ents[fm_sel].type != FS_TYPE_DIR) return;
    path_resolve(fm_path, fm_ents[fm_sel].name, tmp, sizeof(tmp));
    fm_navigate(false, tmp);
}

static void fm_init(void)
{
    hist_len = 0;
    hist_pos = -1;
    fm_count = 0;
    fm_err = FS_OK;
    copy_str(fm_path, "/", sizeof(fm_path));
    fm_navigate(true, "/");
}

static void fm_key(const input_key_event_t* e)
{
    fmg_t g;
    fm_geom(W_COMPUTER, &g);
    if (e->is_extended && e->scancode == SC_UP) {
        if (!fm_computer && fm_sel > 0) fm_sel--;
        fm_ensure_visible(g.rows);
        damage_window(W_COMPUTER);
    } else if (e->is_extended && e->scancode == SC_DOWN) {
        if (!fm_computer && fm_sel + 1 < fm_count) fm_sel++;
        fm_ensure_visible(g.rows);
        damage_window(W_COMPUTER);
    } else if (e->ascii == '\n' || e->ascii == '\r' ||
        (!e->is_extended && e->scancode == SC_ENTER)) {
        fm_open_selected();
        damage_window(W_COMPUTER);
    } else if (e->ascii == '\b') {
        fm_up();
        damage_window(W_COMPUTER);
    }
}

/* ====================================================================== */
/* ============================ Command Prompt ========================== */
/* ====================================================================== */

static int term_rows(int w)
{
    int32_t ch = wins[w].h - C0Y - FRAME;
    int r = (int)((ch - 14 - 40) / TERM_ROW_H);
    if (r < 1) r = 1;
    if (r > TERM_HIST) r = TERM_HIST;
    return r;
}

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
        term_push("help cls pwd cd dir mkdir create read write delete");
        term_push("mem uptime ver scheme exit shell");
        term_push("write <path> <text>   read <path>   cd <path>");
        term_push("scheme <0-2>  changes the colour scheme");
    } else if (streq(argv[0], "cls") || streq(argv[0], "clear")) {
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
    } else if (streq(argv[0], "ver") || streq(argv[0], "version")) {
        term_push("RockOS V.2 [ROK kernel]");
    } else if (streq(argv[0], "scheme") || streq(argv[0], "theme")) {
        if (argc >= 2 && argv[1][0] >= '0' && argv[1][0] < '0' + THEME_COUNT &&
            argv[1][1] == '\0') {
            theme_index = argv[1][0] - '0';
            damage_all();
        }
        p = put_str(line, "scheme: ");
        put_str(p, TH.name);
        term_add_line(line);
    } else if (streq(argv[0], "exit")) {
        win_close(W_TERM);
    } else if (streq(argv[0], "shell")) {
        desktop_stop();
    } else {
        p = put_str(line, "'");
        p = put_str(p, argv[0]);
        copy_str(p, "' is not recognized. Type help.", (size_t)(TERM_COLS + 1) -
            (size_t)(p - line));
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
        damage_window(W_TERM);
    } else if (e->ascii == '\b') {
        if (term_len > 0) term_cmd[--term_len] = '\0';
        damage_window(W_TERM);
    } else if (e->ascii >= 0x20 && e->ascii <= 0x7E && term_len < TERM_CMD_MAX) {
        term_cmd[term_len++] = e->ascii;
        term_cmd[term_len] = '\0';
        damage_window(W_TERM);
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
    wins[w].w = win_def_w[w];
    wins[w].h = win_def_h[w];
    if (wins[w].w > sw) wins[w].w = sw;
    if (wins[w].h > tb_y) wins[w].h = tb_y;
    wins[w].x = clampi(60 + w * 34, 0, sw - wins[w].w);
    wins[w].y = clampi(24 + w * 28, 0, tb_y - wins[w].h);
    wins[w].maximized = false;
}

static void win_toggle_max(int w)
{
    if (wins[w].maximized) {
        wins[w].x = wins[w].rx;
        wins[w].y = wins[w].ry;
        wins[w].w = wins[w].rw;
        wins[w].h = wins[w].rh;
        wins[w].maximized = false;
    } else {
        wins[w].rx = wins[w].x;
        wins[w].ry = wins[w].y;
        wins[w].rw = wins[w].w;
        wins[w].rh = wins[w].h;
        wins[w].x = 0;
        wins[w].y = 0;
        wins[w].w = sw;
        wins[w].h = tb_y;
        wins[w].maximized = true;
    }
    if (w == W_COMPUTER) {
        fmg_t g;
        fm_geom(W_COMPUTER, &g);
        fm_ensure_visible(g.rows);
    }
}

static void win_open(int w)
{
    if (!wins[w].open) {
        wins[w].open = true;
        win_place(w);
        if (w == W_COMPUTER) fm_init();
        if (w == W_TERM && term_count == 0) {
            term_push("RockOS V.2 [ROK kernel]");
            term_push("Type help for a list of commands.");
        }
    }
    win_focus(w);
}

static void win_close(int w)
{
    wins[w].open = false;
    wins[w].minimized = false;
    wins[w].maximized = false;
    z_remove(w);
    if (focused == w) focus_top();
}

static void win_minimize(int w)
{
    wins[w].minimized = true;
    if (focused == w) focus_top();
}

static void run_action(int a)
{
    switch (a) {
    case A_COMPUTER:
        win_open(W_COMPUTER);
        fm_navigate(true, "/");
        break;
    case A_DOCS:
        win_open(W_COMPUTER);
        fm_navigate(false, "/");
        break;
    case A_TERM:    win_open(W_TERM); break;
    case A_SYSINFO: win_open(W_SYSINFO); break;
    case A_DISPLAY: win_open(W_DISPLAY); break;
    case A_ABOUT:   win_open(W_ABOUT); break;
    case A_NETWORK: win_open(W_NETWORK); break;
    case A_RECYCLE: win_open(W_RECYCLE); break;
    case A_LOGOFF:  desktop_stop(); break;
    case A_SHUTDOWN:
        desktop_stop();
        acpi_poweroff();
        break;
    default:
        break;
    }
}

/* ====================================================================== */
/* ============================== Hit test ============================== */
/* ====================================================================== */

static int32_t fm_menu_x(int i)
{
    int32_t x = C0X + 6;
    int k;
    for (k = 0; k < i; k++) x += fm_menu_widths[k];
    return x;
}

/* Toolbar button i: local x and width. */
static void fm_btn_geom(int i, int32_t* x, int32_t* w)
{
    static const int32_t xs[5] = { 6, 96, 140, 192, 302 };
    static const int32_t ws[5] = { 86, 40, 40, 106, 106 };
    *x = C0X + xs[i];
    *w = ws[i];
}

static region_t hit_test(int32_t x, int32_t y)
{
    region_t r = { R_NONE, 0, 0 };
    int i, k;

    if (menu_open) {
        if (sub_open >= 0) {
            int32_t sx, sy, swd, shg;
            sub_geom(sub_open, &sx, &sy, &swd, &shg);
            if (inside(x, y, sx, sy, swd, shg)) {
                int32_t item = (y - sy - 4) / MENU_ITEM_H;
                r.kind = R_SUBITEM;
                r.idx = (int8_t)sub_open;
                r.sub = (y - sy - 4 >= 0 &&
                    item < menu_items[sub_open].sub_count)
                    ? (int8_t)item : (int8_t)-1;
                return r;
            }
        }
        if (inside(x, y, 0, menu_top(), MENU_W, menu_total_h())) {
            r.kind = R_MENUITEM;
            r.idx = -1;
            if (x >= BANNER_W + 4) {
                for (i = 0; i < MENU_COUNT; i++) {
                    int32_t ry = menu_row_y(i);
                    if (y >= ry && y < ry + menu_item_h(i)) {
                        r.idx = menu_items[i].label ? (int8_t)i : (int8_t)-1;
                        break;
                    }
                }
            }
            return r;
        }
    }

    if (y >= tb_y) {
        int32_t bw = task_btn_w();
        if (inside(x, y, 4, tb_y + 5, START_W, 30)) {
            r.kind = R_START;
            return r;
        }
        k = 0;
        for (i = 0; i < W_TYPES; i++) {
            if (!wins[i].open) continue;
            if (inside(x, y, task_btn_x(k), tb_y + 5, bw, 30)) {
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
        int32_t wx = wins[w].x, wy = wins[w].y, ww = wins[w].w, wh = wins[w].h;
        if (wins[w].minimized) continue;
        if (!inside(x, y, wx, wy, ww, wh)) continue;
        r.idx = (int8_t)w;
        r.kind = R_WBODY;

        if (inside(x, y, wx + FRAME, wy + FRAME, ww - 2 * FRAME, TITLE_H)) {
            int32_t tr = wx + ww - FRAME;
            int32_t cx_close = tr - 2 - CAP_W;
            int32_t cx_max = cx_close - 2 - CAP_W;
            int32_t cx_min = cx_max - CAP_W;
            int32_t by = wy + FRAME + 3;
            if (inside(x, y, cx_close, by, CAP_W, CAP_H)) r.kind = R_WCLOSE;
            else if (inside(x, y, cx_max, by, CAP_W, CAP_H)) r.kind = R_WMAX;
            else if (inside(x, y, cx_min, by, CAP_W, CAP_H)) r.kind = R_WMIN;
            else r.kind = R_WTITLE;
            return r;
        }

        {
            int32_t lx = x - wx, ly = y - wy;
            if (w == W_COMPUTER) {
                fmg_t g;
                fm_geom(W_COMPUTER, &g);
                if (ly >= g.menu_y && ly < g.menu_y + FM_MENU_H) {
                    for (k = 0; k < 6; k++) {
                        if (lx >= fm_menu_x(k) &&
                            lx < fm_menu_x(k) + fm_menu_widths[k]) {
                            r.kind = R_FM_MENU;
                            r.sub = (int8_t)k;
                        }
                    }
                } else if (ly >= g.tool_y + 4 && ly < g.tool_y + FM_TOOL_H - 4) {
                    for (k = 0; k < 5; k++) {
                        int32_t bx, bw;
                        fm_btn_geom(k, &bx, &bw);
                        if (lx >= bx && lx < bx + bw) {
                            r.kind = R_FM_BTN;
                            r.sub = (int8_t)k;
                        }
                    }
                } else if (fm_computer) {
                    if (inside(lx, ly, C0X + 12, g.list_y + 44, 320, 54) &&
                        filesystem_is_mounted()) {
                        r.kind = R_FM_DRIVE;
                    }
                } else if (ly >= g.list_y + 2 &&
                    ly < g.list_y + 2 + g.rows * FM_ROW_H) {
                    int row = (ly - g.list_y - 2) / FM_ROW_H;
                    if (fm_scroll + row < fm_count) {
                        r.kind = R_FM_ROW;
                        r.sub = (int8_t)row;
                    }
                }
            } else if (w == W_DISPLAY) {
                for (k = 0; k < THEME_COUNT; k++) {
                    if (inside(lx, ly, SET_GX + 12 + k * SET_STEP, SET_G1Y + 26,
                        SET_CARD_W, SET_CARD_H)) {
                        r.kind = R_SET_THEME;
                        r.sub = (int8_t)k;
                    }
                }
                for (k = 0; k < WALL_STYLES; k++) {
                    if (inside(lx, ly, SET_GX + 12 + k * SET_STEP, SET_G2Y + 26,
                        SET_CARD_W, SET_CARD_H)) {
                        r.kind = R_SET_WALL;
                        r.sub = (int8_t)k;
                    }
                }
            } else if (w == W_ABOUT || w == W_NETWORK || w == W_RECYCLE) {
                if (inside(lx, ly, ww - FRAME - 100, wh - FRAME - 40, 88, 28)) {
                    r.kind = R_OK;
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
/* ================================ Icons =============================== */
/* ====================================================================== */

/* Icons are designed on a 48x48 grid and scaled to any size. */
static int32_t g_ix, g_iy, g_is;
static bool g_dim;

#define CL (g_dim ? IC_MD : IC_LINE)

static int32_t isc(int32_t v)
{
    return (v * g_is) / 48;
}

static int32_t isc1(int32_t v)
{
    int32_t r = (v * g_is) / 48;
    return (r < 1 && v > 0) ? 1 : r;
}

static void ir(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c)
{
    rect(g_ix + isc(x), g_iy + isc(y), isc1(w), isc1(h), c);
}

static void irr(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
    uint32_t c)
{
    rrect(g_ix + isc(x), g_iy + isc(y), isc1(w), isc1(h), isc(r), c);
}

static void icirc(int32_t cx, int32_t cy, int32_t r, uint32_t c)
{
    circle(g_ix + isc(cx), g_iy + isc(cy), isc1(r), c);
}

static void itri(int32_t ax, int32_t ay, int32_t bx, int32_t by,
    int32_t cx, int32_t cy, uint32_t c)
{
    tri_rgb(g_ix + isc(ax), g_iy + isc(ay), g_ix + isc(bx), g_iy + isc(by),
        g_ix + isc(cx), g_iy + isc(cy), c);
}

static void draw_icon(int type, int32_t x, int32_t y, int32_t size)
{
    int i;
    g_ix = x;
    g_iy = y;
    g_is = size;

    switch (type) {
    case IC_COMPUTER:
    case IC_SYSINFO:
    case IC_DISPLAY:
        irr(5, 5, 38, 28, 3, IC_LINE);       /* monitor */
        irr(7, 7, 34, 24, 2, IC_LT);
        ir(10, 10, 28, 18, IC_LINE);
        ir(11, 11, 26, 16, IC_DK);
        ir(12, 12, 24, 4, IC_MD);            /* screen shine */
        ir(19, 33, 10, 4, IC_LINE);          /* neck */
        irr(8, 36, 32, 9, 2, IC_LINE);       /* base unit */
        irr(9, 37, 30, 7, 1, IC_LT);
        ir(13, 40, 14, 2, IC_DK);
        ir(32, 40, 4, 2, IC_WH);
        if (type == IC_SYSINFO) {
            ir(22, 13, 4, 4, IC_WH);
            ir(22, 19, 4, 8, IC_WH);
        } else if (type == IC_DISPLAY) {
            ir(12, 12, 7, 15, IC_WH);
            ir(20, 12, 7, 15, IC_MD);
            ir(28, 12, 9, 15, IC_LT);
        }
        break;
    case IC_DOCS:
    case IC_FOLDER:
    case IC_PROGRAMS:
        irr(3, 9, 20, 9, 2, CL);
        irr(4, 10, 18, 7, 1, IC_MD);
        irr(3, 14, 42, 30, 3, CL);
        irr(4, 15, 40, 28, 2, IC_LT);
        ir(6, 18, 36, 2, IC_WH);
        ir(6, 38, 36, 2, IC_MD);
        break;
    case IC_FILE:
        irr(11, 5, 26, 38, 2, IC_LINE);
        irr(12, 6, 24, 36, 1, IC_WH);
        ir(16, 16, 16, 2, IC_MD);
        ir(16, 22, 16, 2, IC_MD);
        ir(16, 28, 16, 2, IC_MD);
        itri(27, 6, 36, 6, 36, 15, IC_MD);
        break;
    case IC_NETWORK:
        irr(4, 6, 22, 17, 2, IC_LINE);
        irr(5, 7, 20, 15, 1, IC_LT);
        ir(7, 9, 16, 10, IC_DK);
        irr(24, 24, 22, 17, 2, IC_LINE);
        irr(25, 25, 20, 15, 1, IC_LT);
        ir(27, 27, 16, 10, IC_DK);
        ir(13, 23, 3, 7, IC_LINE);
        ir(13, 29, 13, 3, IC_LINE);
        ir(9, 22, 11, 2, IC_LINE);
        ir(29, 41, 11, 2, IC_LINE);
        break;
    case IC_RECYCLE:
        irr(12, 14, 24, 30, 3, IC_LINE);
        irr(13, 15, 22, 28, 2, IC_MD);
        ir(18, 18, 3, 22, IC_DK);
        ir(24, 18, 3, 22, IC_DK);
        ir(30, 18, 3, 22, IC_DK);
        ir(9, 10, 30, 5, IC_LINE);
        ir(10, 11, 28, 3, IC_LT);
        ir(19, 6, 10, 4, IC_LINE);
        break;
    case IC_CMD:
        irr(4, 8, 40, 32, 2, IC_LINE);
        ir(6, 10, 36, 28, 0x0C0C0C);
        ir(6, 10, 36, 5, IC_DK);
        itri(10, 20, 10, 32, 19, 26, IC_WH);
        ir(24, 30, 11, 3, IC_WH);
        break;
    case IC_DISK:
        irr(4, 14, 40, 24, 3, IC_LINE);
        irr(5, 15, 38, 22, 2, IC_LT);
        ir(6, 16, 36, 1, IC_WH);
        ir(8, 21, 32, 3, IC_DK);
        ir(8, 30, 22, 3, IC_MD);
        ir(34, 30, 6, 3, IC_DK);
        break;
    case IC_ABOUT:
        itri(24, 4, 6, 18, 42, 18, IC_WH);
        itri(6, 18, 24, 18, 13, 43, IC_LT);
        itri(24, 18, 42, 18, 36, 43, IC_MD);
        itri(13, 43, 24, 18, 36, 43, IC_DK);
        break;
    case IC_SETTINGS:
        ir(21, 3, 6, 9, IC_DK);
        ir(21, 36, 6, 9, IC_DK);
        ir(3, 21, 9, 6, IC_DK);
        ir(36, 21, 9, 6, IC_DK);
        ir(8, 8, 8, 8, IC_DK);
        ir(32, 8, 8, 8, IC_DK);
        ir(8, 32, 8, 8, IC_DK);
        ir(32, 32, 8, 8, IC_DK);
        icirc(24, 24, 15, IC_LINE);
        icirc(24, 24, 13, IC_MD);
        icirc(24, 24, 6, IC_LINE);
        icirc(24, 24, 4, IC_LT);
        break;
    case IC_SEARCH:
        icirc(20, 20, 15, CL);
        icirc(20, 20, 12, IC_WH);
        icirc(20, 20, 10, IC_LT);
        for (i = 0; i < 10; i++) ir(31 + i, 31 + i, 5, 5, CL);
        break;
    case IC_HELP:
        icirc(24, 24, 20, IC_LINE);
        icirc(24, 24, 18, IC_LT);
        ir(17, 12, 14, 4, IC_LINE);
        ir(17, 12, 4, 9, IC_LINE);
        ir(27, 12, 4, 13, IC_LINE);
        ir(21, 22, 10, 4, IC_LINE);
        ir(21, 24, 4, 8, IC_LINE);
        ir(21, 35, 4, 4, IC_LINE);
        break;
    case IC_RUN:
        irr(4, 9, 40, 30, 2, IC_LINE);
        irr(5, 10, 38, 28, 1, IC_WH);
        ir(5, 10, 38, 6, IC_DK);
        itri(13, 22, 13, 34, 25, 28, IC_LINE);
        ir(29, 26, 10, 3, IC_MD);
        break;
    case IC_LOGOFF:
        icirc(21, 14, 8, IC_LINE);
        icirc(21, 14, 6, IC_LT);
        irr(7, 26, 28, 18, 7, IC_LINE);
        irr(9, 28, 24, 15, 5, IC_LT);
        itri(38, 22, 38, 36, 47, 29, IC_DK);
        break;
    case IC_SHUTDOWN:
        icirc(24, 27, 17, IC_LINE);
        icirc(24, 27, 13, IC_LT);
        ir(17, 8, 14, 20, IC_LT);
        ir(21, 6, 6, 22, IC_LINE);
        break;
    case IC_SPEAKER:
        ir(6, 18, 9, 12, IC_LINE);
        itri(15, 18, 28, 8, 28, 40, IC_LINE);
        itri(15, 30, 28, 40, 28, 18, IC_LINE);
        ir(32, 17, 3, 14, IC_DK);
        ir(37, 12, 3, 24, IC_DK);
        break;
    case IC_BACK:
        itri(6, 24, 22, 8, 22, 40, CL);
        ir(22, 18, 20, 12, CL);
        break;
    case IC_FORWARD:
        itri(42, 24, 26, 8, 26, 40, CL);
        ir(6, 18, 20, 12, CL);
        break;
    case IC_UP:
        itri(24, 6, 8, 22, 40, 22, CL);
        ir(18, 22, 12, 20, CL);
        break;
    case IC_VIEWS:
        ir(6, 6, 16, 16, IC_LINE);
        ir(26, 6, 16, 16, IC_LINE);
        ir(6, 26, 16, 16, IC_LINE);
        ir(26, 26, 16, 16, IC_LINE);
        ir(8, 8, 12, 12, IC_LT);
        ir(28, 8, 12, 12, IC_LT);
        ir(8, 28, 12, 12, IC_LT);
        ir(28, 28, 12, 12, IC_LT);
        break;
    default:
        break;
    }
}

static void draw_icon_dim(int type, int32_t x, int32_t y, int32_t size,
    bool dim)
{
    g_dim = dim;
    draw_icon(type, x, y, size);
    g_dim = false;
}

/* ====================================================================== */
/* ============================== Wallpaper ============================= */
/* ====================================================================== */

static void draw_wallpaper(void)
{
    if (wall_style == 2) {
        int32_t y;
        for (y = 0; y < tb_y; y += 4) {
            int32_t bh = (y + 4 <= tb_y) ? 4 : tb_y - y;
            if (y + bh <= clip.y0 || y >= clip.y1) continue;
            rect(0, y, sw, bh,
                mix_rgb(TH.desk_hi, TH.desktop, (y * 100) / tb_y));
        }
    } else {
        rect(0, 0, sw, tb_y, TH.desktop);
    }
}

/* ====================================================================== */
/* =============================== Drawing ============================== */
/* ====================================================================== */

/* Desktop shortcuts use 16x16 transparent pixel sprites. These are
 * original RockOS interpretations of the CC0 pixel-icon aesthetic from
 * Telex Operator's Pixel-Art Icons pack; no Windows icon bitmaps are copied.
 * '.' is transparent, so the desktop wallpaper shows through. */
static const char* const desktop_pixel_icons[5][16] = {
    { /* My Computer: chunky CRT monitor */
        "................", "....########....", "...#llllllll#...", "...#lmmmmmm#....",
        "...#lmmmmmm#....", "...#lmmmmmm#....", "...#llllllll#...", "....########....",
        ".......##.......", ".......##.......", "....########....", "...#llllllll#...",
        "...#lmmmmmm#....", "...#lmmmmmm#....", "....########....", "................"
    },
    { /* My Documents: folder with a sheet */
        "................", "....######......", "...#llllll#.....",
        "...##########...", "...#llllllll#...", "..#llllllllll#..",
        "..#ll######ll#..", "..#ll#wwww#ll#..", "..#ll#wwww#ll#..",
        "..#ll#wwww#ll#..", "..#ll#wwww#ll#..", "..#ll#wwww#ll#..",
        "..#ll######ll#..", "...#llllllll#...", "...##########...", "................"
    },
    { /* Network: linked nodes */
        "................", "..########......", ".#llllllll#.....",
        ".#lmmmmmm#......", ".#lmmmmmm#......", ".#llllllll#.....",
        "..########......", "......##........", "....######......",
        "....#llll#......", "....#llll#......", "....######......",
        "........##......", "......########..", ".....#llllllll#.",
        "......########.."
    },
    { /* Recycle Bin: lid, body and paper slats */
        "................", ".....######.....", "....#llllll#....",
        "......####......", ".......##.......", "...##########...",
        "...#llllllll#...", "...#lmmmmmm#....", "...#l#ll#ll#....",
        "...#l#ll#ll#....", "...#l#ll#ll#....", "...#l#ll#ll#....",
        "...#llllllll#...", "....########....", "................", "................"
    },
    { /* Command Prompt: pixel terminal */
        "................", "..############..", "..#llllllllll#..",
        "..############..", "..#dddddddddd#..", "..#d##d.......#..",
        "..#d##d.......#..", "..#ddd#########..", "..#............#.",
        "..#............#.", "..#............#.", "..#............#.",
        "..############..", "................", "................", "................"
    }
};

static uint32_t desktop_pixel_colour(char pixel)
{
    switch (pixel) {
    case '#': return IC_LINE;
    case 'd': return IC_DK;
    case 'm': return IC_MD;
    case 'l': return IC_LT;
    case 'w': return IC_WH;
    default:  return 0;
    }
}

static void draw_desktop_pixel_icon(int type, int32_t x, int32_t y,
    int32_t size)
{
    int sprite = -1;
    int row, col;

    switch (type) {
    case IC_COMPUTER: sprite = 0; break;
    case IC_DOCS:     sprite = 1; break;
    case IC_NETWORK:  sprite = 2; break;
    case IC_RECYCLE:  sprite = 3; break;
    case IC_CMD:      sprite = 4; break;
    default:
        draw_icon(type, x, y, size);
        return;
    }

    for (row = 0; row < 16; row++) {
        for (col = 0; col < 16; col++) {
            char pixel = desktop_pixel_icons[sprite][row][col];
            uint32_t colour = desktop_pixel_colour(pixel);
            int32_t x0, y0, x1, y1;
            if (pixel == '.' || colour == 0) continue;
            x0 = x + (col * size) / 16;
            y0 = y + (row * size) / 16;
            x1 = x + ((col + 1) * size) / 16;
            y1 = y + ((row + 1) * size) / 16;
            if (x1 > x0 && y1 > y0)
                rect(x0, y0, x1 - x0, y1 - y0, colour);
        }
    }
}

static void draw_icons(void)
{
    int i;
    for (i = 0; i < ICON_COUNT; i++) {
        int32_t x = icon_x(), y = icon_y(i);
        bool sel = (sel_icon == i);
        int32_t ix = x + (ICON_TILE_W - 32) / 2;
        int32_t l1w, l2w, l1x, l2x;
        if (!box_touch(&clip, x, y, ICON_TILE_W, ICON_TILE_H)) continue;

        draw_desktop_pixel_icon(desk_icon_ic[i], ix, y + 6, 32);
        if (sel) {
            display_blend_rect_rgb((uint32_t)(ix - 2), (uint32_t)(y + 4), 36, 36,
                (uint8_t)(TH.select_bg >> 16), (uint8_t)(TH.select_bg >> 8),
                (uint8_t)TH.select_bg, 110);
        }

        l1w = tw(desk_label1[i]);
        l1x = x + (ICON_TILE_W - l1w) / 2;
        if (l1x < x) l1x = x;
        if (sel) rect(l1x - 2, y + 44, l1w + 4, 20, TH.select_bg);
        text(l1x, y + 44, desk_label1[i], sel ? TH.t_sel : TH.t_desk);
        if (desk_label2[i][0]) {
            l2w = tw(desk_label2[i]);
            l2x = x + (ICON_TILE_W - l2w) / 2;
            if (l2x < x) l2x = x;
            if (sel) rect(l2x - 2, y + 64, l2w + 4, 20, TH.select_bg);
            text(l2x, y + 64, desk_label2[i], sel ? TH.t_sel : TH.t_desk);
        }
    }
}

static void cap_button(int32_t bx, int32_t by, int kind)
{
    uint32_t gl = TH.glyph;
    int i;
    button_box(bx, by, CAP_W, CAP_H, false);
    if (kind == 0) {                         /* minimize */
        rect(bx + 6, by + CAP_H - 7, 9, 3, gl);
    } else if (kind == 1) {                  /* maximize */
        rect(bx + 5, by + 4, 12, 3, gl);
        rect(bx + 5, by + 4, 1, 10, gl);
        rect(bx + 16, by + 4, 1, 10, gl);
        rect(bx + 5, by + 13, 12, 1, gl);
    } else if (kind == 3) {                  /* restore */
        rect(bx + 8, by + 4, 9, 2, gl);
        rect(bx + 8, by + 4, 1, 6, gl);
        rect(bx + 16, by + 4, 1, 6, gl);
        rect(bx + 5, by + 8, 9, 2, gl);
        rect(bx + 5, by + 8, 1, 6, gl);
        rect(bx + 13, by + 8, 1, 6, gl);
        rect(bx + 5, by + 13, 9, 1, gl);
    } else {                                 /* close */
        for (i = 0; i < 8; i++) {
            rect(bx + 6 + i, by + 5 + i, 2, 2, gl);
            rect(bx + 13 - i, by + 5 + i, 2, 2, gl);
        }
    }
}

static void draw_computer(int w, int32_t wx, int32_t wy)
{
    fmg_t g;
    int32_t ww = wins[w].w;
    int32_t cx = wx + C0X;
    int32_t cw = ww - 2 * FRAME;
    int32_t i;
    char buf[160];
    char* p;

    fm_geom(w, &g);

    /* ---- menu bar ---- */
    for (i = 0; i < 6; i++) {
        int32_t mx0 = wx + fm_menu_x((int)i);
        bool hot = hover.kind == R_FM_MENU && hover.idx == w && hover.sub == i;
        if (hot) bevel_raised(mx0, wy + g.menu_y + 1, fm_menu_widths[i],
            FM_MENU_H - 2);
        text(mx0 + 10, wy + g.menu_y + 3, fm_menu_labels[i], TH.t_face);
    }
    rect(cx, wy + g.tool_y - 1, cw, 1, TH.shadow);

    /* ---- toolbar ---- */
    {
        bool can_back = hist_pos > 0;
        bool can_fwd = hist_pos + 1 < hist_len;
        bool can_up = !fm_computer;
        for (i = 0; i < 5; i++) {
            int32_t bx, bw;
            bool enabled;
            bool hot = hover.kind == R_FM_BTN && hover.idx == w &&
                hover.sub == i;
            fm_btn_geom((int)i, &bx, &bw);
            enabled = (i == 0 && can_back) || (i == 1 && can_fwd) ||
                (i == 2 && can_up);
            if (hot && enabled) bevel_raised(wx + bx, wy + g.tool_y + 4, bw, 36);
            if (i == 0) {
                draw_icon_dim(IC_BACK, wx + bx + 8, wy + g.tool_y + 12, 20,
                    !enabled);
                text(wx + bx + 34, wy + g.tool_y + 12, "Back",
                    enabled ? TH.t_face : TH.t_dim);
            } else if (i == 1) {
                draw_icon_dim(IC_FORWARD, wx + bx + 10, wy + g.tool_y + 12, 20,
                    !enabled);
            } else if (i == 2) {
                draw_icon_dim(IC_UP, wx + bx + 10, wy + g.tool_y + 12, 20,
                    !enabled);
            } else if (i == 3) {
                draw_icon_dim(IC_SEARCH, wx + bx + 8, wy + g.tool_y + 12, 20,
                    true);
                text(wx + bx + 34, wy + g.tool_y + 12, "Search", TH.t_dim);
            } else {
                draw_icon_dim(IC_FOLDER, wx + bx + 8, wy + g.tool_y + 12, 20,
                    true);
                text(wx + bx + 34, wy + g.tool_y + 12, "Folders", TH.t_dim);
            }
        }
        rect(wx + C0X + 94 + 1, wy + g.tool_y + 6, 1, 32, TH.shadow);
        rect(wx + C0X + 186, wy + g.tool_y + 6, 1, 32, TH.shadow);
        rect(wx + C0X + 186 + 1, wy + g.tool_y + 6, 1, 32, TH.hilite);
        draw_icon(IC_VIEWS, wx + C0X + cw - 40, wy + g.tool_y + 12, 20);
    }
    rect(cx, wy + g.addr_y - 1, cw, 1, TH.shadow);
    rect(cx, wy + g.addr_y, cw, 1, TH.hilite);

    /* ---- address bar ---- */
    text(cx + 8, wy + g.addr_y + 6, "Address", TH.t_face);
    {
        int32_t ax = cx + 84;
        int32_t aw = cw - 84 - 92;
        rect(ax, wy + g.addr_y + 3, aw, 26, TH.window_bg);
        bevel_sunken(ax, wy + g.addr_y + 3, aw, 26);
        draw_icon(fm_computer ? IC_COMPUTER : IC_FOLDER, ax + 4,
            wy + g.addr_y + 7, 18);
        if (fm_computer) {
            put_str(buf, "My Computer");
        } else {
            char* d = put_str(buf, "C:");
            const char* s = fm_path;
            if (s[0] == '/' && s[1] == '\0') {
                *d++ = '\\';
                *d = '\0';
            } else {
                while (*s && d < buf + sizeof(buf) - 2) {
                    *d++ = (*s == '/') ? '\\' : *s;
                    s++;
                }
                *d = '\0';
            }
        }
        text(ax + 28, wy + g.addr_y + 6, buf, TH.t_win);
        button_box(ax + aw - 24, wy + g.addr_y + 5, 20, 22, false);
        tri_rgb(ax + aw - 18, wy + g.addr_y + 14, ax + aw - 8, wy + g.addr_y + 14,
            ax + aw - 13, wy + g.addr_y + 19, TH.glyph);
        button_box(cx + cw - 86, wy + g.addr_y + 3, 78, 26, false);
        tri_rgb(cx + cw - 76, wy + g.addr_y + 10, cx + cw - 76,
            wy + g.addr_y + 22, cx + cw - 66, wy + g.addr_y + 16, TH.glyph);
        text(cx + cw - 58, wy + g.addr_y + 6, "Go", TH.t_face);
    }

    /* ---- list pane ---- */
    {
        int32_t px = cx + 2, py = wy + g.list_y, pw = cw - 4, ph = g.list_h;
        rect(px, py, pw, ph, TH.window_bg);
        bevel_sunken(px, py, pw, ph);

        if (fm_computer) {
            text(px + 12, py + 8, "Hard Disk Drives", TH.t_win);
            rect(px + 8, py + 34, pw - 16, 1, TH.shadow);
            if (filesystem_is_mounted()) {
                filesystem_stats_t st;
                int32_t dx = px + 10, dy = py + 44;
                if (fm_drive_sel) rect(dx, dy, 320, 54, TH.select_bg);
                draw_icon(IC_DISK, dx + 8, dy + 7, 40);
                text(dx + 62, dy + 5, "Local Disk (C:)",
                    fm_drive_sel ? TH.t_sel : TH.t_win);
                if (filesystem_get_stats(&st) == FS_OK) {
                    p = put_capacity(buf, (uint64_t)st.free_sectors * 512);
                    p = put_str(p, " free of ");
                    put_capacity(p, (uint64_t)st.total_sectors * 512);
                } else {
                    put_str(buf, "RockFS volume");
                }
                text(dx + 62, dy + 28, buf, fm_drive_sel ? TH.t_sel : TH.t_dim);
            } else {
                text(px + 24, py + 48, "No drives were found.", TH.t_dim);
            }
        } else {
            int r;
            for (r = 0; r < g.rows; r++) {
                int idx = fm_scroll + r;
                int32_t ry = py + 2 + r * FM_ROW_H;
                bool sel;
                if (idx >= fm_count) break;
                sel = (idx == fm_sel);
                if (sel) rect(px + 2, ry, pw - 4, FM_ROW_H, TH.select_bg);
                draw_icon(fm_ents[idx].type == FS_TYPE_DIR ? IC_FOLDER : IC_FILE,
                    px + 8, ry + 3, 20);
                text(px + 36, ry + 3, fm_ents[idx].name,
                    sel ? TH.t_sel : TH.t_win);
                if (fm_ents[idx].type != FS_TYPE_DIR) {
                    put_size(buf, fm_ents[idx].size);
                    text(px + pw - 120, ry + 3, buf, sel ? TH.t_sel : TH.t_dim);
                }
            }
            if (fm_err != FS_OK) {
                p = put_str(buf, "error: ");
                copy_str(p, filesystem_strerror(fm_err), sizeof(buf) - 7);
                text(px + 12, py + 8, buf, TH.t_dim);
            } else if (fm_count == 0) {
                text(px + 12, py + 8, "This folder is empty.", TH.t_dim);
            }
        }
    }

    /* ---- status bar ---- */
    {
        int32_t sy = wy + g.status_y;
        int count = fm_computer ? (filesystem_is_mounted() ? 1 : 0) : fm_count;
        rect(cx, sy, cw, FM_STATUS_H, TH.face);
        bevel_sunken(cx + 2, sy + 1, cw - 4, FM_STATUS_H - 2);
        p = put_u(buf, (uint64_t)count);
        put_str(p, count == 1 ? " object" : " objects");
        text(cx + 10, sy + 3, buf, TH.t_face);
    }
}

static void draw_terminal(int w, int32_t wx, int32_t wy)
{
    int32_t cx = wx + C0X, cy = wy + C0Y;
    int32_t cw = wins[w].w - 2 * FRAME;
    int32_t ch = wins[w].h - C0Y - FRAME;
    int rows = term_rows(w);
    int first = term_count > rows ? term_count - rows : 0;
    int i;
    char prompt[FS_PATH_MAX + TERM_CMD_MAX + 8];
    char* p;

    rect(cx, cy, cw, ch, 0x0C0C0C);
    bevel_sunken(cx, cy, cw, ch);
    for (i = first; i < term_count; i++) {
        text(cx + 10, cy + 8 + (i - first) * TERM_ROW_H, term_lines[i],
            COLOR_LIGHT_GRAY);
    }
    p = put_str(prompt, term_cwd);
    p = put_str(p, "> ");
    p = put_str(p, term_cmd);
    put_str(p, "_");
    text(cx + 10, cy + ch - 32, prompt, COLOR_WHITE);
}

static void draw_sysinfo(int w, int32_t wx, int32_t wy, uint64_t ticks)
{
    int32_t cx = wx + C0X, cy = wy + C0Y;
    int32_t cw = wins[w].w - 2 * FRAME;
    int32_t ch = wins[w].h - C0Y - FRAME;
    int32_t bx = cx + 22, by = cy + 18;
    uint32_t fw = 0, fh = 0;
    pmm_stats_t ps;
    filesystem_stats_t fs;
    char line[110];
    char* p;

    display_get_framebuffer_size(&fw, &fh);
    pmm_get_stats(&ps);

    rect(cx + 8, cy + 8, cw - 16, ch - 16, TH.window_bg);
    bevel_sunken(cx + 8, cy + 8, cw - 16, ch - 16);

    text(bx, by, "RockOS V.2  [ROK kernel]", TH.t_win);

    p = put_str(line, "Display: ");
    p = put_u(p, fw);
    p = put_str(p, " x ");
    put_u(p, fh);
    text(bx, by + 30, line, TH.t_win);

    p = put_str(line, "Framebuffer: ");
    copy_str(p, display_framebuffer_status(), sizeof(line) - 13);
    text(bx, by + 56, line, TH.t_win);

    p = put_str(line, "Memory: ");
    p = put_u(p, ps.total_bytes >> 20);
    p = put_str(p, " MiB total, ");
    p = put_u(p, ps.free_bytes >> 20);
    put_str(p, " MiB free");
    text(bx, by + 82, line, TH.t_win);

    if (filesystem_is_mounted() && filesystem_get_stats(&fs) == FS_OK) {
        p = put_str(line, "RockFS: ");
        p = put_u(p, fs.free_sectors);
        p = put_str(p, " of ");
        p = put_u(p, fs.total_sectors);
        put_str(p, " sectors free");
    } else {
        put_str(line, "RockFS: not mounted");
    }
    text(bx, by + 108, line, TH.t_win);

    p = put_str(line, "Uptime: ");
    put_uptime(p, ticks);
    text(bx, by + 134, line, TH.t_win);

    p = put_str(line, "Timer ticks: ");
    put_u(p, ticks);
    text(bx, by + 160, line, TH.t_dim);
}

/* One colour-scheme / wallpaper preview card. */
static void draw_card(int32_t x, int32_t y, const theme_t* t, bool selected,
    const char* label, int style)
{
    if (selected) {
        rect(x - 3, y - 3, SET_CARD_W + 6, SET_CARD_H + 6, TH.select_bg);
        rect(x - 1, y - 1, SET_CARD_W + 2, SET_CARD_H + 2, TH.face);
    }
    rect(x, y, SET_CARD_W, 52, t->desktop);
    if (style < 0) {
        rect(x + 14, y + 7, 70, 32, t->face);
        gradient_h(x + 16, y + 9, 66, 7, t->title_a0, t->title_a1);
        rect(x + 16, y + 20, 66, 16, t->window_bg);
        rect(x, y + 44, SET_CARD_W, 8, t->face);
        rect(x + 3, y + 46, 22, 4, t->title_a0);
    } else if (style == 0) {
        draw_icon(IC_ABOUT, x + SET_CARD_W / 2 - 14, y + 10, 28);
    } else if (style == 2) {
        rect(x, y, SET_CARD_W, 26, t->desk_hi);
        rect(x, y + 26, SET_CARD_W, 26, t->desktop);
        rect(x, y + 13, SET_CARD_W, 13, mix_rgb(t->desk_hi, t->desktop, 33));
        rect(x, y + 26, SET_CARD_W, 13, mix_rgb(t->desk_hi, t->desktop, 66));
    }
    bevel_sunken(x, y, SET_CARD_W, 52);
    text(x + 2, y + 52, label, TH.t_face);
}

static void draw_display(int w, int32_t wx, int32_t wy)
{
    int i;
    int32_t cx = wx + C0X;
    int32_t cw = wins[w].w - 2 * FRAME;
    int32_t hh = wins[w].h;

    groupbox(wx + SET_GX, wy + SET_G1Y, cw - 20, SET_GH, "Color scheme");
    for (i = 0; i < THEME_COUNT; i++) {
        draw_card(wx + SET_GX + 12 + i * SET_STEP, wy + SET_G1Y + 26, &themes[i],
            i == theme_index, themes[i].name, -1);
    }
    groupbox(wx + SET_GX, wy + SET_G2Y, cw - 20, SET_GH, "Wallpaper");
    for (i = 0; i < WALL_STYLES; i++) {
        draw_card(wx + SET_GX + 12 + i * SET_STEP, wy + SET_G2Y + 26, &TH,
            i == wall_style, wall_names[i], i);
    }
    text(cx + 14, wy + hh - FRAME - 32,
        "Changes apply immediately. Ctrl+Alt+T cycles schemes.", TH.t_dim);
}

static void ok_button(int w, int32_t wx, int32_t wy)
{
    int32_t bx = wx + wins[w].w - FRAME - 100;
    int32_t by = wy + wins[w].h - FRAME - 40;
    button_box(bx, by, 88, 28, false);
    rect(bx + 3, by + 3, 82, 22, TH.face);
    text(bx + 32, by + 4, "OK", TH.t_face);
    rect(bx + 2, by + 2, 84, 1, TH.dkshadow);
    rect(bx + 2, by + 25, 84, 1, TH.dkshadow);
    rect(bx + 2, by + 2, 1, 24, TH.dkshadow);
    rect(bx + 85, by + 2, 1, 24, TH.dkshadow);
}

static void draw_message(int w, int32_t wx, int32_t wy)
{
    int32_t cx = wx + C0X, cy = wy + C0Y;
    if (w == W_ABOUT) {
        draw_icon(IC_ABOUT, cx + 22, cy + 26, 72);
        text(cx + 116, cy + 26, "RockOS V.2", TH.t_face);
        text(cx + 116, cy + 52, "Classic desktop, 32-bit and 64-bit.", TH.t_face);
        text(cx + 116, cy + 76, "Built on the ROK kernel.", TH.t_face);
        text(cx + 116, cy + 112, "Ctrl+Alt+D  toggle the desktop", TH.t_dim);
        text(cx + 116, cy + 134, "Ctrl+Alt+Q  exit to the shell", TH.t_dim);
        text(cx + 116, cy + 156, "Ctrl+Alt+T  next color scheme", TH.t_dim);
    } else if (w == W_NETWORK) {
        draw_icon(IC_NETWORK, cx + 24, cy + 30, 56);
        text(cx + 108, cy + 36, "No network adapters were found.", TH.t_face);
        text(cx + 108, cy + 62, "RockOS V.2 has no networking yet.", TH.t_dim);
    } else {
        draw_icon(IC_RECYCLE, cx + 24, cy + 26, 56);
        text(cx + 108, cy + 40, "The Recycle Bin is empty.", TH.t_face);
    }
    ok_button(w, wx, wy);
}

static void draw_window(int w, uint64_t ticks)
{
    int32_t x = wins[w].x, y = wins[w].y, ww = wins[w].w, hh = wins[w].h;
    bool act = (focused == w);
    int32_t tx = x + FRAME, ty = y + FRAME, twd = ww - 2 * FRAME;
    int32_t tr = tx + twd;
    int32_t cx_close = tr - 2 - CAP_W;
    int32_t cx_max = cx_close - 2 - CAP_W;
    int32_t cx_min = cx_max - CAP_W;
    int32_t by = ty + 3;

    rect(x, y, ww, hh, TH.face);
    bevel_raised(x, y, ww, hh);

    gradient_h(tx, ty, twd, TITLE_H, act ? TH.title_a0 : TH.title_i0,
        act ? TH.title_a1 : TH.title_i1);
    draw_icon(win_icon[w], tx + 4, ty + 4, 18);
    text(tx + 28, ty + (TITLE_H - (int32_t)DISPLAY_UI_TEXT_HEIGHT) / 2,
        win_title[w], act ? TH.t_title : TH.t_title_idle);

    cap_button(cx_min, by, 0);
    cap_button(cx_max, by, wins[w].maximized ? 3 : 1);
    cap_button(cx_close, by, 2);

    switch (w) {
    case W_COMPUTER: draw_computer(w, x, y); break;
    case W_TERM:     draw_terminal(w, x, y); break;
    case W_SYSINFO:  draw_sysinfo(w, x, y, ticks); break;
    case W_DISPLAY:  draw_display(w, x, y); break;
    default:         draw_message(w, x, y); break;
    }
}

static void draw_taskbar(void)
{
    int i, k = 0;
    char buf[32];
    int32_t bw = task_btn_w();
    int32_t tray_x = sw - TRAY_W - 4;

    rect(0, tb_y, sw, TASKBAR_H, TH.face);
    rect(0, tb_y, sw, 1, TH.hilite);

    /* Start button */
    button_box(4, tb_y + 5, START_W, 30, menu_open);
    draw_icon(IC_ABOUT, 4 + 9 + (menu_open ? 1 : 0),
        tb_y + 9 + (menu_open ? 1 : 0), 22);
    text(4 + 38 + (menu_open ? 1 : 0), tb_y + 10 + (menu_open ? 1 : 0), "Start",
        TH.t_face);

    /* grip */
    rect(START_W + 9, tb_y + 6, 1, 28, TH.shadow);
    rect(START_W + 10, tb_y + 6, 1, 28, TH.hilite);

    /* task buttons */
    for (i = 0; i < W_TYPES; i++) {
        int32_t x;
        bool is_active;
        int32_t maxc, n;
        if (!wins[i].open) continue;
        x = task_btn_x(k++);
        is_active = (focused == i && !wins[i].minimized);
        button_box(x, tb_y + 5, bw, 30, is_active);
        if (is_active) rect(x + 3, tb_y + 8, bw - 6, 24, mix_rgb(TH.face,
            TH.hilite, 35));
        draw_icon(win_icon[i], x + 7, tb_y + 11, 18);
        maxc = (bw - 36) / 9;
        if (maxc < 1) maxc = 1;
        for (n = 0; n < maxc && win_title[i][n] && n < 30; n++) buf[n] = win_title[i][n];
        buf[n] = '\0';
        text(x + 30, tb_y + 10, buf, TH.t_face);
    }

    /* tray */
    rect(tray_x, tb_y + 5, TRAY_W, 30, TH.face);
    bevel_sunken(tray_x, tb_y + 5, TRAY_W, 30);
    draw_icon(IC_SPEAKER, tray_x + 8, tb_y + 11, 18);
    clock_text(buf);
    text(tray_x + TRAY_W - 8 - tw(buf), tb_y + 10, buf, TH.t_face);
}

static void draw_menu_row(int32_t x, int32_t y, int32_t w, int icon,
    const char* label, bool hot, bool arrow)
{
    if (hot) rect(x, y, w, MENU_ITEM_H - 2, TH.select_bg);
    draw_icon(icon, x + 8, y + 5, 22);
    text(x + 38, y + (MENU_ITEM_H - 2 - (int32_t)DISPLAY_UI_TEXT_HEIGHT) / 2,
        label, hot ? TH.t_sel : TH.t_face);
    if (arrow) {
        uint32_t ac = hot ? 0xFFFFFF : TH.glyph;
        tri_rgb(x + w - 16, y + 10, x + w - 16, y + 22, x + w - 10, y + 16, ac);
    }
}

static void draw_menu(void)
{
    int i;
    int32_t top = menu_top();
    int32_t th = menu_total_h();
    int32_t rx = BANNER_W + 4;
    int32_t rw = MENU_W - rx - 4;
    const char* banner = "RockOS";
    int32_t by0;

    rect(0, top, MENU_W, th, TH.face);
    bevel_raised(0, top, MENU_W, th);

    gradient_v(3, top + 3, BANNER_W, th - 6, TH.banner0, TH.banner1);
    by0 = top + th - 6 - 6 * 20 - 8;
    for (i = 0; i < 6; i++) {
        char one[2];
        one[0] = banner[i];
        one[1] = '\0';
        text(3 + 11, by0 + i * 20, one, COLOR_WHITE);
    }

    for (i = 0; i < MENU_COUNT; i++) {
        int32_t ry = menu_row_y(i);
        if (!menu_items[i].label) {
            rect(rx, ry + 4, rw, 1, TH.shadow);
            rect(rx, ry + 5, rw, 1, TH.hilite);
            continue;
        }
        draw_menu_row(rx, ry, rw, menu_items[i].icon, menu_items[i].label,
            (hover.kind == R_MENUITEM && hover.idx == i) || sub_open == i,
            menu_items[i].sub_count > 0);
    }

    if (sub_open >= 0) {
        int32_t sx, sy, swd, shg;
        int k;
        const menu_item_t* it = &menu_items[sub_open];
        sub_geom(sub_open, &sx, &sy, &swd, &shg);
        rect(sx, sy, swd, shg, TH.face);
        bevel_raised(sx, sy, swd, shg);
        for (k = 0; k < it->sub_count; k++) {
            uint8_t act = it->subs[k].action;
            int icon = IC_FOLDER;
            if (act == A_COMPUTER) icon = IC_COMPUTER;
            else if (act == A_DOCS) icon = IC_DOCS;
            else if (act == A_TERM) icon = IC_CMD;
            else if (act == A_SYSINFO) icon = IC_SYSINFO;
            else if (act == A_DISPLAY) icon = IC_DISPLAY;
            draw_menu_row(sx + 4, sy + 4 + k * MENU_ITEM_H, swd - 8, icon,
                it->subs[k].label,
                hover.kind == R_SUBITEM && hover.idx == sub_open &&
                    hover.sub == k, false);
        }
    }
}

static void draw_all(uint64_t ticks)
{
    int i;

    draw_wallpaper();

    if (logo_visible() && box_touch(&clip, logo_x(), logo_y(),
        (int32_t)DISPLAY_ROCKOS_LOGO_WIDTH,
        (int32_t)DISPLAY_ROCKOS_LOGO_HEIGHT)) {
        display_draw_rockos_logo((uint32_t)logo_x(), (uint32_t)logo_y());
    }

    draw_icons();
    for (i = 0; i < zcount; i++) {
        int w = zorder[i];
        if (wins[w].minimized) continue;
        if (!box_touch(&clip, wins[w].x, wins[w].y, wins[w].w, wins[w].h)) {
            continue;
        }
        draw_window(w, ticks);
    }
    if (box_touch(&clip, 0, tb_y, sw, TASKBAR_H)) draw_taskbar();
    if (menu_open) {
        box_t b;
        menu_bbox(&b);
        if (box_touch(&clip, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0)) {
            draw_menu();
        }
    }
}

/* ====================================================================== */
/* ============================ Input handling ========================== */
/* ====================================================================== */

static void on_press(int32_t x, int32_t y)
{
    region_t r = hit_test(x, y);
    bool was_menu = menu_open;
    uint64_t now = timer_get_ticks();

    if (r.kind != R_START && r.kind != R_MENUITEM && r.kind != R_SUBITEM) {
        menu_open = false;
        sub_open = -1;
    }

    switch (r.kind) {
    case R_START:
        menu_open = !was_menu;
        sub_open = -1;
        break;
    case R_MENUITEM:
        if (r.idx >= 0) {
            const menu_item_t* it = &menu_items[r.idx];
            if (it->sub_count) {
                sub_open = (sub_open == r.idx) ? -1 : r.idx;
            } else {
                menu_open = false;
                sub_open = -1;
                run_action(it->action);
                if (!active) return;
            }
        }
        break;
    case R_SUBITEM:
        if (r.sub >= 0) {
            uint8_t act = menu_items[r.idx].subs[r.sub].action;
            menu_open = false;
            sub_open = -1;
            run_action(act);
            if (!active) return;
        }
        break;
    case R_TASKBTN:
        if (wins[r.idx].minimized) win_focus(r.idx);
        else if (focused == r.idx) win_minimize(r.idx);
        else win_focus(r.idx);
        break;
    case R_ICON:
        if (last_click_icon == r.idx &&
            (now - last_click_tick) <= DBLCLICK_TICKS) {
            last_click_icon = -1;
            sel_icon = r.idx;
            run_action(desk_icon_act[r.idx]);
        } else {
            last_click_icon = r.idx;
            last_click_tick = now;
            sel_icon = r.idx;
        }
        break;
    case R_WTITLE:
        win_focus(r.idx);
        if (last_title_win == r.idx && (now - last_title_tick) <= DBLCLICK_TICKS) {
            win_toggle_max(r.idx);
            last_title_win = -1;
        } else {
            last_title_win = r.idx;
            last_title_tick = now;
            if (!wins[r.idx].maximized) {
                drag_win = r.idx;
                drag_dx = x - wins[r.idx].x;
                drag_dy = y - wins[r.idx].y;
            }
        }
        break;
    case R_WBODY:
    case R_FM_MENU:
        win_focus(r.idx);
        break;
    case R_FM_BTN:
        win_focus(r.idx);
        if (r.sub == 0) fm_back();
        else if (r.sub == 1) fm_forward();
        else if (r.sub == 2) fm_up();
        break;
    case R_FM_DRIVE:
        win_focus(r.idx);
        if (fm_last_click_entry == -2 &&
            (now - fm_last_click_tick) <= DBLCLICK_TICKS) {
            fm_drive_sel = true;
            fm_open_selected();
            fm_last_click_entry = -1;
        } else {
            fm_drive_sel = true;
            fm_last_click_entry = -2;
            fm_last_click_tick = now;
        }
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
    case R_SET_THEME:
        win_focus(r.idx);
        theme_index = r.sub;
        break;
    case R_SET_WALL:
        win_focus(r.idx);
        wall_style = r.sub;
        break;
    case R_OK:
        win_close(r.idx);
        break;
    case R_WCLOSE:
        win_close(r.idx);
        break;
    case R_WMAX:
        win_focus(r.idx);
        win_toggle_max(r.idx);
        break;
    case R_WMIN:
        win_minimize(r.idx);
        break;
    default:
        if (y < tb_y) sel_icon = -1;
        break;
    }

    hover = hit_test(x, y);
    damage_all();
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
    if (!display_get_framebuffer_size(&w, &h) || w < 800 || h < 500) return;

    sw = (int32_t)w;
    sh = (int32_t)h;
    tb_y = sh - TASKBAR_H;

    for (i = 0; i < W_TYPES; i++) {
        wins[i].open = false;
        wins[i].minimized = false;
        wins[i].maximized = false;
    }
    zcount = 0;
    focused = -1;
    menu_open = false;
    sub_open = -1;
    sel_icon = -1;
    last_click_icon = -1;
    last_title_win = -1;
    drag_win = -1;
    prev_left = false;
    mx = sw / 2;
    my = sh / 2;
    hover.kind = R_NONE;
    hover.idx = 0;
    hover.sub = 0;
    last_sec = UINT64_MAX;

    clip.x0 = 0; clip.y0 = 0; clip.x1 = sw; clip.y1 = sh;
    dmg_on = false;

    fm_computer = true;
    fm_count = 0;
    fm_sel = 0;
    fm_scroll = 0;
    fm_err = FS_OK;
    hist_len = 0;
    hist_pos = -1;
    term_count = 0;
    term_len = 0;
    term_cmd[0] = '\0';
    copy_str(term_cwd, "/", sizeof(term_cwd));

    clock_poll();
    win_open(W_COMPUTER);

    active = true;
    damage_all();
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
            damage_window(drag_win);
            wins[drag_win].x = clampi(x - drag_dx, 0, sw - wins[drag_win].w);
            wins[drag_win].y = clampi(y - drag_dy, 0, tb_y - TITLE_H);
            damage_window(drag_win);
        } else {
            region_t hv = hit_test(x, y);
            if (!region_eq(hv, hover)) {
                damage_region(hover);
                hover = hv;
                damage_region(hover);
                /* cascading menu follows the pointer */
                if (menu_open && hv.kind == R_MENUITEM && hv.idx >= 0) {
                    int want = menu_items[hv.idx].sub_count ? hv.idx : -1;
                    if (want != sub_open) {
                        damage_menu();
                        sub_open = want;
                        damage_menu();
                    }
                }
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

    /* Ctrl+Alt+T: next colour scheme. */
    if (ctrl_alt && !e->is_extended &&
        (e->ascii == 't' || e->ascii == 'T' || e->scancode == SC_T)) {
        theme_index = (theme_index + 1) % THEME_COUNT;
        damage_all();
        return;
    }

    if (e->scancode == SC_ESC && !e->is_extended && menu_open) {
        menu_open = false;
        sub_open = -1;
        damage_all();
        return;
    }

    if ((e->modifiers & MOD_ALT) && e->scancode == SC_F4 && focused >= 0) {
        win_close(focused);
        damage_all();
        return;
    }

    if (e->modifiers & (MOD_CTRL | MOD_ALT)) return;

    if (focused == W_TERM && !e->is_extended) term_key(e);
    else if (focused == W_COMPUTER) fm_key(e);
}

void desktop_update(uint64_t ticks)
{
    uint64_t sec;

    if (!active) return;

    sec = ticks / DESKTOP_TIMER_HZ;
    if (sec != last_sec) {
        last_sec = sec;
        if (clock_poll()) damage_taskbar();
        if (wins[W_SYSINFO].open && !wins[W_SYSINFO].minimized) {
            damage_window(W_SYSINFO);
        }
    }
    if (!dmg_on) return;

    damage_close();
    clip = dmg;
    dmg_on = false;
    draw_all(ticks);
    clip.x0 = 0; clip.y0 = 0; clip.x1 = sw; clip.y1 = sh;
}

bool desktop_is_launch_hotkey(const input_key_event_t* e)
{
    return e && e->is_pressed && !e->is_extended &&
        (e->modifiers & (MOD_CTRL | MOD_ALT)) == (MOD_CTRL | MOD_ALT) &&
        (e->ascii == 'd' || e->ascii == 'D' || e->scancode == SC_D);
}

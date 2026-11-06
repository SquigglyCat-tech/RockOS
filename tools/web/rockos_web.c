/*
 * rockos_web.c - RockOS Web demo (WASM).
 *
 * A canvas-running slice of the desktop: wallpaper, taskbar with
 * clock, Start menu slide, an About window with zoom rectangles and
 * the same 100 Hz-tick animation model as src/kernel/desktop.c,
 * driven at requestAnimationFrame. Real kernel lives in the ISO ISO;
 * this build presents the UI.
 *
 * Build with emscripten:
 *   emcc tools/web/rockos_web.c -O2 -sWASM=1 -sENVIRONMENT=web \
 *       -o web/rockos-web.js
 * Point a browser at web/index.html.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 800
#define SCREEN_H 520
#define TASK_H (int32_t)32
#define TITLE_H (int32_t)26
#define MENU_W (int32_t)190
#define MENU_ITEM_H (int32_t)30
#define MENU_TICKS 10
#define ZOOM_TICKS 12
#define WIN_W (int32_t)460
#define WIN_H (int32_t)220

typedef struct { bool active, grow; uint32_t tick;
    int32_t x, y, w, h; } zoom_s;

static uint32_t fb[SCREEN_W * SCREEN_H];
typedef struct { int32_t x0, y0, x1, y1; } box_t;

static int32_t wb_x, wb_y;
static bool wb_open, wb_min, wb_max;
static bool menu_open;
static uint32_t menu_anim;
static int32_t mx = SCREEN_W / 2, my = SCREEN_H / 2;
#ifdef __EMSCRIPTEN__
static bool drag_w;
static int32_t drag_dx, drag_dy;
#endif
static zoom_s zoom;

static const char* const menu_labels[] = {
    "About RockOS", "Command Prompt", "Network", "Recycle Bin",
    "Shut Down"
};
#define MENU_COUNT 5

enum {
    C_DESK = 0x56687A,
    C_TASK = 0x353F49,
    C_FACE = 0xC9D1D8,
    C_DK = 0x353F49,
    C_HI = 0xFFFFFF,
    C_SH = 0x7F8C98,
    C_B0 = 0x1E3146,
    C_B1 = 0x506E8E,
    C_MENU = 0xD6DBE1,
    C_SEL = 0x2F4A66,
    C_TITLE = 0x22364A,
    C_TITLE_I = 0x6E8294,
    C_TEXT = 0x1E1E1E
};

/* ----------------------- emscripten interop ----------------------- */

static void blit(void)
{
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var cv = document.getElementById('screen');
        var g = cv.getContext('2d');
        if (!Module.blit) {
            Module.blit = g.createImageData($2, $3);
            Module.buf = new Uint32Array(Module.blit.data.buffer);
        }
        Module.buf.set(Module.HEAPU32.subarray($0 >> 2,
            ($0 >> 2) + $2 * $3));
        g.putImageData(Module.blit, 0, 0);
    }, (uintptr_t)fb, 0, SCREEN_W, SCREEN_H);
#else
#endif
}

static void web_text(int32_t x, int32_t y, const char* s, uint32_t rgb,
    int px, bool bold)
{
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var g = document.getElementById('screen').getContext('2d');
        g.fillStyle = '#' + ($3 >>> 0).toString(16).padStart(6, '0');
        g.font = ($5 ? 'bold ' : '') + $4 + 'px sans-serif';
        g.textBaseline = 'top';
        g.fillText(UTF8ToString($0), $1, $2);
    }, s, x, y, rgb, px, bold);
#else
    (void)x; (void)y; (void)s; (void)rgb; (void)px; (void)bold;
#endif
}

static void web_clock(int32_t* h, int32_t* m)
{
#ifdef __EMSCRIPTEN__
    *h = EM_ASM_INT({ var d = new Date(); return d.getHours(); });
    *m = EM_ASM_INT({ var d = new Date(); return d.getMinutes(); });
#else
    *h = 12; *m = 34;
#endif
}

/* ---------------------------- primitives --------------------------- */

static void set(int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
    fb[y * SCREEN_W + x] = c;
}

static void fill(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c)
{
    int32_t cx, cy, x0 = x > 0 ? x : 0, y0 = y > 0 ? y : 0;
    int32_t x1 = x + w, y1 = y + h;
    if (w <= 0 || h <= 0) return;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    for (cy = y0; cy < y1; cy++) {
        for (cx = x0; cx < x1; cx++) fb[cy * SCREEN_W + cx] = c;
    }
}

static uint32_t mix(uint32_t a, uint32_t b, uint32_t t, uint32_t den)
{
    uint32_t r = ((a >> 16) & 0xFF) * (den - t) + ((b >> 16) & 0xFF) * t;
    uint32_t g = ((a >> 8) & 0xFF) * (den - t) + ((b >> 8) & 0xFF) * t;
    uint32_t bl = (a & 0xFF) * (den - t) + (b & 0xFF) * t;
    return ((r / den) << 16) | ((g / den) << 8) | (bl / den);
}

static void gradient_v(int32_t x, int32_t y, int32_t w, int32_t h,
    uint32_t a, uint32_t b)
{
    int32_t i;
    for (i = 0; i < h; i++) fill(x, y + i, w, 1,
        mix(a, b, (uint32_t)i, (uint32_t)(h > 0 ? h : 1)));
}

static void bevel(int32_t x, int32_t y, int32_t w, int32_t h, bool up)
{
    fill(x, y, w, 1, up ? C_HI : C_SH);
    fill(x, y, 1, h, up ? C_HI : C_SH);
    fill(x, y + h - 1, w, 1, up ? C_SH : C_HI);
    fill(x + w - 1, y, 1, h, up ? C_SH : C_HI);
    fill(x + 1, y + h - 2, w - 2, 1, C_DK);
    fill(x + w - 2, y + 1, 1, h - 2, C_DK);
}

static bool in(int x, int y, int32_t rx, int32_t ry, int32_t rw,
    int32_t rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static int32_t window_w(void) { return wb_max ? SCREEN_W : WIN_W; }
static int32_t window_h(void) { return wb_max ? SCREEN_H - TASK_H
    : WIN_H; }

/* ----------------------------- geometry ---------------------------- */

static int32_t menu_total_h(void) { return MENU_COUNT * MENU_ITEM_H + 8; }
static int32_t menu_top(void)
{
    return SCREEN_H - TASK_H - menu_total_h();
}
static int32_t menu_row_y(int i)
{
    return menu_top() + 4 + i * MENU_ITEM_H;
}

static void zoom_seeds(int32_t* x, int32_t* y, int32_t* w, int32_t* h)
{
    *x = wb_max ? 0 : wb_x;
    *y = wb_max ? 0 : wb_y;
    *w = window_w();
    *h = window_h();
}

static void zoom_box(box_t* b)
{
    uint32_t raw = zoom.grow ? zoom.tick : ZOOM_TICKS - zoom.tick;
    uint32_t e = raw * raw * (3 * ZOOM_TICKS - 2 * raw);
    uint32_t den = ZOOM_TICKS * ZOOM_TICKS * ZOOM_TICKS;
    int32_t cw = (int32_t)((uint32_t)zoom.w * e / den);
    int32_t ch = TITLE_H + (int32_t)((uint32_t)(zoom.h - TITLE_H) *
        e / den);
    int32_t cx = zoom.x + zoom.w / 2;

    if (cw > zoom.w) cw = zoom.w;
    b->x0 = cx - cw / 2;
    b->y0 = zoom.y;
    b->x1 = b->x0 + cw;
    b->y1 = zoom.y + ch;
}

static void zoom_start(bool grow)
{
    zoom.active = true;
    zoom.grow = grow;
    zoom.tick = 0;
    zoom_seeds(&zoom.x, &zoom.y, &zoom.w, &zoom.h);
}

/* ------------------------------ painting --------------------------- */

static void paint_cursor(void)
{
    static const uint8_t arrow[] = {
        1,0,0,0,0,0,0,0,0,
        1,1,0,0,0,0,0,0,0,
        1,1,1,0,0,0,0,0,0,
        1,1,1,1,0,0,0,0,0,
        1,1,1,1,1,0,0,0,0,
        1,1,1,1,1,1,0,0,0,
        1,1,1,1,1,1,1,0,0,
        1,1,1,1,1,1,1,1,0,
        1,1,1,1,1,1,1,1,1,
        1,1,1,1,1,1,1,0,0,
        1,1,1,1,1,1,1,0,0,
        1,1,1,1,1,0,0,0,0,
        1,1,1,1,1,1,0,0,0,
        1,1,1,1,0,0,0,0,0,
        0,1,0,0,0,0,0,0,0
    };
    int r, c;
    for (r = 0; r < 15; r++) {
        for (c = 0; c < 9; c++) {
            if (!arrow[r * 9 + c]) continue;
            set(mx - 2 + c, my - 2 + r, C_DK);
            set(mx - 1 + c, my - 1 + r, C_HI);
        }
    }
}

static void paint_zoom(void)
{
    box_t r;
    int32_t w, h;
    if (!zoom.active) return;
    zoom_box(&r);
    w = r.x1 - r.x0;
    h = r.y1 - r.y0;
    if (w < 10 || h < 10) return;
    fill(r.x0, r.y0, w, 1, C_HI);
    fill(r.x0, r.y0, 1, h, C_HI);
    fill(r.x0, r.y1 - 1, w, 1, C_DK);
    fill(r.x1 - 1, r.y0, 1, h, C_DK);
    fill(r.x0 + 3, r.y0 + 3, w - 6, 1, C_SH);
    fill(r.x0 + 3, r.y0 + 3, 1, h - 6, C_SH);
    fill(r.x0 + 3, r.y0 + h - 4, w - 6, 1, C_FACE);
    fill(r.x0 + w - 4, r.y0 + 3, 1, h - 6, C_FACE);
}

static void paint_window(void)
{
    int32_t w = window_w(), h = window_h();
    if (!wb_open) return;
    fill(wb_x + 2, wb_y + 2, w, h, C_SH);
    bevel(wb_x, wb_y, w, h, true);
    gradient_v(wb_x + 4, wb_y + 4, w - 8, TITLE_H - 4, C_TITLE,
        C_TITLE_I);
    fill(wb_x + 4, wb_y + TITLE_H, w - 8, h - TITLE_H - 4, C_FACE);
    fill(wb_x + w - TITLE_H - 6, wb_y + 8, TITLE_H - 2, TITLE_H - 6,
        C_FACE);
    fill(wb_x + w - TITLE_H - 4, wb_y + 10, 12, 3, C_TITLE);
    fill(wb_x + w - TITLE_H - 4, wb_y + 14, 12, 3, C_TITLE);
}

static void paint_menu(void)
{
    int32_t th = menu_total_h(), h_eff = th, top;
    int i;
    if (!menu_open) return;
    top = menu_top();
    if (menu_anim > 0) {
        h_eff = th * menu_anim / MENU_TICKS;
        if (h_eff < 8) h_eff = 8;
        if (h_eff > th) h_eff = th;
    }
    fill(0, top, MENU_W, h_eff, C_MENU);
    bevel(0, top, MENU_W, h_eff, true);
    gradient_v(3, top + 3, 26, h_eff - 6, C_B0, C_B1);
    for (i = 0; i < MENU_COUNT; i++) {
        int32_t ry = menu_row_y(i);
        if (menu_anim > 0 && ry + 8 > top + h_eff) continue;
        if (in(mx, my, 4, ry, MENU_W - 8, MENU_ITEM_H - 2)) {
            fill(4, ry, MENU_W - 8, MENU_ITEM_H - 2, C_SEL);
        }
    }
}

static void paint_texts(void)
{
    int32_t h, m;
    char buf[6];
    int i;

    if (menu_open) {
        for (i = 0; i < MENU_COUNT; i++) {
            int32_t ry = menu_row_y(i);
            web_text(36, ry + 5, menu_labels[i],
                in(mx, my, 4, ry, MENU_W - 8, MENU_ITEM_H - 2)
                    ? C_HI : C_TEXT, 13, false);
        }
    }
    web_text(14, SCREEN_H - TASK_H + 7, menu_open ? "Close" : "Start",
        C_HI, 13, true);
    web_clock(&h, &m);
    buf[0] = (char)('0' + (h / 10) % 10);
    buf[1] = (char)('0' + h % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + (m / 10) % 10);
    buf[4] = (char)('0' + m % 10);
    buf[5] = '\0';
    web_text(SCREEN_W - 56, SCREEN_H - TASK_H + 7, buf, C_HI, 13, true);

    if (wb_open && !wb_min) {
        web_text(wb_x + 6, wb_y + 6, "About RockOS", C_HI, 12, true);
        web_text(wb_x + 18, wb_y + TITLE_H + 18, "RockOS",
            C_TITLE, 26, true);
        web_text(wb_x + 18, wb_y + TITLE_H + 56, "kernel: ROK x86",
            C_TEXT, 13, false);
        web_text(wb_x + 18, wb_y + TITLE_H + 78,
            "this build: wasm desktop demo", C_TEXT, 13, false);
        web_text(wb_x + 18, wb_y + TITLE_H + 118,
            "(c) 2026 SquigglyCat Tech", C_TEXT, 12, false);
    }
}

static void paint(void)
{
    int32_t i;
    fill(0, 0, SCREEN_W, SCREEN_H, C_DESK);
    for (i = 0; i < 7; i++) { /* soft desktop 'hills' lane */
        int32_t y = 90 + i * 7, x = 24 + i * 46;
        for (; y < 250; y += 21) {
            fill(x + (y / 7), y, 96, 4, mix(C_DESK, C_DK, 0, 1));
        }
    }
    fill(0, 0, SCREEN_W, SCREEN_H, C_DESK);
    gradient_v(0, 0, SCREEN_W, SCREEN_H - TASK_H, C_B0, C_DESK);
    fill(0, SCREEN_H - TASK_H, SCREEN_W, TASK_H, C_TASK);
    fill(0, SCREEN_H - TASK_H, SCREEN_W, 1, C_HI);
    {
        int32_t sb = SCREEN_H - TASK_H + 4;
        bool hot = menu_open ||
            in(mx, my, 4, sb, 116, TASK_H - 8);
        fill(4, sb, 116, TASK_H - 8, hot ? C_SEL : C_TASK);
        bevel(4, sb, 116, TASK_H - 8, !menu_open);
        fill(10, sb + 4, 10, TASK_H - 16, C_B0);
        gradient_v(25, sb + 4, 60, TASK_H - 16, C_B0, C_B1);
    }
    fill(SCREEN_W - 76, SCREEN_H - TASK_H + 4, 70, TASK_H - 8, C_MENU);
    bevel(SCREEN_W - 76, SCREEN_H - TASK_H + 4, 70, TASK_H - 8, true);
    paint_window();
    paint_menu();
    paint_zoom();
    paint_cursor();
    blit();
    paint_texts();
}

/* ------------------------------- actions --------------------------- */

static void anim_tick(void)
{
    if (menu_anim > 0) {
        if (menu_anim < MENU_TICKS) menu_anim++;
        else menu_anim = 0;
    }
    if (zoom.active) {
        zoom.tick++;
        if (zoom.tick > ZOOM_TICKS) zoom.active = false;
    }
}

static void open_about(void)
{
    wb_open = true;
    wb_min = false;
    wb_max = false;
    zoom_start(true);
}

static void tick(void)
{
    anim_tick();
    paint();
}

/* --------------------------- event plumbing ------------------------ */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>

static EM_BOOL on_mousemove(int type, const EmscriptenMouseEvent* e,
    void* u)
{
    (void)type; (void)u;
    mx = e->canvasX;
    my = e->canvasY;
    return false;
}

static EM_BOOL on_mousedown(int type, const EmscriptenMouseEvent* e,
    void* u)
{
    int x = e->canvasX, y = e->canvasY, i;
    (void)type; (void)u;

    if (in(x, y, 4, SCREEN_H - TASK_H + 4, 116, TASK_H - 8)) {
        menu_open = !menu_open;
        menu_anim = menu_open ? 1 : 0;
        return true;
    }
    if (menu_open) {
        for (i = 0; i < MENU_COUNT; i++) {
            if (in(x, y, 4, menu_row_y(i), MENU_W - 8,
                    MENU_ITEM_H - 2)) {
                menu_open = false;
                menu_anim = 0;
                if (i == 0) open_about();
                return true;
            }
        }
    }
    if (wb_open && !wb_min) {
        int32_t w = window_w();
        if (in(x, y, wb_x + w - TITLE_H - 6, wb_y + 12, TITLE_H - 2,
                TITLE_H - 6)) {
            wb_open = false;
            zoom_start(false);
            return true;
        }
        if (in(x, y, wb_x, wb_y, w, TITLE_H)) {
            drag_w = true;
            drag_dx = x - wb_x;
            drag_dy = y - wb_y;
            return true;
        }
    }
    return false;
}

static EM_BOOL on_mousemove_press(int type, const EmscriptenMouseEvent*
    e, void* u)
{
    (void)type; (void)u;
    if (drag_w) {
        mx = e->canvasX;
        my = e->canvasY;
        wb_x = mx - drag_dx;
        wb_y = my - drag_dy;
        if (wb_x < 0) wb_x = 0;
        if (wb_y < 0) wb_y = 0;
        if (wb_x + window_w() > SCREEN_W)
            wb_x = SCREEN_W - window_w();
        if (wb_y > SCREEN_H - TASK_H - TITLE_H)
            wb_y = SCREEN_H - TASK_H - TITLE_H;
        return true;
    }
    return false;
}

static EM_BOOL on_mouseup(int type, const EmscriptenMouseEvent* e,
    void* u)
{
    (void)type; (void)u;
    drag_w = false;
    return false;
}

#endif

/* ------------------------------- bootstrap ------------------------- */

int main(void)
{
    wb_x = (SCREEN_W - WIN_W) / 2;
    wb_y = 44;
    memset(fb, 0, sizeof(fb));
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var cv = document.getElementById('screen');
        cv.width = $2; cv.height = $3;
        Module.ctxPix = $0;
    }, (uintptr_t)fb, 0, SCREEN_W, SCREEN_H);
    emscripten_set_mousemove_callback("#screen", 0, true, on_mousemove);
    emscripten_set_mousedown_callback("#screen", 0, true,
        on_mousedown);
    emscripten_set_mousemove_callback("#screen", 0, true,
        on_mousemove_press);
    emscripten_set_mouseup_callback("#screen", 0, true, on_mouseup);
    emscripten_set_main_loop(tick, 0, 1);
#else
    open_about();
    {
        long f;
        for (f = 0; f < 60; f++) tick();
    }
    printf("web demo host ticks ok\n");
#endif
    return 0;
}

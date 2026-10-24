/* RockOS boot animation - "Assemble".
 *
 * The five facets of the RockOS rock fly in and lock together, the letters
 * rise in one by one, and the facets then fill up as the boot progress
 * (5 facets = 5 stages). Pieces come from boot_logo_data.h, which is
 * generated from assets/rockos-logo-outlined.png by scripts/make_boot_logo.py.
 *
 * Integer-only (no floats, no 64-bit division) so it builds for both the
 * 64-bit and the 32-bit kernel. Everything is drawn with display_fill_rect_rgb
 * using horizontal runs, so it stays cheap on low-end machines.
 * The PIT runs at 100 Hz, so one tick = 10 ms. */
#include "boot_animation.h"

#include "boot_logo_data.h"
#include "display.h"
#include "timer.h"

#include <stdbool.h>
#include <stdint.h>

#define BG 8                    /* background grey level */
#define FG 205                  /* logo grey level */
#define DIM 40                  /* unfilled facet brightness (of 256) */
#define TRACK 30                /* progress track level */
#define NOGLINT 0x7FFFFFFF

#define FRAME_TICKS 2U
#define T_LETTERS 105           /* first letter starts rising */
#define T_UI 160                /* progress bar fades in */
#define T_PROGRESS 150          /* timed progress starts */
#define PROGRESS_TICKS 420U     /* ...and takes this long (no external progress) */
#define T_GLINT1 200
#define GLINT_TICKS 120
#define FADE_TICKS 40U
#define BAR_W 280
#define BAR_H 3

static uint32_t g_external_permille = 0xFFFFFFFFU;

/* Optional: call during boot (0..100) to drive the bar from real progress
 * instead of the built-in timer. */
void boot_animation_set_progress(uint32_t percent) {
    g_external_permille = percent >= 100U ? 1000U : percent * 10U;
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 0..1024 -> 0..1024 */
static int32_t ease_out_cubic(int32_t t) {
    int32_t u = 1024 - t;
    return 1024 - ((u * u) / 1024) * u / 1024;
}

/* Overshoots slightly past 1024 and settles (the "snap" into place). */
static int32_t ease_out_back(int32_t t) {
    int32_t u = t - 1024;
    int32_t u2 = (u * u) / 1024;
    int32_t u3 = (u2 * u) / 1024;
    return 1024 + (26 * u3) / 10 + (16 * u2) / 10;
}

static void wait_until(uint64_t start, uint32_t ticks) {
    while ((uint32_t)(timer_get_ticks() - start) < ticks) {
        asm volatile("pause");
    }
}

typedef struct {
    int32_t ga;       /* global brightness 0..256 (fade-out) */
    int32_t glint;    /* glint sweep position, or NOGLINT */
    uint32_t sw, sh;  /* screen size */
} fx_t;

/* Draw one alpha mask at (px, py). fill < 0: plain piece (letters).
 * fill 0..1000: facet whose bottom `fill`/1000 is lit with a soft edge.
 * ea 0..256: fade-in. (lx, ly): resting position inside the logo (for glint). */
static void draw_part(const blogo_part_t* p, int32_t px, int32_t py,
    int32_t lx, int32_t ly, int32_t fill, int32_t ea, const fx_t* fx) {
    const int32_t w = p->w, h = p->h;
    const int32_t soft = h / 5 > 0 ? h / 5 : 1;
    const int32_t edge = fill < 0 ? 0 : ((1000 - fill) * (h + soft)) / 1000;

    for (int32_t r = 0; r < h; r++) {
        const int32_t y = py + r;
        if (y < 0 || y >= (int32_t)fx->sh) continue;
        int32_t k = 256;
        if (fill >= 0) {
            int32_t ramp = clampi(((r - edge + soft) * 256) / soft, 0, 256);
            k = DIM + (((256 - DIM) * ramp) >> 8);
        }
        k = (k * ea) >> 8;
        if (k <= 0) continue;

        const uint8_t* row = p->a + r * w;
        int32_t prev = BG, run = 0;
        for (int32_t c = 0; c <= w; c++) {
            int32_t l = BG;
            if (c < w && row[c]) {
                const int32_t a = row[c];
                l = BG + (((FG - BG) * a * k) >> 16);
                if (fx->glint != NOGLINT) {
                    int32_t d = (lx + c) + (ly + r) / 3 - fx->glint;
                    if (d < 0) d = -d;
                    const int32_t gw = BLOGO_W / 6;
                    if (d < gw) {
                        int32_t amt = ((gw - d) * 256) / gw;
                        l += ((((255 - l) * amt * 230) >> 16) * a) / 255;
                    }
                }
                if (fx->ga < 256) l = BG + (((l - BG) * fx->ga) >> 8);
            }
            if (c == w || l != prev) {
                if (prev > BG && c > run) {
                    int32_t x0 = px + run, x1 = px + c;
                    if (x0 < 0) x0 = 0;
                    if (x1 > (int32_t)fx->sw) x1 = (int32_t)fx->sw;
                    if (x1 > x0) {
                        display_fill_rect_rgb((uint32_t)x0, (uint32_t)y,
                            (uint32_t)(x1 - x0), 1U,
                            (uint8_t)prev, (uint8_t)prev, (uint8_t)prev);
                    }
                }
                prev = l;
                run = c;
            }
        }
    }
}

static void draw_number(char* out, int32_t v) {   /* "NN%" */
    int n = 0;
    if (v >= 100) out[n++] = (char)('0' + v / 100);
    if (v >= 10) out[n++] = (char)('0' + (v / 10) % 10);
    out[n++] = (char)('0' + v % 10);
    out[n++] = '%';
    out[n] = '\0';
}

static int32_t mix(int32_t level, int32_t a256) {
    return BG + (((level - BG) * a256) >> 8);
}

void boot_animation_run(void) {
    uint32_t sw, sh;
    if (!display_get_framebuffer_size(&sw, &sh) ||
        sw < BLOGO_W + 80U || sh < 300U) {
        return;
    }

    static const char* const stages[] = {
        "Starting kernel", "Setting up memory", "Mounting RockFS",
        "Loading drivers", "Preparing desktop"
    };
    /* Facet -> boot stage, and the direction each one flies in from (x10). */
    static const int32_t order[5] = {3, 0, 1, 2, 4};
    static const int32_t dirx[5] = {-10, -9, 1, 10, 3};
    static const int32_t diry[5] = {5, -8, -10, 1, 10};

    const int32_t ox = (int32_t)(sw - BLOGO_W) / 2;
    const int32_t oy = (int32_t)sh / 2 - BLOGO_H / 2 - 30;
    const int32_t bar_x = (int32_t)(sw - BAR_W) / 2;
    const int32_t bar_y = oy + BLOGO_H + 46;

    display_fill_rect_rgb(0, 0, sw, sh, BG, BG, BG);
    display_present();
    uint64_t start = timer_get_ticks();
    wait_until(start, 20U);
    start = timer_get_ticks();

    int32_t done_t = -1;
    uint32_t t = 0;
    for (;;) {
        t = (uint32_t)(timer_get_ticks() - start);
        const int32_t T = (int32_t)t;

        /* Progress in permille: external value, or a timed ease. */
        int32_t pe;
        if (g_external_permille != 0xFFFFFFFFU) {
            pe = (int32_t)g_external_permille;
        } else {
            int32_t pt = clampi(((T - T_PROGRESS) * 1000) / (int32_t)PROGRESS_TICKS, 0, 1000);
            int32_t sm = ((pt * pt) / 1000) * (3000 - 2 * pt) / 1000;
            pe = (pt * 6) / 10 + (sm * 4) / 10;
        }
        if (pe >= 1000 && done_t < 0 && T > T_PROGRESS) done_t = T;
        if (done_t >= 0 && T > done_t + GLINT_TICKS + (int32_t)FADE_TICKS) break;

        fx_t fx = { 256, NOGLINT, sw, sh };
        if (done_t >= 0) {
            fx.ga = 256 - clampi(((T - done_t - GLINT_TICKS) * 256) / (int32_t)FADE_TICKS, 0, 256);
            if (T - done_t < GLINT_TICKS) {
                fx.glint = -BLOGO_W / 6 + ((T - done_t) * (BLOGO_W + BLOGO_W / 3)) / GLINT_TICKS;
            }
        } else if (T >= T_GLINT1 && T < T_GLINT1 + GLINT_TICKS) {
            fx.glint = -BLOGO_W / 6 + ((T - T_GLINT1) * (BLOGO_W + BLOGO_W / 3)) / GLINT_TICKS;
        }

        /* Clear the area the logo can occupy (wide while pieces are flying). */
        const int32_t m = T < 160 ? BLOGO_FLY + 14 : 12;
        int32_t cx = ox - m, cy = oy - m;
        int32_t cw = BLOGO_W + 2 * m, ch = BLOGO_H + 2 * m;
        if (cx < 0) { cw += cx; cx = 0; }
        if (cy < 0) { ch += cy; cy = 0; }
        if (cx + cw > (int32_t)sw) cw = (int32_t)sw - cx;
        if (cy + ch > (int32_t)sh) ch = (int32_t)sh - cy;
        display_fill_rect_rgb((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch, BG, BG, BG);

        /* Facets: fly in, snap, then fill as progress. */
        for (int32_t k = 0; k < BLOGO_FACETS; k++) {
            const blogo_part_t* p = &blogo_parts[k];
            const int32_t lt = T - 12 * k;
            const int32_t e = ease_out_back(clampi((lt * 1024) / 110, 0, 1024));
            const int32_t inv = 1024 - e;
            const int32_t ea = clampi((lt * 256) / 50, 0, 256);
            const int32_t dx = (dirx[k] * BLOGO_FLY * inv) / (10 * 1024);
            const int32_t dy = (diry[k] * BLOGO_FLY * inv) / (10 * 1024);
            const int32_t f = clampi(pe * 5 - order[k] * 1000, 0, 1000);
            draw_part(p, ox + p->x + dx, oy + p->y + dy, p->x, p->y, f, ea, &fx);
        }
        /* Letters: rise and fade in, R-o-c-k-O-S. */
        for (int32_t i = 0; i < BLOGO_LETTERS; i++) {
            const blogo_part_t* p = &blogo_parts[BLOGO_FACETS + i];
            const int32_t lt = T - T_LETTERS - 9 * i;
            const int32_t e = ease_out_cubic(clampi((lt * 1024) / 80, 0, 1024));
            draw_part(p, ox + p->x, oy + p->y + ((1024 - e) * (BLOGO_FLY / 6)) / 1024,
                p->x, p->y, -1, e >> 2, &fx);
        }

        /* Progress line + status text. */
        display_fill_rect_rgb((uint32_t)(bar_x - 12), (uint32_t)(bar_y - 8),
            BAR_W + 24U, 40U, BG, BG, BG);
        const int32_t ua = clampi(((T - T_UI) * 256) / 60, 0, 256) * fx.ga / 256;
        if (ua > 8) {
            const int32_t track = mix(TRACK, ua), lit = mix(FG, ua);
            display_fill_rect_rgb((uint32_t)bar_x, (uint32_t)bar_y, BAR_W, BAR_H,
                (uint8_t)track, (uint8_t)track, (uint8_t)track);
            const int32_t fw = (BAR_W * pe) / 1000;
            if (fw > 0) {
                display_fill_rect_rgb((uint32_t)bar_x, (uint32_t)bar_y, (uint32_t)fw, BAR_H,
                    (uint8_t)lit, (uint8_t)lit, (uint8_t)lit);
                const int32_t hx = fw > 2 ? fw - 2 : 0;
                const int32_t hv = mix(255, ua);
                display_fill_rect_rgb((uint32_t)(bar_x + hx), (uint32_t)bar_y, 2U, BAR_H,
                    (uint8_t)hv, (uint8_t)hv, (uint8_t)hv);
            }
            if (ua > 200) {
                const char* msg = done_t >= 0 ? "Welcome" : stages[clampi(pe / 200, 0, 4)];
                char pct[8];
                draw_number(pct, pe / 10);
                int32_t plen = 0;
                while (pct[plen]) plen++;
                display_draw_text_ui((uint32_t)bar_x, (uint32_t)(bar_y + 12), msg, COLOR_LIGHT_GRAY);
                display_draw_text_ui((uint32_t)(bar_x + BAR_W - plen * 8), (uint32_t)(bar_y + 12),
                    pct, COLOR_DARK_GRAY);
            }
        }

        display_present();
        wait_until(start, t + FRAME_TICKS);
    }

    display_fill_rect_rgb(0, 0, sw, sh, BG, BG, BG);
    display_present();
    wait_until(timer_get_ticks(), 12U);
}

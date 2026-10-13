#include "boot_animation.h"

#include "display.h"
#include "timer.h"

#include <stdint.h>

#define BOOT_BG_R 7U
#define BOOT_BG_G 14U
#define BOOT_BG_B 27U
#define BOOT_BLUE_R 83U
#define BOOT_BLUE_G 166U
#define BOOT_BLUE_B 255U
#define BOOT_PALE_R 218U
#define BOOT_PALE_G 235U
#define BOOT_PALE_B 255U
#define BOOT_ANIMATION_FRAMES 28U
#define BOOT_FRAME_TICKS 2ULL
#define BOOT_LOADING_TICKS 90ULL

static void boot_wait_ticks(uint64_t duration) {
    uint64_t start = timer_get_ticks();
    while (timer_get_ticks() - start < duration) {
        asm volatile("pause");
    }
}

static void boot_background(uint32_t width, uint32_t height) {
    display_fill_rect_rgb(0, 0, width, height,
        BOOT_BG_R, BOOT_BG_G, BOOT_BG_B);

}

static void boot_draw_piece(uint32_t x, uint32_t y, uint32_t size,
    uint8_t red, uint8_t green, uint8_t blue) {
    display_fill_rect_rgb(x, y, size, size, red, green, blue);
    if (size > 5U) {
        display_fill_rect_rgb(x + 3U, y + 3U, size - 6U, size - 6U,
            BOOT_PALE_R, BOOT_PALE_G, BOOT_PALE_B);
    }
}

static void boot_draw_loading(uint32_t width, uint32_t height,
    uint64_t phase) {
    const uint32_t bar_width = 220U;
    const uint32_t bar_height = 4U;
    uint32_t x = width > bar_width ? (width - bar_width) / 2U : 0U;
    uint32_t y = height / 2U + 102U;
    uint32_t head = (uint32_t)(phase % (bar_width + 48U));
    uint32_t left = head > 48U ? head - 48U : 0U;
    uint32_t fill = head > bar_width ? bar_width : head;

    display_fill_rect_rgb(x, y, bar_width, bar_height,
        24U, 43U, 65U);
    if (fill > left) {
        display_fill_rect_rgb(x + left, y, fill - left, bar_height,
            BOOT_BLUE_R, BOOT_BLUE_G, BOOT_BLUE_B);
    }
}

void boot_animation_run(void) {
    uint32_t width;
    uint32_t height;
    if (!display_get_framebuffer_size(&width, &height) ||
        width < 240U || height < 160U) {
        return;
    }

    const uint32_t piece_size = 22U;
    const int32_t center_x = (int32_t)(width / 2U);
    const int32_t center_y = (int32_t)(height / 2U) - 52;
    const int32_t spread_x = (int32_t)(width / 2U) + 30;
    const int32_t spread_y = (int32_t)(height / 2U) + 30;

    /* Paint the static background once; subsequent frames only touch the
     * small moving fragments so boot animation does not reintroduce full-
     * screen framebuffer copies on every frame. */
    boot_background(width, height);
    uint32_t old_x[4] = {0, 0, 0, 0};
    uint32_t old_y[4] = {0, 0, 0, 0};
    bool old_visible[4] = {false, false, false, false};

    /* The four bright fragments accelerate toward the center. */
    for (uint32_t frame = 0; frame <= BOOT_ANIMATION_FRAMES; frame++) {
        uint32_t progress = frame * 100U / BOOT_ANIMATION_FRAMES;
        uint32_t inverse = 100U - progress;
        int32_t offset_x = (spread_x * (int32_t)inverse) / 100;
        int32_t offset_y = (spread_y * (int32_t)inverse) / 100;
        uint32_t x_left = center_x > offset_x
            ? (uint32_t)(center_x - offset_x) : 0U;
        uint32_t x_right = (uint32_t)(center_x + offset_x);
        uint32_t y_top = center_y > offset_y
            ? (uint32_t)(center_y - offset_y) : 0U;
        uint32_t y_bottom = (uint32_t)(center_y + offset_y);

        for (uint32_t piece = 0; piece < 4U; piece++) {
            if (old_visible[piece]) {
                display_fill_rect_rgb(old_x[piece], old_y[piece],
                    piece_size, piece_size, BOOT_BG_R, BOOT_BG_G, BOOT_BG_B);
            }
        }

        uint32_t next_x[4] = {x_left, x_right, x_left, x_right};
        uint32_t next_y[4] = {y_top, y_top, y_bottom, y_bottom};
        const uint8_t colors[4][3] = {
            {BOOT_BLUE_R, BOOT_BLUE_G, BOOT_BLUE_B},
            {BOOT_PALE_R, BOOT_PALE_G, BOOT_PALE_B},
            {BOOT_PALE_R, BOOT_PALE_G, BOOT_PALE_B},
            {BOOT_BLUE_R, BOOT_BLUE_G, BOOT_BLUE_B}
        };
        for (uint32_t piece = 0; piece < 4U; piece++) {
            old_visible[piece] = next_x[piece] + piece_size < width &&
                next_y[piece] + piece_size < height;
            old_x[piece] = next_x[piece];
            old_y[piece] = next_y[piece];
            if (old_visible[piece]) {
                boot_draw_piece(next_x[piece], next_y[piece], piece_size,
                    colors[piece][0], colors[piece][1], colors[piece][2]);
            }
        }
        display_present();
        boot_wait_ticks(BOOT_FRAME_TICKS);
    }

    /* Let the real, existing RockOS logo asset resolve out of the fragments. */
    boot_background(width, height);
    uint32_t logo_x = width > DISPLAY_ROCKOS_LOGO_WIDTH
        ? (width - DISPLAY_ROCKOS_LOGO_WIDTH) / 2U : 0U;
    uint32_t logo_y = height / 2U > DISPLAY_ROCKOS_LOGO_HEIGHT / 2U
        ? height / 2U - DISPLAY_ROCKOS_LOGO_HEIGHT / 2U - 26U : 0U;
    display_draw_rockos_logo(logo_x, logo_y);

    const char* tagline = "A different kind of operating system";
    uint32_t tagline_x = width > 390U ? (width - 390U) / 2U : 0U;
    uint32_t tagline_y = logo_y + DISPLAY_ROCKOS_LOGO_HEIGHT + 12U;
    display_draw_text_ui(tagline_x, tagline_y, tagline,
        COLOR_LIGHT_GRAY);

    uint32_t status_y = height / 2U + 88U;
    display_draw_text_ui(width / 2U > 72U ? width / 2U - 72U : 0U,
        status_y, "Starting RockOS", COLOR_WHITE);
    display_present();

    uint64_t start = timer_get_ticks();
    while (timer_get_ticks() - start < BOOT_LOADING_TICKS) {
        boot_draw_loading(width, height, timer_get_ticks() - start);
        display_present();
        boot_wait_ticks(2ULL);
    }

    /* Clear all boot graphics; shell_init() draws the actual next screen. */
    display_clear();
    display_present();
}

#include "boot_animation.h"

#include "display.h"
#include "timer.h"

#include <stdint.h>

#define BOOT_BG_R 0U
#define BOOT_BG_G 0U
#define BOOT_BG_B 0U
#define BOOT_DARK_R 24U
#define BOOT_DARK_G 24U
#define BOOT_DARK_B 24U
#define BOOT_MID_R 112U
#define BOOT_MID_G 112U
#define BOOT_MID_B 112U
#define BOOT_LIGHT_R 224U
#define BOOT_LIGHT_G 224U
#define BOOT_LIGHT_B 224U
#define BOOT_WHITE_R 255U
#define BOOT_WHITE_G 255U
#define BOOT_WHITE_B 255U

/* PIT is configured for 100 Hz before this animation is called. */
#define BOOT_ASSEMBLY_FRAMES 64U
#define BOOT_FRAME_TICKS 3ULL
#define BOOT_LOADING_TICKS 520ULL
#define BOOT_STATUS_TICKS 160ULL

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
            BOOT_DARK_R, BOOT_DARK_G, BOOT_DARK_B);
    }
}

/* A restrained grayscale marquee, like a firmware/OS startup indicator. */
static void boot_draw_loading(uint32_t width, uint32_t height,
    uint64_t phase) {
    const uint32_t bar_width = width < 300U ? width - 32U : 268U;
    const uint32_t bar_height = 5U;
    const uint32_t x = (width - bar_width) / 2U;
    const uint32_t y = height / 2U + 154U;
    const uint32_t segment_width = bar_width / 4U;
    const uint32_t travel = bar_width + segment_width;
    const uint32_t head = (uint32_t)(phase % travel);
    const uint32_t left = head > segment_width ? head - segment_width : 0U;
    const uint32_t right = head < bar_width ? head : bar_width;
    const uint32_t fill_start = left < bar_width ? left : bar_width;
    const uint32_t fill_end = right > fill_start ? right : fill_start;

    display_fill_rect_rgb(x, y, bar_width, bar_height,
        BOOT_DARK_R, BOOT_DARK_G, BOOT_DARK_B);
    if (fill_end > fill_start) {
        display_fill_rect_rgb(x + fill_start, y,
            fill_end - fill_start, bar_height,
            BOOT_LIGHT_R, BOOT_LIGHT_G, BOOT_LIGHT_B);
    }
    /* A narrow highlight gives the moving segment a crisp pixel edge. */
    if (fill_end > fill_start) {
        display_fill_rect_rgb(x + fill_end - 1U, y, 1U, bar_height,
            BOOT_WHITE_R, BOOT_WHITE_G, BOOT_WHITE_B);
    }
}

static void boot_draw_status(uint32_t width, uint32_t y, uint32_t stage) {
    static const char* const messages[] = {
        "Starting RockOS",
        "Preparing system services",
        "Initializing user interface",
        "Almost ready"
    };
    const char* message = messages[stage % 4U];
    /* UI text is fixed-width, so center by its character count. */
    uint32_t length = 0;
    while (message[length] != '\0') length++;
    uint32_t text_width = length * 8U;
    uint32_t x = width > text_width ? (width - text_width) / 2U : 0U;
    display_fill_rect_rgb(0, y, width, DISPLAY_UI_TEXT_HEIGHT,
        BOOT_BG_R, BOOT_BG_G, BOOT_BG_B);
    display_draw_text_ui(x, y, message, COLOR_LIGHT_GRAY);
}

void boot_animation_run(void) {
    uint32_t width;
    uint32_t height;
    if (!display_get_framebuffer_size(&width, &height) ||
        width < 320U || height < 240U) {
        return;
    }

    const uint32_t piece_size = 24U;
    const int32_t center_x = (int32_t)(width / 2U);
    const int32_t center_y = (int32_t)(height / 2U) - 62;
    const int32_t spread_x = (int32_t)(width / 2U) + 24;
    const int32_t spread_y = (int32_t)(height / 2U) + 24;

    /* Begin with a clean black screen; only small regions change per frame. */
    boot_background(width, height);
    display_present();
    boot_wait_ticks(35ULL);

    uint32_t old_x[4] = {0, 0, 0, 0};
    uint32_t old_y[4] = {0, 0, 0, 0};
    bool old_visible[4] = {false, false, false, false};

    /* Four grayscale fragments glide in and lock together. */
    for (uint32_t frame = 0; frame <= BOOT_ASSEMBLY_FRAMES; frame++) {
        uint32_t progress = frame * 100U / BOOT_ASSEMBLY_FRAMES;
        uint32_t inverse = 100U - progress;
        int32_t offset_x = (spread_x * (int32_t)inverse) / 100;
        int32_t offset_y = (spread_y * (int32_t)inverse) / 100;
        int32_t positions_x[4] = {
            center_x - offset_x, center_x + offset_x,
            center_x - offset_x, center_x + offset_x
        };
        int32_t positions_y[4] = {
            center_y - offset_y, center_y - offset_y,
            center_y + offset_y, center_y + offset_y
        };
        const uint8_t shades[4] = {224U, 160U, 160U, 255U};

        for (uint32_t piece = 0; piece < 4U; piece++) {
            if (old_visible[piece]) {
                display_fill_rect_rgb(old_x[piece], old_y[piece],
                    piece_size, piece_size, BOOT_BG_R, BOOT_BG_G, BOOT_BG_B);
            }
        }

        for (uint32_t piece = 0; piece < 4U; piece++) {
            int32_t px = positions_x[piece];
            int32_t py = positions_y[piece];
            old_visible[piece] = px >= 0 && py >= 0 &&
                (uint32_t)px + piece_size < width &&
                (uint32_t)py + piece_size < height;
            if (old_visible[piece]) {
                old_x[piece] = (uint32_t)px;
                old_y[piece] = (uint32_t)py;
                boot_draw_piece(old_x[piece], old_y[piece], piece_size,
                    shades[piece], shades[piece], shades[piece]);
            }
        }
        display_present();
        boot_wait_ticks(BOOT_FRAME_TICKS);
    }

    /* Hold the assembled logo briefly before the longer startup phase. */
    boot_background(width, height);
    uint32_t logo_x = width > DISPLAY_ROCKOS_LOGO_WIDTH
        ? (width - DISPLAY_ROCKOS_LOGO_WIDTH) / 2U : 0U;
    uint32_t logo_y = height / 2U > DISPLAY_ROCKOS_LOGO_HEIGHT / 2U + 44U
        ? height / 2U - DISPLAY_ROCKOS_LOGO_HEIGHT / 2U - 44U : 0U;
    display_draw_rockos_logo(logo_x, logo_y);

    uint32_t tagline_y = logo_y + DISPLAY_ROCKOS_LOGO_HEIGHT + 14U;
    const char* tagline = "A different kind of operating system";
    uint32_t tagline_x = width > 296U ? (width - 296U) / 2U : 0U;
    display_draw_text_ui(tagline_x, tagline_y, tagline, COLOR_LIGHT_GRAY);

    uint32_t status_y = height / 2U + 102U;
    boot_draw_status(width, status_y, 0U);
    display_present();
    boot_wait_ticks(45ULL);

    uint64_t start = timer_get_ticks();
    uint32_t previous_stage = UINT32_MAX;
    while (timer_get_ticks() - start < BOOT_LOADING_TICKS) {
        uint64_t elapsed = timer_get_ticks() - start;
        uint32_t stage = (uint32_t)(elapsed / BOOT_STATUS_TICKS);
        if (stage != previous_stage) {
            boot_draw_status(width, status_y, stage);
            previous_stage = stage;
        }
        boot_draw_loading(width, height, elapsed / 2ULL);
        display_present();
        boot_wait_ticks(2ULL);
    }

    /* Give the completed screen a short beat, then hand off to the OS UI. */
    boot_background(width, height);
    display_present();
    boot_wait_ticks(12ULL);
}

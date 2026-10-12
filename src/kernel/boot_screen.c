#include "boot_screen.h"

#include "display.h"

#include <stdint.h>

#define BOOT_BAR_BLOCKS 12U
#define BOOT_BAR_BLOCK_WIDTH 18U
#define BOOT_BAR_BLOCK_HEIGHT 8U
#define BOOT_BAR_GAP 5U
#define BOOT_FRAME_CYCLES 120000000ULL
#define BOOT_HOLD_CYCLES 3000000000ULL

static uint64_t read_tsc(void) {
    uint32_t low;
    uint32_t high;
    asm volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static void wait_cycles(uint64_t cycles) {
    uint64_t start = read_tsc();
    while (read_tsc() - start < cycles) {
        asm volatile ("pause");
    }
}

static void draw_boot_screen(uint32_t width, uint32_t height,
    uint32_t* bar_x_out, uint32_t* bar_y_out) {
    const uint32_t bar_width = BOOT_BAR_BLOCKS * BOOT_BAR_BLOCK_WIDTH +
        (BOOT_BAR_BLOCKS - 1U) * BOOT_BAR_GAP;
    uint32_t logo_y = height > 520U ? height / 3U - 18U : 24U;
    uint32_t bar_x = width > bar_width ? (width - bar_width) / 2U : 8U;
    uint32_t bar_y = logo_y + DISPLAY_ROCKOS_LOGO_HEIGHT + 48U;
    uint32_t text_x = width > 220U ? (width - 220U) / 2U : 8U;

    display_fill_rect_rgb(0, 0, width, height, 8, 8, 8);
    if (width >= DISPLAY_ROCKOS_LOGO_WIDTH &&
        height >= DISPLAY_ROCKOS_LOGO_HEIGHT) {
        (void)display_draw_rockos_logo(
            (width - DISPLAY_ROCKOS_LOGO_WIDTH) / 2U, logo_y);
    } else {
        display_draw_text_scaled(text_x, logo_y, "ROCKOS",
            COLOR_WHITE, COLOR_BLACK, 3);
    }

    display_draw_text_ui(text_x, bar_y - 30U, "Starting RockOS...",
        COLOR_LIGHT_GRAY);
    display_fill_rect_rgb(bar_x - 2U, bar_y - 2U,
        bar_width + 4U, BOOT_BAR_BLOCK_HEIGHT + 4U, 56, 56, 56);
    for (uint32_t block = 0; block < BOOT_BAR_BLOCKS; block++) {
        uint32_t x = bar_x + block * (BOOT_BAR_BLOCK_WIDTH + BOOT_BAR_GAP);
        (void)display_fill_rect_rgb(x, bar_y, BOOT_BAR_BLOCK_WIDTH,
            BOOT_BAR_BLOCK_HEIGHT, 40, 40, 40);
    }

    const char* edition = "64-bit operating system";
#ifdef ROCKOS_ARCH_32
    edition = "32-bit operating system";
#endif
    display_draw_text_ui(text_x, height - 38U, edition, COLOR_DARK_GRAY);
    *bar_x_out = bar_x;
    *bar_y_out = bar_y;
    display_present();
}

static void draw_progress_block(uint32_t block, uint32_t bar_x,
    uint32_t bar_y) {
    uint32_t x = bar_x + block * (BOOT_BAR_BLOCK_WIDTH + BOOT_BAR_GAP);
    (void)display_fill_rect_rgb(x, bar_y, BOOT_BAR_BLOCK_WIDTH,
        BOOT_BAR_BLOCK_HEIGHT, 232, 232, 232);
    display_present();
}

void boot_screen_show(void) {
    uint32_t width = 640U;
    uint32_t height = 480U;
    if (!display_get_framebuffer_size(&width, &height)) {
        display_clear();
        display_set_color(COLOR_WHITE, COLOR_BLACK);
        display_puts("                         ROCKOS\n");
        display_puts("                    Starting RockOS...\n");
        return;
    }

    display_clear();

    uint32_t bar_x;
    uint32_t bar_y;
    draw_boot_screen(width, height, &bar_x, &bar_y);
    for (uint32_t block = 0; block < BOOT_BAR_BLOCKS; block++) {
        wait_cycles(BOOT_FRAME_CYCLES);
        draw_progress_block(block, bar_x, bar_y);
    }
    wait_cycles(BOOT_HOLD_CYCLES);
}

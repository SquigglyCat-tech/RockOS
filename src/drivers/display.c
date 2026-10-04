#include "display.h"

#include "io.h"
#include "multiboot1.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "timer.h"
#include "ui_assets.h"

#include <stddef.h>
#include <stdint.h>

#define VGA_WIDTH 80U
#define VGA_HEIGHT 25U
#define VGA_MEMORY ((volatile uint16_t*)0xB8000)
#define CELL_WIDTH 12U
#define CELL_HEIGHT 24U
#define FRAMEBUFFER_MAX_BYTES (32U * 1024U * 1024U)
#define MOUSE_CURSOR_WIDTH 12U
#define MOUSE_CURSOR_HEIGHT 18U

static const uint8_t font_alphanumeric[36][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x62,0x51,0x49,0x49,0x46}, {0x22,0x41,0x49,0x49,0x36},
    {0x18,0x14,0x12,0x7F,0x10}, {0x2F,0x49,0x49,0x49,0x31},
    {0x3E,0x49,0x49,0x49,0x32}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x26,0x49,0x49,0x49,0x3E}
};

static const uint8_t font_lowercase[26][5] = {
    {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02},
    {0x0C,0x52,0x52,0x52,0x3E}, {0x7F,0x08,0x04,0x04,0x78},
    {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00},
    {0x7C,0x04,0x18,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78},
    {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08},
    {0x48,0x54,0x54,0x54,0x20}, {0x04,0x3F,0x44,0x40,0x20},
    {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44},
    {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44}
};

static const uint8_t color_rgb[16][3] = {
    {0,0,0}, {0,0,170}, {0,170,0}, {0,170,170},
    {170,0,0}, {170,0,170}, {170,85,0}, {170,170,170},
    {85,85,85}, {85,85,255}, {85,255,85}, {85,255,255},
    {255,85,85}, {255,85,255}, {255,255,85}, {255,255,255}
};

static uint8_t cursor_x;
static uint8_t cursor_y;
static uint8_t current_color = 0x0F;
static uint16_t text_buffer[VGA_WIDTH * VGA_HEIGHT];
static bool framebuffer_active;
static const char* framebuffer_status = "not initialized";
static volatile uint8_t* framebuffer;
static uint32_t framebuffer_width;
static uint32_t framebuffer_height;
static uint32_t framebuffer_pitch;
static uint8_t framebuffer_bytes_per_pixel;
static uint8_t framebuffer_red_position;
static uint8_t framebuffer_red_size;
static uint8_t framebuffer_green_position;
static uint8_t framebuffer_green_size;
static uint8_t framebuffer_blue_position;
static uint8_t framebuffer_blue_size;
static uint32_t framebuffer_origin_x;
static uint32_t framebuffer_origin_y;
static uint8_t* framebuffer_shadow;
static bool framebuffer_dirty;
static uint32_t framebuffer_dirty_left;
static uint32_t framebuffer_dirty_top;
static uint32_t framebuffer_dirty_right;
static uint32_t framebuffer_dirty_bottom;
static uint64_t framebuffer_last_present_ticks;
static uint64_t framebuffer_last_present_cycles;
static uint32_t mouse_pixel_x;
static uint32_t mouse_pixel_y;
static bool mouse_pixel_visible;
static bool mouse_cursor_visible;
static uint8_t mouse_cursor_x;
static uint8_t mouse_cursor_y;
static uint16_t mouse_cursor_saved_entry;

static uint64_t display_read_timestamp_counter(void) {
    uint32_t low;
    uint32_t high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static uint16_t vga_entry(unsigned char character, uint8_t color) {
    return (uint16_t)character | ((uint16_t)color << 8);
}

static uint32_t channel_value(uint8_t value, uint8_t position,
    uint8_t size) {
    uint32_t maximum = (1U << size) - 1U;
    return (((uint32_t)value * maximum + 127U) / 255U) << position;
}

static uint32_t framebuffer_color(uint8_t color) {
    const uint8_t* rgb = color_rgb[color & 0x0F];
    return channel_value(rgb[0], framebuffer_red_position,
            framebuffer_red_size) |
        channel_value(rgb[1], framebuffer_green_position,
            framebuffer_green_size) |
        channel_value(rgb[2], framebuffer_blue_position,
            framebuffer_blue_size);
}

static uint8_t coverage_at(const uint8_t* bitmap, size_t pixel_index) {
    uint8_t packed = bitmap[pixel_index / 4];
    uint8_t shift = (uint8_t)(6 - (pixel_index % 4) * 2);
    return (uint8_t)((packed >> shift) & 0x03);
}

static void blend_foreground_over_pixel(uint32_t x, uint32_t y,
    uint8_t foreground, uint8_t coverage);

static void mark_framebuffer_dirty(uint32_t x, uint32_t y) {
    if (!framebuffer_dirty) {
        framebuffer_dirty_left = x;
        framebuffer_dirty_top = y;
        framebuffer_dirty_right = x + 1;
        framebuffer_dirty_bottom = y + 1;
        framebuffer_dirty = true;
        return;
    }
    if (x < framebuffer_dirty_left) framebuffer_dirty_left = x;
    if (y < framebuffer_dirty_top) framebuffer_dirty_top = y;
    if (x >= framebuffer_dirty_right) framebuffer_dirty_right = x + 1;
    if (y >= framebuffer_dirty_bottom) framebuffer_dirty_bottom = y + 1;
}

static void mark_framebuffer_dirty_rect(uint32_t x, uint32_t y,
    uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (!framebuffer_dirty) {
        framebuffer_dirty_left = x;
        framebuffer_dirty_top = y;
        framebuffer_dirty_right = x + width;
        framebuffer_dirty_bottom = y + height;
        framebuffer_dirty = true;
        return;
    }
    if (x < framebuffer_dirty_left) framebuffer_dirty_left = x;
    if (y < framebuffer_dirty_top) framebuffer_dirty_top = y;
    if (x + width > framebuffer_dirty_right) {
        framebuffer_dirty_right = x + width;
    }
    if (y + height > framebuffer_dirty_bottom) {
        framebuffer_dirty_bottom = y + height;
    }
}

static void put_pixel_untracked(uint32_t x, uint32_t y, uint32_t color) {
    size_t offset = (size_t)y * framebuffer_pitch +
        (size_t)x * framebuffer_bytes_per_pixel;
    uint8_t* shadow_pixel = framebuffer_shadow + offset;
    for (uint8_t byte = 0; byte < framebuffer_bytes_per_pixel; byte++) {
        shadow_pixel[byte] = (uint8_t)(color >> (byte * 8));
    }
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= framebuffer_width || y >= framebuffer_height) return;
    mark_framebuffer_dirty(x, y);
    put_pixel_untracked(x, y, color);
}

static void fill_rect_untracked(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint32_t color) {
    for (uint32_t row = y; row < y + height; row++) {
        uint8_t* pixel = framebuffer_shadow + (size_t)row *
            framebuffer_pitch + (size_t)x * framebuffer_bytes_per_pixel;
        for (uint32_t column = 0; column < width; column++) {
            for (uint8_t byte = 0; byte < framebuffer_bytes_per_pixel; byte++) {
                pixel[byte] = (uint8_t)(color >> (byte * 8));
            }
            pixel += framebuffer_bytes_per_pixel;
        }
    }
}

static void put_pixel_overlay(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= framebuffer_width || y >= framebuffer_height) return;
    volatile uint8_t* pixel = framebuffer +
        (size_t)y * framebuffer_pitch +
        (size_t)x * framebuffer_bytes_per_pixel;
    for (uint8_t byte = 0; byte < framebuffer_bytes_per_pixel; byte++) {
        pixel[byte] = (uint8_t)(color >> (byte * 8));
    }
}

static void draw_pixel_mouse_cursor(void) {
    static const uint16_t arrow[MOUSE_CURSOR_HEIGHT] = {
        0x800, 0xC00, 0xE00, 0xF00, 0xF80, 0xFC0,
        0xFE0, 0xFF0, 0xFF8, 0xFFC, 0xFE0, 0xDC0,
        0x8E0, 0x060, 0x070, 0x030, 0x030, 0x000
    };
    uint32_t white = framebuffer_color(COLOR_WHITE);
    uint32_t black = framebuffer_color(COLOR_BLACK);
    for (uint32_t row = 0; row < MOUSE_CURSOR_HEIGHT; row++) {
        for (uint32_t column = 0; column < MOUSE_CURSOR_WIDTH; column++) {
            uint16_t bit = (uint16_t)(1U <<
                (MOUSE_CURSOR_WIDTH - column - 1));
            if (!(arrow[row] & bit)) {
                continue;
            }
            bool edge = row == 0 || !(arrow[row - 1] & bit) ||
                row + 1 == MOUSE_CURSOR_HEIGHT ||
                    !(arrow[row + 1] & bit) ||
                column == 0 || !(arrow[row] & (bit << 1)) ||
                column + 1 == MOUSE_CURSOR_WIDTH ||
                    !(arrow[row] & (bit >> 1));
            put_pixel_overlay(mouse_pixel_x + column,
                mouse_pixel_y + row, edge ? black : white);
        }
    }
    mouse_pixel_visible = true;
}

static void erase_pixel_mouse_cursor(void) {
    if (!mouse_pixel_visible || !framebuffer_shadow) return;
    for (uint32_t row = 0; row < MOUSE_CURSOR_HEIGHT; row++) {
        uint32_t y = mouse_pixel_y + row;
        if (y >= framebuffer_height) continue;
        for (uint32_t column = 0; column < MOUSE_CURSOR_WIDTH; column++) {
            uint32_t x = mouse_pixel_x + column;
            if (x >= framebuffer_width) continue;
            size_t offset = (size_t)y * framebuffer_pitch +
                (size_t)x * framebuffer_bytes_per_pixel;
            volatile uint8_t* pixel = framebuffer + offset;
            const uint8_t* shadow_pixel = framebuffer_shadow + offset;
            for (uint8_t byte = 0; byte < framebuffer_bytes_per_pixel; byte++) {
                pixel[byte] = shadow_pixel[byte];
            }
        }
    }
    mouse_pixel_visible = false;
}

static void redraw_pixel_mouse_cursor(void) {
    if (!framebuffer_active || !framebuffer_shadow) return;
    erase_pixel_mouse_cursor();
    draw_pixel_mouse_cursor();
}

static void copy_to_framebuffer(volatile uint8_t* destination,
    const uint8_t* source, size_t byte_count) {
    size_t word_count = byte_count / sizeof(uint64_t);
    if (word_count != 0) {
        asm volatile("rep movsq"
            : "+D"(destination), "+S"(source), "+c"(word_count)
            :
            : "memory");
    }
    size_t remaining_bytes = byte_count % sizeof(uint64_t);
    while (remaining_bytes > 0) {
        *destination++ = *source++;
        remaining_bytes--;
    }
}

void display_present(void) {
    framebuffer_last_present_ticks = 0;
    framebuffer_last_present_cycles = 0;
    if (!framebuffer_active || !framebuffer_shadow || !framebuffer_dirty) {
        return;
    }

    uint64_t start_ticks = timer_get_ticks();
    uint64_t start_cycles = display_read_timestamp_counter();
    if (framebuffer_dirty_left == 0 && framebuffer_dirty_top == 0 &&
        framebuffer_dirty_right == framebuffer_width &&
        framebuffer_dirty_bottom == framebuffer_height) {
        size_t framebuffer_bytes =
            (size_t)framebuffer_pitch * framebuffer_height;
        copy_to_framebuffer(framebuffer, framebuffer_shadow,
            framebuffer_bytes);
    } else {
        size_t row_bytes = (size_t)(framebuffer_dirty_right -
            framebuffer_dirty_left) * framebuffer_bytes_per_pixel;
        size_t column_offset =
            (size_t)framebuffer_dirty_left * framebuffer_bytes_per_pixel;
        for (uint32_t y = framebuffer_dirty_top;
             y < framebuffer_dirty_bottom; y++) {
            size_t offset = (size_t)y * framebuffer_pitch + column_offset;
            copy_to_framebuffer(framebuffer + offset,
                framebuffer_shadow + offset, row_bytes);
        }
    }
    framebuffer_last_present_ticks = timer_get_ticks() - start_ticks;
    framebuffer_last_present_cycles =
        display_read_timestamp_counter() - start_cycles;
    framebuffer_dirty = false;
    if (mouse_pixel_visible) {
        draw_pixel_mouse_cursor();
    }
}

uint64_t display_get_last_present_ticks(void) {
    return framebuffer_last_present_ticks;
}

uint64_t display_get_last_present_cycles(void) {
    return framebuffer_last_present_cycles;
}

static const uint8_t* glyph_for(unsigned char character) {
    static const uint8_t space[5] = {0,0,0,0,0};
    static const uint8_t period[5] = {0,0x60,0x60,0,0};
    static const uint8_t comma[5] = {0,0x40,0x20,0,0};
    static const uint8_t colon[5] = {0,0x36,0x36,0,0};
    static const uint8_t semicolon[5] = {0,0x56,0x36,0,0};
    static const uint8_t dash[5] = {0x08,0x08,0x08,0x08,0x08};
    static const uint8_t underscore[5] = {0x40,0x40,0x40,0x40,0x40};
    static const uint8_t plus[5] = {0x08,0x08,0x3E,0x08,0x08};
    static const uint8_t equal[5] = {0x14,0x14,0x14,0x14,0x14};
    static const uint8_t slash[5] = {0x20,0x10,0x08,0x04,0x02};
    static const uint8_t backslash[5] = {0x02,0x04,0x08,0x10,0x20};
    static const uint8_t left_bracket[5] = {0,0x7F,0x41,0x41,0};
    static const uint8_t right_bracket[5] = {0,0x41,0x41,0x7F,0};
    static const uint8_t left_paren[5] = {0,0x1C,0x22,0x41,0};
    static const uint8_t right_paren[5] = {0,0x41,0x22,0x1C,0};
    static const uint8_t less_than[5] = {0x08,0x14,0x22,0x41,0};
    static const uint8_t greater_than[5] = {0,0x41,0x22,0x14,0x08};
    static const uint8_t exclamation[5] = {0,0,0x5F,0,0};
    static const uint8_t question[5] = {0x02,0x01,0x51,0x09,0x06};
    static const uint8_t quote[5] = {0,0x07,0,0x07,0};
    static const uint8_t apostrophe[5] = {0,0x07,0,0,0};
    static const uint8_t star[5] = {0x14,0x08,0x3E,0x08,0x14};
    static const uint8_t percent[5] = {0x63,0x13,0x08,0x64,0x63};
    static const uint8_t hash[5] = {0x14,0x7F,0x14,0x7F,0x14};
    static const uint8_t at[5] = {0x3E,0x41,0x5D,0x55,0x1E};
    static const uint8_t ampersand[5] = {0x36,0x49,0x55,0x22,0x50};
    static const uint8_t pipe[5] = {0,0,0x7F,0,0};
    static const uint8_t caret[5] = {0x04,0x02,0x01,0x02,0x04};
    static const uint8_t tilde[5] = {0x08,0x04,0x08,0x10,0x08};

    if (character >= 'a' && character <= 'z') {
        return font_lowercase[character - 'a'];
    }
    if (character >= 'A' && character <= 'Z') {
        return font_alphanumeric[character - 'A'];
    }
    if (character >= '0' && character <= '9') {
        return font_alphanumeric[26 + character - '0'];
    }
    switch (character) {
        case ' ': return space;
        case '.': return period;
        case ',': return comma;
        case ':': return colon;
        case ';': return semicolon;
        case '-': return dash;
        case '_': return underscore;
        case '+': return plus;
        case '=': return equal;
        case '/': return slash;
        case '\\': return backslash;
        case '[': return left_bracket;
        case ']': return right_bracket;
        case '(': return left_paren;
        case ')': return right_paren;
        case '<': return less_than;
        case '>': return greater_than;
        case '!': return exclamation;
        case '?': return question;
        case '"': return quote;
        case '\'': return apostrophe;
        case '*': return star;
        case '%': return percent;
        case '#': return hash;
        case '@': return at;
        case '&': return ampersand;
        case '|': return pipe;
        case '^': return caret;
        case '~': return tilde;
        default: return space;
    }
}

static void draw_framebuffer_character(uint32_t left, uint32_t top,
    uint8_t character, uint8_t foreground, uint8_t background,
    uint8_t scale) {
    uint32_t foreground_pixel = framebuffer_color(foreground);
    uint32_t background_pixel = framebuffer_color(background);
    const uint8_t* glyph = glyph_for(character);

    uint32_t scaled_width = CELL_WIDTH * scale;
    uint32_t scaled_height = CELL_HEIGHT * scale;
    for (uint32_t row = 0; row < scaled_height; row++) {
        for (uint32_t column = 0; column < scaled_width; column++) {
            bool set = false;
            if (column >= scale && column < 11 * scale &&
                row >= 5 * scale && row < 19 * scale) {
                uint32_t glyph_x = (column - scale) / (2 * scale);
                uint32_t glyph_y = (row - 5 * scale) / (2 * scale);
                set = (glyph[glyph_x] & (1U << glyph_y)) != 0;
            }
            put_pixel(left + column, top + row,
                set ? foreground_pixel : background_pixel);
        }
    }
}

static void draw_framebuffer_cell(uint8_t x, uint8_t y, uint16_t entry) {
    uint8_t colors = (uint8_t)(entry >> 8);
    draw_framebuffer_character(
        framebuffer_origin_x + (uint32_t)x * CELL_WIDTH,
        framebuffer_origin_y + (uint32_t)y * CELL_HEIGHT,
        (uint8_t)entry, colors & 0x0F, colors >> 4, 1);
}

static void render_cell(uint8_t x, uint8_t y) {
    uint16_t entry = text_buffer[(size_t)y * VGA_WIDTH + x];
    if (framebuffer_active) {
        draw_framebuffer_cell(x, y, entry);
    } else {
        VGA_MEMORY[(size_t)y * VGA_WIDTH + x] = entry;
    }
}

static void hide_mouse_cursor(void) {
    if (!mouse_cursor_visible) return;
    text_buffer[(size_t)mouse_cursor_y * VGA_WIDTH + mouse_cursor_x] =
        mouse_cursor_saved_entry;
    render_cell(mouse_cursor_x, mouse_cursor_y);
    mouse_cursor_visible = false;
}

static void show_mouse_cursor(void) {
    uint8_t color = (uint8_t)(mouse_cursor_saved_entry >> 8);
    uint8_t inverted_color = (uint8_t)((color << 4) | (color >> 4));
    render_cell(mouse_cursor_x, mouse_cursor_y);
    if (!framebuffer_active) {
        VGA_MEMORY[(size_t)mouse_cursor_y * VGA_WIDTH + mouse_cursor_x] =
            vga_entry((uint8_t)mouse_cursor_saved_entry, inverted_color);
    } else {
        draw_framebuffer_cell(mouse_cursor_x, mouse_cursor_y,
            vga_entry((uint8_t)mouse_cursor_saved_entry, inverted_color));
    }
    mouse_cursor_visible = true;
}

static void update_hardware_cursor(void) {
    if (framebuffer_active) return;
    uint16_t position = (uint16_t)(cursor_y * VGA_WIDTH + cursor_x);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)(position >> 8));
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)position);
}

static void redraw_all(void) {
    for (uint8_t y = 0; y < VGA_HEIGHT; y++) {
        for (uint8_t x = 0; x < VGA_WIDTH; x++) {
            render_cell(x, y);
        }
    }
}

static void scroll(void) {
    if (cursor_y < VGA_HEIGHT) return;
    for (uint8_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (uint8_t x = 0; x < VGA_WIDTH; x++) {
            text_buffer[(size_t)y * VGA_WIDTH + x] =
                text_buffer[(size_t)(y + 1) * VGA_WIDTH + x];
        }
    }
    uint16_t blank = vga_entry(' ', current_color);
    for (uint8_t x = 0; x < VGA_WIDTH; x++) {
        text_buffer[(size_t)(VGA_HEIGHT - 1) * VGA_WIDTH + x] = blank;
    }
    cursor_y = VGA_HEIGHT - 1;
    redraw_all();
}

void display_set_color(uint8_t foreground, uint8_t background) {
    current_color = (uint8_t)((foreground & 0x0F) |
        ((background & 0x0F) << 4));
}

void display_clear(void) {
    mouse_cursor_visible = false;
    uint16_t blank = vga_entry(' ', current_color);
    for (size_t index = 0; index < VGA_WIDTH * VGA_HEIGHT; index++) {
        text_buffer[index] = blank;
    }
    cursor_x = 0;
    cursor_y = 0;
    if (framebuffer_active) {
        uint32_t background = framebuffer_color(current_color >> 4);
        mark_framebuffer_dirty_rect(0, 0, framebuffer_width,
            framebuffer_height);
        fill_rect_untracked(0, 0, framebuffer_width, framebuffer_height,
            background);
        redraw_pixel_mouse_cursor();
    } else {
        redraw_all();
    }
    update_hardware_cursor();
}

bool display_init_framebuffer(uintptr_t boot_info_address) {
    serial_write_string("[fb] checking Multiboot framebuffer data\n");
    framebuffer_active = false;
    if (!boot_info_address) {
        framebuffer_status = "boot info missing";
        return false;
    }
    const multiboot1_info_t* info =
        (const multiboot1_info_t*)boot_info_address;
    serial_write_string("[fb] flags=");
    serial_write_hex(info->flags);
    serial_write_string(" address=");
    serial_write_hex(info->framebuffer_addr);
    serial_write_string(" pitch=");
    serial_write_dec(info->framebuffer_pitch);
    serial_write_string(" size=");
    serial_write_dec(info->framebuffer_width);
    serial_write_string("x");
    serial_write_dec(info->framebuffer_height);
    serial_write_string(" bpp=");
    serial_write_dec(info->framebuffer_bpp);
    serial_write_string(" type=");
    serial_write_dec(info->framebuffer_type);
    serial_write_string("\n");
    if (!(info->flags & MULTIBOOT1_INFO_FRAMEBUFFER)) {
        framebuffer_status = "bootloader did not provide one";
        return false;
    }
    if (info->framebuffer_type != 1) {
        framebuffer_status = info->framebuffer_type == 0
            ? "bootloader returned indexed pixels"
            : info->framebuffer_type == 2
                ? "bootloader returned text mode"
                : "unknown framebuffer format";
        return false;
    }
    if (info->framebuffer_bpp != 24 && info->framebuffer_bpp != 32) {
        framebuffer_status = "unsupported color depth";
        return false;
    }
    if (info->framebuffer_width < VGA_WIDTH * CELL_WIDTH ||
        info->framebuffer_height < VGA_HEIGHT * CELL_HEIGHT) {
        framebuffer_status = "resolution is below 960x600";
        return false;
    }

    uint8_t bytes_per_pixel = info->framebuffer_bpp / 8;
    uint64_t minimum_pitch =
        (uint64_t)info->framebuffer_width * bytes_per_pixel;
    uint64_t framebuffer_bytes =
        (uint64_t)info->framebuffer_pitch * info->framebuffer_height;
    uint8_t color_positions[3] = {
        info->framebuffer_color_info[0],
        info->framebuffer_color_info[2],
        info->framebuffer_color_info[4]
    };
    uint8_t color_sizes[3] = {
        info->framebuffer_color_info[1],
        info->framebuffer_color_info[3],
        info->framebuffer_color_info[5]
    };
    uint8_t missing_color_masks = 0;
    uint8_t missing_color_index = 0;
    for (uint8_t index = 0; index < 3; index++) {
        if (color_sizes[index] == 0) {
            missing_color_masks++;
            missing_color_index = index;
        }
    }
    if (info->framebuffer_bpp == 32 && missing_color_masks == 1 &&
        color_sizes[(missing_color_index + 1) % 3] == 8 &&
        color_sizes[(missing_color_index + 2) % 3] == 8 &&
        color_positions[missing_color_index] <= 24) {
        color_sizes[missing_color_index] = 8;
        serial_write_string("[fb] inferred missing 8-bit RGB mask at bit ");
        serial_write_dec(color_positions[missing_color_index]);
        serial_write_string("\n");
    }
    serial_write_string("[fb] minimum_pitch=");
    serial_write_dec(minimum_pitch);
    serial_write_string(" bytes=");
    serial_write_dec(framebuffer_bytes);
    serial_write_string(" rgb=");
    for (uint8_t index = 0; index < sizeof(info->framebuffer_color_info);
         index++) {
        if (index) {
            serial_write_string(",");
        }
        serial_write_dec(info->framebuffer_color_info[index]);
    }
    serial_write_string("\n");
    if (info->framebuffer_pitch < minimum_pitch ||
        framebuffer_bytes == 0 || framebuffer_bytes > FRAMEBUFFER_MAX_BYTES ||
        info->framebuffer_addr > UINT64_MAX - framebuffer_bytes ||
        color_sizes[0] == 0 || color_sizes[1] == 0 || color_sizes[2] == 0 ||
        color_sizes[0] > 8 || color_sizes[1] > 8 || color_sizes[2] > 8 ||
        (uint16_t)color_positions[0] + color_sizes[0] >
            info->framebuffer_bpp ||
        (uint16_t)color_positions[1] + color_sizes[1] >
            info->framebuffer_bpp ||
        (uint16_t)color_positions[2] + color_sizes[2] >
            info->framebuffer_bpp) {
        framebuffer_status = "invalid framebuffer layout";
        return false;
    }
    uint32_t color_masks[3];
    for (uint8_t index = 0; index < 3; index++) {
        color_masks[index] = ((1U << color_sizes[index]) - 1U) <<
            color_positions[index];
    }
    if ((color_masks[0] & color_masks[1]) ||
        (color_masks[0] & color_masks[2]) ||
        (color_masks[1] & color_masks[2])) {
        framebuffer_status = "overlapping RGB channels";
        return false;
    }

    size_t page_count = (size_t)((framebuffer_bytes + PMM_PAGE_SIZE - 1) /
        PMM_PAGE_SIZE);
    uint8_t* shadow = (uint8_t*)pmm_alloc(page_count);
    if (!shadow) {
        framebuffer_status = "unable to allocate framebuffer buffer";
        return false;
    }

    serial_write_string("[fb] shadow allocated; mapping framebuffer MMIO\n");
    uintptr_t mapped_address;
    if (!paging_map_kernel_mmio((uintptr_t)info->framebuffer_addr,
            (size_t)framebuffer_bytes, &mapped_address)) {
        pmm_free(shadow, page_count);
        framebuffer_status = "unable to map framebuffer memory";
        return false;
    }

    framebuffer = (volatile uint8_t*)mapped_address;
    framebuffer_shadow = shadow;
    for (size_t index = 0; index < (size_t)framebuffer_bytes; index++) {
        framebuffer_shadow[index] = 0;
    }
    framebuffer_width = info->framebuffer_width;
    framebuffer_height = info->framebuffer_height;
    framebuffer_pitch = info->framebuffer_pitch;
    framebuffer_bytes_per_pixel = bytes_per_pixel;
    framebuffer_red_position = color_positions[0];
    framebuffer_red_size = color_sizes[0];
    framebuffer_green_position = color_positions[1];
    framebuffer_green_size = color_sizes[1];
    framebuffer_blue_position = color_positions[2];
    framebuffer_blue_size = color_sizes[2];
    framebuffer_dirty = false;
    framebuffer_origin_x =
        (framebuffer_width - VGA_WIDTH * CELL_WIDTH) / 2;
    framebuffer_origin_y =
        (framebuffer_height - VGA_HEIGHT * CELL_HEIGHT) / 2;
    framebuffer_active = true;
    mouse_pixel_x = 0;
    mouse_pixel_y = 0;
    mouse_pixel_visible = false;
    serial_write_string("[fb] mapping succeeded; clearing framebuffer\n");
    display_clear();
    display_present();
    serial_write_string("[fb] framebuffer clear completed\n");
    framebuffer_status = "enabled";
    return true;
}

const char* display_framebuffer_status(void) {
    return framebuffer_status;
}

bool display_get_framebuffer_size(uint32_t* width, uint32_t* height) {
    if (!framebuffer_active || !width || !height) return false;
    *width = framebuffer_width;
    *height = framebuffer_height;
    return true;
}

bool display_draw_pixel(uint32_t x, uint32_t y, uint8_t color) {
    if (!framebuffer_active || x >= framebuffer_width ||
        y >= framebuffer_height) {
        return false;
    }
    put_pixel(x, y, framebuffer_color(color));
    redraw_pixel_mouse_cursor();
    return true;
}

bool display_fill_rect(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint8_t color) {
    if (!framebuffer_active || width == 0 || height == 0 ||
        x >= framebuffer_width || y >= framebuffer_height ||
        width > framebuffer_width - x || height > framebuffer_height - y) {
        return false;
    }
    uint32_t pixel = framebuffer_color(color);
    mark_framebuffer_dirty_rect(x, y, width, height);
    fill_rect_untracked(x, y, width, height, pixel);
    redraw_pixel_mouse_cursor();
    return true;
}

bool display_fill_rect_rgb(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint8_t red, uint8_t green, uint8_t blue) {
    if (!framebuffer_active || width == 0 || height == 0 ||
        x >= framebuffer_width || y >= framebuffer_height ||
        width > framebuffer_width - x || height > framebuffer_height - y) {
        return false;
    }
    uint32_t pixel = channel_value(red, framebuffer_red_position,
            framebuffer_red_size) |
        channel_value(green, framebuffer_green_position,
            framebuffer_green_size) |
        channel_value(blue, framebuffer_blue_position,
            framebuffer_blue_size);
    mark_framebuffer_dirty_rect(x, y, width, height);
    fill_rect_untracked(x, y, width, height, pixel);
    redraw_pixel_mouse_cursor();
    return true;
}

static uint32_t shadow_pixel_value(size_t offset) {
    if (framebuffer_bytes_per_pixel == 4) {
        const uint8_t* pixel = framebuffer_shadow + offset;
        return (uint32_t)pixel[0] |
            ((uint32_t)pixel[1] << 8) |
            ((uint32_t)pixel[2] << 16) |
            ((uint32_t)pixel[3] << 24);
    }
    uint32_t pixel = 0;
    for (uint8_t byte = 0; byte < framebuffer_bytes_per_pixel; byte++) {
        pixel |= (uint32_t)framebuffer_shadow[offset + byte] << (byte * 8);
    }
    return pixel;
}

static uint8_t shadow_channel_value(uint32_t pixel, uint8_t position,
    uint8_t size) {
    if (size == 8) return (uint8_t)(pixel >> position);
    uint32_t maximum = (1U << size) - 1U;
    uint32_t value = (pixel >> position) & maximum;
    return (uint8_t)((value * 255U + maximum / 2U) / maximum);
}

static uint8_t blend_channel(uint8_t foreground, uint8_t background,
    uint8_t alpha, uint16_t inverse_alpha) {
    uint32_t value = (uint32_t)foreground * alpha +
        (uint32_t)background * inverse_alpha + 128U;
    return (uint8_t)((value + (value >> 8)) >> 8);
}

bool display_blend_rect_rgb(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint8_t red, uint8_t green, uint8_t blue,
    uint8_t alpha) {
    if (!framebuffer_active || !framebuffer_shadow || width == 0 ||
        height == 0 || x >= framebuffer_width || y >= framebuffer_height ||
        width > framebuffer_width - x || height > framebuffer_height - y) {
        return false;
    }
    uint16_t inverse_alpha = 255U - alpha;
    bool fast_rgb = framebuffer_red_size == 8 &&
        framebuffer_green_size == 8 && framebuffer_blue_size == 8;
    mark_framebuffer_dirty_rect(x, y, width, height);
    for (uint32_t row = y; row < y + height; row++) {
        for (uint32_t column = x; column < x + width; column++) {
            size_t offset = (size_t)row * framebuffer_pitch +
                (size_t)column * framebuffer_bytes_per_pixel;
            uint32_t old_pixel = shadow_pixel_value(offset);
            uint8_t old_red = shadow_channel_value(old_pixel,
                framebuffer_red_position, framebuffer_red_size);
            uint8_t old_green = shadow_channel_value(old_pixel,
                framebuffer_green_position, framebuffer_green_size);
            uint8_t old_blue = shadow_channel_value(old_pixel,
                framebuffer_blue_position, framebuffer_blue_size);
            uint8_t new_red;
            uint8_t new_green;
            uint8_t new_blue;
            uint32_t new_pixel;
            if (fast_rgb) {
                new_red = blend_channel(red, old_red, alpha, inverse_alpha);
                new_green = blend_channel(green, old_green, alpha,
                    inverse_alpha);
                new_blue = blend_channel(blue, old_blue, alpha,
                    inverse_alpha);
                new_pixel = ((uint32_t)new_red << framebuffer_red_position) |
                    ((uint32_t)new_green << framebuffer_green_position) |
                    ((uint32_t)new_blue << framebuffer_blue_position);
            } else {
                new_red = (uint8_t)((red * alpha +
                    old_red * inverse_alpha + 127U) / 255U);
                new_green = (uint8_t)((green * alpha +
                    old_green * inverse_alpha + 127U) / 255U);
                new_blue = (uint8_t)((blue * alpha +
                    old_blue * inverse_alpha + 127U) / 255U);
                new_pixel = channel_value(new_red,
                        framebuffer_red_position, framebuffer_red_size) |
                    channel_value(new_green, framebuffer_green_position,
                        framebuffer_green_size) |
                    channel_value(new_blue, framebuffer_blue_position,
                        framebuffer_blue_size);
            }
            put_pixel_untracked(column, row, new_pixel);
        }
    }
    redraw_pixel_mouse_cursor();
    return true;
}

static bool draw_text_scaled(uint32_t x, uint32_t y, const char* text,
    uint8_t foreground, uint8_t background, uint8_t scale) {
    if (!framebuffer_active || !text || scale == 0) return false;
    uint32_t origin_x = x;
    uint32_t cell_width = CELL_WIDTH * scale;
    uint32_t cell_height = CELL_HEIGHT * scale;
    while (*text) {
        if (*text == '\n') {
            x = origin_x;
            if (y > UINT32_MAX - cell_height) return false;
            y += cell_height;
        } else if (*text != '\r') {
            if (x > UINT32_MAX - cell_width ||
                y > UINT32_MAX - cell_height) {
                return false;
            }
            if (x < framebuffer_width && y < framebuffer_height) {
                draw_framebuffer_character(x, y, (uint8_t)*text,
                    foreground & 0x0F, background & 0x0F, scale);
            }
            x += cell_width;
        }
        text++;
    }
    redraw_pixel_mouse_cursor();
    return true;
}

bool display_draw_text(uint32_t x, uint32_t y, const char* text,
    uint8_t foreground, uint8_t background) {
    return draw_text_scaled(x, y, text, foreground, background, 1);
}

static void draw_ui_character(uint32_t x, uint32_t y, uint8_t character,
    uint8_t foreground) {
    if (character < ROCKOS_UI_FONT_FIRST_CHAR ||
        character >= ROCKOS_UI_FONT_FIRST_CHAR + ROCKOS_UI_FONT_CHAR_COUNT) {
        character = '?';
    }
    size_t glyph_offset = (size_t)(character - ROCKOS_UI_FONT_FIRST_CHAR) *
        ROCKOS_UI_FONT_CELL_WIDTH * ROCKOS_UI_FONT_HEIGHT;
    for (uint32_t row = 0; row < ROCKOS_UI_FONT_HEIGHT; row++) {
        for (uint32_t column = 0; column < ROCKOS_UI_FONT_CELL_WIDTH;
             column++) {
            size_t pixel_index = glyph_offset +
                (size_t)row * ROCKOS_UI_FONT_CELL_WIDTH + column;
            uint8_t coverage = coverage_at(rockos_ui_font_2bpp,
                pixel_index);
            if (coverage == 0) continue;
            blend_foreground_over_pixel(x + column, y + row,
                foreground, coverage);
        }
    }
}

bool display_draw_text_ui(uint32_t x, uint32_t y, const char* text,
    uint8_t foreground) {
    if (!framebuffer_active || !text) return false;
    uint32_t origin_x = x;
    int64_t pen_x_q =
        (int64_t)x * ROCKOS_UI_FONT_ADVANCE_SCALE;
    uint8_t previous_character = 0;
    while (*text) {
        if (*text == '\n') {
            if (y > UINT32_MAX - ROCKOS_UI_FONT_HEIGHT) return false;
            y += ROCKOS_UI_FONT_HEIGHT;
            pen_x_q = (int64_t)origin_x * ROCKOS_UI_FONT_ADVANCE_SCALE;
            previous_character = 0;
        } else if (*text != '\r') {
            uint8_t character = (uint8_t)*text;
            if (character < ROCKOS_UI_FONT_FIRST_CHAR ||
                character >= ROCKOS_UI_FONT_FIRST_CHAR +
                    ROCKOS_UI_FONT_CHAR_COUNT) {
                character = '?';
            }
            uint8_t character_index =
                character - ROCKOS_UI_FONT_FIRST_CHAR;
            if (previous_character) {
                uint8_t previous_index =
                    previous_character - ROCKOS_UI_FONT_FIRST_CHAR;
                int8_t kerning_q = rockos_ui_font_kerning_q[
                    (size_t)previous_index * ROCKOS_UI_FONT_CHAR_COUNT +
                    character_index];
                pen_x_q += kerning_q;
            }
            if (pen_x_q < 0 || pen_x_q > (int64_t)UINT32_MAX *
                    ROCKOS_UI_FONT_ADVANCE_SCALE) {
                return false;
            }
            uint32_t draw_x = (uint32_t)((pen_x_q +
                ROCKOS_UI_FONT_ADVANCE_SCALE / 2) /
                ROCKOS_UI_FONT_ADVANCE_SCALE);
            if (draw_x < framebuffer_width && y < framebuffer_height) {
                draw_ui_character(draw_x, y, character, foreground);
            }
            pen_x_q += rockos_ui_font_advance_q[character_index];
            previous_character = character;
        }
        text++;
    }
    redraw_pixel_mouse_cursor();
    return true;
}

static void blend_foreground_over_pixel(uint32_t x, uint32_t y,
    uint8_t foreground, uint8_t coverage) {
    if (x >= framebuffer_width || y >= framebuffer_height) return;
    size_t offset = (size_t)y * framebuffer_pitch +
        (size_t)x * framebuffer_bytes_per_pixel;
    uint32_t old_pixel = shadow_pixel_value(offset);
    uint8_t old_red = shadow_channel_value(old_pixel,
        framebuffer_red_position, framebuffer_red_size);
    uint8_t old_green = shadow_channel_value(old_pixel,
        framebuffer_green_position, framebuffer_green_size);
    uint8_t old_blue = shadow_channel_value(old_pixel,
        framebuffer_blue_position, framebuffer_blue_size);
    uint8_t foreground_red = color_rgb[foreground & 0x0F][0];
    uint8_t foreground_green = color_rgb[foreground & 0x0F][1];
    uint8_t foreground_blue = color_rgb[foreground & 0x0F][2];
    uint8_t red = (uint8_t)((foreground_red * coverage +
        old_red * (3 - coverage) + 1) / 3);
    uint8_t green = (uint8_t)((foreground_green * coverage +
        old_green * (3 - coverage) + 1) / 3);
    uint8_t blue = (uint8_t)((foreground_blue * coverage +
        old_blue * (3 - coverage) + 1) / 3);
    uint32_t pixel;
    if (framebuffer_red_size == 8 && framebuffer_green_size == 8 &&
        framebuffer_blue_size == 8) {
        pixel = ((uint32_t)red << framebuffer_red_position) |
            ((uint32_t)green << framebuffer_green_position) |
            ((uint32_t)blue << framebuffer_blue_position);
    } else {
        pixel = channel_value(red, framebuffer_red_position,
                framebuffer_red_size) |
            channel_value(green, framebuffer_green_position,
                framebuffer_green_size) |
            channel_value(blue, framebuffer_blue_position,
                framebuffer_blue_size);
    }
    put_pixel(x, y, pixel);
}

bool display_draw_mask_bitmap(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, const uint8_t* coverage_2bpp, uint8_t foreground) {
    if (!framebuffer_active || !coverage_2bpp || width == 0 || height == 0 ||
        x >= framebuffer_width || y >= framebuffer_height ||
        width > framebuffer_width - x || height > framebuffer_height - y ||
        (uint64_t)width * height > SIZE_MAX - 3) {
        return false;
    }
    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t column = 0; column < width; column++) {
            uint8_t coverage = coverage_at(coverage_2bpp,
                (size_t)row * width + column);
            if (coverage) {
                blend_foreground_over_pixel(x + column, y + row,
                    foreground, coverage);
            }
        }
    }
    redraw_pixel_mouse_cursor();
    return true;
}

bool display_draw_rockos_logo(uint32_t x, uint32_t y) {
    return display_draw_mask_bitmap(x, y, ROCKOS_LOGO_WIDTH,
        ROCKOS_LOGO_HEIGHT, rockos_logo_2bpp, COLOR_WHITE);
}

bool display_draw_text_scaled(uint32_t x, uint32_t y, const char* text,
    uint8_t foreground, uint8_t background, uint8_t scale) {
    return draw_text_scaled(x, y, text, foreground, background, scale);
}

static int64_t triangle_edge(int64_t ax, int64_t ay, int64_t bx,
    int64_t by, int64_t px, int64_t py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

bool display_fill_triangle(uint32_t x0, uint32_t y0, uint32_t x1,
    uint32_t y1, uint32_t x2, uint32_t y2, uint8_t color) {
    if (!framebuffer_active ||
        x0 >= framebuffer_width || x1 >= framebuffer_width ||
        x2 >= framebuffer_width || y0 >= framebuffer_height ||
        y1 >= framebuffer_height || y2 >= framebuffer_height) {
        return false;
    }
    int64_t area = triangle_edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return false;

    uint32_t min_x = x0 < x1 ? x0 : x1;
    if (x2 < min_x) min_x = x2;
    uint32_t max_x = x0 > x1 ? x0 : x1;
    if (x2 > max_x) max_x = x2;
    uint32_t min_y = y0 < y1 ? y0 : y1;
    if (y2 < min_y) min_y = y2;
    uint32_t max_y = y0 > y1 ? y0 : y1;
    if (y2 > max_y) max_y = y2;
    if (min_x >= framebuffer_width || min_y >= framebuffer_height) {
        return true;
    }
    if (max_x >= framebuffer_width) max_x = framebuffer_width - 1;
    if (max_y >= framebuffer_height) max_y = framebuffer_height - 1;

    uint32_t pixel = framebuffer_color(color);
    for (uint32_t y = min_y; y <= max_y; y++) {
        for (uint32_t x = min_x; x <= max_x; x++) {
            int64_t e0 = triangle_edge(x0, y0, x1, y1, x, y);
            int64_t e1 = triangle_edge(x1, y1, x2, y2, x, y);
            int64_t e2 = triangle_edge(x2, y2, x0, y0, x, y);
            if ((area > 0 && e0 >= 0 && e1 >= 0 && e2 >= 0) ||
                (area < 0 && e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                put_pixel(x, y, pixel);
            }
        }
    }
    redraw_pixel_mouse_cursor();
    return true;
}

bool display_set_mouse_position(uint32_t x, uint32_t y) {
    if (!framebuffer_active || !framebuffer_shadow) return false;
    uint32_t clamped_x = x < framebuffer_width ? x : framebuffer_width - 1;
    uint32_t clamped_y = y < framebuffer_height ? y : framebuffer_height - 1;
    if (mouse_pixel_visible && mouse_pixel_x == clamped_x &&
        mouse_pixel_y == clamped_y) {
        return true;
    }
    erase_pixel_mouse_cursor();
    mouse_pixel_x = clamped_x;
    mouse_pixel_y = clamped_y;
    draw_pixel_mouse_cursor();
    return true;
}

void display_set_mouse_cursor(uint8_t x, uint8_t y) {
    if (x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    hide_mouse_cursor();
    mouse_cursor_x = x;
    mouse_cursor_y = y;
    mouse_cursor_saved_entry = text_buffer[(size_t)y * VGA_WIDTH + x];
    show_mouse_cursor();
}

void display_set_cursor(uint8_t x, uint8_t y) {
    if (x < VGA_WIDTH) cursor_x = x;
    if (y < VGA_HEIGHT) cursor_y = y;
    update_hardware_cursor();
}

void display_write_at(uint8_t x, uint8_t y, const char* string) {
    if (!string || x >= VGA_WIDTH || y >= VGA_HEIGHT) return;
    bool redraw_mouse = mouse_cursor_visible;
    uint8_t saved_x = mouse_cursor_x;
    uint8_t saved_y = mouse_cursor_y;
    hide_mouse_cursor();
    while (*string && x < VGA_WIDTH) {
        text_buffer[(size_t)y * VGA_WIDTH + x] =
            vga_entry((uint8_t)*string++, current_color);
        render_cell(x, y);
        x++;
    }
    if (redraw_mouse) {
        mouse_cursor_x = saved_x;
        mouse_cursor_y = saved_y;
        mouse_cursor_saved_entry =
            text_buffer[(size_t)saved_y * VGA_WIDTH + saved_x];
        show_mouse_cursor();
    }
    redraw_pixel_mouse_cursor();
}

void display_putchar(char character) {
    bool redraw_mouse = mouse_cursor_visible;
    uint8_t saved_x = mouse_cursor_x;
    uint8_t saved_y = mouse_cursor_y;
    hide_mouse_cursor();
    if (character == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (character == '\r') {
        cursor_x = 0;
    } else if (character == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            text_buffer[(size_t)cursor_y * VGA_WIDTH + cursor_x] =
                vga_entry(' ', current_color);
            render_cell(cursor_x, cursor_y);
        }
    } else if (character == '\t') {
        cursor_x = (uint8_t)((cursor_x + 4) & ~3U);
    } else {
        text_buffer[(size_t)cursor_y * VGA_WIDTH + cursor_x] =
            vga_entry((uint8_t)character, current_color);
        render_cell(cursor_x, cursor_y);
        cursor_x++;
    }

    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }
    scroll();
    update_hardware_cursor();
    if (redraw_mouse) {
        mouse_cursor_x = saved_x;
        mouse_cursor_y = saved_y;
        mouse_cursor_saved_entry =
            text_buffer[(size_t)saved_y * VGA_WIDTH + saved_x];
        show_mouse_cursor();
    }
    redraw_pixel_mouse_cursor();
}

void display_puts(const char* string) {
    if (!string) return;
    while (*string) display_putchar(*string++);
}

void display_init(void) {
    framebuffer_active = false;
    framebuffer = NULL;
    cursor_x = 0;
    cursor_y = 0;
    display_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    display_clear();
}

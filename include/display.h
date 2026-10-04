#ifndef ROCKOS_DISPLAY_H
#define ROCKOS_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#define COLOR_BLACK         0
#define COLOR_BLUE          1
#define COLOR_GREEN         2
#define COLOR_CYAN          3
#define COLOR_RED           4
#define COLOR_MAGENTA       5
#define COLOR_BROWN         6
#define COLOR_LIGHT_GRAY    7
#define COLOR_DARK_GRAY     8
#define COLOR_LIGHT_BLUE    9
#define COLOR_LIGHT_GREEN   10
#define COLOR_LIGHT_CYAN    11
#define COLOR_LIGHT_RED     12
#define COLOR_LIGHT_MAGENTA 13
#define COLOR_YELLOW        14
#define COLOR_WHITE         15

#define DISPLAY_ROCKOS_LOGO_WIDTH 480U
#define DISPLAY_ROCKOS_LOGO_HEIGHT 116U
#define DISPLAY_UI_TEXT_HEIGHT 20U

void display_init(void);
bool display_init_framebuffer(uintptr_t boot_info_address);
const char* display_framebuffer_status(void);
bool display_get_framebuffer_size(uint32_t* width, uint32_t* height);
void display_present(void);
uint64_t display_get_last_present_ticks(void);
uint64_t display_get_last_present_cycles(void);
bool display_draw_pixel(uint32_t x, uint32_t y, uint8_t color);
bool display_fill_rect(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint8_t color);
bool display_fill_rect_rgb(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint8_t red, uint8_t green, uint8_t blue);
bool display_blend_rect_rgb(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, uint8_t red, uint8_t green, uint8_t blue,
    uint8_t alpha);
bool display_draw_text(uint32_t x, uint32_t y, const char* text,
    uint8_t foreground, uint8_t background);
bool display_draw_text_ui(uint32_t x, uint32_t y, const char* text,
    uint8_t foreground);
bool display_draw_text_scaled(uint32_t x, uint32_t y, const char* text,
    uint8_t foreground, uint8_t background, uint8_t scale);
bool display_draw_mask_bitmap(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height, const uint8_t* coverage_2bpp, uint8_t foreground);
bool display_draw_rockos_logo(uint32_t x, uint32_t y);
bool display_fill_triangle(uint32_t x0, uint32_t y0, uint32_t x1,
    uint32_t y1, uint32_t x2, uint32_t y2, uint8_t color);
void display_clear(void);
void display_set_color(uint8_t fg, uint8_t bg);
void display_putchar(char c);
void display_puts(const char* str);
void display_set_cursor(uint8_t x, uint8_t y);
void display_write_at(uint8_t x, uint8_t y, const char* str);
void display_set_mouse_cursor(uint8_t x, uint8_t y);
bool display_set_mouse_position(uint32_t x, uint32_t y);

#endif // ROCKOS_DISPLAY_H
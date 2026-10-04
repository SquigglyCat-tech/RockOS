#include "shell.h"

#include "display.h"
#include "audio.h"
#include "acpi.h"
#include "filesystem.h"
#include "io.h"
#include "multiboot1.h"
#include "pci.h"
#include "pic.h"
#include "pmm.h"
#include "ring3.h"
#include "serial.h"
#include "storage.h"
#include "task.h"
#include "timer.h"
#include "tour.h"

#include <stddef.h>
#include <stdint.h>

#define SHELL_COMMAND_CAPACITY 64
#define SHELL_COPY_CAPACITY 71680U
#define SHELL_TICKS_PER_SECOND 100ULL
#define ROCKOS_VERSION "0.1"
#define SHELL_EDITOR_FIRST_ROW 3
#define SHELL_EDITOR_VISIBLE_ROWS 20
#define SHELL_INSTALLER_IMAGE_ID "ROCKOS_INSTALL_IMAGE"
#define SHELL_INSTALLER_TRANSFER_SECTORS BLOCK_MAX_TRANSFER_SECTORS
#define SHELL_INSTALLER_TRANSFER_BYTES \
    (BLOCK_SECTOR_SIZE * SHELL_INSTALLER_TRANSFER_SECTORS)
#define SHELL_INSTALLER_PANEL_LEFT 36U
#define SHELL_INSTALLER_PANEL_TOP 28U
#define SHELL_INSTALLER_PANEL_RIGHT 36U
#define SHELL_INSTALLER_PANEL_BOTTOM 28U
#define SHELL_INSTALLER_NEXT_WIDTH 104U
#define SHELL_INSTALLER_NEXT_HEIGHT 36U
#define SHELL_INSTALLER_NEXT_RIGHT_INSET 28U
#define SHELL_INSTALLER_NEXT_BOTTOM_INSET 28U
#define SHELL_INSTALLER_COPYRIGHT_LEFT_INSET 24U
#define SHELL_INSTALLER_DIALOG_PANEL_LEFT 104U
#define SHELL_INSTALLER_DIALOG_PANEL_TOP 104U
#define SHELL_INSTALLER_DIALOG_PANEL_RIGHT 104U
#define SHELL_INSTALLER_DIALOG_PANEL_BOTTOM 64U
#define SHELL_INSTALLER_DIALOG_BUTTON_RIGHT_INSET 28U
#define SHELL_INSTALLER_DIALOG_BUTTON_BOTTOM_INSET 24U
#define SHELL_INSTALLER_NAV_BUTTON_GAP 16U
#define SHELL_INSTALLER_BACK_BUTTON_WIDTH 124U
#define SHELL_INSTALLER_FORWARD_BUTTON_WIDTH 114U
#define SHELL_INSTALLER_NAV_BUTTON_HEIGHT 40U
#define SHELL_INSTALLER_VALUE_X 324U
#define SHELL_INSTALLER_PROGRESS_VALUE_X 360U
#define SHELL_INSTALLER_PROGRESS_UPDATE_SECTORS 512ULL
#define SHELL_INSTALLER_PROGRESS_VALUE_WIDTH 280U
#define SHELL_INSTALLER_PROGRESS_VALUE_HEIGHT DISPLAY_UI_TEXT_HEIGHT
#define SHELL_INSTALLER_PANEL_TEXT_RED 25U
#define SHELL_INSTALLER_PANEL_TEXT_GREEN 27U
#define SHELL_INSTALLER_PANEL_TEXT_BLUE 30U

enum {
    SHELL_INSTALLER_PAGE_PREFERENCES = 0,
    SHELL_INSTALLER_PAGE_TARGET,
    SHELL_INSTALLER_PAGE_CONFIRM,
    SHELL_INSTALLER_PAGE_RESULT
};

typedef struct {
    char vendor[13];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
} cpu_info_t;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} shell_ui_rect_t;

typedef enum {
    SHELL_INSTALLER_BUTTON_NONE = 0,
    SHELL_INSTALLER_BUTTON_NEXT,
    SHELL_INSTALLER_BUTTON_BACK,
    SHELL_INSTALLER_BUTTON_CANCEL,
    SHELL_INSTALLER_BUTTON_INSTALL,
    SHELL_INSTALLER_BUTTON_FINISH
} shell_installer_button_t;

typedef enum {
    SHELL_INSTALLER_ACTION_NEXT,
    SHELL_INSTALLER_ACTION_BACK,
    SHELL_INSTALLER_ACTION_CANCEL,
    SHELL_INSTALLER_ACTION_CONFIRM,
    SHELL_INSTALLER_ACTION_INSTALL,
    SHELL_INSTALLER_ACTION_FINISH
} shell_installer_action_t;

static char command_buffer[SHELL_COMMAND_CAPACITY];
static uint8_t shell_copy_buffer[SHELL_COPY_CAPACITY];
static size_t command_length;
static bool shell_editor_active;
static char shell_editor_path[FS_PATH_MAX];
static uint32_t shell_editor_length;
static bool shell_editor_modified;
static bool shell_editor_buffer_full;
static bool shell_installer_active;
static bool shell_installer_confirmed;
static uint8_t shell_installer_page;
static uint8_t shell_installer_field;
static uint8_t shell_installer_language;
static uint8_t shell_installer_region;
static uint8_t shell_installer_keyboard;
static shell_installer_button_t shell_installer_pressed_button;
static uint8_t shell_installer_pressed_page;
static bool shell_profile_enabled;
static bool shell_profile_active;
static uint64_t shell_profile_page_start;
static uint64_t shell_profile_page_start_cycles;
static uint64_t shell_profile_text_cycles;
static uint64_t shell_profile_logo_cycles;
static uint32_t shell_profile_text_calls;
static const char* shell_profile_page_name;
static uintptr_t shell_installer_image;
static uint32_t shell_installer_image_size;
static uint8_t shell_installer_transfer_buffer[SHELL_INSTALLER_TRANSFER_BYTES];

static void shell_print_filesystem_error(int result);

static void shell_write(const char* text) {
    display_puts(text);
}

static void shell_write_decimal(uint64_t value) {
    char digits[20];
    size_t count = 0;

    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < sizeof(digits));

    while (count > 0) {
        display_putchar(digits[--count]);
    }
}

static void shell_write_hex_digits(uint64_t value, unsigned width) {
    static const char digits[] = "0123456789ABCDEF";
    for (unsigned index = width; index > 0; index--) {
        unsigned shift = (index - 1) * 4;
        display_putchar(digits[(value >> shift) & 0xF]);
    }
}

static void shell_write_hex(uint64_t value, unsigned width) {
    display_puts("0x");
    shell_write_hex_digits(value, width);
}

static bool command_is(const char* command, const char* expected) {
    while (*command == ' ') command++;
    while (*command && *expected && *command == *expected) {
        command++;
        expected++;
    }
    while (*command == ' ') command++;
    return *command == '\0' && *expected == '\0';
}

static bool shell_get_argument(const char* command, const char* name,
    char* argument, size_t argument_capacity) {
    while (*command == ' ') command++;
    while (*command && *name && *command == *name) {
        command++;
        name++;
    }
    if (*name != '\0' || (*command != '\0' && *command != ' ')) return false;
    while (*command == ' ') command++;
    if (*command == '\0') return false;

    size_t argument_length = 0;
    while (command[argument_length] != '\0' && command[argument_length] != ' ') {
        if (argument_length + 1 >= argument_capacity) return false;
        argument[argument_length] = command[argument_length];
        argument_length++;
    }
    argument[argument_length] = '\0';
    while (command[argument_length] == ' ') argument_length++;
    return command[argument_length] == '\0';
}

static bool shell_get_two_arguments(const char* command, const char* name,
    char* first_argument, char* second_argument, size_t argument_capacity) {
    while (*command == ' ') command++;
    while (*command && *name && *command == *name) {
        command++;
        name++;
    }
    if (*name != '\0' || (*command != '\0' && *command != ' ')) return false;
    while (*command == ' ') command++;

    size_t first_length = 0;
    while (command[first_length] != '\0' && command[first_length] != ' ') {
        if (first_length + 1 >= argument_capacity) return false;
        first_argument[first_length] = command[first_length];
        first_length++;
    }
    if (first_length == 0) return false;
    first_argument[first_length] = '\0';

    command += first_length;
    while (*command == ' ') command++;
    size_t second_length = 0;
    while (command[second_length] != '\0' && command[second_length] != ' ') {
        if (second_length + 1 >= argument_capacity) return false;
        second_argument[second_length] = command[second_length];
        second_length++;
    }
    if (second_length == 0) return false;
    second_argument[second_length] = '\0';

    command += second_length;
    while (*command == ' ') command++;
    return *command == '\0';
}

static bool shell_get_path_and_text(const char* command, const char* name,
    char* path, size_t path_capacity, const char** text) {
    while (*command == ' ') command++;
    while (*command && *name && *command == *name) {
        command++;
        name++;
    }
    if (*name != '\0' || (*command != '\0' && *command != ' ')) return false;
    while (*command == ' ') command++;
    if (*command == '\0') return false;

    size_t path_length = 0;
    while (command[path_length] != '\0' && command[path_length] != ' ') {
        if (path_length + 1 >= path_capacity) return false;
        path[path_length] = command[path_length];
        path_length++;
    }
    if (path_length == 0 || command[path_length] != ' ') return false;
    path[path_length] = '\0';

    command += path_length;
    while (*command == ' ') command++;
    if (*command == '\0') return false;
    *text = command;
    return true;
}

static void shell_draw_prompt(void) {
    display_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    shell_write("ROK>");
    display_set_color(COLOR_WHITE, COLOR_BLACK);
}

static bool shell_get_graphics_size(uint32_t* width, uint32_t* height) {
    return display_get_framebuffer_size(width, height);
}

static uint64_t shell_profile_read_timestamp_counter(void) {
    uint32_t low;
    uint32_t high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static uint64_t shell_profile_cycles_to_ms(uint64_t cycles,
    uint64_t page_cycles, uint64_t page_ticks) {
    if (page_cycles == 0) return 0;
    uint64_t page_milliseconds = page_ticks * 10;
    return (cycles * page_milliseconds + page_cycles / 2) / page_cycles;
}

static void shell_ui_text(uint32_t x, uint32_t y, const char* text,
    uint8_t foreground, uint8_t background) {
    /* UI glyph edges blend over the existing surface to preserve its glass effect. */
    (void)background;
    uint64_t start_cycles = shell_profile_active
        ? shell_profile_read_timestamp_counter() : 0;
    display_draw_text_ui(x, y, text, foreground);
    if (shell_profile_active) {
        shell_profile_text_cycles +=
            shell_profile_read_timestamp_counter() - start_cycles;
        shell_profile_text_calls++;
    }
}

static void shell_profile_page_begin(const char* page_name) {
    shell_profile_active = shell_profile_enabled;
    if (!shell_profile_active) return;
    shell_profile_page_name = page_name;
    shell_profile_text_cycles = 0;
    shell_profile_logo_cycles = 0;
    shell_profile_text_calls = 0;
    shell_profile_page_start = timer_get_ticks();
    shell_profile_page_start_cycles = shell_profile_read_timestamp_counter();
}

static void shell_profile_page_end(void) {
    display_present();
    if (!shell_profile_active) return;
    uint64_t elapsed_ticks = timer_get_ticks() - shell_profile_page_start;
    uint64_t elapsed_cycles = shell_profile_read_timestamp_counter() -
        shell_profile_page_start_cycles;
    uint64_t blit_ticks = display_get_last_present_ticks();
    uint64_t blit_cycles = display_get_last_present_cycles();
    uint64_t text_ms = shell_profile_cycles_to_ms(shell_profile_text_cycles,
        elapsed_cycles, elapsed_ticks);
    uint64_t logo_ms = shell_profile_cycles_to_ms(shell_profile_logo_cycles,
        elapsed_cycles, elapsed_ticks);
    uint64_t blit_ms = shell_profile_cycles_to_ms(blit_cycles,
        elapsed_cycles, elapsed_ticks);
    shell_profile_active = false;
    serial_write_string("[profile] page=");
    serial_write_string(shell_profile_page_name);
    serial_write_string(" full_redraw_ticks=");
    serial_write_dec(elapsed_ticks);
    serial_write_string(" text_ms_est=");
    serial_write_dec(text_ms);
    serial_write_string(" text_calls=");
    serial_write_dec(shell_profile_text_calls);
    serial_write_string(" logo_ms_est=");
    serial_write_dec(logo_ms);
    serial_write_string(" framebuffer_copy_ticks=");
    serial_write_dec(blit_ticks);
    serial_write_string(" framebuffer_copy_ms_est=");
    serial_write_dec(blit_ms);
    serial_write_string("\n");
}

static void shell_ui_decimal(uint32_t x, uint32_t y, uint64_t value,
    uint8_t foreground, uint8_t background) {
    char digits[21];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits) - 1);
    for (size_t index = 0; index < count / 2; index++) {
        char temporary = digits[index];
        digits[index] = digits[count - 1 - index];
        digits[count - 1 - index] = temporary;
    }
    digits[count] = '\0';
    shell_ui_text(x, y, digits, foreground, background);
}

static void shell_ui_size(uint32_t x, uint32_t y, uint64_t bytes,
    uint8_t foreground, uint8_t background) {
    const uint64_t mebibyte = 1024ULL * 1024;
    const uint64_t gibibyte = 1024ULL * mebibyte;
    uint64_t unit = bytes >= gibibyte ? gibibyte : mebibyte;
    uint64_t whole = bytes / unit;
    uint64_t tenths = (bytes % unit) * 10 / unit;
    char text[32];
    size_t position = 0;
    char digits[21];
    size_t count = 0;

    do {
        digits[count++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole && count < sizeof(digits));
    while (count) {
        text[position++] = digits[--count];
    }
    text[position++] = '.';
    text[position++] = (char)('0' + tenths);
    text[position++] = ' ';
    const char* suffix = unit == gibibyte ? "GiB" : "MiB";
    while (*suffix) {
        text[position++] = *suffix++;
    }
    text[position] = '\0';
    shell_ui_text(x, y, text, foreground, background);
}

static void shell_installer_draw_progress_value(uint32_t y,
    uint64_t sectors) {
    display_fill_rect_rgb(SHELL_INSTALLER_PROGRESS_VALUE_X, y,
        SHELL_INSTALLER_PROGRESS_VALUE_WIDTH,
        SHELL_INSTALLER_PROGRESS_VALUE_HEIGHT,
        SHELL_INSTALLER_PANEL_TEXT_RED, SHELL_INSTALLER_PANEL_TEXT_GREEN,
        SHELL_INSTALLER_PANEL_TEXT_BLUE);
    shell_ui_decimal(SHELL_INSTALLER_PROGRESS_VALUE_X, y, sectors,
        COLOR_WHITE, COLOR_BLACK);
}

static void shell_installer_clear_vga_value(uint8_t x, uint8_t y) {
    display_write_at(x, y, "                    ");
}

static void shell_ui_draw_backdrop(const char* title, const char* subtitle) {
    uint32_t width;
    uint32_t height;
    if (!shell_get_graphics_size(&width, &height)) return;
    display_clear();
    for (uint32_t y = 0; y < height; y += 32) {
        uint8_t shade = (uint8_t)(34 - (y * 15 / height));
        uint32_t band_height = height - y < 32 ? height - y : 32;
        display_fill_rect_rgb(0, y, width, band_height, shade,
            (uint8_t)(shade + 1), (uint8_t)(shade + 4));
    }
    display_fill_rect_rgb(0, 0, width, 58, 11, 12, 14);
    display_blend_rect_rgb(0, 56, width, 2, 230, 232, 235, 75);
    display_fill_rect_rgb(0, height - 48, width, 48, 8, 9, 11);
    shell_ui_text(28, 10, title, COLOR_WHITE, COLOR_DARK_GRAY);
    if (subtitle) {
        shell_ui_text(28, 33, subtitle, COLOR_LIGHT_GRAY, COLOR_DARK_GRAY);
    }
    shell_ui_text(28, height - 33, "ROCKOS", COLOR_LIGHT_GRAY, COLOR_BLACK);
}

static void shell_ui_draw_glass_panel(uint32_t x, uint32_t y,
    uint32_t width, uint32_t height) {
    display_fill_rect_rgb(x + 5, y + 6, width, height, 0, 0, 0);
    display_fill_rect_rgb(x, y, width, height, 108, 111, 116);
    display_fill_rect_rgb(x + 1, y + 1, width - 2, height - 2,
        18, 20, 23);
    display_blend_rect_rgb(x + 2, y + 2, width - 4, height - 4,
        130, 134, 140, 15);
    display_blend_rect_rgb(x + 2, y + 2, width - 4, 2,
        255, 255, 255, 105);
    display_blend_rect_rgb(x + 2, y + 4, 2, height - 6,
        255, 255, 255, 38);
    display_blend_rect_rgb(x + 2, y + height - 4, width - 4, 2,
        0, 0, 0, 100);
}

static void shell_ui_draw_glass_button(uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, bool emphasized) {
    display_fill_rect_rgb(x + 2, y + 3, width, height, 0, 0, 0);
    display_fill_rect_rgb(x, y, width, height, 170, 173, 178);
    display_fill_rect_rgb(x + 1, y + 1, width - 2, height - 2,
        emphasized ? 86 : 49, emphasized ? 90 : 53, emphasized ? 96 : 59);
    display_blend_rect_rgb(x + 1, y + 1, width - 2, 2,
        255, 255, 255, 110);
    display_blend_rect_rgb(x + 1, y + height - 3, width - 2, 2,
        0, 0, 0, 95);
}

static bool shell_ui_rect_contains(const shell_ui_rect_t* rect,
    uint32_t x, uint32_t y) {
    return rect && x >= rect->x && y >= rect->y &&
        x - rect->x < rect->width && y - rect->y < rect->height;
}

static bool shell_installer_button_rect(uint8_t page,
    shell_installer_button_t button, uint32_t screen_width,
    uint32_t screen_height, shell_ui_rect_t* rect) {
    if (!rect) return false;

    if (page == SHELL_INSTALLER_PAGE_PREFERENCES &&
        button == SHELL_INSTALLER_BUTTON_NEXT) {
        uint32_t panel_width = screen_width -
            SHELL_INSTALLER_PANEL_LEFT - SHELL_INSTALLER_PANEL_RIGHT;
        uint32_t panel_height = screen_height -
            SHELL_INSTALLER_PANEL_TOP - SHELL_INSTALLER_PANEL_BOTTOM;
        rect->x = SHELL_INSTALLER_PANEL_LEFT + panel_width -
            SHELL_INSTALLER_NEXT_RIGHT_INSET - SHELL_INSTALLER_NEXT_WIDTH;
        rect->y = SHELL_INSTALLER_PANEL_TOP + panel_height -
            SHELL_INSTALLER_NEXT_BOTTOM_INSET - SHELL_INSTALLER_NEXT_HEIGHT;
        rect->width = SHELL_INSTALLER_NEXT_WIDTH;
        rect->height = SHELL_INSTALLER_NEXT_HEIGHT;
        return true;
    }

    if (page == SHELL_INSTALLER_PAGE_TARGET &&
        (button == SHELL_INSTALLER_BUTTON_BACK ||
         button == SHELL_INSTALLER_BUTTON_NEXT)) {
        bool back = button == SHELL_INSTALLER_BUTTON_BACK;
        uint32_t next_x = screen_width -
            SHELL_INSTALLER_DIALOG_PANEL_RIGHT -
            SHELL_INSTALLER_DIALOG_BUTTON_RIGHT_INSET -
            SHELL_INSTALLER_FORWARD_BUTTON_WIDTH;
        rect->x = back
            ? next_x - SHELL_INSTALLER_NAV_BUTTON_GAP -
                SHELL_INSTALLER_BACK_BUTTON_WIDTH
            : next_x;
        rect->y = screen_height -
            SHELL_INSTALLER_DIALOG_PANEL_BOTTOM -
            SHELL_INSTALLER_DIALOG_BUTTON_BOTTOM_INSET -
            SHELL_INSTALLER_NAV_BUTTON_HEIGHT;
        rect->width = back ? SHELL_INSTALLER_BACK_BUTTON_WIDTH
            : SHELL_INSTALLER_FORWARD_BUTTON_WIDTH;
        rect->height = SHELL_INSTALLER_NAV_BUTTON_HEIGHT;
        return true;
    }

    if (page == SHELL_INSTALLER_PAGE_CONFIRM &&
        (button == SHELL_INSTALLER_BUTTON_CANCEL ||
         button == SHELL_INSTALLER_BUTTON_INSTALL)) {
        bool cancel = button == SHELL_INSTALLER_BUTTON_CANCEL;
        uint32_t install_x = screen_width -
            SHELL_INSTALLER_DIALOG_PANEL_RIGHT -
            SHELL_INSTALLER_DIALOG_BUTTON_RIGHT_INSET -
            SHELL_INSTALLER_FORWARD_BUTTON_WIDTH;
        rect->x = cancel
            ? install_x - SHELL_INSTALLER_NAV_BUTTON_GAP -
                SHELL_INSTALLER_BACK_BUTTON_WIDTH
            : install_x;
        rect->y = screen_height -
            SHELL_INSTALLER_DIALOG_PANEL_BOTTOM -
            SHELL_INSTALLER_DIALOG_BUTTON_BOTTOM_INSET -
            SHELL_INSTALLER_NAV_BUTTON_HEIGHT;
        rect->width = cancel ? SHELL_INSTALLER_BACK_BUTTON_WIDTH
            : SHELL_INSTALLER_FORWARD_BUTTON_WIDTH;
        rect->height = SHELL_INSTALLER_NAV_BUTTON_HEIGHT;
        return true;
    }

    if (page == SHELL_INSTALLER_PAGE_RESULT &&
        button == SHELL_INSTALLER_BUTTON_FINISH) {
        rect->x = screen_width -
            SHELL_INSTALLER_DIALOG_PANEL_RIGHT -
            SHELL_INSTALLER_DIALOG_BUTTON_RIGHT_INSET -
            SHELL_INSTALLER_FORWARD_BUTTON_WIDTH;
        rect->y = screen_height -
            SHELL_INSTALLER_DIALOG_PANEL_BOTTOM -
            SHELL_INSTALLER_DIALOG_BUTTON_BOTTOM_INSET -
            SHELL_INSTALLER_NAV_BUTTON_HEIGHT;
        rect->width = SHELL_INSTALLER_FORWARD_BUTTON_WIDTH;
        rect->height = SHELL_INSTALLER_NAV_BUTTON_HEIGHT;
        return true;
    }
    return false;
}

static void shell_ui_draw_button(const shell_ui_rect_t* rect,
    const char* label, uint32_t text_x_inset, bool emphasized) {
    shell_ui_draw_glass_button(rect->x, rect->y, rect->width, rect->height,
        emphasized);
    shell_ui_text(rect->x + text_x_inset,
        rect->y + (rect->height - DISPLAY_UI_TEXT_HEIGHT) / 2,
        label, COLOR_WHITE, COLOR_BLACK);
}

static void shell_draw_screen(void) {
    uint32_t width;
    uint32_t height;
    bool graphical = shell_get_graphics_size(&width, &height);
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_clear();
    if (graphical) {
        shell_ui_draw_backdrop("ROCKOS DESKTOP",
            "Command shell | type help for commands");
        shell_ui_draw_glass_panel(16, 68, width - 32, height - 132);
        shell_ui_text(28, height - 33,
            "ROCKOS  |  KERNEL MODE SHELL", COLOR_LIGHT_GRAY, COLOR_BLACK);
    }
    display_set_color(COLOR_YELLOW, COLOR_BLACK);
    shell_write("ROK Technical Diagnostic Shell\n");
    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    shell_write("ROK kernel diagnostics | freestanding x86_64\n");
    display_set_cursor(0, 5);
    command_length = 0;
    command_buffer[0] = '\0';
}

static void shell_editor_draw(void) {
    uint32_t rows = 0;
    uint32_t column = 0;
    for (uint32_t index = 0; index < shell_editor_length; index++) {
        if (shell_copy_buffer[index] == '\n') {
            rows++;
            column = 0;
        } else if (++column == 80) {
            rows++;
            column = 0;
        }
    }

    uint32_t first_row = rows >= SHELL_EDITOR_VISIBLE_ROWS
        ? rows - SHELL_EDITOR_VISIBLE_ROWS + 1 : 0;
    uint32_t current_row = 0;
    column = 0;
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_clear();
    display_set_color(COLOR_YELLOW, COLOR_BLACK);
    display_write_at(0, 0, "RockOS Text Editor");
    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    display_write_at(0, 1, shell_editor_path);
    display_write_at(0, 2, "Type to edit | Ctrl+S save | Esc discard");
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_set_cursor(0, SHELL_EDITOR_FIRST_ROW);

    for (uint32_t index = 0; index < shell_editor_length; index++) {
        uint8_t character = shell_copy_buffer[index];
        if (current_row >= first_row) display_putchar((char)character);
        if (character == '\n') {
            current_row++;
            column = 0;
        } else if (++column == 80) {
            current_row++;
            column = 0;
        }
    }

    if (shell_editor_buffer_full) {
        display_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        display_write_at(0, 23, "File is at the maximum supported size.");
    }
    display_set_color(shell_editor_modified ? COLOR_YELLOW : COLOR_LIGHT_CYAN,
        COLOR_BLACK);
    display_write_at(0, 24, "Ctrl+S saves | Esc discards changes");
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    uint32_t cursor_row = rows - first_row;
    if (cursor_row >= SHELL_EDITOR_VISIBLE_ROWS) {
        cursor_row = SHELL_EDITOR_VISIBLE_ROWS - 1;
    }
    display_set_cursor(column, (uint8_t)(SHELL_EDITOR_FIRST_ROW + cursor_row));
}

static void shell_editor_finish(const char* message) {
    shell_editor_active = false;
    shell_draw_screen();
    shell_write(message);
    shell_write(shell_editor_path);
    display_putchar('\n');
    shell_draw_prompt();
}

static void shell_editor_open(const char* path) {
    filesystem_info_t info;
    uint32_t bytes_read = 0;
    int result = filesystem_stat(path, &info);
    if (result == FS_OK) {
        if (info.type != FS_TYPE_FILE) {
            shell_print_filesystem_error(FS_ERR_ISDIR);
            return;
        }
        if (info.size > sizeof(shell_copy_buffer)) {
            shell_write("File is too large for the text editor.\n");
            return;
        }
        result = filesystem_read(path, shell_copy_buffer,
            (uint32_t)sizeof(shell_copy_buffer), &bytes_read);
        if (result != FS_OK) {
            shell_print_filesystem_error(result);
            return;
        }
    } else if (result == FS_ERR_NOENT) {
        bytes_read = 0;
    } else {
        shell_print_filesystem_error(result);
        return;
    }

    size_t path_length = 0;
    while (path[path_length] != '\0') {
        if (path_length + 1 >= sizeof(shell_editor_path)) {
            shell_write("Path is too long for the editor.\n");
            return;
        }
        shell_editor_path[path_length] = path[path_length];
        path_length++;
    }
    shell_editor_path[path_length] = '\0';
    shell_editor_length = bytes_read;
    shell_editor_modified = false;
    shell_editor_buffer_full = false;
    shell_editor_active = true;
    shell_editor_draw();
}

static void shell_editor_save(void) {
    int result = filesystem_write(shell_editor_path, shell_copy_buffer,
        shell_editor_length);
    if (result != FS_OK) {
        shell_editor_draw();
        display_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        display_write_at(0, 23, filesystem_strerror(result));
        display_set_color(COLOR_WHITE, COLOR_BLACK);
        return;
    }
    shell_editor_finish("Saved: ");
}

static void shell_editor_handle_key(const input_key_event_t* event) {
    if (event->modifiers & MOD_CTRL) {
        if (event->ascii == 's' || event->ascii == 'S') {
            shell_editor_save();
        }
        return;
    }
    if (event->ascii == 27) {
        shell_editor_finish("Cancelled: ");
        return;
    }
    if (event->ascii == '\b') {
        if (shell_editor_length > 0) {
            shell_editor_length--;
            shell_editor_modified = true;
            shell_editor_buffer_full = false;
            shell_editor_draw();
        }
        return;
    }
    if (event->ascii == '\n' || event->ascii == '\r') {
        if (shell_editor_length < sizeof(shell_copy_buffer)) {
            shell_copy_buffer[shell_editor_length++] = '\n';
            shell_editor_modified = true;
            shell_editor_buffer_full = false;
            shell_editor_draw();
        } else {
            shell_editor_buffer_full = true;
            shell_editor_draw();
        }
        return;
    }
    if (event->ascii >= 0x20 && event->ascii <= 0x7E) {
        if (shell_editor_length < sizeof(shell_copy_buffer)) {
            shell_copy_buffer[shell_editor_length++] = (uint8_t)event->ascii;
            shell_editor_modified = true;
            shell_editor_buffer_full = false;
            shell_editor_draw();
        } else {
            shell_editor_buffer_full = true;
            shell_editor_draw();
        }
    }
}

static const char* const shell_installer_languages[] = {
    "English (United States)",
    "English (United Kingdom)",
    "French",
    "Spanish",
    "German"
};

static const char* const shell_installer_regions[] = {
    "English (United States)",
    "English (United Kingdom)",
    "French (France)",
    "Spanish (Spain)",
    "German (Germany)"
};

static const char* const shell_installer_keyboards[] = {
    "US",
    "United Kingdom",
    "French",
    "German"
};

#define SHELL_INSTALLER_LANGUAGE_COUNT \
    (sizeof(shell_installer_languages) / sizeof(shell_installer_languages[0]))
#define SHELL_INSTALLER_REGION_COUNT \
    (sizeof(shell_installer_regions) / sizeof(shell_installer_regions[0]))
#define SHELL_INSTALLER_KEYBOARD_COUNT \
    (sizeof(shell_installer_keyboards) / sizeof(shell_installer_keyboards[0]))

static void shell_installer_draw_preference_row(uint32_t width,
    uint32_t height, uint8_t index, bool draw_label) {
    static const char* labels[3] = {
        "Language to install:",
        "Time and currency format:",
        "Keyboard or input method:"
    };
    const char* value = index == 0
        ? shell_installer_languages[shell_installer_language]
        : index == 1
            ? shell_installer_regions[shell_installer_region]
            : shell_installer_keyboards[shell_installer_keyboard];
    uint32_t center_x = width / 2;
    uint32_t option_x = center_x - 120;
    uint32_t option_width = width - option_x - 76;
    uint32_t y = height / 2 - 40 + (uint32_t)index * 64;
    if (draw_label) {
        shell_ui_text(72, y + 10, labels[index], COLOR_LIGHT_GRAY,
            COLOR_BLACK);
    }
    display_fill_rect_rgb(option_x - 3, y - 3, option_width + 6, 46,
        shell_installer_field == index ? 190 : 93,
        shell_installer_field == index ? 193 : 96,
        shell_installer_field == index ? 198 : 101);
    display_fill_rect_rgb(option_x, y, option_width, 40, 34, 36, 40);
    display_blend_rect_rgb(option_x + 1, y + 1, option_width - 2, 2,
        255, 255, 255, 48);
    uint32_t arrow_width = 38;
    display_fill_rect_rgb(option_x + option_width - arrow_width,
        y + 2, arrow_width - 2, 36, 91, 94, 100);
    display_blend_rect_rgb(option_x + option_width - arrow_width + 1,
        y + 3, arrow_width - 4, 2, 255, 255, 255, 65);
    shell_ui_text(option_x + 16, y + 8, value, COLOR_WHITE, COLOR_BLACK);
    display_fill_triangle(option_x + option_width - 28, y + 15,
        option_x + option_width - 12, y + 15,
        option_x + option_width - 20, y + 25, COLOR_WHITE);
}

static bool shell_string_equals(const char* left, const char* right) {
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static bool shell_bounded_string_equals(const char* value,
    const char* expected, size_t capacity) {
    if (!value || !expected) return false;
    for (size_t index = 0; index < capacity; index++) {
        if (value[index] != expected[index]) return false;
        if (expected[index] == '\0') return true;
    }
    return false;
}

static void shell_installer_draw_wordmark(void) {
    static const uint8_t glyph_r[5] = {0x1E, 0x11, 0x1E, 0x14, 0x12};
    static const uint8_t glyph_o[5] = {0x0E, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t glyph_c[5] = {0x0F, 0x10, 0x10, 0x10, 0x0F};
    static const uint8_t glyph_k[5] = {0x11, 0x12, 0x1C, 0x12, 0x11};
    static const uint8_t glyph_s[5] = {0x0F, 0x10, 0x0E, 0x01, 0x1E};
    static const char wordmark[] = "ROCKOS";

    display_set_color(COLOR_WHITE, COLOR_BLACK);
    for (uint8_t letter = 0; wordmark[letter] != '\0'; letter++) {
        const uint8_t* glyph = glyph_o;
        switch (wordmark[letter]) {
            case 'R': glyph = glyph_r; break;
            case 'C': glyph = glyph_c; break;
            case 'K': glyph = glyph_k; break;
            case 'S': glyph = glyph_s; break;
            default: break;
        }
        uint8_t letter_x = (uint8_t)(4 + letter * 12);
        for (uint8_t row = 0; row < 5; row++) {
            for (uint8_t column = 0; column < 5; column++) {
                if (!(glyph[row] & (0x10U >> column))) continue;
                display_write_at((uint8_t)(letter_x + column * 2),
                    (uint8_t)(1 + row * 2), "##");
                display_write_at((uint8_t)(letter_x + column * 2),
                    (uint8_t)(2 + row * 2), "##");
            }
        }
    }
}

static void shell_installer_draw_option(uint8_t row, bool active,
    const char* value) {
    display_set_color(COLOR_BLACK,
        active ? COLOR_LIGHT_CYAN : COLOR_LIGHT_GRAY);
    display_write_at(40, row, "[                                ]");
    display_write_at(42, row, value);
    display_write_at(70, row, "v");
}

static const block_device_t* shell_installer_get_target(void) {
    const block_device_t* install_target = NULL;
    size_t device_count = storage_get_device_count();
    for (size_t index = 0; index < device_count; index++) {
        const block_device_t* device = storage_get_device_at(index);
        if (device && shell_string_equals(device->name,
                "ata0-primary-slave")) {
            install_target = device;
            break;
        }
    }
    return install_target;
}

void shell_set_boot_info(uintptr_t boot_info_address) {
    shell_installer_image = 0;
    shell_installer_image_size = 0;
    if (!boot_info_address) return;

    const multiboot1_info_t* info =
        (const multiboot1_info_t*)boot_info_address;
    if (!(info->flags & MULTIBOOT1_INFO_MODULES) ||
        info->mods_count == 0 || info->mods_count > 128 ||
        info->mods_addr == 0) {
        return;
    }

    const multiboot1_module_t* modules =
        (const multiboot1_module_t*)(uintptr_t)info->mods_addr;
    for (uint32_t index = 0; index < info->mods_count; index++) {
        const multiboot1_module_t* module = &modules[index];
        if (module->mod_end <= module->mod_start ||
            !module->string) {
            continue;
        }
        const char* identifier =
            (const char*)(uintptr_t)module->string;
        if (!shell_bounded_string_equals(identifier,
                SHELL_INSTALLER_IMAGE_ID, 128)) {
            continue;
        }
        uint32_t image_size = module->mod_end - module->mod_start;
        if (image_size % BLOCK_SECTOR_SIZE != 0) return;
        shell_installer_image = module->mod_start;
        shell_installer_image_size = image_size;
        return;
    }
}

static void shell_installer_draw_setup(void) {
    shell_profile_page_begin("preferences");
    uint32_t width;
    uint32_t height;
    if (shell_get_graphics_size(&width, &height)) {
        display_clear();
        for (uint32_t y = 0; y < height; y += 32) {
            uint8_t shade = (uint8_t)(34 - (y * 15 / height));
            uint32_t band_height = height - y < 32 ? height - y : 32;
            display_fill_rect_rgb(0, y, width, band_height, shade,
                (uint8_t)(shade + 1), (uint8_t)(shade + 4));
        }
        shell_ui_draw_glass_panel(SHELL_INSTALLER_PANEL_LEFT,
            SHELL_INSTALLER_PANEL_TOP,
            width - SHELL_INSTALLER_PANEL_LEFT -
                SHELL_INSTALLER_PANEL_RIGHT,
            height - SHELL_INSTALLER_PANEL_TOP -
                SHELL_INSTALLER_PANEL_BOTTOM);
        uint32_t center_x = width / 2;
        uint64_t logo_start = shell_profile_active
            ? shell_profile_read_timestamp_counter() : 0;
        display_draw_rockos_logo(center_x - DISPLAY_ROCKOS_LOGO_WIDTH / 2,
            58);
        if (shell_profile_active) {
            shell_profile_logo_cycles +=
                shell_profile_read_timestamp_counter() - logo_start;
        }
        shell_ui_text(center_x - 144, 196, "INSTALLATION PREFERENCES",
            COLOR_WHITE, COLOR_BLACK);
        shell_ui_text(center_x - 306, 224,
            "Choose your language, region, and keyboard layout.",
            COLOR_LIGHT_GRAY, COLOR_BLACK);

        for (uint8_t index = 0; index < 3; index++) {
            shell_installer_draw_preference_row(width, height, index, true);
        }
        shell_ui_text(72, height - 116,
            "Choose your language and other preferences, then click Next.",
            COLOR_LIGHT_GRAY, COLOR_BLACK);
        shell_ui_rect_t button_rect;
        shell_installer_button_rect(SHELL_INSTALLER_PAGE_PREFERENCES,
            SHELL_INSTALLER_BUTTON_NEXT, width, height, &button_rect);
        shell_ui_text(SHELL_INSTALLER_PANEL_LEFT +
                SHELL_INSTALLER_COPYRIGHT_LEFT_INSET,
            button_rect.y + (SHELL_INSTALLER_NEXT_HEIGHT -
                DISPLAY_UI_TEXT_HEIGHT) / 2,
            "(c) 2026 RockOS. All rights reserved.", COLOR_LIGHT_GRAY,
            COLOR_BLACK);
        shell_ui_draw_button(&button_rect, "Next", 34, true);
        shell_profile_page_end();
        return;
    }

    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_clear();
    display_set_color(COLOR_WHITE, COLOR_BLUE);
    display_write_at(0, 0, "RockOS Setup");
    shell_installer_draw_wordmark();
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_write_at(6, 13, "Language to install:");
    display_write_at(6, 15, "Time and currency format:");
    display_write_at(6, 17, "Keyboard or input method:");
    shell_installer_draw_option(13,
        shell_installer_field == 0,
        shell_installer_languages[shell_installer_language]);
    shell_installer_draw_option(15,
        shell_installer_field == 1,
        shell_installer_regions[shell_installer_region]);
    shell_installer_draw_option(17,
        shell_installer_field == 2,
        shell_installer_keyboards[shell_installer_keyboard]);
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_write_at(12, 20,
        "Choose your preferences, then press Enter to continue.");
    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    display_write_at(14, 21, "Tab: select  Up/Down: change  Esc: exit");
    display_write_at(10, 24, "(c) 2026 RockOS. All rights reserved.");
    display_set_color(COLOR_WHITE, COLOR_BLUE);
    display_write_at(60, 23, "[   Next   ]");
    display_set_color(COLOR_YELLOW, COLOR_BLACK);
    display_write_at(0, 22, "Graphics unavailable:");
    display_write_at(23, 22, display_framebuffer_status());
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    shell_profile_page_end();
}

static void shell_installer_redraw_preference_rows(uint8_t previous_field) {
    uint32_t width;
    uint32_t height;
    if (!shell_get_graphics_size(&width, &height)) {
        shell_installer_draw_setup();
        return;
    }
    shell_installer_draw_preference_row(width, height, previous_field, false);
    if (shell_installer_field != previous_field) {
        shell_installer_draw_preference_row(width, height,
            shell_installer_field, false);
    }
}

static void shell_installer_draw_preflight(void) {
    shell_profile_page_begin("target");
    const block_device_t* install_target = shell_installer_get_target();
    uint64_t image_sectors = shell_installer_image_size /
        BLOCK_SECTOR_SIZE;
    bool target_ready = install_target &&
        install_target->sector_size == BLOCK_SECTOR_SIZE &&
        shell_installer_image != 0 &&
        image_sectors != 0 &&
        image_sectors <= install_target->sector_count;
    uint32_t width;
    uint32_t height;
    if (shell_get_graphics_size(&width, &height)) {
        shell_ui_draw_backdrop("ROCKOS SETUP  /  INSTALLATION TARGET",
            "Review the target before continuing");
        shell_ui_draw_glass_panel(SHELL_INSTALLER_DIALOG_PANEL_LEFT,
            SHELL_INSTALLER_DIALOG_PANEL_TOP,
            width - SHELL_INSTALLER_DIALOG_PANEL_LEFT -
                SHELL_INSTALLER_DIALOG_PANEL_RIGHT,
            height - SHELL_INSTALLER_DIALOG_PANEL_TOP -
                SHELL_INSTALLER_DIALOG_PANEL_BOTTOM);
        shell_ui_text(156, 150, "Dedicated installation disk",
            COLOR_WHITE, COLOR_BLACK);
        shell_ui_text(156, 202,
            "The primary master (RockFS/data) disk will be preserved.",
            COLOR_LIGHT_GRAY, COLOR_BLACK);
        if (install_target) {
            shell_ui_text(156, 270, "Target: ATA primary slave",
                COLOR_WHITE, COLOR_BLACK);
            shell_ui_text(156, 310, "Disk capacity:",
                COLOR_LIGHT_GRAY, COLOR_BLACK);
            shell_ui_size(SHELL_INSTALLER_VALUE_X, 310,
                install_target->sector_count * install_target->sector_size,
                COLOR_WHITE, COLOR_BLACK);
            shell_ui_text(156, 350, "Image size:", COLOR_LIGHT_GRAY,
                COLOR_BLACK);
            shell_ui_size(SHELL_INSTALLER_VALUE_X, 350,
                shell_installer_image_size, COLOR_WHITE, COLOR_BLACK);
        } else {
            shell_ui_text(156, 286, "No ATA primary-slave disk detected.",
                COLOR_LIGHT_GRAY, COLOR_BLACK);
        }
        shell_ui_text(156, 414, target_ready ?
            "Ready to install RockOS." :
            "Installer image or target disk is unavailable or too small.",
            target_ready ? COLOR_WHITE : COLOR_LIGHT_GRAY, COLOR_BLACK);
        if (target_ready) {
            shell_ui_rect_t back_rect;
            shell_ui_rect_t next_rect;
            shell_installer_button_rect(SHELL_INSTALLER_PAGE_TARGET,
                SHELL_INSTALLER_BUTTON_BACK, width, height, &back_rect);
            shell_installer_button_rect(SHELL_INSTALLER_PAGE_TARGET,
                SHELL_INSTALLER_BUTTON_NEXT, width, height, &next_rect);
            shell_ui_draw_button(&back_rect, "BACK", 40, false);
            shell_ui_draw_button(&next_rect, "NEXT", 28, true);
        } else {
            shell_ui_rect_t back_rect;
            shell_installer_button_rect(SHELL_INSTALLER_PAGE_TARGET,
                SHELL_INSTALLER_BUTTON_BACK, width, height, &back_rect);
            shell_ui_draw_button(&back_rect, "BACK", 40, false);
        }
        shell_ui_text(156, height - 92,
            "Enter: continue   Backspace: preferences   Esc: exit",
            COLOR_LIGHT_GRAY, COLOR_BLACK);
        shell_profile_page_end();
        return;
    }

    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_clear();
    display_set_color(COLOR_WHITE, COLOR_BLUE);
    display_write_at(0, 0, "RockOS Setup - Installation target");
    display_set_color(COLOR_YELLOW, COLOR_BLACK);
    display_write_at(2, 2, "Installation disk preflight");
    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    display_write_at(2, 4, "Only a separate dedicated target disk may be used.");
    display_write_at(2, 6, "Primary master (RockFS/data) is preserved.");
    display_write_at(2, 8, "BIOS + UEFI boot support is included.");
    if (install_target) {
        display_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        display_write_at(2, 11, "Dedicated target detected: ata0-primary-slave");
        display_set_color(COLOR_WHITE, COLOR_BLACK);
        display_write_at(2, 12, "Capacity:");
        display_set_cursor(12, 12);
        shell_write_decimal(install_target->sector_count *
            install_target->sector_size);
        shell_write(" bytes");
        display_write_at(2, 14, "Install image:");
        display_set_cursor(17, 14);
        if (shell_installer_image_size) {
            shell_write_decimal(shell_installer_image_size);
            shell_write(" bytes");
        } else {
            shell_write("unavailable (boot the installer ISO)");
        }
    } else {
        display_set_color(COLOR_YELLOW, COLOR_BLACK);
        display_write_at(2, 11, "No dedicated primary-slave target disk detected.");
        display_write_at(2, 13, "Attach a separate blank disk as ATA primary slave.");
    }
    display_set_color(target_ready ? COLOR_LIGHT_GREEN : COLOR_YELLOW,
        COLOR_BLACK);
    display_write_at(2, 17, target_ready ?
        "Ready to install to the dedicated target." :
        "Cannot install: image or target is unavailable/too small.");
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_set_color(COLOR_WHITE, COLOR_BLUE);
    display_write_at(60, 22, "[  Next  ]");
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_write_at(2, 24,
        "Enter: continue when ready | Backspace: preferences | Esc: exit.");
    shell_profile_page_end();
}

static void shell_installer_draw_confirmation(void) {
    shell_profile_page_begin("confirmation");
    const block_device_t* install_target = shell_installer_get_target();
    uint32_t width;
    uint32_t height;
    if (shell_get_graphics_size(&width, &height)) {
        shell_ui_draw_backdrop("ROCKOS SETUP  /  CONFIRM",
            "Please review this destructive operation");
        shell_ui_draw_glass_panel(SHELL_INSTALLER_DIALOG_PANEL_LEFT,
            SHELL_INSTALLER_DIALOG_PANEL_TOP,
            width - SHELL_INSTALLER_DIALOG_PANEL_LEFT -
                SHELL_INSTALLER_DIALOG_PANEL_RIGHT,
            height - SHELL_INSTALLER_DIALOG_PANEL_TOP -
                SHELL_INSTALLER_DIALOG_PANEL_BOTTOM);
        shell_ui_draw_glass_button(156, 150, 56, 48, true);
        display_fill_rect_rgb(181, 160, 6, 18, 235, 235, 235);
        display_fill_rect_rgb(181, 183, 6, 6, 235, 235, 235);
        shell_ui_text(232, 166,
            "This overwrites the beginning of the selected disk.",
            COLOR_WHITE, COLOR_BLACK);
        shell_ui_text(156, 250, "Target: ATA primary slave",
            COLOR_WHITE, COLOR_BLACK);
        shell_ui_text(156, 294, "Install image:", COLOR_LIGHT_GRAY,
            COLOR_BLACK);
        shell_ui_size(SHELL_INSTALLER_VALUE_X, 294,
            shell_installer_image_size, COLOR_WHITE, COLOR_BLACK);
        if (install_target) {
            shell_ui_text(156, 330, "Disk capacity:", COLOR_LIGHT_GRAY,
                COLOR_BLACK);
            shell_ui_size(SHELL_INSTALLER_VALUE_X, 330,
                install_target->sector_count * install_target->sector_size,
                COLOR_WHITE, COLOR_BLACK);
        }
        shell_ui_text(156, 382, "Primary master data disk is not touched.",
            COLOR_LIGHT_GRAY, COLOR_BLACK);
        shell_ui_text(156, 422, "Press Y, then Enter; or click Install twice.",
            COLOR_LIGHT_GRAY, COLOR_BLACK);
        shell_ui_rect_t cancel_rect;
        shell_ui_rect_t install_rect;
        shell_installer_button_rect(SHELL_INSTALLER_PAGE_CONFIRM,
            SHELL_INSTALLER_BUTTON_CANCEL, width, height, &cancel_rect);
        shell_installer_button_rect(SHELL_INSTALLER_PAGE_CONFIRM,
            SHELL_INSTALLER_BUTTON_INSTALL, width, height, &install_rect);
        shell_ui_draw_button(&cancel_rect, "CANCEL", 40, false);
        shell_ui_draw_button(&install_rect, "INSTALL", 28, true);
        shell_ui_text(156, height - 92,
            "Esc: cancel   Backspace: return to target selection",
            COLOR_LIGHT_GRAY, COLOR_BLACK);
        shell_profile_page_end();
        return;
    }

    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_clear();
    display_set_color(COLOR_WHITE, COLOR_BLUE);
    display_write_at(0, 0, "RockOS Setup - Confirm installation");
    display_set_color(COLOR_YELLOW, COLOR_BLACK);
    display_write_at(2, 3,
        "This overwrites the beginning of the selected target disk.");
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_write_at(2, 6, "Target: ata0-primary-slave");
    display_write_at(2, 8, "Image size:");
    display_set_cursor(14, 8);
    shell_write_decimal(shell_installer_image_size);
    shell_write(" bytes");
    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    display_write_at(2, 10, "Sectors beyond the image remain unchanged.");
    if (install_target) {
        display_write_at(2, 7, "Disk capacity:");
        display_set_cursor(17, 7);
        shell_write_decimal(install_target->sector_count *
            install_target->sector_size);
        shell_write(" bytes");
    }
    display_write_at(2, 12, "The primary master (RockFS/data) disk is not touched.");
    display_write_at(2, 14, "BIOS and UEFI boot files will be installed.");
    display_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    display_write_at(2, 15,
        "Press Y to confirm overwriting ata0-primary-slave.");
    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    display_write_at(2, 17, "Then press Enter to begin installation.");
    display_write_at(2, 24, "Esc: cancel | Backspace: target check");
    shell_profile_page_end();
}

static void shell_installer_draw_result(const char* message) {
    shell_profile_page_begin("result");
    shell_installer_page = SHELL_INSTALLER_PAGE_RESULT;
    uint32_t width;
    uint32_t height;
    if (shell_get_graphics_size(&width, &height)) {
        shell_ui_draw_backdrop("ROCKOS SETUP  /  INSTALLATION",
            "Setup status");
        shell_ui_draw_glass_panel(SHELL_INSTALLER_DIALOG_PANEL_LEFT,
            SHELL_INSTALLER_DIALOG_PANEL_TOP,
            width - SHELL_INSTALLER_DIALOG_PANEL_LEFT -
                SHELL_INSTALLER_DIALOG_PANEL_RIGHT,
            height - SHELL_INSTALLER_DIALOG_PANEL_TOP -
                SHELL_INSTALLER_DIALOG_PANEL_BOTTOM);
        shell_ui_draw_glass_button(156, 190, 48, 48, true);
        shell_ui_text(168, 206, "OK", COLOR_WHITE, COLOR_BLACK);
        shell_ui_text(228, 206, message, COLOR_WHITE, COLOR_BLACK);
        shell_ui_rect_t finish_rect;
        shell_installer_button_rect(SHELL_INSTALLER_PAGE_RESULT,
            SHELL_INSTALLER_BUTTON_FINISH, width, height, &finish_rect);
        shell_ui_draw_button(&finish_rect, "FINISH", 28, true);
        shell_ui_text(156, height - 92, "Press Esc to return to the shell.",
            COLOR_LIGHT_GRAY, COLOR_BLACK);
        shell_profile_page_end();
        return;
    }

    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_clear();
    display_set_color(COLOR_WHITE, COLOR_BLUE);
    display_write_at(0, 0, "RockOS Setup - Installation");
    display_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    display_write_at(2, 5, message);
    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    display_write_at(2, 24, "Press Esc to return to the shell.");
    shell_profile_page_end();
}

static void shell_installer_draw_storage_error(block_result_t result) {
    if (shell_installer_page != SHELL_INSTALLER_PAGE_RESULT) {
        shell_installer_draw_result("Installation failed during disk I/O.");
    }
    uint32_t width;
    uint32_t height;
    if (shell_get_graphics_size(&width, &height)) {
        shell_ui_text(156, 270, block_result_string(result),
            COLOR_LIGHT_GRAY, COLOR_BLACK);
    } else {
        display_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        display_write_at(2, 7, block_result_string(result));
    }
}

static bool shell_installer_bytes_equal(const uint8_t* left,
    const uint8_t* right, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

static void shell_installer_install(void) {
    const block_device_t* target = shell_installer_get_target();
    if (!target || target->sector_size != BLOCK_SECTOR_SIZE ||
        !shell_installer_image || !shell_installer_image_size ||
        shell_installer_image_size % BLOCK_SECTOR_SIZE != 0) {
        shell_installer_draw_result("Install stopped: target or image unavailable.");
        return;
    }

    uint64_t total_sectors =
        shell_installer_image_size / BLOCK_SECTOR_SIZE;
    if (total_sectors > target->sector_count) {
        shell_installer_draw_result("Install stopped: target disk is too small.");
        return;
    }

    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_clear();
    uint32_t width;
    uint32_t height;
    bool graphical = shell_get_graphics_size(&width, &height);
    if (graphical) {
        shell_ui_draw_backdrop("ROCKOS SETUP  /  INSTALLING",
            "Installing RockOS to the dedicated target disk");
        shell_ui_draw_glass_panel(SHELL_INSTALLER_DIALOG_PANEL_LEFT,
            SHELL_INSTALLER_DIALOG_PANEL_TOP,
            width - SHELL_INSTALLER_DIALOG_PANEL_LEFT -
                SHELL_INSTALLER_DIALOG_PANEL_RIGHT,
            height - SHELL_INSTALLER_DIALOG_PANEL_TOP -
                SHELL_INSTALLER_DIALOG_PANEL_BOTTOM);
        shell_ui_text(156, 182, "Writing bootable system image...",
            COLOR_WHITE, COLOR_BLACK);
        display_fill_rect_rgb(156, 256, width - 360, 32, 45, 48, 53);
        shell_ui_text(156, 312, "Written sectors:",
            COLOR_LIGHT_GRAY, COLOR_BLACK);
        shell_installer_draw_progress_value(312, 0);
        shell_ui_text(156, 352, "Total sectors:",
            COLOR_LIGHT_GRAY, COLOR_BLACK);
        shell_ui_decimal(SHELL_INSTALLER_PROGRESS_VALUE_X, 352,
            total_sectors, COLOR_WHITE, COLOR_BLACK);
        display_present();
    }
    if (!graphical) {
        display_set_color(COLOR_WHITE, COLOR_BLUE);
        display_write_at(0, 0, "RockOS Setup - Installing");
        display_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        display_write_at(2, 4,
            "Writing bootable image to ata0-primary-slave...");
    }

    uint64_t last_progress_lba = 0;
    for (uint64_t lba = 0; lba < total_sectors;) {
        uint32_t count = (uint32_t)(total_sectors - lba);
        if (count > SHELL_INSTALLER_TRANSFER_SECTORS) {
            count = SHELL_INSTALLER_TRANSFER_SECTORS;
        }
        const uint8_t* source = (const uint8_t*)shell_installer_image +
            lba * BLOCK_SECTOR_SIZE;
        block_result_t result = block_write(target, lba, count, source);
        if (result != BLOCK_RESULT_OK) {
            shell_installer_draw_result("Install failed writing target disk.");
            shell_installer_draw_storage_error(result);
            return;
        }
        lba += count;
        if (graphical && (lba - last_progress_lba >=
                SHELL_INSTALLER_PROGRESS_UPDATE_SECTORS ||
                lba == total_sectors)) {
            uint32_t bar_width = width - 360;
            uint32_t filled = (uint32_t)(lba * bar_width / total_sectors);
            display_fill_rect_rgb(156, 256, bar_width, 32, 45, 48, 53);
            if (filled) {
                display_fill_rect_rgb(156, 256, filled, 32,
                    174, 178, 184);
                display_blend_rect_rgb(156, 256, filled, 2,
                    255, 255, 255, 75);
            }
            shell_installer_draw_progress_value(312, lba);
            display_present();
            last_progress_lba = lba;
        }
        if (!graphical) {
            display_write_at(2, 7, "Written sectors:");
            shell_installer_clear_vga_value(19, 7);
            display_set_cursor(19, 7);
            shell_write_decimal(lba);
            display_write_at(2, 8, "Total sectors:");
            shell_installer_clear_vga_value(18, 8);
            display_set_cursor(18, 8);
            shell_write_decimal(total_sectors);
        }
    }

    if (graphical) {
        shell_ui_text(156, 410, "Verifying written data...",
            COLOR_LIGHT_GRAY, COLOR_BLACK);
    }
    if (!graphical) {
        display_write_at(2, 11, "Verifying written data...");
    }
    if (graphical) {
        shell_ui_text(156, 450, "Verified sectors:",
            COLOR_LIGHT_GRAY, COLOR_BLACK);
        shell_installer_draw_progress_value(450, 0);
    }
    last_progress_lba = 0;
    for (uint64_t lba = 0; lba < total_sectors;) {
        uint32_t count = (uint32_t)(total_sectors - lba);
        if (count > SHELL_INSTALLER_TRANSFER_SECTORS) {
            count = SHELL_INSTALLER_TRANSFER_SECTORS;
        }
        block_result_t result = block_read(target, lba, count,
            shell_installer_transfer_buffer);
        size_t bytes = count * BLOCK_SECTOR_SIZE;
        const uint8_t* expected =
            (const uint8_t*)shell_installer_image + lba * BLOCK_SECTOR_SIZE;
        if (result != BLOCK_RESULT_OK) {
            shell_installer_draw_result("Install read-back failed.");
            shell_installer_draw_storage_error(result);
            return;
        }
        if (!shell_installer_bytes_equal(shell_installer_transfer_buffer,
                expected, bytes)) {
            shell_installer_draw_result(
                "Install failed: read-back verification did not match.");
            return;
        }
        lba += count;
        if (graphical && (lba - last_progress_lba >=
                SHELL_INSTALLER_PROGRESS_UPDATE_SECTORS ||
                lba == total_sectors)) {
            shell_installer_draw_progress_value(450, lba);
            display_present();
            last_progress_lba = lba;
        }
        if (!graphical) {
            display_write_at(2, 13, "Verified sectors:");
            shell_installer_clear_vga_value(20, 13);
            display_set_cursor(20, 13);
            shell_write_decimal(lba);
        }
    }

    shell_installer_draw_result(
        "Installation succeeded. Remove ISO and reboot to RockOS.");
}

static void shell_installer_open(void) {
    shell_installer_active = true;
    shell_installer_page = SHELL_INSTALLER_PAGE_PREFERENCES;
    shell_installer_pressed_button = SHELL_INSTALLER_BUTTON_NONE;
    shell_installer_field = 0;
    shell_installer_language = 0;
    shell_installer_region = 0;
    shell_installer_keyboard = 0;
    shell_installer_draw_setup();
}

static void shell_installer_change_option(bool increment) {
    if (shell_installer_field == 0) {
        shell_installer_language = (uint8_t)((shell_installer_language +
            (increment ? 1 : SHELL_INSTALLER_LANGUAGE_COUNT - 1)) %
            SHELL_INSTALLER_LANGUAGE_COUNT);
    } else if (shell_installer_field == 1) {
        shell_installer_region = (uint8_t)((shell_installer_region +
            (increment ? 1 : SHELL_INSTALLER_REGION_COUNT - 1)) %
            SHELL_INSTALLER_REGION_COUNT);
    } else {
        shell_installer_keyboard = (uint8_t)((shell_installer_keyboard +
            (increment ? 1 : SHELL_INSTALLER_KEYBOARD_COUNT - 1)) %
            SHELL_INSTALLER_KEYBOARD_COUNT);
    }
}

static void shell_installer_close(void) {
    shell_installer_active = false;
    shell_installer_pressed_button = SHELL_INSTALLER_BUTTON_NONE;
    shell_draw_screen();
    shell_draw_prompt();
}

static bool shell_installer_target_is_ready(void) {
    const block_device_t* target = shell_installer_get_target();
    uint64_t image_sectors = shell_installer_image_size /
        BLOCK_SECTOR_SIZE;
    return target && target->sector_size == BLOCK_SECTOR_SIZE &&
        shell_installer_image && image_sectors != 0 &&
        image_sectors <= target->sector_count;
}

static void shell_installer_navigate(shell_installer_action_t action) {
    if (action == SHELL_INSTALLER_ACTION_CANCEL ||
        action == SHELL_INSTALLER_ACTION_FINISH) {
        shell_installer_close();
        return;
    }

    if (action == SHELL_INSTALLER_ACTION_CONFIRM &&
        shell_installer_page == SHELL_INSTALLER_PAGE_CONFIRM) {
        shell_installer_confirmed = true;
        uint32_t width;
        uint32_t height;
        if (shell_get_graphics_size(&width, &height)) {
            shell_ui_text(156, 470,
                "Confirmed. Click Install again or press Enter.",
                COLOR_WHITE, COLOR_BLACK);
        } else {
            display_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
            display_write_at(2, 19, "Erase confirmation accepted.");
        }
        return;
    }

    if (action == SHELL_INSTALLER_ACTION_INSTALL &&
        shell_installer_page == SHELL_INSTALLER_PAGE_CONFIRM &&
        shell_installer_target_is_ready()) {
        if (!shell_installer_confirmed) {
            shell_installer_navigate(SHELL_INSTALLER_ACTION_CONFIRM);
            return;
        }
        shell_installer_install();
        return;
    }

    if (action == SHELL_INSTALLER_ACTION_BACK) {
        if (shell_installer_page == SHELL_INSTALLER_PAGE_TARGET) {
            shell_installer_page = SHELL_INSTALLER_PAGE_PREFERENCES;
            shell_installer_draw_setup();
        } else if (shell_installer_page == SHELL_INSTALLER_PAGE_CONFIRM) {
            shell_installer_page = SHELL_INSTALLER_PAGE_TARGET;
            shell_installer_draw_preflight();
        }
        return;
    }

    if (action == SHELL_INSTALLER_ACTION_NEXT) {
        if (shell_installer_page == SHELL_INSTALLER_PAGE_PREFERENCES) {
            shell_installer_page = SHELL_INSTALLER_PAGE_TARGET;
            shell_installer_draw_preflight();
        } else if (shell_installer_page == SHELL_INSTALLER_PAGE_TARGET &&
            shell_installer_target_is_ready()) {
            shell_installer_page = SHELL_INSTALLER_PAGE_CONFIRM;
            shell_installer_confirmed = false;
            shell_installer_draw_confirmation();
        }
    }
}

static shell_installer_button_t shell_installer_button_at(
    uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    shell_ui_rect_t* hit_rect) {
    shell_installer_button_t buttons[2];
    size_t button_count = 0;
    switch (shell_installer_page) {
        case SHELL_INSTALLER_PAGE_PREFERENCES:
            buttons[button_count++] = SHELL_INSTALLER_BUTTON_NEXT;
            break;
        case SHELL_INSTALLER_PAGE_TARGET:
            buttons[button_count++] = SHELL_INSTALLER_BUTTON_BACK;
            if (shell_installer_target_is_ready()) {
                buttons[button_count++] = SHELL_INSTALLER_BUTTON_NEXT;
            }
            break;
        case SHELL_INSTALLER_PAGE_CONFIRM:
            buttons[button_count++] = SHELL_INSTALLER_BUTTON_CANCEL;
            if (shell_installer_target_is_ready()) {
                buttons[button_count++] = SHELL_INSTALLER_BUTTON_INSTALL;
            }
            break;
        case SHELL_INSTALLER_PAGE_RESULT:
            buttons[button_count++] = SHELL_INSTALLER_BUTTON_FINISH;
            break;
        default:
            return SHELL_INSTALLER_BUTTON_NONE;
    }

    for (size_t index = 0; index < button_count; index++) {
        shell_ui_rect_t rect;
        if (shell_installer_button_rect(shell_installer_page, buttons[index],
                width, height, &rect) &&
            shell_ui_rect_contains(&rect, x, y)) {
            if (hit_rect) *hit_rect = rect;
            return buttons[index];
        }
    }
    return SHELL_INSTALLER_BUTTON_NONE;
}

static void shell_installer_activate_button(
    shell_installer_button_t button) {
    switch (button) {
        case SHELL_INSTALLER_BUTTON_NEXT:
            shell_installer_navigate(SHELL_INSTALLER_ACTION_NEXT);
            break;
        case SHELL_INSTALLER_BUTTON_BACK:
        case SHELL_INSTALLER_BUTTON_CANCEL:
            shell_installer_navigate(SHELL_INSTALLER_ACTION_BACK);
            break;
        case SHELL_INSTALLER_BUTTON_INSTALL:
            shell_installer_navigate(SHELL_INSTALLER_ACTION_INSTALL);
            break;
        case SHELL_INSTALLER_BUTTON_FINISH:
            shell_installer_navigate(SHELL_INSTALLER_ACTION_FINISH);
            break;
        default:
            break;
    }
}

static void shell_installer_handle_key(const input_key_event_t* event) {
    if (event->ascii == 27) {
        shell_installer_navigate(SHELL_INSTALLER_ACTION_CANCEL);
        return;
    }
    if (event->ascii == '\b') {
        shell_installer_navigate(SHELL_INSTALLER_ACTION_BACK);
        return;
    }
    if (event->ascii == '\n' || event->ascii == '\r') {
        if (shell_installer_page == SHELL_INSTALLER_PAGE_CONFIRM) {
            if (shell_installer_confirmed) {
                shell_installer_navigate(SHELL_INSTALLER_ACTION_INSTALL);
            }
        } else {
            shell_installer_navigate(SHELL_INSTALLER_ACTION_NEXT);
        }
        return;
    }
    if (shell_installer_page == SHELL_INSTALLER_PAGE_CONFIRM &&
        (event->ascii == 'y' || event->ascii == 'Y')) {
        shell_installer_navigate(SHELL_INSTALLER_ACTION_CONFIRM);
        return;
    }
    if (shell_installer_page != SHELL_INSTALLER_PAGE_PREFERENCES) return;
    if (event->ascii == '\t') {
        uint8_t previous_field = shell_installer_field;
        shell_installer_field = (uint8_t)((shell_installer_field + 1) % 3);
        shell_installer_redraw_preference_rows(previous_field);
    } else if (event->is_extended &&
        (event->scancode == 0x48 || event->scancode == 0x50)) {
        uint8_t previous_field = shell_installer_field;
        shell_installer_change_option(event->scancode == 0x50);
        shell_installer_redraw_preference_rows(previous_field);
    }
}

void shell_handle_mouse_event(uint32_t x, uint32_t y, bool left_button_down) {
    if (!shell_installer_active) return;
    shell_profile_enabled = true;

    uint32_t width;
    uint32_t height;
    if (!shell_get_graphics_size(&width, &height)) return;

    if (!left_button_down) {
        if (shell_installer_pressed_button != SHELL_INSTALLER_BUTTON_NONE) {
            shell_installer_button_t button =
                shell_installer_pressed_button;
            uint8_t page = shell_installer_pressed_page;
            shell_ui_rect_t rect;
            shell_installer_pressed_button = SHELL_INSTALLER_BUTTON_NONE;
            if (page == shell_installer_page &&
                shell_installer_button_rect(page, button, width, height,
                    &rect) &&
                shell_ui_rect_contains(&rect, x, y)) {
                shell_installer_activate_button(button);
            }
        }
        return;
    }

    shell_installer_pressed_button = SHELL_INSTALLER_BUTTON_NONE;
    shell_ui_rect_t rect;
    shell_installer_button_t button = shell_installer_button_at(x, y,
        width, height, &rect);
    if (button != SHELL_INSTALLER_BUTTON_NONE) {
        shell_installer_pressed_button = button;
        shell_installer_pressed_page = shell_installer_page;
        return;
    }

    if (shell_installer_page != SHELL_INSTALLER_PAGE_PREFERENCES) return;
    uint32_t center_x = width / 2;
    uint32_t option_x = center_x - 120;
    uint32_t option_width = width - option_x - 76;
    for (uint8_t index = 0; index < 3; index++) {
        uint32_t row_y = height / 2 - 40 + (uint32_t)index * 64;
        if (y < row_y - 3 || y >= row_y + 43 ||
            x < 72 || x >= option_x + option_width) {
            continue;
        }
        uint8_t previous_field = shell_installer_field;
        shell_installer_field = index;
        if (x >= option_x + option_width - 44) {
            shell_installer_change_option(true);
        }
        shell_installer_redraw_preference_rows(previous_field);
        return;
    }
}

static void read_cpu_info(cpu_info_t* info) {
    uint32_t max_leaf;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t eax;

    asm volatile("cpuid"
        : "=a"(max_leaf), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0), "c"(0));

    info->vendor[0] = (char)(ebx & 0xFF);
    info->vendor[1] = (char)((ebx >> 8) & 0xFF);
    info->vendor[2] = (char)((ebx >> 16) & 0xFF);
    info->vendor[3] = (char)((ebx >> 24) & 0xFF);
    info->vendor[4] = (char)(edx & 0xFF);
    info->vendor[5] = (char)((edx >> 8) & 0xFF);
    info->vendor[6] = (char)((edx >> 16) & 0xFF);
    info->vendor[7] = (char)((edx >> 24) & 0xFF);
    info->vendor[8] = (char)(ecx & 0xFF);
    info->vendor[9] = (char)((ecx >> 8) & 0xFF);
    info->vendor[10] = (char)((ecx >> 16) & 0xFF);
    info->vendor[11] = (char)((ecx >> 24) & 0xFF);
    info->vendor[12] = '\0';
    info->family = 0;
    info->model = 0;
    info->stepping = 0;

    if (max_leaf < 1) return;

    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1), "c"(0));
    uint32_t base_family = (eax >> 8) & 0xF;
    uint32_t base_model = (eax >> 4) & 0xF;
    uint32_t extended_family = (eax >> 20) & 0xFF;
    uint32_t extended_model = (eax >> 16) & 0xF;
    info->family = base_family == 0xF ? base_family + extended_family : base_family;
    info->model = (base_family == 0x6 || base_family == 0xF)
        ? base_model | (extended_model << 4) : base_model;
    info->stepping = eax & 0xF;
}

static void shell_print_cpu(void) {
    cpu_info_t info;
    read_cpu_info(&info);
    shell_write("CPU vendor: ");
    shell_write(info.vendor);
    shell_write("\nFamily: ");
    shell_write_decimal(info.family);
    shell_write("  Model: ");
    shell_write_decimal(info.model);
    shell_write("  Stepping: ");
    shell_write_decimal(info.stepping);
    display_putchar('\n');
}

static void shell_print_memory(void) {
    pmm_stats_t stats;
    pmm_get_stats(&stats);
    shell_write("Total: "); shell_write_decimal(stats.total_bytes); shell_write(" bytes\n");
    shell_write("Usable: "); shell_write_decimal(stats.usable_bytes); shell_write(" bytes\n");
    shell_write("Managed: "); shell_write_decimal(stats.managed_bytes); shell_write(" bytes\n");
    shell_write("Free: "); shell_write_decimal(stats.free_bytes); shell_write(" bytes\n");
    shell_write("Used: "); shell_write_decimal(stats.used_bytes); shell_write(" bytes\n");
    shell_write("Reserved/unmanaged: ");
    shell_write_decimal(stats.reserved_bytes);
    shell_write(" bytes\n");
}

static void shell_print_storage(void) {
    block_result_t result = storage_initialization_result();
    shell_write("Storage: ");
    shell_write(block_result_string(result));
    display_putchar('\n');

    const block_device_t* device = storage_get_device();
    if (!device) return;

    shell_write("Device: "); shell_write(device->name);
    shell_write("\nCapacity: "); shell_write_decimal(device->sector_count);
    shell_write(" sectors x "); shell_write_decimal(device->sector_size);
    shell_write(" bytes = ");
    shell_write_decimal(device->sector_count * device->sector_size);
    shell_write(" bytes\nSelf-test reserved LBA: ");
    shell_write_decimal(device->sector_count - STORAGE_TEST_RESERVED_SECTORS);
    display_putchar('\n');
}

static int shell_print_directory_entry(const filesystem_info_t* info, void* context) {
    (void)context;
    shell_write(info->type == FS_TYPE_DIR ? "DIR  " : "FILE ");
    shell_write(info->name);
    shell_write("  ");
    shell_write_decimal(info->size);
    shell_write(" bytes\n");
    return 0;
}

static void shell_print_filesystem_error(int result) {
    shell_write("Filesystem error: ");
    shell_write(filesystem_strerror(result));
    display_putchar('\n');
    audio_play_error_sound();
}

static void shell_print_directory(const char* path) {
    int result = filesystem_list(path, shell_print_directory_entry, NULL);
    if (result != FS_OK) shell_print_filesystem_error(result);
}

static void shell_print_working_directory(void) {
    char path[FS_PATH_MAX];
    int result = filesystem_getcwd(path, sizeof(path));
    if (result != FS_OK) {
        shell_print_filesystem_error(result);
        return;
    }
    shell_write(path);
    display_putchar('\n');
}

static void shell_make_directory(const char* path) {
    int result = filesystem_mkdir(path);
    if (result != FS_OK) shell_print_filesystem_error(result);
}

static void shell_change_directory(const char* path) {
    int result = filesystem_chdir(path);
    if (result != FS_OK) shell_print_filesystem_error(result);
}

static void shell_create_file(const char* path) {
    int result = filesystem_create(path);
    if (result != FS_OK) {
        shell_print_filesystem_error(result);
        return;
    }
    shell_write("Created empty file: ");
    shell_write(path);
    display_putchar('\n');
}

static void shell_write_file(const char* path, const char* contents) {
    uint32_t length = 0;
    while (contents[length] != '\0') length++;
    int result = filesystem_write(path, contents, length);
    if (result != FS_OK) {
        shell_print_filesystem_error(result);
        return;
    }
    shell_write("Wrote ");
    shell_write_decimal(length);
    shell_write(" bytes to ");
    shell_write(path);
    display_putchar('\n');
}

static void shell_append_file(const char* path, const char* contents) {
    uint32_t length = 0;
    while (contents[length] != '\0') length++;
    int result = filesystem_append(path, contents, length);
    if (result != FS_OK) {
        shell_print_filesystem_error(result);
        return;
    }
    shell_write("Appended ");
    shell_write_decimal(length);
    shell_write(" bytes to ");
    shell_write(path);
    display_putchar('\n');
}

static void shell_read_file(const char* path) {
    char contents[512];
    uint32_t bytes_read = 0;
    int result = filesystem_read(path, contents, sizeof(contents) - 1, &bytes_read);
    if (result != FS_OK) {
        shell_print_filesystem_error(result);
        return;
    }

    contents[bytes_read] = '\0';
    shell_write(contents);
    if (bytes_read == 0 || contents[bytes_read - 1] != '\n') display_putchar('\n');
}

static void shell_delete_file(const char* path) {
    int result = filesystem_delete(path);
    if (result != FS_OK) shell_print_filesystem_error(result);
}

static void shell_copy_file(const char* source, const char* destination) {
    filesystem_info_t source_info;
    uint32_t bytes_read = 0;
    int result = filesystem_stat(source, &source_info);
    if (result != FS_OK) {
        shell_print_filesystem_error(result);
        return;
    }
    if (source_info.type != FS_TYPE_FILE || source_info.size > SHELL_COPY_CAPACITY) {
        shell_write("Filesystem error: source is not a supported file\n");
        audio_play_error_sound();
        return;
    }

    result = filesystem_read(source, shell_copy_buffer, SHELL_COPY_CAPACITY,
        &bytes_read);
    if (result != FS_OK) {
        shell_print_filesystem_error(result);
        return;
    }
    result = filesystem_write(destination, shell_copy_buffer, bytes_read);
    if (result != FS_OK) shell_print_filesystem_error(result);
}

static void shell_print_tasks(void) {
    bool found = false;
    shell_write("PID  NAME                             STATE\n");
    for (size_t index = 0; index < MAX_TASKS; index++) {
        task_t* task = task_get_at(index);
        if (!task) continue;
        found = true;
        shell_write_decimal(task->pid);
        shell_write("  ");
        shell_write(task->name);
        shell_write("  ");
        shell_write(task_state_to_string(task->state));
        display_putchar('\n');
    }
    if (!found) shell_write("No active tasks.\n");
}

static void shell_print_pci(void) {
    if (!pci_was_scanned()) {
        shell_write("PCI discovery has not run.\n");
        return;
    }

    size_t count = pci_get_device_count();
    shell_write("PCI devices: ");
    shell_write_decimal(count);
    display_putchar('\n');
    for (size_t index = 0; index < count; index++) {
        const pci_device_t* device = pci_get_device(index);
        if (!device) continue;
        shell_write("BDF ");
        shell_write_hex_digits(device->bus, 2);
        display_putchar(':');
        shell_write_hex_digits(device->device, 2);
        display_putchar('.');
        shell_write_hex_digits(device->function, 1);
        shell_write("  ID ");
        shell_write_hex(device->vendor_id, 4);
        display_putchar(':');
        shell_write_hex(device->device_id, 4);
        shell_write("  Class ");
        shell_write_hex(device->class_code, 2);
        display_putchar(':');
        shell_write_hex(device->subclass, 2);
        shell_write(" ");
        shell_write(pci_class_name(device->class_code));
        shell_write("/");
        shell_write(pci_subclass_name(device->class_code, device->subclass));
        display_putchar('\n');
    }
    if (pci_overflow_count() != 0) {
        shell_write("Discovery list overflow: ");
        shell_write_decimal(pci_overflow_count());
        display_putchar('\n');
    }
}

static void shell_print_uptime(void) {
    uint64_t ticks = timer_get_ticks();
    shell_write("Uptime: ");
    shell_write_decimal(ticks / SHELL_TICKS_PER_SECOND);
    shell_write(" seconds (");
    shell_write_decimal(ticks);
    shell_write(" ticks at 100 Hz)\n");
}

static void shell_print_irq(void) {
    uint16_t masks = pic_get_masks();
    shell_write("PIC master mask: ");
    shell_write_hex(masks & 0xFF, 2);
    shell_write("  slave mask: ");
    shell_write_hex((masks >> 8) & 0xFF, 2);
    shell_write("\nIRQ  VECTOR  STATE\n");
    for (uint8_t irq = 0; irq < 16; irq++) {
        uint8_t mask = irq < 8 ? (uint8_t)masks : (uint8_t)(masks >> 8);
        shell_write_decimal(irq);
        shell_write("    ");
        shell_write_decimal(32 + irq);
        shell_write("      ");
        shell_write((mask & (1u << (irq & 7))) ? "masked" : "unmasked");
        display_putchar('\n');
    }
    shell_write("IDT IRQ vectors: 32-47. Per-IRQ counts are not tracked.\n");
}

static void shell_print_audio(void) {
    bool prepared = audio_prepare_test_tone();
    audio_diagnostics_t diagnostics;
    audio_get_diagnostics(&diagnostics);

    shell_write("AC'97 PCI device: ");
    shell_write(diagnostics.pci_device_found ? "found\n" : "not found\n");
    if (!diagnostics.pci_device_found) return;

    shell_write("PCI command: ");
    shell_write_hex(diagnostics.pci_command, 4);
    shell_write("\nBAR0 (codec): ");
    shell_write_hex(diagnostics.codec_bar_address, 16);
    shell_write("\nBAR1 (bus master): ");
    shell_write_hex(diagnostics.bus_master_bar_address, 16);
    shell_write("\nI/O BARs valid: ");
    shell_write(diagnostics.io_bars_valid ? "yes\n" : "no\n");
    shell_write("Initialized: ");
    shell_write(diagnostics.initialized ? "yes\n" : "no\n");
    shell_write("Codec reset completed: ");
    shell_write(diagnostics.reset_completed ? "yes\n" : "no\n");

    if (!diagnostics.io_bars_valid) return;
    shell_write("Global status: ");
    shell_write_hex(diagnostics.global_status, 8);
    shell_write("\nBDL register/memory: ");
    shell_write_hex(diagnostics.bdl_register, 8);
    display_putchar('/');
    shell_write_hex(diagnostics.bdl_memory_address, 16);
    shell_write("\nPrepared PCM DMA buffer: ");
    shell_write_hex(diagnostics.dma_buffer_address, 16);

    if (!prepared) {
        shell_write("\nDMA test preparation: FAILED\n");
        return;
    }

    shell_write("\nPICB before START: ");
    shell_write_hex(diagnostics.picb_before_start, 4);
    if (!audio_start_prepared()) {
        shell_write("\nDMA test start: FAILED\n");
        return;
    }

    audio_get_diagnostics(&diagnostics);
    shell_write("\nPICB immediately after START: ");
    shell_write_hex(diagnostics.picb_after_start, 4);

    uint64_t start_tick = timer_get_ticks();
    while (timer_get_ticks() - start_tick < 2) {
        asm volatile("pause");
    }

    audio_get_diagnostics(&diagnostics);
    shell_write("\nPICB after 20 ms: ");
    shell_write_hex(diagnostics.picb_current, 4);
    shell_write("\nPCM status/control after 20 ms: ");
    shell_write_hex(diagnostics.stream_status, 2);
    display_putchar('/');
    shell_write_hex(diagnostics.stream_control, 2);
    shell_write("\nDMA progress: ");
    shell_write(diagnostics.picb_after_start > diagnostics.picb_current
        ? "YES\n" : "NO\n");

    shell_write("Active BDL descriptors: ");
    shell_write_decimal(diagnostics.descriptor_count);
    display_putchar('\n');
    for (size_t index = 0; index < diagnostics.descriptor_count; index++) {
        audio_descriptor_info_t descriptor;
        if (!audio_get_descriptor(index, &descriptor)) {
            shell_write("Unable to read BDL descriptor ");
            shell_write_decimal(index);
            display_putchar('\n');
            continue;
        }

        shell_write("BDL[");
        shell_write_decimal(index);
        shell_write("] buffer=");
        shell_write_hex(descriptor.buffer_address, 8);
        shell_write(" samples=");
        shell_write_decimal(descriptor.sample_count);
        shell_write(" bytes=");
        shell_write_decimal((uint32_t)descriptor.sample_count * sizeof(int16_t));
        shell_write(" control/status=");
        shell_write_hex(descriptor.control_status, 4);
        display_putchar('\n');
    }
}

static void shell_print_sysinfo(void) {
    pmm_stats_t stats;
    cpu_info_t cpu;
    pmm_get_stats(&stats);
    read_cpu_info(&cpu);

    shell_write("RockOS "); shell_write(ROCKOS_VERSION);
    shell_write(" (ROK), x86_64\nCPU: ");
    shell_write(cpu.vendor);
    shell_write(" family "); shell_write_decimal(cpu.family);
    shell_write(" model "); shell_write_decimal(cpu.model);
    shell_write("\nMemory: "); shell_write_decimal(stats.total_bytes);
    shell_write(" total, "); shell_write_decimal(stats.free_bytes);
    shell_write(" free bytes\nUptime: ");
    shell_write_decimal(timer_get_ticks() / SHELL_TICKS_PER_SECOND);
    shell_write(" seconds\nTasks: ");
    shell_write_decimal(task_get_count());
    shell_write("  PCI devices: ");
    if (pci_was_scanned()) shell_write_decimal(pci_get_device_count());
    else shell_write("not scanned");
    display_putchar('\n');
}

static void shell_print_help(void) {
    shell_write("help      List supported commands\n");
    shell_write("tour      Start the RockOS Tour\n");
    shell_write("clear     Clear and redraw the terminal\n");
    shell_write("mem       Show PMM memory statistics\n");
    shell_write("storage   Show block-device status and capacity\n");
    shell_write("dir [path] List directory contents\n");
    shell_write("mkdir path Create a directory\n");
    shell_write("cd path   Change the current directory\n");
    shell_write("pwd       Show the current directory\n");
    shell_write("create path Create an empty file\n");
    shell_write("write path text Replace file contents\n");
    shell_write("append path text Append text to a file\n");
    shell_write("edit path  Edit a text file (Ctrl+S saves, Esc cancels)\n");
    shell_write("read path  Read file contents\n");
    shell_write("delete path Delete a file\n");
    shell_write("copy src dst Copy a file\n");
    shell_write("tasks     List kernel tasks and states\n");
    shell_write("pci       List discovered PCI devices\n");
    shell_write("uptime    Show system uptime\n");
    shell_write("cpu       Show CPU vendor and identification\n");
    shell_write("irq       Show IDT vectors and PIC masks\n");
    shell_write("audio     Show AC'97 device and playback diagnostics\n");
    shell_write("ring3fault Trigger the isolated Ring 3 fault test\n");
    shell_write("installer Check for a safe dedicated install disk\n");
    shell_write("version   Show RockOS version and kernel identity\n");
    shell_write("sysinfo   Show a compact system summary\n");
    shell_write("reboot    Reset the machine\n");
    shell_write("shutdown  Power off using ACPI\n");
}

static void shell_reboot(void) __attribute__((noreturn));
static void shell_reboot(void) {
    display_set_color(COLOR_YELLOW, COLOR_BLACK);
    shell_write("Requesting reboot via the PS/2 controller...\n");
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    asm volatile("cli" : : : "memory");
    for (uint32_t attempt = 0; attempt < 1000000; attempt++) {
        if ((inb(0x64) & 0x02) == 0) break;
    }
    outb(0x64, 0xFE);
    for (;;) asm volatile("hlt" : : : "memory");
}

static void shell_execute(void) {
    char argument[SHELL_COMMAND_CAPACITY];
    char second_argument[SHELL_COMMAND_CAPACITY];
    const char* text;

    if (command_buffer[0] == '\0') {
        return;
    } else if (command_is(command_buffer, "help")) {
        shell_print_help();
    } else if (command_is(command_buffer, "tour")) {
        tour_start();
    } else if (command_is(command_buffer, "clear")) {
        shell_draw_screen();
        return;
    } else if (command_is(command_buffer, "mem")) {
        shell_print_memory();
    } else if (command_is(command_buffer, "storage")) {
        shell_print_storage();
    } else if (command_is(command_buffer, "pwd")) {
        shell_print_working_directory();
    } else if (command_is(command_buffer, "dir")) {
        shell_print_directory(".");
    } else if (shell_get_argument(command_buffer, "dir", argument, sizeof(argument))) {
        shell_print_directory(argument);
    } else if (shell_get_argument(command_buffer, "mkdir", argument, sizeof(argument))) {
        shell_make_directory(argument);
    } else if (shell_get_argument(command_buffer, "cd", argument, sizeof(argument))) {
        shell_change_directory(argument);
    } else if (shell_get_argument(command_buffer, "create", argument, sizeof(argument))) {
        shell_create_file(argument);
    } else if (shell_get_path_and_text(command_buffer, "write", argument,
        sizeof(argument), &text)) {
        shell_write_file(argument, text);
    } else if (shell_get_path_and_text(command_buffer, "append", argument,
        sizeof(argument), &text)) {
        shell_append_file(argument, text);
    } else if (shell_get_argument(command_buffer, "edit", argument, sizeof(argument))) {
        shell_editor_open(argument);
    } else if (command_is(command_buffer, "edit")) {
        shell_write("Usage: edit path\n");
    } else if (shell_get_argument(command_buffer, "read", argument, sizeof(argument))) {
        shell_read_file(argument);
    } else if (shell_get_argument(command_buffer, "delete", argument, sizeof(argument))) {
        shell_delete_file(argument);
    } else if (shell_get_two_arguments(command_buffer, "copy", argument,
        second_argument, sizeof(argument))) {
        shell_copy_file(argument, second_argument);
    } else if (command_is(command_buffer, "tasks")) {
        shell_print_tasks();
    } else if (command_is(command_buffer, "pci")) {
        shell_print_pci();
    } else if (command_is(command_buffer, "uptime")) {
        shell_print_uptime();
    } else if (command_is(command_buffer, "cpu")) {
        shell_print_cpu();
    } else if (command_is(command_buffer, "irq")) {
        shell_print_irq();
    } else if (command_is(command_buffer, "audio")) {
        shell_print_audio();
    } else if (command_is(command_buffer, "ring3fault")) {
        if (!task_create("ring3_fault", ring3_fault_test_task)) {
            shell_write("Unable to create the Ring 3 fault-test task.\n");
        }
    } else if (command_is(command_buffer, "installer")) {
        shell_installer_open();
    } else if (command_is(command_buffer, "panic")) {
        asm volatile("int3");
    } else if (command_is(command_buffer, "version")) {
        shell_write("RockOS "); shell_write(ROCKOS_VERSION);
        shell_write(" | ROK freestanding x86_64 kernel\n");
    } else if (command_is(command_buffer, "sysinfo")) {
        shell_print_sysinfo();
    } else if (command_is(command_buffer, "reboot")) {
        shell_reboot();
    } else if (command_is(command_buffer, "shutdown")) {
        shell_write("Requesting ACPI power off...\n");
        if (!acpi_poweroff()) {
            shell_write("ACPI power off is unavailable or the firmware did not power down.\n");
        }
    } else {
        display_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        shell_write("Unknown command: ");
        shell_write(command_buffer);
        display_putchar('\n');
        display_set_color(COLOR_WHITE, COLOR_BLACK);
        audio_play_error_sound();
    }
}

void shell_init(void) {
    if (shell_installer_image && shell_installer_image_size) {
        shell_installer_open();
        return;
    }
    shell_draw_screen();
    shell_draw_prompt();
}

bool shell_is_fullscreen(void) {
    return shell_editor_active || shell_installer_active;
}

bool shell_is_installer_boot(void) {
    return shell_installer_image != 0 && shell_installer_image_size != 0;
}

void shell_handle_key_event(const input_key_event_t* event) {
    if (!event || !event->is_pressed) return;

    if (shell_editor_active) {
        if (!event->is_extended) shell_editor_handle_key(event);
        return;
    }
    if (shell_installer_active) {
        shell_profile_enabled = true;
        shell_installer_handle_key(event);
        return;
    }
    if (event->is_extended) return;

    if (tour_is_active()) {
        tour_handle_key_event(event);
        if (!tour_is_active()) {
            shell_draw_screen();
            shell_draw_prompt();
        }
        return;
    }

    char character = event->ascii;
    if (character == '\b') {
        if (command_length > 0) {
            command_buffer[--command_length] = '\0';
            display_putchar('\b');
        }
        return;
    }

    if (character == '\n' || character == '\r') {
        command_buffer[command_length] = '\0';
        display_putchar('\n');
        shell_execute();
        if (tour_is_active()) return;
        if (command_length != 0 || command_buffer[0] != '\0') {
            command_length = 0;
            command_buffer[0] = '\0';
        }
        if (shell_editor_active) return;
        if (shell_installer_active) return;
        shell_draw_prompt();
        return;
    }

    if (character >= 0x20 && character <= 0x7E &&
        command_length + 1 < SHELL_COMMAND_CAPACITY) {
        command_buffer[command_length++] = character;
        command_buffer[command_length] = '\0';
        display_putchar(character);
    }
}
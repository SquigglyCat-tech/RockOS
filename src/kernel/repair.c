#include "repair.h"

#include "display.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "ps2.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------- */
/* "RockOS Repair could not repair your computer"                          */
/*                                                                         */
/* Layout was designed on a ~1154x637 canvas and is scaled by screen        */
/* height. Text is the native 20px Selawik UI font (not scaled).            */
/* ---------------------------------------------------------------------- */

#define DESIGN_HEIGHT    637U
#define PANIC_DWELL_MS   5000U      /* how long the "!!!" screen stays up */

#define BG_R 222
#define BG_G 229
#define BG_B 232
#define ICON_DARK 74

#define TEXT_HEIGHT 20U

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
} rect_t;

enum {
    PAGE_MAIN = 0,
    PAGE_DETAILS,
    PAGE_REINSTALL
};

#define MAX_ITEMS 3U

static const repair_crash_info_t* g_info;
static uint32_t g_width;
static uint32_t g_height;
static uint32_t g_scale;            /* percent, 100 = design size */
static uint32_t g_page;
static uint32_t g_selected;
static uint32_t g_main_selected;

static uint32_t sc(uint32_t value) {
    return (value * g_scale + 50U) / 100U;
}

static void fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    uint8_t r, uint8_t g, uint8_t b) {
    if (w == 0 || h == 0) return;
    display_fill_rect_rgb(x, y, w, h, r, g, b);
}

static void text(uint32_t x, uint32_t y, const char* s, bool bold) {
    display_draw_text_ui(x, y, s, COLOR_DARK_GRAY);
    if (bold) display_draw_text_ui(x + 1U, y, s, COLOR_DARK_GRAY);
}

static void text_centered(const rect_t* r, const char* s, bool bold) {
    uint32_t w = display_measure_text_ui(s);
    uint32_t x = r->x + (w < r->w ? (r->w - w) / 2U : 0);
    uint32_t y = r->y + (r->h > TEXT_HEIGHT ? (r->h - TEXT_HEIGHT) / 2U : 0);
    text(x, y, s, bold);
}

static void build_hex_line(char* out, size_t capacity, const char* label,
    uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    size_t n = 0;
    while (*label && n + 1 < capacity) out[n++] = *label++;
    if (n + 3 < capacity) {
        out[n++] = '0';
        out[n++] = 'x';
    }
    for (int shift = 60; shift >= 0 && n + 1 < capacity; shift -= 4) {
        out[n++] = digits[(value >> shift) & 0xF];
    }
    out[n] = '\0';
}

static void build_text_line(char* out, size_t capacity, const char* label,
    const char* value) {
    size_t n = 0;
    while (*label && n + 1 < capacity) out[n++] = *label++;
    while (*value && n + 1 < capacity) out[n++] = *value++;
    out[n] = '\0';
}

/* ---------------- machine restart ---------------- */

static void restart_machine(void) __attribute__((noreturn));
static void restart_machine(void) {
    asm volatile ("cli" : : : "memory");
    for (uint32_t attempt = 0; attempt < 1000000U; attempt++) {
        if ((inb(0x64) & 0x02) == 0) break;
    }
    outb(0x64, 0xFE);                       /* 8042 CPU reset line */
    for (uint32_t spin = 0; spin < 200000U; spin++) io_wait();

    /* Fallback: load an empty IDT and fault -> triple fault -> reset. */
    struct {
        uint16_t limit;
        uintptr_t base;
    } __attribute__((packed)) empty_idt = { 0, 0 };
    asm volatile ("lidt %0\n\tint3" : : "m"(empty_idt));
    for (;;) asm volatile ("cli; hlt");
}

/* ---------------- timing (PIT channel 2, interrupts stay off) ---------------- */

/* ~50 ms busy wait. Channel 2 is the PC speaker timer, which we can poll
 * without touching the IRQ0 tick or the scheduler. The poll loop is bounded
 * so an emulator that doesn't model channel 2 can never hang us here. */
static void delay_50ms(void) {
    uint8_t gate = inb(0x61);
    outb(0x61, (uint8_t)((gate & 0xFC) | 0x01));   /* gate on, speaker off */
    outb(0x43, 0xB0);                              /* ch2, lo/hi, mode 0 */
    outb(0x42, 0x0B);                              /* 59659 = 50 ms */
    outb(0x42, 0xE9);
    for (uint32_t guard = 0; guard < 400000U; guard++) {
        if (inb(0x61) & 0x20) break;
    }
    outb(0x61, gate);
}

/* ---------------- input (polled straight from the 8042) ---------------- */

static void poll_input(void) {
    for (int i = 0; i < 16; i++) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) == 0) break;
        keyboard_irq_handler();
        mouse_irq_handler();
    }
}

/* ---------------- layout ---------------- */

static uint32_t page_items(uint32_t page, rect_t* out) {
    if (page == PAGE_MAIN) {
        for (uint32_t i = 0; i < 3; i++) {
            out[i].x = sc(28);
            out[i].y = sc(106) + i * sc(165);
            out[i].w = sc(267);
            out[i].h = sc(155);
        }
        return 3;
    }
    uint32_t button_w = sc(150);
    uint32_t button_h = sc(40);
    uint32_t y = g_height - sc(100);
    out[0].x = sc(28);
    out[0].y = y;
    out[0].w = button_w;
    out[0].h = button_h;
    out[1].x = sc(28) + button_w + sc(16);
    out[1].y = y;
    out[1].w = button_w;
    out[1].h = button_h;
    return 2;
}

static bool point_in_rect(const rect_t* r, int32_t px, int32_t py) {
    if (px < 0 || py < 0) return false;
    uint32_t x = (uint32_t)px;
    uint32_t y = (uint32_t)py;
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* ---------------- icons ---------------- */

static void draw_box(const rect_t* r, bool selected) {
    uint32_t border = selected ? 3U : 1U;
    uint8_t edge = selected ? 55 : 110;
    uint8_t inner = selected ? 248 : 255;
    fill(r->x, r->y, r->w, r->h, edge, edge, edge);
    fill(r->x + border, r->y + border, r->w - 2U * border,
        r->h - 2U * border, inner, inner, inner);
}

static void draw_monitor(const rect_t* tile) {
    uint32_t ox = tile->x + sc(62);
    uint32_t oy = tile->y + sc(22);
    uint32_t ow = sc(144);
    uint32_t oh = sc(89);
    fill(ox, oy, ow, oh, ICON_DARK, ICON_DARK, ICON_DARK);
    uint32_t bezel = sc(6);
    uint32_t ix = ox + bezel;
    uint32_t iy = oy + bezel;
    uint32_t iw = ow - 2U * bezel;
    uint32_t ih = oh - 2U * bezel;
    fill(ix, iy, iw, ih, 255, 255, 255);
    fill(tile->x + sc(129), tile->y + sc(111), sc(10), sc(15),
        ICON_DARK, ICON_DARK, ICON_DARK);
    fill(tile->x + sc(102), tile->y + sc(126), sc(64), sc(6),
        ICON_DARK, ICON_DARK, ICON_DARK);

    uint32_t divisor = iw >= 124U ? 4U : 8U;
    uint32_t logo_w = DISPLAY_ROCKOS_LOGO_WIDTH / divisor;
    uint32_t logo_h = DISPLAY_ROCKOS_LOGO_HEIGHT / divisor;
    if (iw >= logo_w && ih >= logo_h) {
        display_draw_rockos_logo_div(ix + (iw - logo_w) / 2U,
            iy + (ih - logo_h) / 2U, COLOR_DARK_GRAY, divisor);
    }
}

static bool gear_inside(int32_t dx, int32_t dy, int32_t body, int32_t tip,
    int32_t half_tooth) {
    int32_t r2 = dx * dx + dy * dy;
    if (r2 <= body * body) return true;
    if (r2 > tip * tip || half_tooth <= 0) return false;
    int32_t ax = dx < 0 ? -dx : dx;
    int32_t ay = dy < 0 ? -dy : dy;
    if (ax <= half_tooth || ay <= half_tooth) return true;   /* 4 axis teeth */
    int32_t d1 = dx - dy;
    int32_t d2 = dx + dy;
    if (d1 < 0) d1 = -d1;
    if (d2 < 0) d2 = -d2;
    int32_t diagonal = (half_tooth * 1414) / 1000;
    return d1 <= diagonal || d2 <= diagonal;                 /* 4 diagonal */
}

static void draw_gear(const rect_t* tile) {
    int32_t cx = (int32_t)(tile->x + sc(130));
    int32_t cy = (int32_t)(tile->y + sc(78));
    int32_t body = (int32_t)sc(44);
    int32_t tip = (int32_t)sc(58);
    int32_t half_tooth = (int32_t)sc(14);
    int32_t thick = (int32_t)sc(4);
    if (thick < 2) thick = 2;
    int32_t hole = (int32_t)sc(21);

    for (int32_t y = -tip; y <= tip; y++) {
        int32_t run_start = 0;
        bool in_run = false;
        for (int32_t x = -tip; x <= tip + 1; x++) {
            bool on = false;
            if (x <= tip) {
                int32_t r2 = x * x + y * y;
                bool outline =
                    gear_inside(x, y, body, tip, half_tooth) &&
                    !gear_inside(x, y, body - thick, tip - thick,
                        half_tooth - thick);
                bool ring = r2 <= hole * hole &&
                    r2 > (hole - thick) * (hole - thick);
                on = outline || ring;
            }
            if (on && !in_run) {
                run_start = x;
                in_run = true;
            } else if (!on && in_run) {
                fill((uint32_t)(cx + run_start), (uint32_t)(cy + y),
                    (uint32_t)(x - run_start), 1,
                    ICON_DARK, ICON_DARK, ICON_DARK);
                in_run = false;
            }
        }
    }
}

static void draw_big_logo(const rect_t* tile) {
    uint32_t logo_w = DISPLAY_ROCKOS_LOGO_WIDTH / 2U;
    uint32_t logo_h = DISPLAY_ROCKOS_LOGO_HEIGHT / 2U;
    if (tile->w < logo_w || tile->h < logo_h) return;
    display_draw_rockos_logo_half(tile->x + (tile->w - logo_w) / 2U,
        tile->y + (tile->h - logo_h) / 2U, COLOR_DARK_GRAY);
}

/* ---------------- rendering ---------------- */

static void draw_hex_pair(uint32_t y, const char* left_label,
    uint64_t left_value, const char* right_label, uint64_t right_value) {
    char line[48];
    build_hex_line(line, sizeof(line), left_label, left_value);
    text(sc(28), y, line, false);
    if (right_label) {
        build_hex_line(line, sizeof(line), right_label, right_value);
        text(g_width / 2U, y, line, false);
    }
}

static void render(void) {
    const repair_crash_info_t* info = g_info;
    rect_t items[MAX_ITEMS];
    uint32_t item_count = page_items(g_page, items);

    fill(0, 0, g_width, g_height, BG_R, BG_G, BG_B);

    const char* heading = "RockOS Repair could not repair your computer.";
    const char* subheading =
        "Try these options to repair manually. If you can't, contact a "
        "local tech specialist or reinstall RockOS.";
    if (g_page == PAGE_DETAILS) {
        heading = "Advanced repair options";
        subheading = "What went wrong. Share this with the RockOS "
            "developers if it keeps happening.";
    } else if (g_page == PAGE_REINSTALL) {
        heading = "Reinstall RockOS";
        subheading = "Your data may be lost.";
    }
    text(sc(28), 24, heading, true);
    text(sc(28), 52, subheading, false);

    const char* footer = "RockOS, all rights reserved";
    uint32_t footer_w = display_measure_text_ui(footer);
    text(g_width > footer_w + sc(20) ? g_width - footer_w - sc(20) : 0,
        g_height - 34U, footer, false);

    if (g_page == PAGE_MAIN) {
        static const char* const titles[3] = {
            "Exit and continue to RockOS.",
            "Advanced repair options.",
            "Reinstall RockOS."
        };
        static const char* const details[3] = {
            "Attempt to exit and continue to the regular RockOS desktop.",
            "Advanced repair options, such as crash details and more.",
            "Reinstall the RockOS operating system. Your data may be lost."
        };
        for (uint32_t i = 0; i < 3; i++) {
            draw_box(&items[i], i == g_selected);
            if (i == 0) draw_monitor(&items[i]);
            else if (i == 1) draw_gear(&items[i]);
            else draw_big_logo(&items[i]);
            uint32_t tx = items[i].x + items[i].w + sc(12);
            text(tx, items[i].y + 8U, titles[i], i == g_selected);
            text(tx, items[i].y + 36U, details[i], false);
        }
    } else if (g_page == PAGE_DETAILS) {
        char line[80];
        uint32_t y = sc(110);
        build_text_line(line, sizeof(line), "Stop code:   ", info->stop_code);
        text(sc(28), y, line, true);
        y += 30U;
        build_text_line(line, sizeof(line), "Exception:   ", info->name);
        text(sc(28), y, line, false);
        y += 40U;
        draw_hex_pair(y, "VECTOR  ", info->vector, "ERROR CODE  ",
            info->error_code);
        y += 30U;
        draw_hex_pair(y, "RIP  ", info->rip, "CS  ", info->cs);
        y += 30U;
        draw_hex_pair(y, "RFLAGS  ", info->rflags, "RSP  ", info->rsp);
        y += 30U;
        draw_hex_pair(y, "RAX  ", info->rax, "RBX  ", info->rbx);
        y += 30U;
        draw_hex_pair(y, "RCX  ", info->rcx, "RDX  ", info->rdx);
        if (info->has_cr2) {
            y += 30U;
            draw_hex_pair(y, "CR2  ", info->cr2, NULL, 0);
        }
    } else {
        uint32_t y = sc(110);
        text(sc(28), y,
            "Reinstalling has to be started from the RockOS install media.",
            false);
        text(sc(28), y + 30U,
            "Restart the computer, boot from the RockOS ISO or USB drive,",
            false);
        text(sc(28), y + 60U, "then choose Install.", false);
    }

    if (g_page != PAGE_MAIN) {
        for (uint32_t i = 0; i < item_count; i++) {
            draw_box(&items[i], i == g_selected);
            text_centered(&items[i], i == 0 ? "Back" : "Restart",
                i == g_selected);
        }
    }

    display_present();
}

/* ---------------- actions ---------------- */

static void go_to_page(uint32_t page, uint32_t selected) {
    g_page = page;
    g_selected = selected;
}

static void activate(uint32_t item) {
    if (g_page == PAGE_MAIN) {
        g_main_selected = item;
        if (item == 0) restart_machine();
        if (item == 1) go_to_page(PAGE_DETAILS, 0);
        if (item == 2) go_to_page(PAGE_REINSTALL, 0);
    } else {
        if (item == 0) go_to_page(PAGE_MAIN, g_main_selected);
        else restart_machine();
    }
}

static void move_selection(int32_t direction) {
    rect_t items[MAX_ITEMS];
    uint32_t count = page_items(g_page, items);
    int32_t next = (int32_t)g_selected + direction;
    if (next < 0) next = (int32_t)count - 1;
    if (next >= (int32_t)count) next = 0;
    g_selected = (uint32_t)next;
}

/* ---------------- entry point ---------------- */

void repair_screen_run(const repair_crash_info_t* info) {
    g_info = info;
    if (!display_get_framebuffer_size(&g_width, &g_height)) {
        for (;;) asm volatile ("cli; hlt");
    }
    g_scale = (g_height * 100U) / DESIGN_HEIGHT;
    if (g_scale < 80U) g_scale = 80U;
    if (g_scale > 220U) g_scale = 220U;

    /* Throw away anything typed before the crash (e.g. the Enter that ran
     * the command), then let the "!!!" screen sit for a few seconds. */
    input_key_event_t event;
    poll_input();
    while (keyboard_get_event(&event)) { }
    for (uint32_t waited = 0; waited < PANIC_DWELL_MS; waited += 50U) {
        delay_50ms();
        poll_input();
        bool skip = false;
        while (keyboard_get_event(&event)) {
            if (event.is_pressed) skip = true;
        }
        if (skip) break;
    }

    mouse_set_bounds(g_width, g_height);
    input_mouse_state_t mouse;
    mouse_get_state(&mouse);
    int32_t last_x = mouse.x;
    int32_t last_y = mouse.y;
    uint32_t last_press = mouse.left_press_count;

    g_page = PAGE_MAIN;
    g_selected = 0;
    g_main_selected = 0;
    render();
    display_set_mouse_position((uint32_t)mouse.x, (uint32_t)mouse.y);

    for (;;) {
        poll_input();
        bool redraw = false;

        while (keyboard_get_event(&event)) {
            if (!event.is_pressed) continue;
            uint32_t before_page = g_page;
            uint32_t before_sel = g_selected;
            switch (event.scancode) {
                case 0x48: case 0x4B:                  /* up, left */
                    move_selection(-1);
                    break;
                case 0x50: case 0x4D: case 0x0F:       /* down, right, tab */
                    move_selection(1);
                    break;
                case 0x1C: case 0x39:                  /* enter, space */
                    activate(g_selected);
                    break;
                case 0x01:                             /* esc */
                    if (g_page != PAGE_MAIN) {
                        go_to_page(PAGE_MAIN, g_main_selected);
                    }
                    break;
                default:
                    break;
            }
            if (g_page != before_page || g_selected != before_sel) {
                redraw = true;
            }
        }

        mouse_get_state(&mouse);
        rect_t items[MAX_ITEMS];
        uint32_t count = page_items(g_page, items);

        if (mouse.x != last_x || mouse.y != last_y) {
            last_x = mouse.x;
            last_y = mouse.y;
            for (uint32_t i = 0; i < count; i++) {
                if (point_in_rect(&items[i], mouse.x, mouse.y)) {
                    if (g_selected != i) {
                        g_selected = i;
                        redraw = true;
                    }
                    break;
                }
            }
        }

        if (mouse.left_press_count != last_press) {
            last_press = mouse.left_press_count;
            for (uint32_t i = 0; i < count; i++) {
                if (point_in_rect(&items[i], mouse.press_x, mouse.press_y)) {
                    activate(i);
                    redraw = true;
                    break;
                }
            }
        }

        if (redraw) {
            render();
        }
        display_set_mouse_position((uint32_t)mouse.x, (uint32_t)mouse.y);
        io_wait();
    }
}

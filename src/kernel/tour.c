#include "tour.h"

#include "display.h"

#define TOUR_PAGE_COUNT 5

typedef struct {
    const char* title;
    const char* body;
} tour_page_t;

static const tour_page_t tour_pages[TOUR_PAGE_COUNT] = {
    {"Welcome to RockOS", "RockOS, also known as Rock Operating System, is a small x86_64 operating system built from the ground up.\n\nThis tour introduces the shell and the tools available in this build."},
    {"Your desktop", "This build starts in a text-mode workspace rather than a graphical desktop.\n\nThe status area shows the running task and system uptime. The ROK> prompt is where you enter commands."},
    {"Explore the system", "Type help to see available commands. Try sysinfo for a system summary, or use cpu, mem, uptime, and tasks to inspect the kernel."},
    {"Files and storage", "Use dir and pwd to explore RockFS. Create directories with mkdir, move with cd, and inspect files with create, write, and read.\n\nUse storage to check the detected block device."},
    {"Tour complete", "Congratulations! You have completed the RockOS Tour.\n\nType help at the prompt whenever you want to explore more."}
};

static uint8_t current_page;
static bool active;

static void tour_draw_page(void) {
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_clear();
    display_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    display_write_at(2, 2, "ROCKOS TOUR");
    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    display_write_at(2, 3, "------------------------------------------------------------");
    display_set_color(COLOR_YELLOW, COLOR_BLACK);
    display_write_at(2, 5, tour_pages[current_page].title);
    display_set_color(COLOR_WHITE, COLOR_BLACK);
    display_set_cursor(2, 7);
    display_puts(tour_pages[current_page].body);
    display_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    display_write_at(2, 21, current_page + 1 < TOUR_PAGE_COUNT
        ? "Enter or Space: next    Esc: exit"
        : "Enter or Space: finish  Esc: exit");
    display_set_color(COLOR_WHITE, COLOR_BLACK);
}

void tour_start(void) {
    current_page = 0;
    active = true;
    tour_draw_page();
}

bool tour_is_active(void) {
    return active;
}

void tour_handle_key_event(const input_key_event_t* event) {
    if (!active || !event || !event->is_pressed || event->is_extended) return;

    if (event->ascii == 27) {
        active = false;
        return;
    }
    if (event->ascii != '\n' && event->ascii != '\r' && event->ascii != ' ') return;

    if (current_page + 1 < TOUR_PAGE_COUNT) {
        current_page++;
        tour_draw_page();
    } else {
        active = false;
    }
}
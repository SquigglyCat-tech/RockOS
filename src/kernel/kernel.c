#include "display.h"
#include "ps2.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "task.h"
#include "process.h"
#include "ring3.h"
#include "pic.h"
#include "idt.h"
#include "gdt.h"
#include "io.h"
#include "pmm.h"
#include "pci.h"
#include "storage.h"
#include "filesystem.h"
#include "shell.h"
#include "audio.h"
#include "serial.h"

#include <stddef.h>
#include <stdint.h>

#define STATUS_DISPLAY_X 0
#define STATUS_DISPLAY_WIDTH 80
#define STATUS_NAME_WIDTH 12
#define SCHEDULER_DISPLAY_WIDTH 80
#define UPTIME_DISPLAY_MODULUS 10000000000ULL

static volatile uint32_t task_a_progress = 0;
static volatile uint32_t task_b_progress = 0;
static volatile uint32_t sleep_wake_count = 0;
static volatile uint32_t exit_task_runs = 0;
static pid_t exit_task_pid = 0;

static bool pmm_self_test(void) {
    pmm_stats_t before;
    pmm_stats_t after;
    pmm_get_stats(&before);

    void* single_page = pmm_alloc(1);
    void* run = pmm_alloc(3);
    if (!single_page || !run ||
        ((uintptr_t)single_page & (PMM_PAGE_SIZE - 1)) != 0 ||
        ((uintptr_t)run & (PMM_PAGE_SIZE - 1)) != 0 ||
        (uintptr_t)run + 3 * PMM_PAGE_SIZE > PMM_IDENTITY_MAP_LIMIT) {
        if (single_page) pmm_free(single_page, 1);
        if (run) pmm_free(run, 3);
        return false;
    }

    volatile uint64_t* single_word = (volatile uint64_t*)single_page;
    *single_word = 0x524F4B504D4D3131ULL;
    bool contents_ok = *single_word == 0x524F4B504D4D3131ULL &&
        !pmm_is_free((uintptr_t)single_page, 1) &&
        !pmm_is_free((uintptr_t)run, 3);
    volatile uint64_t* run_words[3];
    for (uint64_t i = 0; i < 3; i++) {
        run_words[i] = (volatile uint64_t*)((uintptr_t)run + i * PMM_PAGE_SIZE);
        *run_words[i] = 0x5041474500000000ULL | i;
        contents_ok = contents_ok && *run_words[i] == (0x5041474500000000ULL | i);
    }

    bool single_free_ok = pmm_free(single_page, 1);
    bool run_free_ok = pmm_free(run, 3);
    bool free_state_ok = pmm_is_free((uintptr_t)single_page, 1) &&
        pmm_is_free((uintptr_t)run, 3);
    bool double_free_rejected = !pmm_free(run, 3);
    pmm_get_stats(&after);
    return contents_ok && single_free_ok && run_free_ok && free_state_ok &&
        double_free_rejected &&
        before.free_bytes == after.free_bytes && before.used_bytes == after.used_bytes;
}

static void task_a_entry(void) {
    uint64_t work = 0;
    for (;;) {
        work++;
        if ((work & 0xFFFF) == 0) task_a_progress++;
    }
}

static void task_b_entry(void) {
    uint64_t work = 0;
    for (;;) {
        work++;
        if ((work & 0xFFFF) == 0) task_b_progress++;
    }
}

static void sleep_test_entry(void) {
    for (;;) {
        if (task_sleep_ticks(25)) {
            sleep_wake_count++;
        }
    }
}

static void exit_task_entry(void) {
    exit_task_runs++;
    task_exit();
}

static uint8_t append_decimal(char* buffer, uint8_t position, uint64_t value) {
    char digits[10];
    uint8_t digit_count = 0;

    do {
        digits[digit_count++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && digit_count < sizeof(digits));

    while (digit_count > 0) {
        buffer[position++] = digits[--digit_count];
    }
    return position;
}

static void render_status(uint64_t current_ticks, const task_t* current_task) {
    char status[STATUS_DISPLAY_WIDTH + 1];
    uint8_t position = 0;
    const char task_label[] = "Task: ";
    const char ticks_label[] = " | Ticks: ";
    const char uptime_label[] = " | Up: ";

    for (uint8_t i = 0; task_label[i] != '\0'; i++) {
        status[position++] = task_label[i];
    }
    const char* name = current_task ? current_task->name : "none";
    for (uint8_t i = 0; name[i] != '\0' && i < STATUS_NAME_WIDTH; i++) {
        status[position++] = name[i];
    }

    for (uint8_t i = 0; ticks_label[i] != '\0'; i++) {
        status[position++] = ticks_label[i];
    }
    position = append_decimal(status, position, current_ticks % UPTIME_DISPLAY_MODULUS);

    for (uint8_t i = 0; uptime_label[i] != '\0'; i++) {
        status[position++] = uptime_label[i];
    }
    position = append_decimal(status, position,
        (current_ticks / 100) % UPTIME_DISPLAY_MODULUS);
    status[position++] = 's';

    while (position < STATUS_DISPLAY_WIDTH) {
        status[position++] = ' ';
    }
    status[position] = '\0';

    display_set_color(COLOR_YELLOW, COLOR_DARK_GRAY);
    display_write_at(STATUS_DISPLAY_X, 4, status);
    display_set_color(COLOR_WHITE, COLOR_BLACK);
}

static void render_exit_test(void) {
    char status[SCHEDULER_DISPLAY_WIDTH + 1];
    uint8_t position = 0;
    const char prefix[] = "Exit task runs: ";
    const char* state_text;
    task_t* exit_task = exit_task_pid ? task_get_by_pid(exit_task_pid) : NULL;

    for (uint8_t i = 0; prefix[i] != '\0'; i++) {
        status[position++] = prefix[i];
    }
    position = append_decimal(status, position, exit_task_runs);
    if (!exit_task_pid) {
        state_text = " | NOT CREATED";
    } else if (!exit_task) {
        state_text = " | REAPED";
    } else if (exit_task->state == TASK_STATE_ZOMBIE) {
        state_text = " | ZOMBIE";
    } else {
        state_text = " | ACTIVE";
    }
    for (uint8_t i = 0; state_text[i] != '\0'; i++) {
        status[position++] = state_text[i];
    }
    while (position < SCHEDULER_DISPLAY_WIDTH) {
        status[position++] = ' ';
    }
    status[position] = '\0';

    display_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    display_write_at(0, 2, status);
    display_set_color(COLOR_WHITE, COLOR_BLACK);
}

static void print_number(uint64_t value) {
    char buffer[11];
    uint8_t length = append_decimal(buffer, 0, value);
    buffer[length] = '\0';
    display_puts(buffer);
}

static void render_scheduler_test(void) {
    char status[SCHEDULER_DISPLAY_WIDTH + 1];
    uint8_t position = 0;
    const char prefix[] = "Preempt progress A: ";
    const char separator[] = "  B: ";

    for (uint8_t i = 0; prefix[i] != '\0'; i++) {
        status[position++] = prefix[i];
    }
    position = append_decimal(status, position, task_a_progress);
    for (uint8_t i = 0; separator[i] != '\0'; i++) {
        status[position++] = separator[i];
    }
    position = append_decimal(status, position, task_b_progress);
    while (position < SCHEDULER_DISPLAY_WIDTH) {
        status[position++] = ' ';
    }
    status[position] = '\0';

    display_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    display_write_at(0, 1, status);
    display_set_color(COLOR_WHITE, COLOR_BLACK);
}

static void render_sleep_test(void) {
    char status[SCHEDULER_DISPLAY_WIDTH + 1];
    uint8_t position = 0;
    const char prefix[] = "Sleep wake count: ";

    for (uint8_t i = 0; prefix[i] != '\0'; i++) {
        status[position++] = prefix[i];
    }
    position = append_decimal(status, position, sleep_wake_count);
    while (position < SCHEDULER_DISPLAY_WIDTH) {
        status[position++] = ' ';
    }
    status[position] = '\0';

    display_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    display_write_at(0, 3, status);
    display_set_color(COLOR_WHITE, COLOR_BLACK);
}

static void render_mouse_cursor(void) {
    input_mouse_state_t mouse_state;
    mouse_get_state(&mouse_state);

    if (display_set_mouse_position((uint32_t)mouse_state.x,
            (uint32_t)mouse_state.y)) {
        return;
    }

    int32_t screen_x = mouse_state.x / 8;
    int32_t screen_y = 5 + mouse_state.y / 8;
    if (screen_x < 0) screen_x = 0;
    if (screen_x >= 80) screen_x = 79;
    if (screen_y < 5) screen_y = 5;
    if (screen_y >= 25) screen_y = 24;
    display_set_mouse_cursor((uint8_t)screen_x, (uint8_t)screen_y);
}

void kernel_main(uint32_t boot_magic, uintptr_t boot_info_address) {
    // 1. Keep interrupts disabled during initialization
    asm volatile ("cli");

    serial_init();
    serial_write_string("[boot] kernel_main entered; magic=");
    serial_write_hex(boot_magic);
    serial_write_string(" info=");
    serial_write_hex(boot_info_address);
    serial_write_string("\n");

    // 2. Initialize display subsystem
    display_init();

    // 3. Print startup banner
    display_set_color(COLOR_YELLOW, COLOR_BLACK);
    display_puts("ROCKOS 0.1 | Running on the ROK (RockOS Kernel)\n");
    display_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    display_puts("The Rock Has Booted.\n\n");

    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    display_puts("[+] Initializing Multiboot1 PMM (identity mapped below 1 GiB)...\n");
    serial_write_string("[boot] initializing PMM\n");
    if (!pmm_init(boot_magic, boot_info_address)) {
        serial_write_string("[boot] PMM initialization failed\n");
        display_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        display_puts("[-] PMM initialization failed; halting.\n");
        audio_play_error_sound();
        for (;;) asm volatile ("cli; hlt" : : : "memory");
    }
    serial_write_string("[boot] PMM initialized\n[boot] initializing framebuffer\n");
    if (display_init_framebuffer(boot_info_address)) {
        serial_write_string("[boot] framebuffer initialization succeeded\n");
        display_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        display_puts("[+] Framebuffer graphics enabled.\n");
        display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    } else {
        serial_write_string("[boot] framebuffer initialization failed: ");
        serial_write_string(display_framebuffer_status());
        serial_write_string("\n");
        display_set_color(COLOR_YELLOW, COLOR_BLACK);
        display_puts("[!] Framebuffer unavailable; using VGA text output.\n");
        display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    }
    shell_set_boot_info(boot_info_address);
    if (!pmm_self_test()) {
        display_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        display_puts("[-] PMM self-test failed; halting.\n");
        audio_play_error_sound();
        for (;;) asm volatile ("cli; hlt" : : : "memory");
    }
    display_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    display_puts("[+] PMM allocation/free self-test passed.\n\n");

    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    display_puts("[+] Initializing PIC & IDT...\n");
    pic_init();
    idt_init();

    display_puts("[+] Initializing PIT Timer (100 Hz)...\n");
    timer_init(100);

    display_puts("[+] Initializing PCI Bus...\n");
    pci_init();
    pci_scan();

    display_puts("[+] Initializing AC'97 PCM audio...\n");
    if (audio_init()) {
        display_puts("[+] AC'97 audio ready.\n");
    } else {
        display_puts("[-] AC'97 audio device unavailable.\n");
        audio_play_error_sound();
    }

    display_puts("[+] Initializing ATA PIO block storage...\n");
    block_result_t storage_result = storage_init();
    if (storage_result == BLOCK_RESULT_OK) {
        const block_device_t* device = storage_get_device();
        display_puts("[+] Storage ready: ");
        display_puts(device->name);
        display_puts(", ");
        print_number(device->sector_count);
        display_puts(" sectors x ");
        print_number(device->sector_size);
        display_puts(" bytes\n");

        block_result_t test_result = storage_self_test();
        display_puts(test_result == BLOCK_RESULT_OK
            ? "[+] Storage reserved-sector self-test passed.\n"
            : "[-] Storage self-test failed: ");
        if (test_result != BLOCK_RESULT_OK) {
            display_puts(block_result_string(test_result));
            display_putchar('\n');
            audio_play_error_sound();
        }

        int filesystem_result = filesystem_init();
        if (filesystem_result == FS_OK) {
            display_puts("[+] RockFS initialized.\n");
            int filesystem_test_result = filesystem_selftest();
            display_puts(filesystem_test_result == FS_OK
                ? "[+] RockFS self-test passed.\n"
                : "[-] RockFS self-test failed.\n");
            if (filesystem_test_result != FS_OK) audio_play_error_sound();
        } else {
            display_puts("[-] RockFS initialization failed.\n");
            audio_play_error_sound();
        }
    } else {
        display_puts("[-] Storage unavailable: ");
        display_puts(block_result_string(storage_result));
        display_putchar('\n');
        audio_play_error_sound();
    }

    display_puts("[+] Initializing Task Subsystem...\n");
    task_init();
    task_t* kernel_task = task_get_current();
    if (!kernel_task || !gdt_init(kernel_task->kernel_stack_base +
        kernel_task->kernel_stack_size)) {
        display_puts("[-] GDT/TSS initialization failed; halting.\n");
        audio_play_error_sound();
        for (;;) asm volatile ("cli; hlt" : : : "memory");
    }
    process_init();
    if (!task_create_idle()) {
        display_puts("[-] Unable to register idle task.\n");
        audio_play_error_sound();
    }
    if (!task_create("worker_a", task_a_entry) ||
        !task_create("worker_b", task_b_entry)) {
        display_puts("[-] Unable to register preemption test tasks.\n");
        audio_play_error_sound();
    }
    if (!task_create("sleep_test", sleep_test_entry)) {
        display_puts("[-] Unable to register sleep test task.\n");
        audio_play_error_sound();
    }
    task_t* exit_task = task_create("exit_demo", exit_task_entry);
    if (exit_task) {
        exit_task_pid = exit_task->pid;
    } else {
        display_puts("[-] Unable to register exit test task.\n");
        audio_play_error_sound();
    }
    if (!shell_is_installer_boot() &&
        !task_create("ring3_test", ring3_test_task)) {
        display_puts("[-] Unable to register Ring 3 test task.\n");
        audio_play_error_sound();
    }

    display_puts("[+] Initializing PS/2 Controller...\n");
    ps2_init();

    display_puts("[+] Initializing Keyboard Driver...\n");
    keyboard_init();

    display_puts("[+] Initializing Mouse Driver...\n");
    mouse_init();
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    if (display_get_framebuffer_size(&framebuffer_width,
            &framebuffer_height)) {
        mouse_set_bounds(framebuffer_width, framebuffer_height);
    } else {
        mouse_set_bounds(640, 160);
    }

    // 4. Unmask IRQ 0 (Timer), IRQ 1 (Keyboard), IRQ 2 (Cascade), IRQ 12 (Mouse)
    pic_unmask_irq(0);
    pic_unmask_irq(1);
    pic_unmask_irq(2);
    pic_unmask_irq(12);

    display_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    display_puts("[+] Subsystems online. Registered tasks:\n");
    display_set_color(COLOR_WHITE, COLOR_BLACK);

    for (pid_t pid = 1; pid <= 6; pid++) {
        task_t* task = task_get_by_pid(pid);
        if (task) {
            display_puts("    PID ");
            print_number(task->pid);
            display_puts(" | ");
            display_puts(task->name);
            display_puts(" | State: ");
            display_puts(task_state_to_string(task->state));
            display_putchar('\n');
        }
    }

    shell_init();
    serial_write_string("[boot] shell initialized; enabling interrupts\n");
    audio_play_test_tone();

    // 5. Enable interrupts only after complete hardware setup
    asm volatile ("sti");

    uint64_t last_rendered_tick = UINT64_MAX;
    bool previous_mouse_left = false;

    // 6. Main execution loop
    while (1) {
        audio_poll();
        uint64_t current_ticks = timer_get_ticks();
        if (current_ticks != last_rendered_tick) {
            last_rendered_tick = current_ticks;
            if (!shell_is_fullscreen()) {
                render_status(current_ticks, task_get_current());
                render_scheduler_test();
                render_exit_test();
                render_sleep_test();
            }
            render_mouse_cursor();
        }
        input_mouse_state_t mouse_state;
        mouse_get_state(&mouse_state);
        if (shell_is_fullscreen() &&
            (mouse_state.left_btn != previous_mouse_left)) {
            shell_handle_mouse_event((uint32_t)mouse_state.x,
                (uint32_t)mouse_state.y,
                mouse_state.left_btn && !previous_mouse_left);
        }
        previous_mouse_left = mouse_state.left_btn;

        input_key_event_t key_evt;
        if (keyboard_get_event(&key_evt)) {
            shell_handle_key_event(&key_evt);
        }
        display_present();
        task_yield();
        asm volatile ("hlt");
    }
}
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "idt.h"
#include "io.h"
#include "pic.h"
#include "ps2.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "display.h"
#include "serial.h"
#include "repair.h"
#include "ring3.h"

#define KERNEL_CS     0x08   /* must be the code segment of the GDT your boot code loads */
#define IDT_INT_GATE  0x8E   /* present, DPL0, interrupt gate (clears IF on entry) */
#define IRQ_BASE      32
#define IRQ_COUNT     16
#define VECTOR_COUNT  (IRQ_BASE + IRQ_COUNT)
#define IDT_USER_INT_GATE 0xEE

#ifdef ROCKOS_ARCH_32
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attributes;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* Must match the pusha layout in src/arch/i386/isr32.S:
 * pusha pushes edi esi ebp esp ebx edx ecx eax (in that memory order),
 * then the stub pushed vector + error_code, then the CPU frame. */
typedef struct {
    uint32_t edi, esi, ebp, esp_dummy;
    union { uint32_t ebx; uint32_t rbx; };
    union { uint32_t edx; uint32_t rdx; };
    union { uint32_t ecx; uint32_t rcx; };
    union { uint32_t eax; uint32_t rax; };
    uint32_t vector, error_code;
    union { uint32_t eip; uint32_t rip; };
    uint32_t cs;
    union { uint32_t eflags; uint32_t rflags; };
    /* esp/ss are only pushed by the CPU on a ring 3 -> ring 0 transition. */
    union { uint32_t esp; uint32_t rsp; };
    uint32_t ss;
} interrupt_frame_t;

static struct idt_entry idt[256];
static struct idt_ptr idtp;

/* Defined in isr32.S (32-bit addresses). */
extern const uint32_t isr_stub_table[VECTOR_COUNT];
extern void ring3_syscall_stub(void);
#else
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attributes;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Must match the push order in src/arch/x86_64/isr.S */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} interrupt_frame_t;

static struct idt_entry idt[256];
static struct idt_ptr idtp;

/* Defined in isr.S */
extern const uint64_t isr_stub_table[VECTOR_COUNT];
extern void ring3_syscall_stub(void);
#endif

/* Per-arch frame accessors (syscall ABI differs: ebx on i386, rdi on x86-64). */
#ifdef ROCKOS_ARCH_32
#define FRAME_SYSCALL_ARG(f) ((uint64_t)(f)->rbx)
/* For ring 0 interrupts the CPU did not push esp: it is just past eflags. */
#define FRAME_STACK_POINTER(f) \
    ((((f)->cs & 3) == 3) ? (uint64_t)(f)->rsp : (uint64_t)(uintptr_t)&(f)->rsp)
#else
#define FRAME_SYSCALL_ARG(f) ((f)->rdi)
#define FRAME_STACK_POINTER(f) ((f)->rsp)
#endif

void interrupt_dispatch(interrupt_frame_t* frame);

static const char* const exception_names[] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 Floating-Point", "Alignment Check",
    "Machine Check", "SIMD Floating-Point", "Virtualization",
    "Control Protection"
};
#define EXCEPTION_NAME_COUNT (sizeof(exception_names) / sizeof(exception_names[0]))

void idt_set_gate(uint8_t num, uintptr_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low = base & 0xFFFF;
    idt[num].selector = sel;
#ifdef ROCKOS_ARCH_32
    idt[num].zero = 0;
    idt[num].type_attributes = flags;
    idt[num].offset_high = (base >> 16) & 0xFFFF;
#else
    idt[num].ist = 0;
    idt[num].type_attributes = flags;
    idt[num].offset_mid = (base >> 16) & 0xFFFF;
    idt[num].offset_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].zero = 0;
#endif
}

/* ---------- fatal exception screen ---------- */

static void put_hex64(uint64_t v) {
    static const char digits[] = "0123456789ABCDEF";
    display_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        display_putchar(digits[(v >> shift) & 0xF]);
    }
}

static void put_line(const char* label, uint64_t v) {
    display_puts(label);
    put_hex64(v);
    display_putchar('\n');
}

/* ---------- RockOS error screen ("!!!" design) ---------- */

/* The layout was designed on a 1144x641 canvas; positions/sizes are scaled by
 * screen height. Text uses the Selawik UI font at its native 20px (bold is
 * faked with a 1px double-strike), so it is not scaled. */
#define ERR_DESIGN_HEIGHT 641U
#define ERR_GRAY   85U
#define ERR_WHITE  255U

static uint32_t err_scale(uint32_t value, uint32_t height) {
    return (value * height) / ERR_DESIGN_HEIGHT;
}

static uint32_t err_isqrt(uint32_t n) {
    uint32_t r = 0;
    while ((r + 1U) * (r + 1U) <= n) {
        r++;
    }
    return r;
}

/* Tapered "!" bar: top_w wide at the top edge, bottom_w at the bottom. */
static void err_draw_bar(uint32_t cx, uint32_t y, uint32_t h,
    uint32_t top_w, uint32_t bottom_w, uint8_t c) {
    if (h == 0 || bottom_w > top_w) {
        return;
    }
    for (uint32_t row = 0; row < h; row++) {
        uint32_t w = top_w - ((top_w - bottom_w) * row) / h;
        if (w == 0 || w / 2 > cx) {
            continue;
        }
        display_fill_rect_rgb(cx - w / 2, y + row, w, 1, c, c, c);
    }
}

/* Filled circle with a 1px half-alpha fringe so the edge isn't jagged. */
static void err_draw_dot(uint32_t cx, uint32_t cy, uint32_t r, uint8_t c) {
    if (r < 2 || r > cx || r > cy) {
        return;
    }
    for (int32_t dy = -(int32_t)r; dy <= (int32_t)r; dy++) {
        uint32_t dy2 = (uint32_t)(dy * dy);
        uint32_t row_y = (uint32_t)((int32_t)cy + dy);
        uint32_t outer = err_isqrt(r * r - dy2);
        uint32_t inner_limit = (r - 1U) * (r - 1U);
        if (dy2 <= inner_limit) {
            uint32_t inner = err_isqrt(inner_limit - dy2);
            display_fill_rect_rgb(cx - inner, row_y, inner * 2U + 1U, 1,
                c, c, c);
            if (outer > inner) {
                uint32_t fringe = outer - inner;
                display_blend_rect_rgb(cx - outer, row_y, fringe, 1,
                    c, c, c, 128);
                display_blend_rect_rgb(cx + inner + 1U, row_y, fringe, 1,
                    c, c, c, 128);
            }
        } else {
            display_blend_rect_rgb(cx - outer, row_y, outer * 2U + 1U, 1,
                c, c, c, 150);
        }
    }
}

static void err_draw_exclamation(uint32_t cx, uint32_t height, uint8_t c) {
    err_draw_bar(cx, err_scale(122, height), err_scale(96, height),
        err_scale(34, height), err_scale(17, height), c);
    err_draw_dot(cx, err_scale(245, height), err_scale(17, height), c);
}

static void err_draw_centered_bold(uint32_t width, uint32_t y,
    const char* text, uint8_t color) {
    uint32_t w = display_measure_text_ui(text);
    uint32_t x = w < width ? (width - w) / 2U : 0;
    display_draw_text_ui(x, y, text, color);
    display_draw_text_ui(x + 1U, y, text, color);
}

/* Windows-style stop code for the footer. Unknown vectors fall back to the
 * generic one from the design. */
static const char* err_stop_code(uint64_t vector) {
    switch ((uint32_t)vector) {
        case 0:  return "DIVIDE_ERROR_FAULT";
        case 1:  return "DEBUG_EXCEPTION";
        case 2:  return "NMI_HARDWARE_FAULT";
        case 3:  return "BREAKPOINT_EXCEPTION";
        case 4:  return "OVERFLOW_EXCEPTION";
        case 5:  return "BOUND_RANGE_EXCEEDED";
        case 6:  return "INVALID_OPCODE_FAULT";
        case 7:  return "DEVICE_NOT_AVAILABLE";
        case 8:  return "DOUBLE_FAULT";
        case 10: return "INVALID_TSS";
        case 11: return "SEGMENT_NOT_PRESENT";
        case 12: return "STACK_SEGMENT_FAULT";
        case 13: return "GENERAL_PROTECTION_FAULT";
        case 14: return "PAGE_FAULT_IN_KERNEL";
        case 16: return "FLOATING_POINT_EXCEPTION";
        case 17: return "ALIGNMENT_CHECK_FAULT";
        case 18: return "MACHINE_CHECK_EXCEPTION";
        case 19: return "SIMD_EXCEPTION";
        case 20: return "VIRTUALIZATION_EXCEPTION";
        case 21: return "CONTROL_PROTECTION_FAULT";
        default: return "GENERAL_EXCEPTION_FAULT";
    }
}

static bool draw_graphical_panic(const interrupt_frame_t* f) {
    uint32_t width;
    uint32_t height;
    if (!display_get_framebuffer_size(&width, &height) ||
        width < 640 || height < 400) {
        return false;
    }

    display_fill_rect_rgb(0, 0, width, height, 0, 0, 0);

    /* !!!  - outer two dim, middle one bright */
    uint32_t center = width / 2U;
    uint32_t pitch = err_scale(52, height);
    err_draw_exclamation(center - pitch, height, ERR_GRAY);
    err_draw_exclamation(center + pitch, height, ERR_GRAY);
    err_draw_exclamation(center, height, ERR_WHITE);

    uint32_t text_y = err_scale(298, height);
    err_draw_centered_bold(width, text_y,
        "A problem was detected and we stopped your computer to prevent "
        "any further damage.", COLOR_WHITE);
    err_draw_centered_bold(width, text_y + 29U,
        "Please wait while we repair and diagnose your computer.",
        COLOR_WHITE);

    /* dim logo + stop code pinned to the bottom */
    display_draw_rockos_logo_half((width - DISPLAY_ROCKOS_LOGO_WIDTH / 2U) / 2U,
        height - 140U, COLOR_DARK_GRAY);

    char footer[64];
    size_t length = 0;
    const char* prefix = "Error code: ";
    while (*prefix) {
        footer[length++] = *prefix++;
    }
    const char* code = err_stop_code(f->vector);
    while (*code && length < sizeof(footer) - 1U) {
        footer[length++] = *code++;
    }
    footer[length] = '\0';
    uint32_t footer_width = display_measure_text_ui(footer);
    display_draw_text_ui(footer_width < width ? (width - footer_width) / 2U : 0,
        height - 70U, footer, COLOR_WHITE);

    display_present();
    return true;
}

/* The screen is deliberately clean, so the register dump goes to COM1
 * (QEMU: -serial stdio / -serial file:serial.log). */
static void panic_serial_dump(const interrupt_frame_t* f) {
    serial_write_string("\n[panic] ");
    serial_write_string(f->vector < EXCEPTION_NAME_COUNT
        ? exception_names[f->vector] : "Reserved");
    serial_write_string(" (");
    serial_write_string(err_stop_code(f->vector));
    serial_write_string(")\n[panic] vector=");
    serial_write_hex(f->vector);
    serial_write_string(" error=");
    serial_write_hex(f->error_code);
    serial_write_string("\n[panic] rip=");
    serial_write_hex(f->rip);
    serial_write_string(" cs=");
    serial_write_hex(f->cs);
    serial_write_string(" flags=");
    serial_write_hex(f->rflags);
    serial_write_string(" sp=");
    serial_write_hex(FRAME_STACK_POINTER(f));
    serial_write_string("\n[panic] ax=");
    serial_write_hex(f->rax);
    serial_write_string(" bx=");
    serial_write_hex(f->rbx);
    serial_write_string(" cx=");
    serial_write_hex(f->rcx);
    serial_write_string(" dx=");
    serial_write_hex(f->rdx);
    if (f->vector == 14) {
        uintptr_t cr2;
        asm volatile ("mov %%cr2, %0" : "=r"(cr2));
        serial_write_string(" cr2=");
        serial_write_hex(cr2);
    }
    serial_write_string("\n");
}

static void exception_panic(const interrupt_frame_t* f) {
    asm volatile ("cli");
    panic_serial_dump(f);

    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    display_clear();
    bool graphical = draw_graphical_panic(f);
    if (!graphical) {
        display_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        display_puts("!!! ROCKOS / SYSTEM HALTED !!!\n\n");
        display_puts("A fatal CPU exception stopped execution.\n");

        display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
        display_puts("Type:       ");
        if (f->vector < EXCEPTION_NAME_COUNT) {
            display_puts(exception_names[f->vector]);
        } else {
            display_puts("Reserved");
        }
        display_putchar('\n');

        put_line("Vector:     ", f->vector);
        put_line("Error code: ", f->error_code);
        put_line("RIP:        ", f->rip);
        put_line("CS:         ", f->cs);
        put_line("RFLAGS:     ", f->rflags);
        put_line("RSP:        ", FRAME_STACK_POINTER(f));

        if (f->vector == 14) {
            uintptr_t cr2;
            asm volatile ("mov %%cr2, %0" : "=r"(cr2));
            put_line("CR2:        ", cr2);
        }

        put_line("RAX:        ", f->rax);
        put_line("RBX:        ", f->rbx);
        put_line("RCX:        ", f->rcx);
        put_line("RDX:        ", f->rdx);

        display_puts("\nSystem halted to protect kernel state.\n");
        display_puts("Restart the virtual machine to continue.\n");
    }
    display_present();

    if (graphical) {
        /* Windows-style: "!!!" screen first, then the repair screen. It
         * polls the keyboard/mouse itself and never returns. */
        repair_crash_info_t info;
        info.vector = f->vector;
        info.error_code = f->error_code;
        info.rip = f->rip;
        info.cs = f->cs;
        info.rflags = f->rflags;
        info.rsp = FRAME_STACK_POINTER(f);
        info.rax = f->rax;
        info.rbx = f->rbx;
        info.rcx = f->rcx;
        info.rdx = f->rdx;
        info.cr2 = 0;
        info.has_cr2 = (f->vector == 14);
        if (info.has_cr2) {
            uintptr_t cr2;
            asm volatile ("mov %%cr2, %0" : "=r"(cr2));
            info.cr2 = cr2;
        }
        info.stop_code = err_stop_code(f->vector);
        info.name = f->vector < EXCEPTION_NAME_COUNT
            ? exception_names[f->vector] : "Reserved";
        repair_screen_run(&info);
    }

    for (;;) {
        asm volatile ("cli; hlt");
    }
}

/* ---------- PIC spurious IRQ handling ---------- */

/* Returns true if this IRQ7/IRQ15 was spurious (and already dealt with). */
static bool irq_is_spurious(uint8_t irq) {
    if (irq == 7) {
        outb(PIC1_COMMAND, 0x0B);                    /* read In-Service Register */
        return (inb(PIC1_COMMAND) & 0x80) == 0;      /* spurious: NO EOI at all */
    }
    if (irq == 15) {
        outb(PIC2_COMMAND, 0x0B);
        if ((inb(PIC2_COMMAND) & 0x80) == 0) {
            outb(PIC1_COMMAND, PIC_EOI);             /* master saw the cascade line: EOI master only */
            return true;
        }
    }
    return false;
}

/*
 * Keyboard and mouse share the single 8042 output buffer (port 0x60).
 * Whichever IRQ fired, drain the controller and let each driver take only
 * the bytes that belong to it (the drivers check the AUX bit themselves).
 * Every pass consumes at least one byte, so the loop always terminates.
 */
static void ps2_service(void) {
    for (int i = 0; i < 16; i++) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) == 0) {
            break;
        }
        keyboard_irq_handler();
        mouse_irq_handler();
    }
}

/* ---------- called from isr_common ---------- */

void interrupt_dispatch(interrupt_frame_t* frame) {
    if ((frame->cs & 3) == 3 && frame->vector < IRQ_BASE &&
        frame->vector != 2 && frame->vector != 8 && frame->vector != 18) {
        uintptr_t fault_address = 0;
        if (frame->vector == 14) {
            asm volatile ("mov %%cr2, %0" : "=r"(fault_address));
        }
        ring3_fault_dispatch(frame->vector, frame->error_code, frame->rip,
            fault_address);
    }

    if (frame->vector == RING3_SYSCALL_VECTOR) {
        if ((frame->cs & 3) == 3) {
            frame->rax = ring3_syscall_dispatch(frame->rax,
                FRAME_SYSCALL_ARG(frame));
            return;
        }
        exception_panic(frame);
        return;
    }

    if (frame->vector < IRQ_BASE) {
        exception_panic(frame);
        return;
    }

    if (frame->vector >= VECTOR_COUNT) {
        return;   /* no gate installed for these, can't happen */
    }

    uint8_t irq = (uint8_t)(frame->vector - IRQ_BASE);

    if (irq_is_spurious(irq)) {
        return;
    }

    switch (irq) {
        case 0:
            timer_irq_handler();
            break;
        case 1:
        case 12:
            ps2_service();
            break;
        default:
            break;
    }

    pic_send_eoi(irq);
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uintptr_t)&idt;

    for (int i = 0; i < 256; i++) {
        idt_set_gate((uint8_t)i, 0, 0, 0);
    }

    for (int i = 0; i < VECTOR_COUNT; i++) {
        idt_set_gate((uint8_t)i, isr_stub_table[i], KERNEL_CS, IDT_INT_GATE);
    }
    idt_set_gate(RING3_SYSCALL_VECTOR, (uintptr_t)ring3_syscall_stub,
        KERNEL_CS, IDT_USER_INT_GATE);

    asm volatile ("lidt %0" : : "m"(idtp));
}
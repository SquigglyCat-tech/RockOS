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

static void draw_hex_field(uint32_t x, uint32_t y, const char* label,
    uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char text[48];
    size_t length = 0;
    while (*label && length < sizeof(text) - 19) {
        text[length++] = *label++;
    }
    text[length++] = '0';
    text[length++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) {
        text[length++] = digits[(value >> shift) & 0x0F];
    }
    text[length] = '\0';
    display_draw_text_ui(x, y, text, COLOR_LIGHT_GRAY);
}

static bool draw_graphical_panic(const interrupt_frame_t* f) {
    uint32_t width;
    uint32_t height;
    if (!display_get_framebuffer_size(&width, &height) ||
        width < 600 || height < 520) {
        return false;
    }

    display_fill_rect_rgb(0, 0, width, height, 0, 0, 0);
    display_fill_rect_rgb(0, 0, width, 6, 220, 48, 58);
    display_fill_rect_rgb(32, 32, width - 64, height - 64,
        17, 18, 22);
    display_fill_rect_rgb(32, 32, 6, height - 64, 220, 48, 58);
    display_draw_text_ui(56, 52, "ROCKOS  /  KERNEL PANIC",
        COLOR_LIGHT_RED);
    display_draw_text_ui(56, 88, "SYSTEM HALTED", COLOR_WHITE);
    display_draw_text_ui(56, 122,
        "A fatal CPU exception stopped execution.", COLOR_LIGHT_GRAY);

    display_fill_rect_rgb(56, 164, width - 112, 64, 35, 20, 23);
    display_draw_text_ui(72, 186, "EXCEPTION", COLOR_LIGHT_RED);
    display_draw_text_ui(190, 186,
        f->vector < EXCEPTION_NAME_COUNT
            ? exception_names[f->vector] : "Reserved",
        COLOR_WHITE);

    uint32_t right_x = width / 2;
    uint32_t y = 260;
    draw_hex_field(56, y, "VECTOR  ", f->vector);
    draw_hex_field(right_x, y, "ERROR CODE  ", f->error_code);
    y += 32;
    draw_hex_field(56, y, "RIP  ", f->rip);
    draw_hex_field(right_x, y, "CS  ", f->cs);
    y += 32;
    draw_hex_field(56, y, "RFLAGS  ", f->rflags);
    draw_hex_field(right_x, y, "RSP  ", FRAME_STACK_POINTER(f));
    y += 32;
    draw_hex_field(56, y, "RAX  ", f->rax);
    draw_hex_field(right_x, y, "RBX  ", f->rbx);
    y += 32;
    draw_hex_field(56, y, "RCX  ", f->rcx);
    draw_hex_field(right_x, y, "RDX  ", f->rdx);
    if (f->vector == 14 && y + 32 < height - 80) {
        uintptr_t cr2;
        asm volatile ("mov %%cr2, %0" : "=r"(cr2));
        y += 32;
        draw_hex_field(56, y, "CR2  ", cr2);
    }

    display_fill_rect_rgb(56, height - 92, width - 112, 1,
        70, 72, 78);
    display_draw_text_ui(56, height - 72,
        "Restart the virtual machine to continue.", COLOR_LIGHT_GRAY);
    display_present();
    return true;
}

static void exception_panic(const interrupt_frame_t* f) {
    asm volatile ("cli");

    display_set_color(COLOR_LIGHT_GRAY, COLOR_BLACK);
    display_clear();
    if (!draw_graphical_panic(f)) {
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
#include <stdint.h>
#include <stdbool.h>

#include "idt.h"
#include "io.h"
#include "pic.h"
#include "ps2.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "display.h"   /* if your display header has a different name, change ONLY this line */
#include "audio.h"
#include "ring3.h"

#define KERNEL_CS     0x08   /* must be the 64-bit code segment of the GDT your boot code loads */
#define IDT_INT_GATE  0x8E   /* present, DPL0, 64-bit interrupt gate (clears IF on entry) */
#define IRQ_BASE      32
#define IRQ_COUNT     16
#define VECTOR_COUNT  (IRQ_BASE + IRQ_COUNT)
#define IDT_USER_INT_GATE 0xEE

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

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low = base & 0xFFFF;
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].type_attributes = flags;
    idt[num].offset_mid = (base >> 16) & 0xFFFF;
    idt[num].offset_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].zero = 0;
}

/* ---------- minimal exception screen (no printf in the kernel yet) ---------- */

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

static void exception_panic(const interrupt_frame_t* f) {
    asm volatile ("cli");

    display_set_color(COLOR_YELLOW, COLOR_BLACK);
    display_puts("\n*** ROCKOS KERNEL EXCEPTION ***\n");

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
    put_line("RSP:        ", f->rsp);

    if (f->vector == 14) {
        uint64_t cr2;
        asm volatile ("mov %%cr2, %0" : "=r"(cr2));
        put_line("CR2:        ", cr2);
    }

    put_line("RAX:        ", f->rax);
    put_line("RBX:        ", f->rbx);
    put_line("RCX:        ", f->rcx);
    put_line("RDX:        ", f->rdx);

    display_puts("\nSystem halted.\n");
    audio_play_error_sound();

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
        uint64_t fault_address = 0;
        if (frame->vector == 14) {
            asm volatile ("mov %%cr2, %0" : "=r"(fault_address));
        }
        ring3_fault_dispatch(frame->vector, frame->error_code, frame->rip,
            fault_address);
    }

    if (frame->vector == RING3_SYSCALL_VECTOR) {
        if ((frame->cs & 3) == 3) {
            frame->rax = ring3_syscall_dispatch(frame->rax, frame->rdi);
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
    idtp.base = (uint64_t)&idt;

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
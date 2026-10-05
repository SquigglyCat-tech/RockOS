/* src/arch/i386/gdt32_load.S
 *
 * 32-bit protected-mode GDT reload (V86 / older PCs).
 * Mirrors src/arch/x86_64/gdt_load.S but with 32-bit segments:
 *   0x08 = kernel code, 0x10 = kernel data, 0x28 = TSS.
 *
 * C prototype: void gdt_load(const gdt_pointer_t* pointer);
 * On i386 the pointer arrives on the stack at 4(%esp).
 */
.code32

.section .text
.global gdt_load
.type gdt_load, @function
gdt_load:
    movl 4(%esp), %eax
    lgdt (%eax)

    ljmp $0x08, $.Lgdt32_reloaded
.Lgdt32_reloaded:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movw $0x28, %ax
    ltr %ax
    ret

.size gdt_load, .-gdt_load
.section .note.GNU-stack,"",@progbits

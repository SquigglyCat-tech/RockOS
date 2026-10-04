.code32
.global _start
.extern kernel_main

.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set VIDEO,    1<<2
.set FLAGS,    ALIGN | MEMINFO | VIDEO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM
/* Keep the optional address-field slots so Multiboot graphics fields land at
 * their specified fixed offsets even when flag 16 is clear. */
.long 0
.long 0
.long 0
.long 0
.long 0
/* Linear framebuffer mode, 1024x768x32. */
.long 0
.long 1024
.long 768
.long 32

.section .bss
.align 4096
p4_table:
    .skip 4096
p3_table:
    .skip 4096
p2_table:
    .skip 4096
.global boot_stack_bottom
boot_stack_bottom:
    .skip 16384
.global boot_stack_top
boot_stack_top:

.section .rodata
gdt64:
    .quad 0
gdt64_code:
    .quad (1<<43) | (1<<44) | (1<<47) | (1<<53)
gdt64_data:
    .quad (1<<41) | (1<<44) | (1<<47)
gdt64_pointer:
    .word . - gdt64 - 1
    .quad gdt64

.section .text
_start:
    movl %eax, %esi
    mov $boot_stack_top, %esp

    mov $p3_table, %eax
    or $0x3, %eax
    mov %eax, p4_table

    mov $p2_table, %eax
    or $0x3, %eax
    mov %eax, p3_table

    mov $0, %ecx
.map_p2_table:
    mov $0x200000, %eax
    mul %ecx
    or $0x83, %eax
    mov %eax, p2_table(,%ecx,8)
    inc %ecx
    cmp $512, %ecx
    jne .map_p2_table

    mov $p4_table, %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or $(1 << 5), %eax
    mov %eax, %cr4

    mov $0xC0000080, %ecx
    rdmsr
    or $(1 << 8), %eax
    wrmsr

    mov %cr0, %eax
    or $(1 << 31), %eax
    mov %eax, %cr0

    lgdt (gdt64_pointer)
    ljmp $0x08, $long_mode_start

.code64
long_mode_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    movl %esi, %edi
    movl %ebx, %esi
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
.code64

.section .text
.global gdt_load
.type gdt_load, @function
gdt_load:
    lgdt (%rdi)

    pushq $0x08
    leaq .Lgdt_reloaded(%rip), %rax
    pushq %rax
    lretq

.Lgdt_reloaded:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    movw $0x28, %ax
    ltr %ax
    ret

.size gdt_load, .-gdt_load
.section .note.GNU-stack,"",@progbits
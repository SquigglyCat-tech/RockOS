.code64

.section .text
.global ring3_enter
.type ring3_enter, @function
ring3_enter:
    pushq $0x23
    pushq %rsi
    pushq $0x02
    pushq $0x1B
    pushq %rdi
    iretq
.size ring3_enter, .-ring3_enter

.section .rodata
.global ring3_user_test_program_start
.global ring3_user_test_program_end
ring3_user_test_program_start:
    /* putchar('U'), putchar('\n'), exit(0) */
    .byte 0xB8, 0x01, 0x00, 0x00, 0x00
    .byte 0xBF, 0x55, 0x00, 0x00, 0x00
    .byte 0xCD, 0x80
    .byte 0xBF, 0x0A, 0x00, 0x00, 0x00
    .byte 0xCD, 0x80
    .byte 0x31, 0xC0
    .byte 0xCD, 0x80
    .byte 0x0F, 0x0B
ring3_user_test_program_end:

.global ring3_memory_program_start
.global ring3_memory_program_end
/*
 * mmap(1) -> store 0x5A -> read back -> putchar('M'|'X') -> exit(0)
 */
ring3_memory_program_start:
    .byte 0xB8, 0x05, 0x00, 0x00, 0x00   /* eax=5 (mmap)  */
    .byte 0xBF, 0x01, 0x00, 0x00, 0x00   /* edi=1 (page)  */
    .byte 0xCD, 0x80                     /* int 0x80 -> rax */
    .byte 0x3D, 0xFF, 0xFF, 0xFF, 0x0F   /* cmp eax, 0x0FFFFFFF */
    .byte 0x77, 0x15                     /* ja +21 -> fail */
    .byte 0xC6, 0x00, 0x5A               /* mov byte [rax], 0x5A */
    .byte 0x0F, 0xB6, 0x08               /* movzx ecx, byte [rax] */
    .byte 0x80, 0xF9, 0x5A               /* cmp cl, 0x5A */
    .byte 0xB8, 0x01, 0x00, 0x00, 0x00   /* eax=1 (putchar) */
    .byte 0xBF, 0x4D, 0x00, 0x00, 0x00   /* edi='M' */
    .byte 0x74, 0x05                     /* je +5 -> print */
    .byte 0xBF, 0x58, 0x00, 0x00, 0x00   /* edi='X' */
ring3_memory_print:
    .byte 0xCD, 0x80                     /* int 0x80 */
    .byte 0xB8, 0x00, 0x00, 0x00, 0x00   /* eax=0 (exit) */
    .byte 0xCD, 0x80                     /* int 0x80 */
    .byte 0x0F, 0x0B                     /* ud2 guard */
ring3_memory_program_end:

.global ring3_fault_program_start
.global ring3_fault_program_end
ring3_fault_program_start:
    .byte 0x31, 0xC0
    .byte 0x48, 0xA1
    .quad 0x00100000
    .byte 0xCD, 0x80
    .byte 0x0F, 0x0B
ring3_fault_program_end:

.section .note.GNU-stack,"",@progbits

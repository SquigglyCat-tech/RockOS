.code64

.section .text
.global ring3_enter
.type ring3_enter, @function
ring3_enter:
    pushq $0x1B
    pushq %rsi
    pushq $0x02
    pushq $0x23
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

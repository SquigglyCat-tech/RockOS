/* src/arch/i386/ring3_32.S
 *
 * 32-bit ring-3 entry + test programs. Mirrors src/arch/x86_64/ring3.S.
 *
 * C prototype: void ring3_enter(uintptr_t user_eip, uintptr_t user_esp);
 * On i386: eip at 4(%esp), esp at 8(%esp). Builds an iret frame:
 *   ss=0x23, esp, eflags(IF=1), cs=0x1B, eip. Matches the 32-bit GDT
 *   layout installed by gdt.c (user code 0x1B, user data 0x23).
 */
.code32

.section .text
.global ring3_enter
.type ring3_enter, @function
ring3_enter:
    movl 4(%esp), %eax       /* user eip */
    movl 8(%esp), %ecx       /* user esp */
    pushl $0x23
    pushl %ecx
    pushl $0x202             /* IF=1, bit1 always set */
    pushl $0x1B
    pushl %eax
    iret
.size ring3_enter, .-ring3_enter

.section .rodata
.global ring3_user_test_program_start
.global ring3_user_test_program_end
ring3_user_test_program_start:
    /* putchar('U') via int 0x80: eax=PUTCHAR(1), ebx='U' */
    .byte 0xB8, 0x01, 0x00, 0x00, 0x00   /* mov $1, %eax */
    .byte 0xBB, 0x55, 0x00, 0x00, 0x00   /* mov $0x55, %ebx */
    .byte 0xCD, 0x80                     /* int $0x80 */
    .byte 0xBB, 0x0A, 0x00, 0x00, 0x00   /* mov $0x0A, %ebx */
    .byte 0xCD, 0x80                     /* int $0x80 */
    .byte 0x31, 0xC0                     /* xor %eax, %eax (EXIT=0) */
    .byte 0xCD, 0x80                     /* int $0x80 */
    .byte 0x0F, 0x0B                     /* ud2 (unreachable) */
ring3_user_test_program_end:

.global ring3_fault_program_start
.global ring3_fault_program_end
ring3_fault_program_start:
    .byte 0x31, 0xC0                     /* xor %eax, %eax */
    .byte 0xA1                           /* mov 0x100000, %eax */
    .long 0x00100000
    .byte 0xCD, 0x80                     /* int $0x80 (unreachable) */
    .byte 0x0F, 0x0B                     /* ud2 */
ring3_fault_program_end:

.section .note.GNU-stack,"",@progbits

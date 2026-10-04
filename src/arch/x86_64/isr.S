/*
 * src/arch/x86_64/isr.S
 *
 * Stubs for vectors 0-47 (CPU exceptions + remapped PIC IRQs), plus the
 * dedicated Ring 3 syscall vector 128. Every stub reaches isr_common, which
 * builds one uniform frame and calls interrupt_dispatch(frame) in idt.c.
 *
 * Frame layout (lowest address first) must match interrupt_frame_t in idt.c:
 *   r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rbx rax
 *   vector error_code
 *   rip cs rflags rsp ss          <- pushed by the CPU
 *
 * An interrupt can arrive with arbitrary RSP alignment. isr_common aligns a
 * temporary stack for its C call and restores the original frame pointer.
 */

.code64

.extern interrupt_dispatch
.extern task_interrupt_exit

.section .text
.global ring3_syscall_stub
ring3_syscall_stub:
    pushq $0
    pushq $128
    jmp isr_common

/* Vectors where the CPU does NOT push an error code: push a dummy 0. */
.macro ISR_NOERR num
    .section .text
isr_stub_\num:
    pushq $0
    pushq $\num
    jmp isr_common
    .section .rodata
    .quad isr_stub_\num
.endm

/* Vectors where the CPU pushes an error code itself. */
.macro ISR_ERR num
    .section .text
isr_stub_\num:
    pushq $\num
    jmp isr_common
    .section .rodata
    .quad isr_stub_\num
.endm

/* Table of stub addresses, read by idt_init(). Nothing else may be emitted
 * into .rodata between here and the last ISR_* line. */
.section .rodata
.balign 8
.global isr_stub_table
isr_stub_table:

/* CPU exceptions */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* PIC IRQs 0-15 -> vectors 32-47 */
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

.section .text
isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld                         /* SysV ABI: DF must be clear when calling C */
    movq %rsp, %rdi             /* arg0 = pointer to frame */
    movq %rsp, %rax             /* preserve frame pointer across the C call */
    andq $-16, %rsp              /* SysV ABI: align RSP before CALL */
    subq $16, %rsp
    movq %rax, 8(%rsp)
    call interrupt_dispatch
    movq 8(%rsp), %rsp          /* restore the interrupt frame stack */

    cmpq $128, 120(%rsp)        /* syscalls can return to user mode */
    je .Lisr_skip_task_exit

    /* Schedule only after dispatch has completed and the PIC EOI was sent. */
    movq %rsp, %rax
    andq $-16, %rsp
    subq $16, %rsp
    movq %rax, 8(%rsp)
    call task_interrupt_exit
    movq 8(%rsp), %rsp          /* restore this task's interrupt frame */

.Lisr_skip_task_exit:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp              /* drop vector + error code */
    iretq

.section .note.GNU-stack,"",@progbits
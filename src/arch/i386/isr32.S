/*
 * src/arch/i386/isr32.S
 *
 * 32-bit protected-mode stubs for vectors 0-47 (CPU exceptions + remapped
 * PIC IRQs), plus the dedicated Ring 3 syscall vector 128. Mirrors
 * src/arch/x86_64/isr.S.
 *
 * Frame layout (lowest address first) must match interrupt_frame_t32 in
 * idt.c when ROCKOS_ARCH_32 is defined:
 *   edi esi ebp esp_dummy ebx edx ecx eax
 *   vector error_code
 *   eip cs eflags esp ss          <- pushed by the CPU
 *
 * (esp_dummy is pushed by pusha; the CPU-pushed esp is the one after
 * eflags. The C struct reads the CPU frame, not the pusha slot.)
 */

.code32

.extern interrupt_dispatch
.extern task_interrupt_exit

.section .text
.global ring3_syscall_stub
ring3_syscall_stub:
    pushl $0
    pushl $128
    jmp isr32_common

/* Vectors where the CPU does NOT push an error code: push a dummy 0. */
.macro ISR32_NOERR num
    .section .text
isr32_stub_\num:
    pushl $0
    pushl $\num
    jmp isr32_common
    .section .rodata
    .long isr32_stub_\num
.endm

/* Vectors where the CPU pushes an error code itself. */
.macro ISR32_ERR num
    .section .text
isr32_stub_\num:
    pushl $\num
    jmp isr32_common
    .section .rodata
    .long isr32_stub_\num
.endm

/* Table of stub addresses, read by idt_init(). Nothing else may be emitted
 * into .rodata between here and the last ISR32_* line. */
.section .rodata
.balign 4
.global isr_stub_table
isr_stub_table:

/* CPU exceptions */
ISR32_NOERR 0
ISR32_NOERR 1
ISR32_NOERR 2
ISR32_NOERR 3
ISR32_NOERR 4
ISR32_NOERR 5
ISR32_NOERR 6
ISR32_NOERR 7
ISR32_ERR   8
ISR32_NOERR 9
ISR32_ERR   10
ISR32_ERR   11
ISR32_ERR   12
ISR32_ERR   13
ISR32_ERR   14
ISR32_NOERR 15
ISR32_NOERR 16
ISR32_ERR   17
ISR32_NOERR 18
ISR32_NOERR 19
ISR32_NOERR 20
ISR32_ERR   21
ISR32_NOERR 22
ISR32_NOERR 23
ISR32_NOERR 24
ISR32_NOERR 25
ISR32_NOERR 26
ISR32_NOERR 27
ISR32_NOERR 28
ISR32_ERR   29
ISR32_ERR   30
ISR32_NOERR 31

/* PIC IRQs 0-15 -> vectors 32-47 */
ISR32_NOERR 32
ISR32_NOERR 33
ISR32_NOERR 34
ISR32_NOERR 35
ISR32_NOERR 36
ISR32_NOERR 37
ISR32_NOERR 38
ISR32_NOERR 39
ISR32_NOERR 40
ISR32_NOERR 41
ISR32_NOERR 42
ISR32_NOERR 43
ISR32_NOERR 44
ISR32_NOERR 45
ISR32_NOERR 46
ISR32_NOERR 47

.section .text
isr32_common:
    pusha
    cld                         /* SysV ABI: DF must be clear when calling C */

    /* Pass frame pointer as the single C argument. */
    movl %esp, %eax
    pushl %eax
    call interrupt_dispatch
    addl $4, %esp

    /* vector slot = 32 (pusha). Skip scheduling for the syscall vector,
     * which returns to user mode via its own path. */
    cmpl $128, 32(%esp)
    je .Lisr32_skip_task_exit

    /* task_interrupt_exit() may context-switch to another task. Every task
     * has its own kernel stack and context_switch restores that task's ESP
     * exactly, so when this call returns ESP is already this task's frame.
     * Do NOT stash the frame pointer in a global: another task's interrupt
     * would overwrite it while we are switched out. */
    call task_interrupt_exit

.Lisr32_skip_task_exit:
    popa
    addl $8, %esp               /* drop vector + error code */
    iret

.section .note.GNU-stack,"",@progbits

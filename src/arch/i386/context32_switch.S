/* src/arch/i386/context32_switch.S
 *
 * 32-bit context switch. Mirrors src/arch/x86_64/context_switch.S.
 *
 * C prototype:
 *   void context_switch(uintptr_t* old_sp, uintptr_t new_sp);
 * On i386: old_sp at 4(%esp), new_sp at 8(%esp).
 * Callee-saved: ebx, ebp, esi, edi.
 */
.code32

.section .text
.global context_switch
.type context_switch, @function
context_switch:
    pushl %ebx
    pushl %ebp
    pushl %esi
    pushl %edi

    movl 20(%esp), %eax      /* old_sp (4 regs pushed + ret addr = 20) */
    movl %esp, (%eax)
    movl 24(%esp), %eax      /* new_sp */
    movl %eax, %esp

    popl %edi
    popl %esi
    popl %ebp
    popl %ebx
    ret

.size context_switch, .-context_switch
.section .note.GNU-stack,"",@progbits

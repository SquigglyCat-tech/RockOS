/* src/arch/i386/boot32.S
 *
 * 32-bit protected-mode entry for RockOS (V86 / older PCs).
 * 64-bit path in src/arch/x86_64/boot.S is untouched.
 *
 * Contract with Limine/Multiboot1 (same as 64-bit):
 *   EAX = magic, EBX = info pointer on entry.
 *   kernel_main(magic, info) with both zero-extended to 32-bit.
 *
 * Paging: identity-map the first 1 GiB with 4 MiB large pages in a
 * single page directory (256 entries), then enable PG+PE. This matches
 * PMM_IDENTITY_MAP_LIMIT and needs no 64-bit tables.
 */
.code32

.global _start
.extern kernel_main

/* Boot diagnostics: write a stage letter to VGA text (top-left row) and to
 * COM1 so a black screen can be narrowed down (R = entered _start,
 * P = paging on, K = calling kernel_main). Set BOOT_DIAG to 0 to remove. */
.set BOOT_DIAG, 0
.macro DIAG_MARK col, ch
.if BOOT_DIAG
    movw $(0x0F00 | \ch), (0xB8000 + \col * 2)
    movw $0x3F8, %dx
    movb $\ch, %al
    outb %al, %dx
.endif
.endm

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
pd_table:
    .skip 4096
.global boot_stack_bottom
boot_stack_bottom:
    .skip 16384
.global boot_stack_top
boot_stack_top:

.section .text
_start:
    movl %eax, %esi        /* magic -> ESI (callee-saved across setup) */
    movl %ebx, %edi        /* info  -> EDI */
    mov $boot_stack_top, %esp
    DIAG_MARK 0, 0x52      /* 'R' */

    /* Build PD: 256 x 4MiB large pages covering 0..1GiB, P+RW. */
    mov $pd_table, %eax
    mov $0, %ecx
.map_pd:
    mov %ecx, %edx
    shl $22, %edx          /* phys base = ecx * 4MiB */
    or $0x83, %edx         /* P | RW | PS(4MiB) */
    mov %edx, (%eax,%ecx,4)
    inc %ecx
    cmp $256, %ecx
    jne .map_pd

    /* Zero the remaining 768 entries (3..4GiB unmapped for now). */
    mov $256, %ecx
.clear_pd:
    movl $0, (%eax,%ecx,4)
    inc %ecx
    cmp $1024, %ecx
    jne .clear_pd

    mov $pd_table, %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or $(1 << 4), %eax    /* PSE: allow 4MiB pages */
    mov %eax, %cr4

    mov %cr0, %eax
    or $0x80000001, %eax  /* PG + PE */
    mov %eax, %cr0
    DIAG_MARK 1, 0x50      /* 'P' */

    /* Stack is identity-mapped, flat segments from BIOS are fine.
     * GDT/IDT are installed later by kernel_main via gdt32/idt32. */
    DIAG_MARK 2, 0x4B      /* 'K' */
    subl $8, %esp         /* keep ESP 16-byte aligned at the call (i386 ABI) */
    pushl %edi            /* info  */
    pushl %esi            /* magic */
    call kernel_main
    addl $16, %esp

.hang:
    cli
    hlt
    jmp .hang

.section .note.GNU-stack,"",@progbits

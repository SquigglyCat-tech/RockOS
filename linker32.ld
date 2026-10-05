/* linker32.ld - 32-bit RockOS link (V86 / older PCs).
 * Mirrors linker.ld; only the output format differs. The 64-bit
 * linker.ld path is untouched. OUTPUT_FORMAT keeps i686-elf-ld
 * from emitting a 64-bit ELF.
 */
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)

SECTIONS
{
    . = 1M;
    __kernel_start = .;

    .text ALIGN(4K) :
    {
        *(.multiboot)
        *(.text*)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
    }

    .data ALIGN(4K) :
    {
        *(.data*)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss*)
        *(.lbss*)
    }

    __kernel_end = .;
}

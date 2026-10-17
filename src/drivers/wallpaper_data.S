/* Embedded desktop wallpaper: raw RGB888, rows top to bottom, no header.
 * assets/wallpaper.rgb is generated from assets/wallpaper.png by
 * scripts/make_wallpaper.ps1 (the Makefile does that for you).
 * Shared by the 64-bit and 32-bit kernels, like error_sound.S. */
.section .rodata
.balign 4
.global rockos_wallpaper_start
.global rockos_wallpaper_end
rockos_wallpaper_start:
.incbin "assets/wallpaper.rgb"
rockos_wallpaper_end:
.section .note.GNU-stack,"",@progbits

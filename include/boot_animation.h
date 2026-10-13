#ifndef ROCKOS_BOOT_ANIMATION_H
#define ROCKOS_BOOT_ANIMATION_H

/* Play the framebuffer boot animation. Call with interrupts enabled and
 * after display_init_framebuffer() has succeeded. */
void boot_animation_run(void);

#endif

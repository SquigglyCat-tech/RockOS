#ifndef ROCKOS_BOOT_ANIMATION_H
#define ROCKOS_BOOT_ANIMATION_H

#include <stdint.h>

/* Play the framebuffer boot animation ("Assemble"). Call with interrupts
 * enabled and after display_init_framebuffer() has succeeded. */
void boot_animation_run(void);

/* Optional: drive the progress bar from real boot progress (0..100) instead
 * of the built-in timer. Call before boot_animation_run() or from a hook
 * it can see. */
void boot_animation_set_progress(uint32_t percent);

#endif

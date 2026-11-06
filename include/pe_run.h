#ifndef ROCKOS_PE_RUN_H
#define ROCKOS_PE_RUN_H

/*
 * pe_run.h - launching PE32 (.exe) images (Wine layer 1b).
 *
 * This layer only accepts the 1b envelope: exactly one section,
 * machine matching the running kernel, no imports, and ImageBase ==
 * RING3_PE_BASE (0x60000000) so no relocations are needed yet.
 * Layer 1c will widen this to full relocation + Win32 imports.
 */

#include <stdbool.h>
#include <stdint.h>

#include "pe.h"

#ifdef RING3_PE_BASE
#undef RING3_PE_BASE
#endif
#define RING3_PE_BASE 0x60000000ULL

/* Structural check before staging: true when the loader's 1b envelope
 * accepts the image. */
bool pe_run_can_load(const pe_image_t* image);

/* Stage the section payload and spawn the ring-3 task ("the run").
 * Returns false with pe_run_error() text when staging failed. */
bool pe_run_stage_and_launch(const uint8_t* blob, size_t blob_size);

/* Short error text from the last stage attempt ("" when none). */
const char* pe_run_error(void);

#endif /* ROCKOS_PE_RUN_H */

#ifndef ROCKOS_PE_RUN_H
#define ROCKOS_PE_RUN_H

/*
 * pe_run.h - launching PE32 (.exe) images (Wine layer 1c).
 *
 * Layer 1c envelope: any ImageBase (relocations applied when they
 * differ), up to RING3_PE_MAX_SECTIONS sections, machine matching the
 * running kernel, no imports, SizeOfImage <= RING3_PE_SNAPSHOT_BYTES.
 * The kernel builds a relocation-fixed memory snapshot; the ring-3
 * runner maps every page of that snapshot to RING3_PE_BASE and jumps
 * to (base + entry RVA).
 *
 * The Win32 import surface (kernel32) still lands in the next stage.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pe.h"

#ifdef RING3_PE_BASE
#undef RING3_PE_BASE
#endif
#define RING3_PE_BASE 0x60000000ULL

/* staging caps shared by the launcher and its host tests */
#define RING3_PE_SNAPSHOT_BYTES (256U * 1024U)
#define RING3_PE_MAX_SECTIONS 16

/* Structural check before staging: true when the loader's 1c envelope
 * accepts the image. */
bool pe_run_can_load(const pe_image_t* image);

/* Build the relocation-fixed snapshot and spawn the ring-3 task (the
 * "run"). Returns false with pe_run_error() text on failure. */
bool pe_run_stage_and_launch(const uint8_t* blob, size_t blob_size);

/* Short error text from the last stage attempt ("" when none). */
const char* pe_run_error(void);

#endif /* ROCKOS_PE_RUN_H */

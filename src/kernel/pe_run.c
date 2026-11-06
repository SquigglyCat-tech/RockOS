/*
 * pe_run.c - launching PE32 (.exe) images (Wine layer 1c).
 *
 * Layer 1c scope: relocations + arbitrary single/multi-section images
 * up to the staging cap, no imports yet (kernel32 surface lands with
 * the next stage of 1c). When the import table is empty the image
 * loads; import-bearing exes are still refused until that surface
 * exists.
 */

#include "pe.h"
#include "pe_run.h"
#include "ring3.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>

/* Runtime snapshot buffer: the kernel's post-relocation memory image
 * handed to the ring-3 runner. */
static uint8_t pe_run_snapshot[RING3_PE_SNAPSHOT_BYTES];

static const char* pe_run_last_error;

const char* pe_run_error(void) {
    return pe_run_last_error ? pe_run_last_error : "";
}

/* Envelope check for layer 1c loads. */
bool pe_run_can_load(const pe_image_t* image) {
    if (!image || !image->valid) return false;
#ifdef ROCKOS_ARCH_64
    if (image->machine != PE_MACHINE_AMD64 || !image->is_plus) return false;
#else
    if (image->machine != PE_MACHINE_I386 || image->is_plus) return false;
#endif
    if (image->size_of_image == 0 ||
        image->size_of_image > RING3_PE_SNAPSHOT_BYTES) {
        return false;
    }
    if (image->section_count == 0 ||
        image->section_count > RING3_PE_MAX_SECTIONS) {
        return false;
    }
    if (image->rva_and_sizes_count <= PE_DIR_IMPORT_TABLE) {
        /* no import data directory: loader assumes none */
        return image->data_directories[PE_DIR_IMPORT_TABLE].size == 0;
    }
    if (image->data_directories[PE_DIR_IMPORT_TABLE].size != 0) {
        /* imports are refused until the kernel32 surface lands */
        return false;
    }
    return true;
}

bool pe_run_stage_and_launch(const uint8_t* blob, size_t blob_size) {
    pe_run_last_error = 0;
    pe_image_t image;
    if (!pe_parse(blob, blob_size, &image)) {
        pe_run_last_error = image.error ? image.error : "parse failed";
        return false;
    }
    if (!pe_run_can_load(&image)) {
        pe_run_last_error = "image outside layer-1c envelope "
            "(imports not supported yet; size > 256 KiB; too many sections)";
        return false;
    }
    if (!pe_image_snapshot(&image, blob, blob_size, pe_run_snapshot,
        sizeof(pe_run_snapshot))) {
        pe_run_last_error = "unable to build the memory image";
        return false;
    }
    uint64_t delta = (uint64_t)RING3_PE_BASE - image.image_base;
    if (delta == 0) {
        /* loaded at ImageBase: nothing to relocate */
    } else if (!pe_relocate(&image, pe_run_snapshot, image.size_of_image,
        blob, blob_size, delta)) {
        pe_run_last_error = "unsupported base relocations";
        return false;
    }
    /* stage SizeOfImage, rounded to one page per PMM_PAGE_SIZE mapping */
    uint64_t staged_pages = (image.size_of_image + 0xFFF) / 0x1000;
    if (!ring3_pe_stage(pe_run_snapshot, staged_pages * 0x1000,
        (uintptr_t)RING3_PE_BASE, image.entry_point_rva)) {
        pe_run_last_error = "unable to stage the .exe payload";
        return false;
    }
    if (!task_create("pe_run", ring3_pe_task)) {
        pe_run_last_error = "unable to spawn the interpreter task";
        return false;
    }
    pe_run_last_error = 0;
    return true;
}

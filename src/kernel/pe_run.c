/*
 * pe_run.c - launching PE32 (.exe) images (Wine layer 1b). See pe_run.h.
 */

#include "pe.h"
#include "pe_run.h"
#include "ring3.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>

#define PE_RUN_MAX_SLICE_BYTES 4096U

bool pe_run_can_load(const pe_image_t* image) {
    if (!image || !image->valid) return false;
#ifdef ROCKOS_ARCH_64
    if (image->machine != PE_MACHINE_AMD64 || !image->is_plus) return false;
#else
    if (image->machine != PE_MACHINE_I386 || image->is_plus) return false;
#endif
    if (image->image_base != RING3_PE_BASE) return false;
    if (image->section_count != 1) return false;
    if (image->sections[0].raw_size == 0 ||
        image->sections[0].raw_size > PE_RUN_MAX_SLICE_BYTES) {
        return false;
    }
    if (image->entry_point_rva < image->sections[0].virtual_address) {
        return false;
    }
    if (image->entry_point_rva >=
        image->sections[0].virtual_address + PE_RUN_MAX_SLICE_BYTES) {
        return false;
    }
    return true;
}

static const char* pe_run_last_error;

const char* pe_run_error(void) {
    return pe_run_last_error ? pe_run_last_error : "";
}

bool pe_run_stage_and_launch(const uint8_t* blob, size_t blob_size) {
    pe_run_last_error = 0;
    pe_image_t image;
    if (!pe_parse(blob, blob_size, &image)) {
        pe_run_last_error = image.error ? image.error : "parse failed";
        return false;
    }
    if (!pe_run_can_load(&image)) {
        pe_run_last_error =
            "image outside layer-1b envelope "
            "(arch, ImageBase 0x60000000, one section <=4KiB, no imports)";
        return false;
    }
    /* map the single section: entry rva -> load_base + rva + entry offset
       within the section */
    uint64_t section_offset_in_section =
        image.entry_point_rva - image.sections[0].virtual_address;
    uintptr_t code_base = (uintptr_t)(RING3_PE_BASE +
        image.sections[0].virtual_address);
    if (!ring3_pe_stage(blob + image.sections[0].file_offset,
            (uint64_t)image.sections[0].raw_size, code_base,
            section_offset_in_section)) {
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

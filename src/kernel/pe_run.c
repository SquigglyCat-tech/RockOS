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

/* Kernel32 surface (layer 1d): syscall number RING3_SYSCALL_WIN32 with
 * the function id as the first argument. Args follow the stubs' ABI:
 * i386 keeps stdcall stack args, AMD64 shuffles the Win64 registers. */
typedef struct {
    const char* name;
    uint32_t id;
    uint8_t args;
} pe_win32_fn_t;

static const pe_win32_fn_t pe_win32_fns[] = {
    { "ExitProcess", 0, 1 },
    { "GetStdHandle", 1, 1 },
    { "WriteFile", 2, 4 },
    { "WriteConsoleA", 3, 4 },
};

#define PE_WIN32_FN_COUNT     (sizeof(pe_win32_fns) / sizeof(pe_win32_fns[0]))
#define PE_WIN32_STUB_ALIGN 16

/* case-insensitive equality, ASCII only; returns 0 on match */
static int pe_name_equals(const char* a, const char* b) {
    size_t j;
    for (j = 0; a[j] != '\0' && b[j] != '\0'; j++) {
        char ca = a[j], cb = b[j];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 1;
    }
    return a[j] != b[j];
}

static const pe_win32_fn_t* pe_win32_lookup(const char* name) {
    size_t i, j;
    for (i = 0; i < PE_WIN32_FN_COUNT; i++) {
        const char* a = pe_win32_fns[i].name;
        const char* b = name;
        for (j = 0; a[j] != '\0' && b[j] != '\0'; j++) {
            char ca = a[j], cb = b[j];
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            if (ca != cb) break;
        }
        if (a[j] == b[j]) {
            return &pe_win32_fns[i];
        }
    }
    return 0;
}

/* One import stub: enter RING3_SYSCALL_WIN32 with the function id and
 * its args in the kernel's registers, then return. */
static size_t pe_win32_stub(uint8_t* out, uint32_t id, uint8_t args) {
    size_t n = 0;
#ifdef ROCKOS_ARCH_64
    /* amd64: Win64 RCX,RDX,R8,R9 -> kernel RSI,RDX,RCX,R8 */
    if (args >= 1) {                       /* mov rsi, rcx */
        out[n++] = 0x48; out[n++] = 0x89; out[n++] = 0xCE;
    }
    if (args >= 3) {                       /* mov rcx, r8 */
        out[n++] = 0x4C; out[n++] = 0x89; out[n++] = 0xC1;
    }
    if (args >= 4) {                       /* mov r8, r9 */
        out[n++] = 0x4C; out[n++] = 0x89; out[n++] = 0xC8;
    }
    out[n++] = 0xBF;                       /* mov edi, id */
    out[n++] = (uint8_t)(id & 0xFF);
    out[n++] = (uint8_t)((id >> 8) & 0xFF);
    out[n++] = (uint8_t)((id >> 16) & 0xFF);
    out[n++] = (uint8_t)((id >> 24) & 0xFF);
    out[n++] = 0xB8;                       /* mov eax, RING3_SYSCALL_WIN32 */
    out[n++] = RING3_SYSCALL_WIN32;
    out[n++] = 0; out[n++] = 0; out[n++] = 0;
    out[n++] = 0xCD; out[n++] = 0x80;      /* int 0x80 */
    out[n++] = 0xC3;                       /* ret */
#else
    /* i386: stdcall stack args -> kernel ESI,EDX,ECX,EDI */
    if (args >= 1) {                       /* mov esi, [esp+4] */
        out[n++] = 0x8B; out[n++] = 0x74; out[n++] = 0x24; out[n++] = 0x04;
    }
    if (args >= 2) {                       /* mov edx, [esp+8] */
        out[n++] = 0x8B; out[n++] = 0x54; out[n++] = 0x24; out[n++] = 0x08;
    }
    if (args >= 3) {                       /* mov ecx, [esp+12] */
        out[n++] = 0x8B; out[n++] = 0x4C; out[n++] = 0x24; out[n++] = 0x0C;
    }
    if (args >= 4) {                       /* mov edi, [esp+16] */
        out[n++] = 0x8B; out[n++] = 0x7C; out[n++] = 0x24; out[n++] = 0x10;
    }
    out[n++] = 0xBB;                       /* mov ebx, id */
    out[n++] = (uint8_t)(id & 0xFF);
    out[n++] = (uint8_t)((id >> 8) & 0xFF);
    out[n++] = (uint8_t)((id >> 16) & 0xFF);
    out[n++] = (uint8_t)((id >> 24) & 0xFF);
    out[n++] = 0xB8;                       /* mov eax, RING3_SYSCALL_WIN32 */
    out[n++] = RING3_SYSCALL_WIN32;
    out[n++] = 0; out[n++] = 0; out[n++] = 0;
    out[n++] = 0xCD; out[n++] = 0x80;      /* int 0x80 */
    out[n++] = 0xC2;                       /* ret imm16 (stdcall) */
    {
        uint16_t ret_bytes = (uint16_t)(4u * args);
        out[n++] = (uint8_t)(ret_bytes & 0xFF);
        out[n++] = (uint8_t)(ret_bytes >> 8);
    }
#endif
    return n;
}

bool pe_run_prepare_imports(const pe_image_t* image, uint8_t* snapshot,
    size_t snapshot_size) {
    size_t cursor = image->size_of_image;
    uint32_t thunk_size = image->is_plus ? 8 : 4;
    if (!image || !image->valid || !snapshot) return false;
    if (image->import_count == 0 ||
        image->import_count > PE_IMPORT_MAX) return false;
    if (image->import_rva + image->import_count * thunk_size >
        image->size_of_image) return false;
    for (uint32_t slot = 0; slot < image->import_count; slot++) {
        const pe_win32_fn_t* fn = pe_win32_lookup(image->import_names[slot]);
        size_t stub_len;
        if (!fn) return false;
        if (cursor + PE_WIN32_STUB_ALIGN > snapshot_size) return false;
        cursor = (cursor + PE_WIN32_STUB_ALIGN - 1) &
            ~(size_t)(PE_WIN32_STUB_ALIGN - 1);
        stub_len = pe_win32_stub(snapshot + cursor, fn->id, fn->args);
        /* patch the IAT entry (little-endian, 32-bit RVAs even on
         * PE32+: the stub area lives under the 4 GiB mark) */
        if (image->is_plus) {
            uint64_t rva = (uint64_t)cursor;
            for (uint32_t byte = 0; byte < 8; byte++) {
                snapshot[image->import_rva + slot * 8 + byte] =
                    (uint8_t)(rva >> (byte * 8));
            }
        } else {
            uint32_t rva = (uint32_t)cursor;
            for (uint32_t byte = 0; byte < 4; byte++) {
                snapshot[image->import_rva + slot * 4 + byte] =
                    (uint8_t)(rva >> (byte * 8));
            }
        }
        cursor += stub_len;
    }
    return true;
}

static const char* pe_run_last_error;

const char* pe_run_error(void) {
    return pe_run_last_error ? pe_run_last_error : "";
}

/* Envelope check for layer 1d loads. */
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
    if (image->rva_and_sizes_count <= PE_DIR_IMPORT_TABLE ||
        image->data_directories[PE_DIR_IMPORT_TABLE].size == 0) {
        /* no imports: size_of_image must fit the snapshot */
        return image->size_of_image <= RING3_PE_SNAPSHOT_BYTES;
    }
    /* imports allowed (layer 1d): single kernel32.dll surface, all
     * functions known, plus one extra page for the stub area */
    if (image->import_count == 0 || image->import_count > PE_IMPORT_MAX) {
        return false;
    }
    if (pe_name_equals(image->import_dll, "kernel32.dll") != 0) {
        return false;
    }
    for (uint32_t slot = 0; slot < image->import_count; slot++) {
        if (!pe_win32_lookup(image->import_names[slot])) return false;
    }
    return image->size_of_image + 0x1000 <= RING3_PE_SNAPSHOT_BYTES;
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
    /* import stubs live in one extra page appended after SizeOfImage */
    uint64_t staged_pages = (image.size_of_image + 0xFFF) / 0x1000;
    if (image.import_count > 0) {
        if (!pe_run_prepare_imports(&image, pe_run_snapshot,
            image.size_of_image + 0x1000)) {
            pe_run_last_error = "unsupported import surface";
            return false;
        }
        staged_pages++;
    }
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

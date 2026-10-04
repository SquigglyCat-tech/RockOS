#ifndef ROCKOS_MULTIBOOT1_H
#define ROCKOS_MULTIBOOT1_H

#include <stdint.h>

#define MULTIBOOT1_BOOTLOADER_MAGIC 0x2BADB002U
#define MULTIBOOT1_INFO_MEMORY       (1U << 0)
#define MULTIBOOT1_INFO_CMDLINE      (1U << 2)
#define MULTIBOOT1_INFO_MODULES      (1U << 3)
#define MULTIBOOT1_INFO_AOUT         (1U << 4)
#define MULTIBOOT1_INFO_ELF          (1U << 5)
#define MULTIBOOT1_INFO_MMAP         (1U << 6)
#define MULTIBOOT1_INFO_DRIVES       (1U << 7)
#define MULTIBOOT1_INFO_CONFIG       (1U << 8)
#define MULTIBOOT1_INFO_BOOT_LOADER  (1U << 9)
#define MULTIBOOT1_INFO_APM          (1U << 10)
#define MULTIBOOT1_INFO_VBE          (1U << 11)
#define MULTIBOOT1_INFO_FRAMEBUFFER  (1U << 12)

#define MULTIBOOT1_MMAP_AVAILABLE 1U

typedef struct __attribute__((packed)) {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t symbols[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint8_t framebuffer_color_info[6];
} multiboot1_info_t;

typedef struct __attribute__((packed)) {
    uint32_t size;
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
} multiboot1_mmap_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;
    uint32_t reserved;
} multiboot1_module_t;

#endif
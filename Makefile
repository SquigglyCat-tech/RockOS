TARGET64 = x86_64-elf
TARGET32 = i686-elf
AUDIO_BOOT_TEST_TONE ?= 1
PYTHON ?= $(if $(wildcard /c/Python314/python.exe),/c/Python314/python.exe,python)
LIMINE_TOOL ?= $(if $(wildcard /c/opt/limine/limine-binary/limine-tool-windows-x86/limine.exe),/c/opt/limine/limine-binary/limine-tool-windows-x86/limine.exe,limine)
LIMINE_DATA_DIR ?= $(if $(wildcard /c/opt/limine/limine-binary),/c/opt/limine/limine-binary,$(shell "$(LIMINE_TOOL)" --print-datadir))

CC64  = $(TARGET64)-gcc
LD64  = $(TARGET64)-ld
ASM64 = $(TARGET64)-gcc

CC32  = $(TARGET32)-gcc
LD32  = $(TARGET32)-ld
ASM32 = $(TARGET32)-gcc

CFLAGS64 = -O2 -ffreestanding -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 -fno-builtin -fno-stack-protector -Wall -Wextra -Iinclude -DAUDIO_BOOT_TEST_TONE=$(AUDIO_BOOT_TEST_TONE) -DROCKOS_ARCH_64=1 -c
CFLAGS32 = -O2 -ffreestanding -march=i686 -mno-mmx -mno-sse -mno-sse2 -mno-80387 -fno-builtin -fno-stack-protector -fno-pic -fno-pie -Wall -Wextra -Iinclude -DAUDIO_BOOT_TEST_TONE=$(AUDIO_BOOT_TEST_TONE) -DROCKOS_ARCH_32=1 -c
ASMFLAGS = -Iinclude -c
LDFLAGS64 = -m elf_x86_64 -T linker.ld
LDFLAGS32 = -m elf_i386 -T linker32.ld
POWERSHELL ?= powershell.exe
UI_ASSETS = src/drivers/ui_assets.h
UI_FONT = assets/selawik/selawk.ttf
UI_FONT_LICENSE = assets/selawik/OFL.txt

C_SOURCES = src/kernel/kernel.c \
            src/kernel/desktop.c \
            src/kernel/acpi.c \
            src/kernel/pic.c \
            src/kernel/idt.c \
			src/kernel/gdt.c \
            src/drivers/display.c \
            src/drivers/ps2.c \
            src/drivers/keyboard.c \
			src/drivers/mouse.c \
			src/drivers/serial.c \
			src/drivers/timer.c \
			src/kernel/task.c \
			src/kernel/process.c \
			src/kernel/ring3.c \
			src/kernel/paging.c \
			src/kernel/pmm.c \
			src/kernel/pci.c \
			src/kernel/block.c \
			src/kernel/filesystem.c \
			src/drivers/ata_pio.c \
			src/kernel/shell.c \
			src/kernel/tour.c \
			src/drivers/audio.c

ASM64_SOURCES = src/arch/x86_64/boot.S \
			  src/arch/x86_64/gdt_load.S \
			  src/arch/x86_64/isr.S \
			  src/arch/x86_64/ring3.S \
			  src/arch/x86_64/context_switch.S \
			  src/drivers/error_sound.S

# 32-bit arch sources land in src/arch/i386/ as the port progresses
# (boot32.S, gdt32_load.S, isr32.S, ring3_32.S, context32_switch.S).
# They are listed here so `make ARCH=32` starts compiling them; add the
# files incrementally. error_sound.S is shared (raw PCM + size symbols).
ASM32_SOURCES = src/arch/i386/boot32.S \
			  src/arch/i386/gdt32_load.S \
			  src/arch/i386/isr32.S \
			  src/arch/i386/ring3_32.S \
			  src/arch/i386/context32_switch.S \
			  src/drivers/error_sound.S

OBJS64 = $(ASM64_SOURCES:.S=.o) $(C_SOURCES:.c=.o)
OBJS32 = $(ASM32_SOURCES:.S=.o) $(C_SOURCES:.c=.32.o)

KERNEL64_BIN = rockos64.bin
KERNEL32_BIN = rockos32.bin
KERNEL_BIN = rockos.bin
ISO_IMAGE = rockos.iso
DATA_DISK = rockos-data.img
INSTALL_DISK ?= rockos-install-target.img
INSTALL_IMAGE = build/rockos-install.img
INSTALL_MODULE = $(ISO_BOOT_DIR)/rockos-install.img
ISO_DIR = iso
ISO_BOOT_DIR = $(ISO_DIR)/boot

all: $(ISO_IMAGE)

# Default 64-bit objects.
%.o: %.c
	$(CC64) $(CFLAGS64) $< -o $@

%.o: %.S
	$(ASM64) $(ASMFLAGS) $< -o $@

# Parallel 32-bit objects (foo.c -> foo.32.o) so both kernels can build
# from the same tree without clobbering each other.
%.32.o: %.c
	$(CC32) $(CFLAGS32) $< -o $@

src/arch/i386/%.o: src/arch/i386/%.S
	$(ASM32) $(ASMFLAGS) $< -o $@

src/drivers/error_sound.o: assets/error_48000_stereo_s16le.pcm

src/drivers/error_sound.32.o: assets/error_48000_stereo_s16le.pcm
	$(ASM32) $(ASMFLAGS) src/drivers/error_sound.S -o $@

$(UI_ASSETS): $(UI_FONT) $(UI_FONT_LICENSE) assets/rockos-logo-outlined.png scripts/generate_ui_assets.ps1
	$(POWERSHELL) -NoProfile -ExecutionPolicy Bypass \
		-File scripts/generate_ui_assets.ps1 \
		-FontPath $(UI_FONT) \
		-LogoPath assets/rockos-logo-outlined.png \
		-OutputPath $(UI_ASSETS)

src/drivers/display.o: $(UI_ASSETS)

$(ISO_BOOT_DIR)/selawik-OFL.txt: $(UI_FONT_LICENSE)
	mkdir -p $(ISO_BOOT_DIR)
	cp $< $@

$(KERNEL_BIN): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $(KERNEL_BIN)

$(ISO_BOOT_DIR)/limine-bios.sys: $(LIMINE_DATA_DIR)/limine-bios.sys
	cp $< $@

$(ISO_BOOT_DIR)/limine-bios-cd.bin: $(LIMINE_DATA_DIR)/limine-bios-cd.bin
	cp $< $@

$(ISO_BOOT_DIR)/limine-uefi-cd.bin: $(LIMINE_DATA_DIR)/limine-uefi-cd.bin
	cp $< $@

$(ISO_DIR)/EFI/BOOT/BOOTX64.EFI: $(LIMINE_DATA_DIR)/BOOTX64.EFI
	mkdir -p $(ISO_DIR)/EFI/BOOT
	cp $< $@

$(INSTALL_IMAGE): $(KERNEL_BIN) $(ISO_BOOT_DIR)/limine-bios.sys $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI $(ISO_BOOT_DIR)/limine-disk.conf scripts/make_install_image.py
	$(PYTHON) scripts/make_install_image.py \
		--kernel $(KERNEL_BIN) \
		--bios $(ISO_BOOT_DIR)/limine-bios.sys \
		--efi $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI \
		--config $(ISO_BOOT_DIR)/limine-disk.conf \
		--output $@
	"$(LIMINE_TOOL)" bios-install $@

$(INSTALL_MODULE): $(INSTALL_IMAGE)
	cp $< $@

$(ISO_IMAGE): $(KERNEL_BIN) $(ISO_BOOT_DIR)/limine-bios.sys $(ISO_BOOT_DIR)/limine-bios-cd.bin $(ISO_BOOT_DIR)/limine-uefi-cd.bin $(ISO_BOOT_DIR)/limine.conf $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI $(INSTALL_MODULE) $(ISO_BOOT_DIR)/selawik-OFL.txt
	mkdir -p $(ISO_BOOT_DIR)
	cp $(KERNEL_BIN) $(ISO_BOOT_DIR)/rockos.bin
	xorriso -as mkisofs -R -r -J \
		-b boot/limine-bios-cd.bin \
		-no-emul-boot \
		-boot-load-size 4 \
		-boot-info-table \
		--efi-boot boot/limine-uefi-cd.bin \
		-efi-boot-part \
		--efi-boot-image \
		-o $(ISO_IMAGE) $(ISO_DIR)

$(DATA_DISK):
	qemu-img create -f raw $@ 16M

$(INSTALL_DISK):
	qemu-img create -f raw $@ 64M

# Keep the install target first so reboot boots it; leave the CD as fallback.
run: $(ISO_IMAGE) $(DATA_DISK) $(INSTALL_DISK)
	qemu-system-x86_64 -machine pc -serial stdio \
		-audiodev driver=dsound,id=audio0 \
		-device AC97,audiodev=audio0 \
		-drive file=$(INSTALL_DISK),format=raw,if=none,id=target \
		-device ide-hd,drive=target,bus=ide.0,unit=1,bootindex=1 \
		-drive file=$(DATA_DISK),format=raw,if=none,id=data \
		-device ide-hd,drive=data,bus=ide.0,unit=0,bootindex=2 \
		-drive file=$(ISO_IMAGE),format=raw,media=cdrom,if=none,id=installcd,readonly=on \
		-device ide-cd,drive=installcd,bus=ide.1,unit=0,bootindex=3

run-installed: $(INSTALL_DISK) $(DATA_DISK)
	qemu-system-x86_64 -machine pc -boot order=c \
		-audiodev driver=dsound,id=audio0 \
		-device AC97,audiodev=audio0 \
		-drive file=$(INSTALL_DISK),format=raw,if=ide,index=0 \
		-drive file=$(DATA_DISK),format=raw,if=ide,index=1

clean:
	rm -f $(OBJS) $(KERNEL_BIN) $(ISO_IMAGE) $(INSTALL_MODULE) $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI $(INSTALL_IMAGE) $(UI_ASSETS)

.PHONY: all run run-installed clean
TARGET = x86_64-elf
AUDIO_BOOT_TEST_TONE ?= 1
PYTHON ?= $(if $(wildcard /c/Python314/python.exe),/c/Python314/python.exe,python)
LIMINE_TOOL ?= $(if $(wildcard /c/opt/limine/limine-binary/limine-tool-windows-x86/limine.exe),/c/opt/limine/limine-binary/limine-tool-windows-x86/limine.exe,limine)
LIMINE_DATA_DIR ?= $(if $(wildcard /c/opt/limine/limine-binary),/c/opt/limine/limine-binary,$(shell "$(LIMINE_TOOL)" --print-datadir))

CC  = $(TARGET)-gcc
LD  = $(TARGET)-ld
ASM = $(TARGET)-gcc

CFLAGS = -O2 -ffreestanding -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 -fno-builtin -fno-stack-protector -Wall -Wextra -Iinclude -DAUDIO_BOOT_TEST_TONE=$(AUDIO_BOOT_TEST_TONE) -c
ASMFLAGS = -Iinclude -c
LDFLAGS = -m elf_x86_64 -T linker.ld
POWERSHELL ?= powershell.exe
UI_ASSETS = src/drivers/ui_assets.h
UI_FONT = assets/selawik/selawk.ttf
UI_FONT_LICENSE = assets/selawik/OFL.txt

C_SOURCES = src/kernel/kernel.c \
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

ASM_SOURCES = src/arch/x86_64/boot.S \
			  src/arch/x86_64/gdt_load.S \
			  src/arch/x86_64/isr.S \
			  src/arch/x86_64/ring3.S \
			  src/arch/x86_64/context_switch.S \
			  src/drivers/error_sound.S

OBJS = $(ASM_SOURCES:.S=.o) $(C_SOURCES:.c=.o)

KERNEL_BIN = rockos.bin
ISO_IMAGE = rockos.iso
DATA_DISK = rockos-data.img
INSTALL_DISK ?= rockos-install-target.img
INSTALL_IMAGE = build/rockos-install.img
INSTALL_MODULE = $(ISO_BOOT_DIR)/rockos-install.img
ISO_DIR = iso
ISO_BOOT_DIR = $(ISO_DIR)/boot

all: $(ISO_IMAGE)

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

%.o: %.S
	$(ASM) $(ASMFLAGS) $< -o $@

src/drivers/error_sound.o: assets/error_48000_stereo_s16le.pcm

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

run: $(ISO_IMAGE) $(DATA_DISK) $(INSTALL_DISK)
	qemu-system-x86_64 -machine pc -boot order=d -serial stdio \
		-audiodev driver=dsound,id=audio0 \
		-device AC97,audiodev=audio0 \
		-drive file=$(ISO_IMAGE),format=raw,media=cdrom,if=ide,index=2,readonly=on \
		-drive file=$(DATA_DISK),format=raw,if=ide,index=0 \
		-drive file=$(INSTALL_DISK),format=raw,if=ide,index=1

run-installed: $(INSTALL_DISK) $(DATA_DISK)
	qemu-system-x86_64 -machine pc -boot order=c \
		-audiodev driver=dsound,id=audio0 \
		-device AC97,audiodev=audio0 \
		-drive file=$(INSTALL_DISK),format=raw,if=ide,index=0 \
		-drive file=$(DATA_DISK),format=raw,if=ide,index=1

clean:
	rm -f $(OBJS) $(KERNEL_BIN) $(ISO_IMAGE) $(INSTALL_MODULE) $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI $(INSTALL_IMAGE) $(UI_ASSETS)

.PHONY: all run run-installed clean
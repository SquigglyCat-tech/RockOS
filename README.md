# RockOS

RockOS is an experimental 64-bit x86 hobby operating system built from scratch
in C and x86-64 assembly. Version 1 is an early, QEMU-focused milestone with a
graphical installer and a small interactive kernel environment.

## Highlights

- Limine BIOS and UEFI boot support
- 64-bit kernel with interrupt, paging, task, and TSS foundations
- Framebuffer UI and graphical installation flow
- PS/2 keyboard and mouse input
- ATA PIO storage and a basic filesystem
- AC'97 audio experiments
- Isolated Ring 3 proof of concept

RockOS is work in progress and is currently intended for QEMU. Do not use the
installer on a physical disk.

## Build and run

The build uses GNU Make, an `x86_64-elf` GCC/binutils cross-toolchain, Python 3,
PowerShell, xorriso, Limine boot files, and QEMU. The Makefile expects the
cross-toolchain and Limine files in the paths configured near its top; override
`PATH`, `LIMINE_DATA_DIR`, or `LIMINE_TOOL` for your environment as needed.

From an MSYS2 UCRT64 shell in the cloned RockOS directory:

```sh
export PATH="/c/opt/x86_64-elf-tools/bin:$PATH"
make run
```

### Virtual machine storage

The V.1 installer image is 16 MiB, so the install-target disk must be at least
16 MiB. The default `make run` configuration creates a 64 MiB install-target
disk and a separate 16 MiB data disk (80 MiB total virtual-disk capacity),
in addition to the roughly 21 MB boot ISO. Allow extra host storage for the
build and for any disk images you keep. The installer overwrites its selected
target disk; use a disposable virtual disk.

Build the ISO without launching QEMU:

```sh
make
```

To disable the optional boot test tone:

```sh
make AUDIO_BOOT_TEST_TONE=0
```

## Version history

The `v1.0` tag marks the first public RockOS milestone. Ongoing work belongs on
the `v2.0-dev` branch; keep `main` as the stable V.1 line.

## Third-party assets

The bundled Selawik font is distributed under the SIL Open Font License 1.1;
see [`assets/selawik/OFL.txt`](assets/selawik/OFL.txt). Its generated glyph
atlas is built from the bundled font by `scripts/generate_ui_assets.ps1`.

No separate license has been selected for the RockOS project source yet.

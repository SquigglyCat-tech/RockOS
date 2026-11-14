# RockOS

RockOS is an experimental 64-bit x86 hobby operating system built from scratch
in C and x86-64 assembly. Version 1 is an early, QEMU-focused milestone with a
graphical installer and a small interactive kernel environment.

## Highlights

- Limine BIOS and UEFI boot support
- Boot menu video modes: 1024x768 (default), 1920x1080, and 3840x2160 (4K)
- 64-bit kernel with interrupt, paging, task, and TSS foundations
- Framebuffer UI and graphical installation flow
- Desktop with a start menu and draggable About/shortcuts windows
- PS/2 keyboard and mouse input
- ATA PIO storage and a basic filesystem
- AC'97 audio experiments
- Isolated Ring 3 proof of concept

RockOS is work in progress and is currently intended for QEMU. Do not use the
installer on a physical disk. Expect bugs (If you need them fixed just dm me @spaminhaler_30401 on Discord)

## System requirements

| | Minimum | Recommended |
|---|---|---|
| RAM | 3 GB | 13 GB |
| Storage | 40 MB virtual disk (16 MiB install target + ISO) | 128 MB mainline disk |

3 GB is the minimal requirement and 13 GB is the recommended amount for best
performance and preparation for Wine being ported to RockOS. The kernel also
needs the machine-readable framebuffer that Limine provides plus a 64-bit
x86 CPU (or an x86-target emulator such as QEMU). Fine on 4 GB machines when
the framebuffer resolution is low; 13 GB leaves room for large Wine prefix
disk images and compile-time assets.

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

## Requirements (VirtualBox)

- Type: Other, Version: Other/Unknown (64-bit)
- CPU: VT-x/AMD-V enabled (Hyper-V/WSL2 can interfere on Windows)
- RAM: [minimum TBD, test 64 / 128 / 256 MB]
- Firmware: [BIOS or EFI, test both]
- Disk controller: IDE (PIIX3/PIIX4). SATA/NVMe NOT supported yet
- Optional: COM1 -> Raw File for serial debug logs

## Troubleshooting

- Guru Meditation: check the VM is set to 64-bit and VT-x is on
- Boots straight to the shell: the disk probably isn't on IDE
  (serial log will show "[ata] probe ... none")
  
### Virtual machine storage

The V.1 installer image is 16 MiB, so the install-target disk must be at least
16 MiB. The default `make run` configuration creates a 64 MiB install-target
disk and a separate 16 MiB data disk (80 MiB total virtual-disk capacity),
in addition to the roughly 21 MB boot ISO. Allow extra host storage for the
build and for any disk images you keep. The installer overwrites its selected
target disk; use a disposable virtual disk.

`make run` gives the dedicated install-target disk boot priority, followed by
the data disk and installer CD. After installation completes, restart the VM
to boot the installed system. To boot it directly in QEMU, close the VM and
run `make run-installed`.

The installer prefers the ATA primary-slave disk as its target, preserving
the primary-master data disk. If no slave disk is detected, it can use the
primary-master disk; the confirmation page warns that this erases that disk.
Only legacy IDE/ATA disks are currently supported by the installer.

Audio uses an emulated Intel AC'97 device. If playback is unavailable, check
that the virtual machine has audio output enabled and use the RockOS `audio`
shell command to inspect AC'97 initialization and DMA playback status.

Press `Ctrl+Alt+D` to open or close the desktop (`Ctrl+Alt+Q` also exits).
Its start menu and desktop icons open File Manager, Terminal, System Info,
and About. Title buttons minimize / maximize / close; `Tab` cycles windows,
`Esc` closes menu/window/desktop, `F1` shows desktop help, and `Ctrl+Alt+M`
minimizes the focused window. Terminal keeps Up/Down command history; File
Manager supports Up/Down/PgUp/PgDn/Home/End, Enter to open, Backspace for up.

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

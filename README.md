# RockOS

RockOS is an experimental 64-bit x86 hobby operating system built completely from scratch in C and x86-64 assembly. Version 1 is an early milestone featuring a graphical installer, a Limine bootloader setup, and an interactive desktop environment tailored to run within **Oracle VirtualBox**.

---

## Highlights

* **Boot Support:** Limine BIOS and UEFI boot integration.
* **Video Modes:** Boot menu resolutions include 1024x768 (default), 1920x1080, and 3840x2160 (4K).
* **Kernel Architecture:** 64-bit, 32-bit, and 16-bit kernels featuring interrupt, paging, task, and TSS foundations.
* **Graphical Interface:** Framebuffer-driven UI with a complete graphical installation flow.
* **Desktop Environment:** Includes a start menu, draggable windows, and shortcuts.
* **Peripherals & Drivers:** PS/2 keyboard/mouse input, ATA PIO storage, a basic filesystem, and Intel AC'97 audio support.
* **Security:** Isolated Ring 3 proof-of-concept architecture.

> **Warning:** RockOS is a work in progress intended strictly for virtual machines like Oracle VirtualBox. **Do not** run the installer on a physical hard drive. 

>
> *Encountered bugs? Reach out on Discord via `@spaminhaler_30401`.*

---

## System Requirements

| Resource | Minimum | Recommended | Notes |

| :--- | :--- | :--- | :--- |

| **RAM** | 3 GB | 13 GB | 3 GB is the baseline; 13 GB provides ample headroom for high-resolution buffers and future Wine porting workloads. |

| **Storage** | 30 MB virtual disk | 128 MB+ virtual disk | Accommodates the 26 MiB install target and ISO. |

The kernel requires a 64-bit x86 CPU and a machine-readable framebuffer provided by Limine. 



---



## Building the ISO

To compile RockOS, whether for making your own distro or branch, you need a development environment equipped with GNU Make, an `x86_64-elf` GCC/binutils cross-toolchain, Python 3, xorriso, and Limine boot files.
To build the bootable ISO without launching external emulators:



```sh

make

```
To disable the optional boot test tone during compilation:



```sh

make AUDIO_BOOT_TEST_TONE=0

```



---



## Oracle VirtualBox Setup Guide

Because RockOS currently relies on legacy ATA controllers, configure your VirtualBox virtual machine according to the following specifications:

### 1. VM Configuration Settings

* **Name / OS Type:** Type: `Other`, Version: `Other/Unknown (64-bit)`
* **Processor / CPU:** Enable **VT-x/AMD-V** hardware virtualization. *(Note: On Windows hosts, Hyper-V or WSL2 can sometimes interfere with nested virtualization).*
* **Memory (RAM):** Set to at least 3 GB (or lower e.g., 512 MB – 1 GB for basic testing).
* **Firmware:** Compatible with both BIOS and EFI (test both if needed).
* **Audio:** Enable audio output (emulates an Intel AC'97 device).



### 2. Storage Configuration (Crucial)

* **Disk Controller:** Must use **IDE (PIIX3 / PIIX4)**. *SATA and NVMe controllers are not supported in V.1.*

* **Virtual Disks:** 

  * The installer image requires an install-target disk of at least 16 MiB (the default build generates a 64 MiB install disk + a separate 16 MiB data disk).
  * Attach the generated RockOS ISO file to the virtual optical drive.
  * Set boot priority so the VM boots from the optical drive or install target.

---

## Using RockOS



Once booted into the desktop environment, use the following shortcuts and navigation tips:

* **Desktop Toggle:** Press `Ctrl + Alt + D` to open/close the desktop interface (`Ctrl + Alt + Q` to exit).
* **Start Menu / Icons:** Access the File Manager, Terminal, System Info, and About windows.
* **Window Management:** Title bar buttons minimize, maximize, and close windows. 
  * `Tab` cycles through active windows.
  * `Esc` closes active menus, windows, or the desktop overlay.
  * `F1` displays desktop help.
  * `Ctrl + Alt + M` minimizes the currently focused window.

* **Terminal & Navigation:** 

  * Terminal retains Up/Down command history.
  * File Manager supports Arrow keys, `PgUp`/`PgDn`, `Home`/`End`, `Enter` to open, and `Backspace` to go up a directory.

* **Audio Check:** If audio playback is missing, verify that VirtualBox audio is enabled and use the RockOS shell command `audio` to inspect AC'97 initialization and DMA status.
---

## Troubleshooting

* **Guru Meditation Error on Boot:** Verify that the VM is configured as **64-bit** and that **VT-x/AMD-V** is enabled in your CPU settings.
* **Boots Straight to Shell:** The virtual disk is likely attached to a SATA/NVMe controller instead of **IDE**. Check serial debug logs (`COM1 -> Raw File`) for probe errors like `[ata] probe ... none`.
* **Installer Disk Overwrites:** The installer targets the ATA primary-slave disk by default to preserve the primary-master data disk. If no slave disk is present, it will prompt to use the primary-master disk (which will be formatted).

---

## Version History & Assets

* **v1.0:** Marks the first public milestone of RockOS. `main` remains the stable V.1 branch, while ongoing development takes place on `v2.0-dev`.
* **Third-Party Assets:** The bundled Selawik font is distributed under the terms of the SIL Open Font License 1.1.

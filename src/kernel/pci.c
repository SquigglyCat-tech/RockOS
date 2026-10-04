/*
 * ROK PCI discovery - prototype implementation
 * Freestanding, no malloc, no interrupts, single-CPU safe.
 *
 * Hardware notes:
 *  - Mechanism #1: write 32-bit address to 0xCF8 with bit31=1,
 *    then read 0xCFC. Address = 1<<31 | bus<<16 | slot<<11 | func<<8 | (off & 0xFC).
 *  - Vendor 0xFFFF = no device. Must check before reading further.
 *  - Header byte 0x0E bit7 = multifunction. If clear, functions 1..7 are invalid.
 *  - Class/Subclass/ProgIf/Rev live in dword at 0x08.
 *  - BARs at 0x10..0x24. Bit0=1 => I/O, else MMIO. Bits[2:1]=00=>32-bit,
 *    10=>64-bit (consumes next slot), 01=>reserved. We ONLY read BARs.
 *    Sizing (write 0xFFFFFFFF) would disable decoding -> deliberately omitted.
 */

#include "pci.h"  /* integrator: add -Iinclude or fix to "../../include/pci.h" */
#include "io.h"   /* integrator: fix path if your io.h lives elsewhere (see below) */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =====================================================================
 * PORT I/O ADAPTER -- EDIT ON IMPORT IF NEEDED (only place to edit)
 *
 * This file does NOT implement port I/O (would conflict with ROK's io layer).
 * It needs 32-bit port I/O. Default assumes hobby-OS names:
 *    void     outl(uint16_t port, uint32_t val);
 *    uint32_t inl(uint16_t port);
 *
 * If ROK uses different names/order, change ONLY the two defines below.
 * Example if ROK uses io_out32/io_in32:
 *    #define PCI_OUTL(p,v) io_out32((p),(v))
 *    #define PCI_INL(p)    io_in32((p))
 * Example if ROK uses outl(val,port) order:
 *    #define PCI_OUTL(p,v) outl((v),(p))
 * ===================================================================== */
#ifndef PCI_OUTL
#define PCI_OUTL(port, val) outl((port), (val))
#endif
#ifndef PCI_INL
#define PCI_INL(port) inl((port))
#endif

static inline void pci_port_out32(uint16_t port, uint32_t val) {
    PCI_OUTL(port, val);
}
static inline uint32_t pci_port_in32(uint16_t port) {
    return PCI_INL(port);
}

/* ---- static storage: no malloc ---- */
static pci_device_t g_devs[PCI_MAX_DEVICES];
static size_t g_count;
static size_t g_overflow;
static bool   g_scanned;

/* ---- low-level Mechanism #1 ---- */
static uint32_t pci_make_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint32_t)(0x80000000u
        | ((uint32_t)bus << 16)
        | (((uint32_t)slot & 0x1Fu) << 11)
        | (((uint32_t)func & 0x07u) << 8)
        | ((uint32_t)off & 0xFCu));
}

static uint32_t pci_raw_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    pci_port_out32(PCI_CONFIG_ADDRESS, pci_make_addr(bus, slot, func, off));
    return pci_port_in32(PCI_CONFIG_DATA);
}

/* ---- public config reads ---- */
uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    if (device >= PCI_MAX_SLOTS || function >= PCI_MAX_FUNCS) {
        return 0xFFFFFFFFu;
    }
    return pci_raw_read32(bus, device, function, (uint8_t)(offset & 0xFCu));
}

uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t d = pci_config_read32(bus, device, function, offset);
    /* offset&2 selects low/high word of the dword */
    return (uint16_t)((d >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

void pci_config_write16(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint16_t value) {
    if (device >= PCI_MAX_SLOTS || function >= PCI_MAX_FUNCS) {
        return;
    }

    pci_port_out32(PCI_CONFIG_ADDRESS,
        pci_make_addr(bus, device, function, offset));
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2u)), value);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t d = pci_config_read32(bus, device, function, offset);
    return (uint8_t)((d >> ((offset & 3u) * 8u)) & 0xFFu);
}

/* ---- BAR decode (read-only, safe) ---- */
static void pci_decode_bars(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t hdr_layout, pci_device_t *d) {
    unsigned i;
    for (i = 0; i < 6; i++) {
        d->bars[i].raw = 0;
        d->bars[i].addr = 0;
        d->bars[i].present = false;
        d->bars[i].is_io = false;
        d->bars[i].is_64bit = false;
        d->bars[i].prefetchable = false;
        d->bars[i].is_upper_half = false;
    }

    /* Only header 0x00 has 6 BARs, 0x01 (bridge) has 2. Others: skip. */
    unsigned max_bars = 0;
    uint8_t layout = (uint8_t)(hdr_layout & 0x7Fu);
    if (layout == 0x00u) {
        max_bars = 6;
    } else if (layout == 0x01u) {
        max_bars = 2;
    } else {
        return;
    }

    for (i = 0; i < max_bars; i++) {
        uint8_t off = (uint8_t)(PCI_OFF_BAR0 + i * 4u);
        uint32_t raw = pci_raw_read32(bus, slot, func, off);
        d->bars[i].raw = raw;
        if (raw == 0u) {
            continue; /* unimplemented BAR */
        }
        if (raw & 0x01u) {
            /* I/O space: base = bits 31:2 */
            d->bars[i].present = true;
            d->bars[i].is_io = true;
            d->bars[i].addr = (uint64_t)(raw & 0xFFFFFFFCu);
        } else {
            uint8_t memtype = (uint8_t)((raw >> 1) & 0x03u);
            bool pref = ((raw >> 3) & 0x01u) != 0;
            if (memtype == 0x00u) {
                d->bars[i].present = true;
                d->bars[i].is_io = false;
                d->bars[i].prefetchable = pref;
                d->bars[i].addr = (uint64_t)(raw & 0xFFFFFFF0u);
            } else if (memtype == 0x02u) {
                /* 64-bit: consumes next slot */
                if (i + 1 >= max_bars) {
                    /* malformed, treat low as 32-bit */
                    d->bars[i].present = true;
                    d->bars[i].prefetchable = pref;
                    d->bars[i].addr = (uint64_t)(raw & 0xFFFFFFF0u);
                } else {
                    uint32_t upper = pci_raw_read32(bus, slot, func, (uint8_t)(off + 4u));
                    d->bars[i].present = true;
                    d->bars[i].is_io = false;
                    d->bars[i].is_64bit = true;
                    d->bars[i].prefetchable = pref;
                    d->bars[i].addr = (((uint64_t)upper << 32) | (raw & 0xFFFFFFF0u));
                    /* mark next slot consumed so dump/find skip it */
                    i++;
                    d->bars[i].raw = upper;
                    d->bars[i].present = false;
                    d->bars[i].is_upper_half = true;
                    d->bars[i].addr = 0;
                }
            } else {
                /* memtype 01 = reserved/16-bit legacy. Keep raw, flag present
                   but do not trust addr. */
                d->bars[i].present = true;
                d->bars[i].is_io = false;
                d->bars[i].prefetchable = pref;
                d->bars[i].addr = (uint64_t)(raw & 0xFFFFFFF0u);
            }
        }
    }
}

static bool pci_probe_one(uint8_t bus, uint8_t slot, uint8_t func, pci_device_t *out) {
    uint32_t id = pci_raw_read32(bus, slot, func, PCI_OFF_VENDOR_ID);
    uint16_t vend = (uint16_t)(id & 0xFFFFu);
    if (vend == PCI_VENDOR_NONE) {
        return false;
    }
    uint16_t dev = (uint16_t)((id >> 16) & 0xFFFFu);

    uint32_t cls = pci_raw_read32(bus, slot, func, PCI_OFF_REVISION);
    uint8_t hdr  = pci_config_read8(bus, slot, func, PCI_OFF_HEADER_TYPE);
    uint8_t irq_l = pci_config_read8(bus, slot, func, PCI_OFF_IRQ_LINE);
    uint8_t irq_p = pci_config_read8(bus, slot, func, PCI_OFF_IRQ_PIN);

    out->bus = bus;
    out->device = slot;
    out->function = func;
    out->vendor_id = vend;
    out->device_id = dev;
    out->revision = (uint8_t)(cls & 0xFFu);
    out->prog_if = (uint8_t)((cls >> 8) & 0xFFu);
    out->subclass = (uint8_t)((cls >> 16) & 0xFFu);
    out->class_code = (uint8_t)((cls >> 24) & 0xFFu);
    out->header_type = hdr;
    out->irq_line = irq_l;
    out->irq_pin = irq_p;
    out->subsys_vendor = 0;
    out->subsys_id = 0;

    if ((hdr & 0x7Fu) == 0x00u) {
        out->subsys_vendor = pci_config_read16(bus, slot, func, PCI_OFF_SUBSYS_VEND);
        out->subsys_id = pci_config_read16(bus, slot, func, PCI_OFF_SUBSYS_ID);
    }

    pci_decode_bars(bus, slot, func, hdr, out);
    return true;
}

static void pci_add(const pci_device_t *d) {
    if (g_count < PCI_MAX_DEVICES) {
        g_devs[g_count] = *d; /* struct copy, no memcpy */
        g_count++;
    } else {
        g_overflow++;
    }
}

/* ---- lifecycle ---- */
void pci_init(void) {
    g_count = 0;
    g_overflow = 0;
    g_scanned = false;
    /* no need to scrub g_devs; valid entries are 0..g_count-1 after scan */
}

void pci_scan(void) {
    unsigned bus, slot;
    pci_device_t tmp;

    g_count = 0;
    g_overflow = 0;

    /* Bounded loops: 256*32*(1..8) reads max. Cannot infinite-loop. */
    for (bus = 0; bus < PCI_MAX_BUSES; bus++) {
        for (slot = 0; slot < PCI_MAX_SLOTS; slot++) {
            /* function 0 decides: empty slot vs single vs multi */
            if (!pci_probe_one((uint8_t)bus, (uint8_t)slot, 0, &tmp)) {
                continue;
            }
            pci_add(&tmp);

            if ((tmp.header_type & 0x80u) == 0u) {
                continue; /* single-function, skip 1..7 */
            }
            /* multifunction: probe 1..7 individually */
            for (unsigned f = 1; f < PCI_MAX_FUNCS; f++) {
                if (pci_probe_one((uint8_t)bus, (uint8_t)slot, (uint8_t)f, &tmp)) {
                    pci_add(&tmp);
                }
            }
        }
    }
    g_scanned = true;
}

size_t pci_get_device_count(void) { return g_count; }

const pci_device_t *pci_get_device(size_t index) {
    if (index >= g_count) {
        return NULL;
    }
    return &g_devs[index];
}

const pci_device_t *pci_find_by_id(uint16_t vendor, uint16_t device) {
    for (size_t i = 0; i < g_count; i++) {
        if (g_devs[i].vendor_id == vendor && g_devs[i].device_id == device) {
            return &g_devs[i];
        }
    }
    return NULL;
}

const pci_device_t *pci_find_by_class(uint8_t class_code, uint8_t subclass) {
    for (size_t i = 0; i < g_count; i++) {
        if (g_devs[i].class_code == class_code && g_devs[i].subclass == subclass) {
            return &g_devs[i];
        }
    }
    return NULL;
}

size_t pci_overflow_count(void) { return g_overflow; }
bool pci_was_scanned(void) { return g_scanned; }

/* ---- name helpers (tiny tables, no heap) ---- */
const char *pci_vendor_name(uint16_t vendor_id) {
    /* Common x86/QEMU vendors only. Unknown => "Unknown". */
    switch (vendor_id) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x10EC: return "Realtek";
        case 0x1AF4: return "RedHat/VirtIO";
        case 0x1B36: return "RedHat/QEMU-PCIe";
        case 0x1234: return "Bochs/QEMU";
        case 0x15AD: return "VMware";
        case 0x80EE: return "VirtualBox";
        case 0x1013: return "CirrusLogic";
        case 0x106B: return "Apple";
        case 0x1106: return "VIA";
        case 0x1039: return "SiS";
        case 0x1002: return "AMD/ATI";
        case 0x1AE0: return "Google";
        default: return "Unknown";
    }
}

const char *pci_class_name(uint8_t class_code) {
    switch (class_code) {
        case 0x00: return "Unclassified";
        case 0x01: return "MassStorage";
        case 0x02: return "Network";
        case 0x03: return "Display";
        case 0x04: return "Multimedia";
        case 0x05: return "Memory";
        case 0x06: return "Bridge";
        case 0x07: return "Comm";
        case 0x08: return "GenericSys";
        case 0x09: return "Input";
        case 0x0A: return "Docking";
        case 0x0B: return "Processor";
        case 0x0C: return "SerialBus";
        case 0x0D: return "Wireless";
        case 0x0E: return "Intelligent";
        case 0x0F: return "Satellite";
        case 0x10: return "Encryption";
        case 0x11: return "SignalProc";
        case 0x12: return "ProcAccel";
        case 0x13: return "NonEssential";
        case 0x40: return "CoProcessor";
        case 0xFF: return "Unassigned";
        default: return "Reserved";
    }
}

const char *pci_subclass_name(uint8_t class_code, uint8_t subclass) {
    /* Minimal useful subset; fallback "Unknown". */
    if (class_code == 0x01) {
        switch (subclass) {
            case 0x00: return "SCSI";
            case 0x01: return "IDE";
            case 0x05: return "ATA";
            case 0x06: return "SATA";
            case 0x07: return "SAS";
            case 0x08: return "NVMe";
            default: return "Storage-Other";
        }
    }
    if (class_code == 0x02) {
        return (subclass == 0x00) ? "Ethernet" : "Net-Other";
    }
    if (class_code == 0x03) {
        switch (subclass) {
            case 0x00: return "VGA";
            case 0x01: return "SVGA";
            case 0x02: return "3D";
            default: return "Display-Other";
        }
    }
    if (class_code == 0x06) {
        switch (subclass) {
            case 0x00: return "Host";
            case 0x01: return "ISA";
            case 0x04: return "PCI-PCI";
            case 0x80: return "OtherBridge";
            default: return "Bridge-Other";
        }
    }
    if (class_code == 0x0C) {
        switch (subclass) {
            case 0x00: return "FireWire";
            case 0x03: return "USB";
            case 0x05: return "SMBus";
            default: return "Serial-Other";
        }
    }
    return "Unknown";
}

/* ---- formatting without libc (only calls puts) ---- */
static void pci_emit_hex(pci_puts_fn_t out, uint32_t v, int width) {
    static const char H[] = "0123456789ABCDEF";
    char tmp[9];
    if (width < 1) width = 1;
    if (width > 8) width = 8;
    for (int i = 0; i < width; i++) {
        tmp[width - 1 - i] = H[v & 0xFu];
        v >>= 4;
    }
    tmp[width] = '\0';
    out(tmp);
}

static void pci_emit_dec(pci_puts_fn_t out, uint32_t v) {
    char tmp[11];
    char rev[10];
    int n = 0;
    if (v == 0u) { out("0"); return; }
    while (v > 0u && n < 10) {
        rev[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (int i = 0; i < n; i++) {
        tmp[i] = rev[n - 1 - i];
    }
    tmp[n] = '\0';
    out(tmp);
}

static void pci_emit_hex64(pci_puts_fn_t out, uint64_t v) {
    /* 0x + trimmed hex */
    bool started = false;
    out("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned d = (unsigned)((v >> shift) & 0xFu);
        if (!started) {
            if (d == 0 && shift != 0) continue;
            started = true;
        }
        char b[2];
        b[0] = (char)(d < 10 ? ('0' + d) : ('A' + d - 10));
        b[1] = '\0';
        out(b);
    }
}

void pci_dump_devices(pci_puts_fn_t puts) {
    if (!puts) return;
    if (!g_scanned) {
        puts("PCI: not scanned yet (call pci_scan())\n");
        return;
    }
    puts("## PCI Devices\n");
    for (size_t i = 0; i < g_count; i++) {
        const pci_device_t *d = &g_devs[i];
        /* Example: 00:1f.0 Vendor 8086 Device 7000 Class 06:01 Bridge-Other Intel */
        pci_emit_hex(puts, d->bus, 2); puts(":");
        pci_emit_hex(puts, d->device, 2); puts(".");
        pci_emit_hex(puts, d->function, 1); puts(" ");
        puts("Vendor "); pci_emit_hex(puts, d->vendor_id, 4);
        puts(" Device "); pci_emit_hex(puts, d->device_id, 4);
        puts(" Class "); pci_emit_hex(puts, d->class_code, 2);
        puts(":"); pci_emit_hex(puts, d->subclass, 2);
        puts(" Prog "); pci_emit_hex(puts, d->prog_if, 2);
        puts(" Rev "); pci_emit_hex(puts, d->revision, 2);
        puts(" Hdr "); pci_emit_hex(puts, d->header_type, 2);
        puts(" ");
        puts(pci_class_name(d->class_code));
        puts("/");
        puts(pci_subclass_name(d->class_code, d->subclass));
        puts(" ");
        puts(pci_vendor_name(d->vendor_id));
        puts("\n");

        /* BAR line(s), decoded only */
        for (unsigned b = 0; b < 6; b++) {
            const pci_bar_t *bar = &d->bars[b];
            if (bar->is_upper_half) continue;
            if (!bar->present) continue;
            puts("    BAR"); pci_emit_dec(puts, b); puts(": ");
            if (bar->is_io) {
                puts("IO ");
                pci_emit_hex64(puts, bar->addr);
            } else {
                puts(bar->is_64bit ? "MEM64 " : "MEM32 ");
                if (bar->prefetchable) puts("PREF ");
                pci_emit_hex64(puts, bar->addr);
            }
            puts(" (raw ");
            pci_emit_hex(puts, bar->raw, 8);
            puts(")\n");
        }
    }
    puts("PCI: count ");
    pci_emit_dec(puts, (uint32_t)g_count);
    puts(", overflow ");
    pci_emit_dec(puts, (uint32_t)g_overflow);
    puts("\n");
}

/* ---- optional QEMU / bring-up self-test ---- */
bool pci_self_test(pci_puts_fn_t puts) {
    bool ok = true;
    size_t bridges = 0;

    if (!g_scanned) {
        pci_scan();
    }

    if (!puts) {
        /* still compute pass/fail without printing */
    } else {
        puts("PCI self-test: start\n");
    }

    if (g_count == 0) {
        if (puts) puts("PCI self-test: FAIL: zero devices (expected host bridge on QEMU)\n");
        ok = false;
    }
    if (g_overflow != 0) {
        if (puts) puts("PCI self-test: FAIL: device list truncated, raise PCI_MAX_DEVICES\n");
        ok = false;
    }
    for (size_t i = 0; i < g_count; i++) {
        if (g_devs[i].vendor_id == PCI_VENDOR_NONE) {
            if (puts) puts("PCI self-test: FAIL: 0xFFFF entry leaked into list\n");
            ok = false;
            break;
        }
        if (g_devs[i].device >= 32 || g_devs[i].function >= 8) {
            if (puts) puts("PCI self-test: FAIL: out-of-range BDF\n");
            ok = false;
            break;
        }
        if (g_devs[i].class_code == 0x06) bridges++;
    }
    /* QEMU pc/q35 always has at least one bridge/host. Warn, don't hard-fail
       exotic configs, but flag it: helps catch broken port I/O. */
    if (bridges == 0 && ok) {
        if (puts) puts("PCI self-test: WARN: no class 06 bridge found (unusual on QEMU PC)\n");
    }

    if (puts) {
        puts("PCI self-test: devices ");
        pci_emit_dec(puts, (uint32_t)g_count);
        puts(", bridges ");
        pci_emit_dec(puts, (uint32_t)bridges);
        puts(ok ? ": PASS\n" : ": FAIL\n");
    }
    return ok;
}
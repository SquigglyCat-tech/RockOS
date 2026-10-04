#ifndef ROK_PCI_H
#define ROK_PCI_H
/*
 * ROK PCI discovery - public header (prototype)
 * Freestanding C, x86-64, no libc, no malloc.
 *
 * What this is:
 *   Read-only PCI enumeration using IO-port Mechanism #1
 *   (CONFIG_ADDRESS 0xCF8 / CONFIG_DATA 0xCFC).
 *
 * Dependencies (read before import):
 *   1. PORT I/O: pci.c needs 32-bit x86 port I/O. See the
 *      "PORT I/O ADAPTER" section at the top of pci.c.
 *      Default expects: outl(port,val) / inl(port).
 *   2. OUTPUT: this header never touches the terminal directly.
 *      Caller supplies a `pci_puts_fn_t` (e.g. wrapper around
 *      terminal_write / serial_write_string). See pci_dump_devices().
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PCI_CONFIG_ADDRESS  0xCF8u
#define PCI_CONFIG_DATA     0xCFCu

#define PCI_MAX_BUSES 256u
#define PCI_MAX_SLOTS 32u
#define PCI_MAX_FUNCS 8u

/* Static storage bound for v1. Increase if pci_overflow_count() > 0. */
#ifndef PCI_MAX_DEVICES
#define PCI_MAX_DEVICES 64u
#endif

#define PCI_VENDOR_NONE 0xFFFFu

/* Config-space offsets (first 256 bytes, Mechanism #1) */
#define PCI_OFF_VENDOR_ID   0x00u
#define PCI_OFF_DEVICE_ID   0x02u
#define PCI_OFF_COMMAND     0x04u
#define PCI_OFF_STATUS      0x06u
#define PCI_OFF_REVISION    0x08u
#define PCI_OFF_PROG_IF     0x09u
#define PCI_OFF_SUBCLASS    0x0Au
#define PCI_OFF_CLASS       0x0Bu
#define PCI_OFF_HEADER_TYPE 0x0Eu
#define PCI_OFF_BAR0        0x10u
#define PCI_OFF_SUBSYS_VEND 0x2Cu
#define PCI_OFF_SUBSYS_ID   0x2Eu
#define PCI_OFF_IRQ_LINE    0x3Cu
#define PCI_OFF_IRQ_PIN     0x3Du

/* One BAR slot. Read-only decode. We never write BARs in this prototype. */
typedef struct {
    uint32_t raw;       /* raw 32-bit value read from config space */
    uint64_t addr;      /* decoded base (masked). For 64-bit, combined with upper. */
    bool     present;   /* false if raw==0 or slot unused / consumed */
    bool     is_io;     /* true=I/O space (bit0=1), false=MMIO */
    bool     is_64bit;  /* true=64-bit MMIO, occupies this + next slot */
    bool     prefetchable;
    bool     is_upper_half; /* true=this slot is upper half of prev 64-bit BAR */
} pci_bar_t;

/* Device record: enough for later drivers to bind to. */
typedef struct {
    uint8_t  bus;
    uint8_t  device;    /* slot 0..31 */
    uint8_t  function;  /* 0..7 */
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type; /* raw byte 0x0E, bit7 = multifunction */
    uint8_t  irq_line;
    uint8_t  irq_pin;
    uint16_t subsys_vendor; /* valid for header 0x00, else 0 */
    uint16_t subsys_id;
    pci_bar_t bars[6];
} pci_device_t;

/* Abstracted output: void my_puts(const char *s) { terminal_write(s); } */
typedef void (*pci_puts_fn_t)(const char *s);

/* Lifecycle */
void pci_init(void);   /* clear list */
void pci_scan(void);   /* brute-force enumerate, safe to call again */

/* Accessors */
size_t pci_get_device_count(void);
const pci_device_t *pci_get_device(size_t index); /* NULL if out of range */
const pci_device_t *pci_find_by_id(uint16_t vendor, uint16_t device);
const pci_device_t *pci_find_by_class(uint8_t class_code, uint8_t subclass);
size_t pci_overflow_count(void); /* >0 means list truncated, raise PCI_MAX_DEVICES */
bool   pci_was_scanned(void);

/* Raw config-space reads (read-only prototype, no write API on purpose) */
uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint8_t  pci_config_read8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_config_write16(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint16_t value);

/* Helpers */
const char *pci_vendor_name(uint16_t vendor_id); /* "Intel", else "Unknown" */
const char *pci_class_name(uint8_t class_code);
const char *pci_subclass_name(uint8_t class_code, uint8_t subclass);

/* Diagnostics through abstracted output. puts==NULL = no-op. */
void pci_dump_devices(pci_puts_fn_t puts);

/* Optional QEMU / bring-up smoke test. Prints via puts, returns true=PASS. */
bool pci_self_test(pci_puts_fn_t puts);

#endif /* ROK_PCI_H */
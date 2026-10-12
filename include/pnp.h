#ifndef ROCKOS_PNP_H
#define ROCKOS_PNP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pci.h"

/* Wildcards for driver match tables. */
#define PNP_ANY_ID    0xFFFFu
#define PNP_ANY_CLASS 0xFFu

typedef bool (*pnp_probe_fn_t)(const pci_device_t* device);

typedef struct {
    const char* name;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    pnp_probe_fn_t probe;
} pnp_driver_t;

typedef struct {
    const pci_device_t* device;
    const pnp_driver_t* driver;
    bool probe_attempted;
    bool bound;
} pnp_binding_t;

/* PCI Plug and Play v1: static storage, no heap, boot-time binding. */
void pnp_init(void);
bool pnp_register_driver(const pnp_driver_t* driver);
size_t pnp_bind_devices(void);
size_t pnp_get_driver_count(void);
size_t pnp_get_bound_count(void);
size_t pnp_get_binding_count(void);
const pnp_binding_t* pnp_get_binding(size_t index);
const pnp_driver_t* pnp_get_driver(size_t index);
bool pnp_device_matches(const pci_device_t* device, const pnp_driver_t* driver);

#endif

#include "pnp.h"

#define PNP_MAX_DRIVERS 32u
#define PNP_MAX_BINDINGS PCI_MAX_DEVICES

static const pnp_driver_t* registered_drivers[PNP_MAX_DRIVERS];
static pnp_binding_t bindings[PNP_MAX_BINDINGS];
static size_t driver_count;
static size_t binding_count;
static size_t bound_count;

void pnp_init(void) {
    driver_count = 0;
    binding_count = 0;
    bound_count = 0;
    for (size_t i = 0; i < PNP_MAX_BINDINGS; i++) {
        bindings[i].device = NULL;
        bindings[i].driver = NULL;
        bindings[i].probe_attempted = false;
        bindings[i].bound = false;
    }
}

bool pnp_register_driver(const pnp_driver_t* driver) {
    if (!driver || !driver->name || !driver->probe ||
        driver_count >= PNP_MAX_DRIVERS) {
        return false;
    }

    for (size_t i = 0; i < driver_count; i++) {
        if (registered_drivers[i] == driver) {
            return true;
        }
    }

    registered_drivers[driver_count++] = driver;
    return true;
}

bool pnp_device_matches(const pci_device_t* device, const pnp_driver_t* driver) {
    if (!device || !driver) {
        return false;
    }

    if (driver->vendor_id != PNP_ANY_ID &&
        device->vendor_id != driver->vendor_id) {
        return false;
    }
    if (driver->device_id != PNP_ANY_ID &&
        device->device_id != driver->device_id) {
        return false;
    }
    if (driver->class_code != PNP_ANY_CLASS &&
        device->class_code != driver->class_code) {
        return false;
    }
    if (driver->subclass != PNP_ANY_CLASS &&
        device->subclass != driver->subclass) {
        return false;
    }
    if (driver->prog_if != PNP_ANY_CLASS &&
        device->prog_if != driver->prog_if) {
        return false;
    }
    return true;
}

size_t pnp_bind_devices(void) {
    binding_count = 0;
    bound_count = 0;

    if (!pci_was_scanned()) {
        return 0;
    }

    size_t device_count = pci_get_device_count();
    if (device_count > PNP_MAX_BINDINGS) {
        device_count = PNP_MAX_BINDINGS;
    }

    for (size_t i = 0; i < device_count; i++) {
        const pci_device_t* device = pci_get_device(i);
        pnp_binding_t* binding = &bindings[binding_count++];
        binding->device = device;
        binding->driver = NULL;
        binding->probe_attempted = false;
        binding->bound = false;

        for (size_t d = 0; d < driver_count; d++) {
            const pnp_driver_t* driver = registered_drivers[d];
            if (!pnp_device_matches(device, driver)) {
                continue;
            }

            binding->driver = driver;
            binding->probe_attempted = true;
            if (driver->probe(device)) {
                binding->bound = true;
                bound_count++;
                break;
            }

            /* A matching driver can decline the device; try later matches. */
            binding->driver = NULL;
            binding->probe_attempted = false;
        }
    }

    return bound_count;
}

size_t pnp_get_driver_count(void) {
    return driver_count;
}

size_t pnp_get_bound_count(void) {
    return bound_count;
}

size_t pnp_get_binding_count(void) {
    return binding_count;
}

const pnp_binding_t* pnp_get_binding(size_t index) {
    if (index >= binding_count) {
        return NULL;
    }
    return &bindings[index];
}

const pnp_driver_t* pnp_get_driver(size_t index) {
    if (index >= driver_count) {
        return NULL;
    }
    return registered_drivers[index];
}

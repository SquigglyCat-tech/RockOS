/*
 * network.c - network subsystem for the RockOS setup (see network.h).
 */

#include "network.h"

#include "filesystem.h"

#include <stddef.h>

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    const char* name;
} network_nic_table_entry_t;

/* PCI IDs of NICs the setup recognizes (stage 1 of the network plan).
 * Extend the table (and name) when drivers land; unknown adapters still
 * count as NO_ADAPTER until their driver can bind them. */
static const network_nic_table_entry_t network_nic_table[] = {
    {0x10EC, 0x8139, "Realtek RTL8139"},
    {0x10EC, 0x8136, "Realtek RTL8101E"},
    {0x10EC, 0x8168, "Realtek RTL8168 (gigabit)"},
    {0x8086, 0x100E, "Intel e1000 (82540EM)"},
    {0x8086, 0x100F, "Intel e1000 (82545EM)"},
    {0x8086, 0x10EA, "Intel e1000 (82574L)"},
    {0x1AF4, 0x1000, "virtio-net (legacy)"},
    {0x1AF4, 0x1041, "virtio-net (modern)"}
};

static network_status_t network_current_status;
static const char* network_adapter_name;
static uint16_t network_vendor_id;
static uint16_t network_device_id;
static uint8_t network_bus;
static uint8_t network_slot;
static uint8_t network_function;
static bool network_transport_attached;

void network_transport_attach(void) {
    network_transport_attached = true;
    /* Refresh the status right away so a runtime-attached transport is
     * visible on the next probe without shell involvement. */
    (void)network_probe();
}

network_status_t network_probe(void) {
    if (filesystem_is_mounted() != 1) {
        /* A mounted RockFS disk is mandatory: the setup stages its
         * downloads and the offline rollback mirror there. */
        network_current_status = NETWORK_STATUS_NO_MOUNT;
        network_adapter_name = 0;
        return network_current_status;
    }
    if (!pci_was_scanned()) {
        pci_init();
        pci_scan();
    }
    for (size_t index = 0; index < sizeof(network_nic_table) /
            sizeof(network_nic_table[0]); index++) {
        const network_nic_table_entry_t* entry =
            &network_nic_table[index];
        const pci_device_t* found =
            pci_find_by_id(entry->vendor_id, entry->device_id);
        if (!found) {
            continue;
        }
        network_vendor_id = entry->vendor_id;
        network_device_id = entry->device_id;
        network_adapter_name = entry->name;
        network_bus = found->bus;
        network_slot = found->device;
        network_function = found->function;
        if (network_transport_attached) {
            network_current_status = NETWORK_STATUS_READY;
        } else {
            network_current_status = NETWORK_STATUS_ADAPTER_FOUND;
        }
        return network_current_status;
    }
    network_current_status = NETWORK_STATUS_NO_ADAPTER;
    network_adapter_name = 0;
    return network_current_status;
}

network_status_t network_describe(network_info_t* info) {
    if (!info) {
        return network_current_status;
    }
    info->status = network_current_status;
    info->device_name = network_adapter_name;
    info->vendor_id = network_vendor_id;
    info->device_id = network_device_id;
    info->bus = network_bus;
    info->device = network_slot;
    info->function = network_function;
    return info->status;
}

bool network_is_online(void) {
    return network_current_status == NETWORK_STATUS_READY;
}

bool network_has_mount(void) {
    return filesystem_is_mounted() == 1;
}

const char* network_adapter_text(void) {
    return network_adapter_name ? network_adapter_name : "";
}

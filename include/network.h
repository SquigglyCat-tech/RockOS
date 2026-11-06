#ifndef ROCKOS_NETWORK_H
#define ROCKOS_NETWORK_H

/*
 * network.h - network subsystem for the RockOS setup.
 *
 * v1 provides real adapter detection over the PCI database and gates the
 * RockOS Setup on a usable connection. The staged design:
 *
 *   1. Adapter presence (implemented): PCI enumeration of known NICs
 *      (Realtek RTL8139, Intel e1000/virtio-net), shown on the setup's
 *      network page ("Please connect your network.").
 *   2. Transport (follow-up): NIC driver + ARP/IPv4/DHCP + HTTP range
 *      downloads. `network_transport_ready` flips to true once that
 *      module attaches; until then an adapter alone reports
 *      NETWORK_ADAPTER_NO_TRANSPORT and setup stays on the connect page.
 *
 * Setup downloads and rollback:
 *   The network page must pass before any disk write happens, and the
 *   installer journals the sectors it is about to overwrite (full
 *   offline backup when physical memory allows, boot-record-only
 *   otherwise; see shell_installer_install). A failed download or a
 *   failed write triggers a rollback pass that restores the journal;
 *   the disk is left exactly as the setup found it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pci.h"

typedef enum {
    /* No supported NIC found on the PCI bus. */
    NETWORK_STATUS_NO_ADAPTER = 0,
    /* Supported NIC found but its driver/transport is not attached yet. */
    NETWORK_STATUS_ADAPTER_FOUND = 1,
    /* Adapter + transport up and usable for setup downloads. */
    NETWORK_STATUS_READY = 2,
    /* PC-grade "no mounted system" case: the download cache and the
     * offline rollback mirror live on a mounted RockFS disk. */
    NETWORK_STATUS_NO_MOUNT = 3
} network_status_t;

typedef struct {
    network_status_t status;
    const char* device_name;   /* human-readable adapter name or NULL */
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} network_info_t;

/* Attach the future TCP download module: setup then treats the adapter
 * as fully usable (NETWORK_STATUS_READY). No-op until a transport is
 * linked in. */
void network_transport_attach(void);

/* Re-enumerate the PCI bus for known adapters. Safe to call repeatedly. */
network_status_t network_probe(void);

/* Grid the current adapter into `info`. Returns the same status code. */
network_status_t network_describe(network_info_t* info);

/* True only with NETWORK_STATUS_READY - gates the setup's Next button. */
bool network_is_online(void);

/* True when a filesystem is mounted (required even with Skip). */
bool network_has_mount(void);

/* Short, printable adapter description ("" when none). */
const char* network_adapter_text(void);

#endif /* ROCKOS_NETWORK_H */

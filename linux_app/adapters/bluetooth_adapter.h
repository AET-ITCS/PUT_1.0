#ifndef BLUETOOTH_ADAPTER_H
#define BLUETOOTH_ADAPTER_H

#include "physical_interface_adapter.h"
#include "status_collector.h"
#include <stdbool.h>
#include <stdint.h>

/* Bluetooth status structure */
typedef struct {
    bool enabled;
    bool listening;
    bool connected;
    bool stopped;
    uint8_t rfcomm_channel;
    char status_dir[256];
    uint64_t rx_count;
    uint64_t tx_count;
    uint64_t rx_bytes;
    uint64_t error_count;
    uint64_t parse_error_count;
    uint64_t shm_alloc_fail;
    uint64_t ipc_error_count;
    uint64_t consecutive_error_count;
    uint64_t started_at_ms;
    uint64_t updated_at_ms;
    uint64_t last_seen_ms;
    uint64_t last_tx_ms;
    char connected_client_addr[128];
    char last_error_stage[128];
    char last_error_message[128];
} bluetooth_status_t;

/* Exported adapter instance */
extern physical_interface_adapter_t bluetooth_adapter;

/* Service API */
int bluetooth_server_start(status_collector_t *collector);
void bluetooth_server_stop(void);

#endif // BLUETOOTH_ADAPTER_H

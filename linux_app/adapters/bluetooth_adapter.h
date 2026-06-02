#ifndef BLUETOOTH_ADAPTER_H
#define BLUETOOTH_ADAPTER_H

#include "physical_interface_adapter.h"
#include "status_collector.h"
#include "shared_memory_ipc.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
    uint8_t channel;
} bluetooth_config_t;

/* Exported adapter instance */
extern physical_interface_adapter_t bluetooth_adapter;

/* Service API */
int bluetooth_server_start(const bluetooth_config_t *config);
void bluetooth_server_stop(void);

#endif // BLUETOOTH_ADAPTER_H

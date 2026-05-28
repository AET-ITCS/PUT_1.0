#ifndef EGRESS_WORKER_H
#define EGRESS_WORKER_H

#include <stdbool.h>
#include <stdint.h>

#include "linux_shm_ipc.h"
#include "physical_interface_adapter.h"
#include "status_collector.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EGRESS_WORKER_DEFAULT_BUDGET 16u

typedef struct {
    put_shm_interface_t interface_id;
    status_module_id_t status_module;
    physical_interface_adapter_t *adapter;
    void *adapter_ctx;
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
    bool enabled;
    uint32_t budget;
} egress_worker_context_t;

typedef struct {
    uint32_t dequeued;
    uint32_t tx_ok;
    uint32_t tx_failed;
    uint32_t validation_failed;
    uint32_t ipc_errors;
    uint32_t release_errors;
} egress_worker_drain_result_t;

unified_error_t egress_worker_drain_once(egress_worker_context_t *ctx,
                                         egress_worker_drain_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif /* EGRESS_WORKER_H */

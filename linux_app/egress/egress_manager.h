#ifndef EGRESS_MANAGER_H
#define EGRESS_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include <pthread.h>

#include "egress_worker.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EGRESS_MANAGER_DEFAULT_PERIODIC_DRAIN_MS 100u

typedef struct {
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
    physical_interface_adapter_t *adapters[PUT_SHM_INTERFACE_COUNT];
    void *adapter_contexts[PUT_SHM_INTERFACE_COUNT];
    bool enabled[PUT_SHM_INTERFACE_COUNT];
    uint32_t worker_budget;
    uint32_t periodic_drain_ms;
} egress_manager_config_t;

typedef struct {
    egress_worker_context_t context;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool thread_started;
    bool stop_requested;
    bool wake_requested;
} egress_worker_thread_t;

typedef struct {
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t periodic_drain_ms;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool thread_started;
    bool stop_requested;
    egress_worker_thread_t workers[PUT_SHM_INTERFACE_COUNT];
} egress_manager_t;

void egress_manager_init(egress_manager_t *manager);
unified_error_t egress_manager_start(egress_manager_t *manager,
                                     const egress_manager_config_t *config);
void egress_manager_stop(egress_manager_t *manager);
void egress_manager_destroy(egress_manager_t *manager);
unified_error_t egress_manager_drain_reclaim_once(linux_shm_ipc_t *ipc,
                                                  status_collector_t *collector,
                                                  uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* EGRESS_MANAGER_H */

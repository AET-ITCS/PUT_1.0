#define _POSIX_C_SOURCE 200809L

#include "egress_manager.h"

#include <errno.h>
#include <string.h>
#include <time.h>

static status_module_id_t status_module_from_interface(put_shm_interface_t interface_id)
{
    switch (interface_id) {
    case PUT_SHM_INTERFACE_CAN:
        return STATUS_MODULE_CAN;
    case PUT_SHM_INTERFACE_ETHERNET:
        return STATUS_MODULE_ETHERNET;
    case PUT_SHM_INTERFACE_WIFI:
        return STATUS_MODULE_WIFI;
    case PUT_SHM_INTERFACE_BLUETOOTH:
        return STATUS_MODULE_BLUETOOTH;
    case PUT_SHM_INTERFACE_4G:
        return STATUS_MODULE_4G;
    case PUT_SHM_INTERFACE_RS485:
        return STATUS_MODULE_RS485;
    default:
        return STATUS_MODULE_CAN;
    }
}

static void record_reclaim_error(status_collector_t *collector, unified_error_t err)
{
    if (collector != 0) {
        status_collector_record_error(collector,
                                      STATUS_MODULE_ETHERNET,
                                      "egress_ipc_reclaim",
                                      err);
    }
}

static void make_abs_timeout(struct timespec *timeout, uint32_t delay_ms)
{
    if (clock_gettime(CLOCK_REALTIME, timeout) != 0) {
        timeout->tv_sec = 0;
        timeout->tv_nsec = 0;
        return;
    }

    timeout->tv_sec += (time_t)(delay_ms / 1000u);
    timeout->tv_nsec += (long)((delay_ms % 1000u) * 1000000u);
    if (timeout->tv_nsec >= 1000000000L) {
        timeout->tv_sec++;
        timeout->tv_nsec -= 1000000000L;
    }
}

unified_error_t egress_manager_drain_reclaim_once(linux_shm_ipc_t *ipc,
                                                  status_collector_t *collector,
                                                  uint32_t *out_count)
{
    uint32_t count;

    if (ipc == 0) {
        return UNIFIED_ERR_NULL;
    }

    count = 0u;
    for (;;) {
        put_shm_reclaim_descriptor_t descriptor;
        unified_error_t err;

        memset(&descriptor, 0, sizeof(descriptor));
        err = linux_shm_dequeue_reclaim_descriptor(ipc, &descriptor);
        if (err == UNIFIED_ERR_IPC_QUEUE_EMPTY) {
            break;
        }
        if (err != UNIFIED_OK) {
            record_reclaim_error(collector, err);
            return err;
        }
        count++;
    }

    if (out_count != 0) {
        *out_count = count;
    }
    return UNIFIED_OK;
}

static void wake_worker(egress_worker_thread_t *worker)
{
    if (worker == 0) {
        return;
    }

    (void)pthread_mutex_lock(&worker->mutex);
    worker->wake_requested = true;
    (void)pthread_cond_signal(&worker->cond);
    (void)pthread_mutex_unlock(&worker->mutex);
}

static void *egress_worker_thread_main(void *arg)
{
    egress_worker_thread_t *worker;

    worker = (egress_worker_thread_t *)arg;
    for (;;) {
        bool stop_requested;

        (void)pthread_mutex_lock(&worker->mutex);
        while (!worker->wake_requested && !worker->stop_requested) {
            (void)pthread_cond_wait(&worker->cond, &worker->mutex);
        }
        stop_requested = worker->stop_requested;
        worker->wake_requested = false;
        (void)pthread_mutex_unlock(&worker->mutex);

        if (stop_requested) {
            break;
        }

        (void)egress_worker_drain_once(&worker->context, 0);
    }

    return 0;
}

static void wake_pending_workers(egress_manager_t *manager, uint32_t pending_bits)
{
    if (manager == 0) {
        return;
    }

    for (uint32_t i = 0u; i < PUT_SHM_INTERFACE_COUNT; ++i) {
        if ((pending_bits & (uint32_t)(1u << i)) != 0u) {
            wake_worker(&manager->workers[i]);
        }
    }
}

static void refresh_ipc_status(egress_manager_t *manager)
{
    linux_shm_ipc_stats_t stats;

    if ((manager == 0) || (manager->ipc == 0) || (manager->collector == 0)) {
        return;
    }

    linux_shm_ipc_get_stats(manager->ipc, &stats);
    status_collector_update_ipc_stats(manager->collector, &stats, false, 0u);
}

static void *egress_manager_thread_main(void *arg)
{
    egress_manager_t *manager;

    manager = (egress_manager_t *)arg;
    for (;;) {
        linux_shm_ipc_stats_t stats;
        struct timespec timeout;
        bool stop_requested;

        memset(&stats, 0, sizeof(stats));
        linux_shm_ipc_get_stats(manager->ipc, &stats);
        wake_pending_workers(manager, stats.tx_pending_bits);
        (void)egress_manager_drain_reclaim_once(manager->ipc, manager->collector, 0);
        linux_shm_ipc_record_periodic_drain(manager->ipc);
        refresh_ipc_status(manager);

        make_abs_timeout(&timeout, manager->periodic_drain_ms);
        (void)pthread_mutex_lock(&manager->mutex);
        if (!manager->stop_requested) {
            (void)pthread_cond_timedwait(&manager->cond, &manager->mutex, &timeout);
        }
        stop_requested = manager->stop_requested;
        (void)pthread_mutex_unlock(&manager->mutex);
        if (stop_requested) {
            break;
        }
    }

    return 0;
}

void egress_manager_init(egress_manager_t *manager)
{
    if (manager == 0) {
        return;
    }

    memset(manager, 0, sizeof(*manager));
    manager->periodic_drain_ms = EGRESS_MANAGER_DEFAULT_PERIODIC_DRAIN_MS;
    (void)pthread_mutex_init(&manager->mutex, 0);
    (void)pthread_cond_init(&manager->cond, 0);
    for (uint32_t i = 0u; i < PUT_SHM_INTERFACE_COUNT; ++i) {
        (void)pthread_mutex_init(&manager->workers[i].mutex, 0);
        (void)pthread_cond_init(&manager->workers[i].cond, 0);
    }
}

unified_error_t egress_manager_start(egress_manager_t *manager,
                                     const egress_manager_config_t *config)
{
    uint32_t budget;

    if ((manager == 0) || (config == 0) || (config->ipc == 0)) {
        return UNIFIED_ERR_NULL;
    }

    manager->ipc = config->ipc;
    manager->collector = config->collector;
    manager->periodic_drain_ms = (config->periodic_drain_ms == 0u) ?
        EGRESS_MANAGER_DEFAULT_PERIODIC_DRAIN_MS : config->periodic_drain_ms;
    manager->stop_requested = false;
    budget = (config->worker_budget == 0u) ?
        EGRESS_WORKER_DEFAULT_BUDGET : config->worker_budget;

    for (uint32_t i = 0u; i < PUT_SHM_INTERFACE_COUNT; ++i) {
        egress_worker_thread_t *worker;

        worker = &manager->workers[i];
        worker->context.interface_id = (put_shm_interface_t)i;
        worker->context.status_module = status_module_from_interface((put_shm_interface_t)i);
        worker->context.adapter = config->adapters[i];
        worker->context.adapter_ctx = config->adapter_contexts[i];
        worker->context.ipc = config->ipc;
        worker->context.collector = config->collector;
        worker->context.linux_epoch = config->linux_epoch;
        worker->context.enabled = config->enabled[i];
        worker->context.budget = budget;
        worker->stop_requested = false;
        worker->wake_requested = false;
        if (pthread_create(&worker->thread, 0, egress_worker_thread_main, worker) != 0) {
            egress_manager_stop(manager);
            return UNIFIED_ERR_IPC_NOT_READY;
        }
        worker->thread_started = true;
    }

    if (pthread_create(&manager->thread, 0, egress_manager_thread_main, manager) != 0) {
        egress_manager_stop(manager);
        return UNIFIED_ERR_IPC_NOT_READY;
    }
    manager->thread_started = true;
    return UNIFIED_OK;
}

void egress_manager_stop(egress_manager_t *manager)
{
    if (manager == 0) {
        return;
    }

    (void)pthread_mutex_lock(&manager->mutex);
    manager->stop_requested = true;
    (void)pthread_cond_signal(&manager->cond);
    (void)pthread_mutex_unlock(&manager->mutex);

    for (uint32_t i = 0u; i < PUT_SHM_INTERFACE_COUNT; ++i) {
        egress_worker_thread_t *worker;

        worker = &manager->workers[i];
        (void)pthread_mutex_lock(&worker->mutex);
        worker->stop_requested = true;
        worker->wake_requested = true;
        (void)pthread_cond_signal(&worker->cond);
        (void)pthread_mutex_unlock(&worker->mutex);
    }

    if (manager->thread_started) {
        (void)pthread_join(manager->thread, 0);
        manager->thread_started = false;
    }

    for (uint32_t i = 0u; i < PUT_SHM_INTERFACE_COUNT; ++i) {
        if (manager->workers[i].thread_started) {
            (void)pthread_join(manager->workers[i].thread, 0);
            manager->workers[i].thread_started = false;
        }
    }

    refresh_ipc_status(manager);
}

void egress_manager_destroy(egress_manager_t *manager)
{
    if (manager == 0) {
        return;
    }

    egress_manager_stop(manager);
    for (uint32_t i = 0u; i < PUT_SHM_INTERFACE_COUNT; ++i) {
        (void)pthread_cond_destroy(&manager->workers[i].cond);
        (void)pthread_mutex_destroy(&manager->workers[i].mutex);
    }
    (void)pthread_cond_destroy(&manager->cond);
    (void)pthread_mutex_destroy(&manager->mutex);
}

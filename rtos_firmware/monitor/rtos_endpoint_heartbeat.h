/**
 * @file rtos_endpoint_heartbeat.h
 * @brief P1 endpoint heartbeat table used by the router host tests.
 * @author Yukikaze
 */
#ifndef RTOS_ENDPOINT_HEARTBEAT_H
#define RTOS_ENDPOINT_HEARTBEAT_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_firmware_config.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Endpoint heartbeat state.
 */
typedef enum {
    RTOS_ENDPOINT_HEARTBEAT_STATE_EMPTY = 0,
    RTOS_ENDPOINT_HEARTBEAT_STATE_ONLINE = 1,
} rtos_endpoint_heartbeat_state_t;

/**
 * @brief One endpoint heartbeat entry.
 */
typedef struct {
    bool in_use;                                  /**< Entry is allocated. */
    uint8_t source_cid[ANYMSG_CID_LENGTH];       /**< Endpoint source CID. */
    put_shm_interface_t last_rx_interface;       /**< Interface of the last heartbeat. */
    uint32_t last_rtos_time_ms;                  /**< RTOS time when heartbeat arrived. */
    uint32_t last_frame_local_time;              /**< anyMSG local_time snapshot. */
    uint32_t rx_count;                           /**< Consumed heartbeat count. */
    rtos_endpoint_heartbeat_state_t state;       /**< P1 heartbeat state. */
} rtos_endpoint_heartbeat_entry_t;

/**
 * @brief Read-only heartbeat snapshot.
 */
typedef struct {
    bool gateway_configured;                     /**< Gateway CID is configured. */
    uint8_t gateway_cid[ANYMSG_CID_LENGTH];      /**< Configured gateway CID. */
    uint32_t entry_count;                        /**< Number of allocated entries. */
    rtos_endpoint_heartbeat_entry_t entries[RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY];
} rtos_endpoint_heartbeat_snapshot_t;

/**
 * @brief Endpoint heartbeat table context.
 */
typedef struct {
    bool gateway_configured;
    uint8_t gateway_cid[ANYMSG_CID_LENGTH];
    rtos_endpoint_heartbeat_entry_t entries[RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY];
} rtos_endpoint_heartbeat_table_t;

/**
 * @brief Initialize an endpoint heartbeat table.
 */
void rtos_endpoint_heartbeat_init(rtos_endpoint_heartbeat_table_t *table);

/**
 * @brief Configure the gateway CID required for P1 heartbeat table updates.
 */
unified_error_t rtos_endpoint_heartbeat_set_gateway(
    rtos_endpoint_heartbeat_table_t *table,
    const uint8_t gateway_cid[ANYMSG_CID_LENGTH]);

/**
 * @brief Clear the configured gateway CID.
 */
void rtos_endpoint_heartbeat_clear_gateway(rtos_endpoint_heartbeat_table_t *table);

/**
 * @brief Consume one endpoint-to-gateway heartbeat.
 */
unified_error_t rtos_endpoint_heartbeat_consume(
    rtos_endpoint_heartbeat_table_t *table,
    const uint8_t source_cid[ANYMSG_CID_LENGTH],
    const uint8_t destination_cid[ANYMSG_CID_LENGTH],
    put_shm_interface_t source_interface,
    uint32_t now_ms,
    uint32_t frame_local_time);

/**
 * @brief Read heartbeat table snapshot.
 */
void rtos_endpoint_heartbeat_get_snapshot(
    const rtos_endpoint_heartbeat_table_t *table,
    rtos_endpoint_heartbeat_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_ENDPOINT_HEARTBEAT_H */

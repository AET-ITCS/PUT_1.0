/**
 * @file rtos_endpoint_heartbeat.c
 * @brief P1 endpoint heartbeat table implementation.
 * @author Yukikaze
 */
#include "rtos_endpoint_heartbeat.h"

#include <string.h>

/**
 * @brief Return true when two raw CIDs are identical.
 */
static bool cid_equals(const uint8_t left[ANYMSG_CID_LENGTH],
                       const uint8_t right[ANYMSG_CID_LENGTH])
{
    if ((left == 0) || (right == 0)) {
        return false;
    }

    return memcmp(left, right, ANYMSG_CID_LENGTH) == 0;
}

/**
 * @brief Find an entry by source CID.
 */
static rtos_endpoint_heartbeat_entry_t *find_entry(
    rtos_endpoint_heartbeat_table_t *table,
    const uint8_t source_cid[ANYMSG_CID_LENGTH])
{
    uint32_t i; /**< Entry scan index. */

    if ((table == 0) || (source_cid == 0)) {
        return 0;
    }

    for (i = 0u; i < RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY; ++i) {
        if (table->entries[i].in_use && cid_equals(table->entries[i].source_cid, source_cid)) {
            return &table->entries[i];
        }
    }

    return 0;
}

/**
 * @brief Allocate a free heartbeat entry.
 */
static rtos_endpoint_heartbeat_entry_t *allocate_entry(
    rtos_endpoint_heartbeat_table_t *table,
    const uint8_t source_cid[ANYMSG_CID_LENGTH])
{
    uint32_t i; /**< Entry scan index. */

    if ((table == 0) || (source_cid == 0)) {
        return 0;
    }

    for (i = 0u; i < RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY; ++i) {
        if (!table->entries[i].in_use) {
            table->entries[i].in_use = true;
            (void)memcpy(table->entries[i].source_cid, source_cid, ANYMSG_CID_LENGTH);
            return &table->entries[i];
        }
    }

    return 0;
}

void rtos_endpoint_heartbeat_init(rtos_endpoint_heartbeat_table_t *table)
{
    if (table != 0) {
        (void)memset(table, 0, sizeof(*table));
    }
}

unified_error_t rtos_endpoint_heartbeat_set_gateway(
    rtos_endpoint_heartbeat_table_t *table,
    const uint8_t gateway_cid[ANYMSG_CID_LENGTH])
{
    if ((table == 0) || (gateway_cid == 0)) {
        return UNIFIED_ERR_NULL;
    }

    (void)memcpy(table->gateway_cid, gateway_cid, ANYMSG_CID_LENGTH);
    table->gateway_configured = true;
    return UNIFIED_OK;
}

void rtos_endpoint_heartbeat_clear_gateway(rtos_endpoint_heartbeat_table_t *table)
{
    if (table != 0) {
        table->gateway_configured = false;
        (void)memset(table->gateway_cid, 0, sizeof(table->gateway_cid));
    }
}

unified_error_t rtos_endpoint_heartbeat_consume(
    rtos_endpoint_heartbeat_table_t *table,
    const uint8_t source_cid[ANYMSG_CID_LENGTH],
    const uint8_t destination_cid[ANYMSG_CID_LENGTH],
    put_shm_interface_t source_interface,
    uint32_t now_ms,
    uint32_t frame_local_time)
{
    rtos_endpoint_heartbeat_entry_t *entry; /**< Entry being updated. */

    if ((table == 0) || (source_cid == 0) || (destination_cid == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (!table->gateway_configured) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if (!cid_equals(table->gateway_cid, destination_cid)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    entry = find_entry(table, source_cid);
    if (entry == 0) {
        entry = allocate_entry(table, source_cid);
    }

    if (entry == 0) {
        return UNIFIED_ERR_IPC_QUEUE_FULL;
    }

    entry->last_rx_interface = source_interface;
    entry->last_rtos_time_ms = now_ms;
    entry->last_frame_local_time = frame_local_time;
    entry->rx_count = entry->rx_count + 1u;
    entry->state = RTOS_ENDPOINT_HEARTBEAT_STATE_ONLINE;

    return UNIFIED_OK;
}

void rtos_endpoint_heartbeat_get_snapshot(
    const rtos_endpoint_heartbeat_table_t *table,
    rtos_endpoint_heartbeat_snapshot_t *out_snapshot)
{
    uint32_t i; /**< Entry scan index. */

    if (out_snapshot == 0) {
        return;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    if (table == 0) {
        return;
    }

    out_snapshot->gateway_configured = table->gateway_configured;
    (void)memcpy(out_snapshot->gateway_cid, table->gateway_cid, ANYMSG_CID_LENGTH);
    for (i = 0u; i < RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY; ++i) {
        out_snapshot->entries[i] = table->entries[i];
        if (table->entries[i].in_use) {
            out_snapshot->entry_count = out_snapshot->entry_count + 1u;
        }
    }
}

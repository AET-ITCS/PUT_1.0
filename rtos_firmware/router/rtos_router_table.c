/**
 * @file rtos_router_table.c
 * @brief P1 fixed CID route table.
 * @author Yukikaze
 */
#include "rtos_router.h"

#include <string.h>

/**
 * @brief Return true when an interface value is one of the six ABI interfaces.
 */
static bool interface_is_valid(put_shm_interface_t interface_id)
{
    return (uint8_t)interface_id < (uint8_t)PUT_SHM_INTERFACE_COUNT;
}

void rtos_router_route_table_default(rtos_route_table_snapshot_t *out_table)
{
    if (out_table == 0) {
        return;
    }

    (void)memset(out_table, 0, sizeof(*out_table));
    out_table->route_version = RTOS_FIRMWARE_ROUTE_TABLE_VERSION;
    out_table->active_route_epoch = RTOS_FIRMWARE_ROUTE_EPOCH;
    out_table->valid = true;
    out_table->cid_segment_targets[ANYMSG_CID_SEGMENT_CAN] = PUT_SHM_INTERFACE_CAN;
    out_table->cid_segment_targets[ANYMSG_CID_SEGMENT_ETHERNET] = PUT_SHM_INTERFACE_ETHERNET;
    out_table->cid_segment_targets[ANYMSG_CID_SEGMENT_WIFI] = PUT_SHM_INTERFACE_WIFI;
    out_table->cid_segment_targets[ANYMSG_CID_SEGMENT_BLUETOOTH] = PUT_SHM_INTERFACE_BLUETOOTH;
    out_table->cid_segment_targets[ANYMSG_CID_SEGMENT_4G] = PUT_SHM_INTERFACE_4G;
    out_table->cid_segment_targets[ANYMSG_CID_SEGMENT_RS485] = PUT_SHM_INTERFACE_RS485;
}

unified_error_t rtos_router_route_table_lookup(
    const rtos_route_table_snapshot_t *table,
    uint8_t destination_cid_first_byte,
    put_shm_interface_t *out_interface)
{
    anymsg_cid_segment_t segment;       /**< CID segment parsed from first byte. */
    put_shm_interface_t target_interface; /**< Configured route target. */

    if ((table == 0) || (out_interface == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (!table->valid) {
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    segment = anymsg_cid_segment_from_first_byte(destination_cid_first_byte);
    if ((segment == ANYMSG_CID_SEGMENT_RESERVED_LOW) ||
        (segment == ANYMSG_CID_SEGMENT_RESERVED_HIGH)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    target_interface = table->cid_segment_targets[segment];
    if (!interface_is_valid(target_interface)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    *out_interface = target_interface;
    return UNIFIED_OK;
}

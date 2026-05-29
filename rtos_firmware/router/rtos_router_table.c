/**
 * @file rtos_router_table.c
 * @brief P1 固定 CID 路由表实现。
 * @author Yukikaze
 */
#include "rtos_router.h"

#include <stddef.h>
#include <string.h>

#include "crc16.h"

/**
 * @brief 判断接口值是否属于 ABI 固定的六类接口。
 *
 * @param interface_id 待检查接口。
 * @return true 表示合法，false 表示非法。
 */
static bool interface_is_valid(put_shm_interface_t interface_id)
{
    return (uint8_t)interface_id < (uint8_t)PUT_SHM_INTERFACE_COUNT;
}

/**
 * @brief 填充 P1 默认 CID 路由表。
 *
 * @param out_table 输出路由表快照。
 */
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

/**
 * @brief 计算 route table 快照 CRC。
 *
 * @param table route table 快照。
 * @return CRC-16/CCITT-FALSE，NULL 时返回 0。
 */
uint16_t rtos_router_route_table_calculate_crc(
    const rtos_route_table_snapshot_t *table)
{
    if (table == 0) {
        return 0u;
    }

    return unified_crc16_ccitt_false((const uint8_t *)table,
                                     offsetof(rtos_route_table_snapshot_t, crc16));
}

/**
 * @brief 检查 route table CRC。
 *
 * @param table route table 快照。
 * @return true 表示 CRC 未启用或校验通过。
 */
bool rtos_router_route_table_crc_is_valid(const rtos_route_table_snapshot_t *table)
{
    if (table == 0) {
        return false;
    }

    if (table->crc16 == 0u) {
        return true;
    }

    return table->crc16 == rtos_router_route_table_calculate_crc(table);
}

/**
 * @brief 按目的 CID 首字节查找目标接口。
 *
 * @param table 路由表快照。
 * @param destination_cid_first_byte 目的 CID 首字节。
 * @param out_interface 输出目标接口。
 * @return UNIFIED_OK 表示找到路由，否则返回公共错误码。
 */
unified_error_t rtos_router_route_table_lookup(
    const rtos_route_table_snapshot_t *table,
    uint8_t destination_cid_first_byte,
    put_shm_interface_t *out_interface)
{
    anymsg_cid_segment_t segment;       /**< 从首字节解析出的 CID 段。 */
    put_shm_interface_t target_interface; /**< 路由表中配置的目标接口。 */

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

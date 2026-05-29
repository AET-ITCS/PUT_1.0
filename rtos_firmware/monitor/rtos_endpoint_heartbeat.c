/**
 * @file rtos_endpoint_heartbeat.c
 * @brief P1 端心跳表实现。
 * @author Yukikaze
 */
#include "rtos_endpoint_heartbeat.h"

#include <string.h>

/**
 * @brief 判断两个 raw CID 是否完全相同。
 *
 * @param left 左侧 CID。
 * @param right 右侧 CID。
 * @return true 表示相同，false 表示不同。
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
 * @brief 判断 source CID 是否为可维护端设备地址。
 *
 * @param source_cid source CID。
 * @return true 表示 source CID 位于 0x20..0xDF。
 */
static bool source_cid_is_valid(const uint8_t source_cid[ANYMSG_CID_LENGTH])
{
    if (source_cid == 0) {
        return false;
    }

    return (source_cid[0] >= ANYMSG_CID_CAN_MIN) &&
           (source_cid[0] <= ANYMSG_CID_RS485_MAX);
}

/**
 * @brief 按 source CID 查找心跳记录。
 *
 * @param table 端心跳表上下文。
 * @param source_cid 端设备源 CID。
 * @return 找到的记录指针，未找到时返回 NULL。
 */
static rtos_endpoint_heartbeat_entry_t *find_entry(
    rtos_endpoint_heartbeat_table_t *table,
    const uint8_t source_cid[ANYMSG_CID_LENGTH])
{
    uint32_t i; /**< 记录扫描下标。 */

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
 * @brief 分配一个空闲心跳记录。
 *
 * @param table 端心跳表上下文。
 * @param source_cid 端设备源 CID。
 * @return 新分配的记录指针，无空闲记录时返回 NULL。
 */
static rtos_endpoint_heartbeat_entry_t *allocate_entry(
    rtos_endpoint_heartbeat_table_t *table,
    const uint8_t source_cid[ANYMSG_CID_LENGTH])
{
    uint32_t i; /**< 记录扫描下标。 */

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

/**
 * @brief 初始化端心跳表。
 *
 * @param table 端心跳表上下文。
 */
void rtos_endpoint_heartbeat_init(rtos_endpoint_heartbeat_table_t *table)
{
    if (table != 0) {
        (void)memset(table, 0, sizeof(*table));
    }
}

/**
 * @brief 配置用于 P1 心跳表更新的 gateway CID。
 *
 * @param table 端心跳表上下文。
 * @param gateway_cid gateway CID。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
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

/**
 * @brief 清除已配置的 gateway CID。
 *
 * @param table 端心跳表上下文。
 */
void rtos_endpoint_heartbeat_clear_gateway(rtos_endpoint_heartbeat_table_t *table)
{
    if (table != 0) {
        table->gateway_configured = false;
        (void)memset(table->gateway_cid, 0, sizeof(table->gateway_cid));
    }
}

/**
 * @brief 消费一帧端到网关心跳。
 *
 * @param table 端心跳表上下文。
 * @param source_cid 心跳源 CID。
 * @param destination_cid 心跳目的 CID。
 * @param source_interface 心跳来源接口。
 * @param now_ms 当前 RTOS 时间，单位毫秒。
 * @param frame_local_time anyMSG local_time 快照。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
unified_error_t rtos_endpoint_heartbeat_consume(
    rtos_endpoint_heartbeat_table_t *table,
    const uint8_t source_cid[ANYMSG_CID_LENGTH],
    const uint8_t destination_cid[ANYMSG_CID_LENGTH],
    put_shm_interface_t source_interface,
    uint32_t now_ms,
    uint32_t frame_local_time)
{
    rtos_endpoint_heartbeat_entry_t *entry; /**< 当前更新的记录。 */

    if ((table == 0) || (source_cid == 0) || (destination_cid == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (!table->gateway_configured) {
        table->statistics.invalid_count = table->statistics.invalid_count + 1u;
        return UNIFIED_ERR_IPC_NOT_READY;
    }

    if (!source_cid_is_valid(source_cid)) {
        table->statistics.invalid_count = table->statistics.invalid_count + 1u;
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (!cid_equals(table->gateway_cid, destination_cid)) {
        table->statistics.invalid_count = table->statistics.invalid_count + 1u;
        return UNIFIED_ERR_INVALID_ARG;
    }

    entry = find_entry(table, source_cid);
    if (entry == 0) {
        entry = allocate_entry(table, source_cid);
    }

    if (entry == 0) {
        table->statistics.table_full_count = table->statistics.table_full_count + 1u;
        return UNIFIED_ERR_IPC_QUEUE_FULL;
    }

    if ((entry->state == RTOS_ENDPOINT_HEARTBEAT_STATE_WARN) ||
        (entry->state == RTOS_ENDPOINT_HEARTBEAT_STATE_OFFLINE)) {
        table->statistics.recover_count = table->statistics.recover_count + 1u;
    }

    entry->last_rx_interface = source_interface;
    entry->last_rtos_time_ms = now_ms;
    entry->last_frame_local_time = frame_local_time;
    entry->rx_count = entry->rx_count + 1u;
    entry->state = RTOS_ENDPOINT_HEARTBEAT_STATE_ONLINE;
    table->statistics.rx_count = table->statistics.rx_count + 1u;

    return UNIFIED_OK;
}

/**
 * @brief 周期扫描端心跳超时状态。
 *
 * @param table 端心跳表上下文。
 * @param now_ms 当前 RTOS 时间。
 * @return 本次发生状态转换的 entry 数量。
 */
uint32_t rtos_endpoint_heartbeat_scan_timeouts(
    rtos_endpoint_heartbeat_table_t *table,
    uint32_t now_ms)
{
    uint32_t i;           /**< 记录扫描下标。 */
    uint32_t transitions; /**< 本轮状态转换数。 */
    uint32_t age_ms;      /**< 距上次心跳时间。 */

    if (table == 0) {
        return 0u;
    }

    transitions = 0u;
    for (i = 0u; i < RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY; ++i) {
        if (!table->entries[i].in_use ||
            (table->entries[i].state == RTOS_ENDPOINT_HEARTBEAT_STATE_EMPTY)) {
            continue;
        }

        age_ms = now_ms - table->entries[i].last_rtos_time_ms;
        if ((age_ms >= RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_OFFLINE_MS) &&
            (table->entries[i].state != RTOS_ENDPOINT_HEARTBEAT_STATE_OFFLINE)) {
            table->entries[i].state = RTOS_ENDPOINT_HEARTBEAT_STATE_OFFLINE;
            table->entries[i].timeout_count = table->entries[i].timeout_count + 1u;
            table->statistics.timeout_count = table->statistics.timeout_count + 1u;
            transitions = transitions + 1u;
        } else if ((age_ms >= RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_WARN_MS) &&
                   (table->entries[i].state == RTOS_ENDPOINT_HEARTBEAT_STATE_ONLINE)) {
            table->entries[i].state = RTOS_ENDPOINT_HEARTBEAT_STATE_WARN;
            table->entries[i].timeout_count = table->entries[i].timeout_count + 1u;
            table->statistics.timeout_count = table->statistics.timeout_count + 1u;
            transitions = transitions + 1u;
        }
    }

    return transitions;
}

/**
 * @brief 清空端心跳 entry，保留 gateway 配置和累计统计。
 *
 * @param table 端心跳表上下文。
 */
void rtos_endpoint_heartbeat_clear_entries(rtos_endpoint_heartbeat_table_t *table)
{
    if (table != 0) {
        (void)memset(table->entries, 0, sizeof(table->entries));
    }
}

/**
 * @brief 读取端心跳统计。
 *
 * @param table 端心跳表上下文。
 * @param out_statistics 输出统计。
 */
void rtos_endpoint_heartbeat_get_statistics(
    const rtos_endpoint_heartbeat_table_t *table,
    rtos_endpoint_heartbeat_statistics_t *out_statistics)
{
    if (out_statistics == 0) {
        return;
    }

    (void)memset(out_statistics, 0, sizeof(*out_statistics));
    if (table != 0) {
        *out_statistics = table->statistics;
    }
}

/**
 * @brief 读取端心跳表快照。
 *
 * @param table 端心跳表上下文。
 * @param out_snapshot 输出心跳表快照。
 */
void rtos_endpoint_heartbeat_get_snapshot(
    const rtos_endpoint_heartbeat_table_t *table,
    rtos_endpoint_heartbeat_snapshot_t *out_snapshot)
{
    uint32_t i; /**< 记录扫描下标。 */

    if (out_snapshot == 0) {
        return;
    }

    (void)memset(out_snapshot, 0, sizeof(*out_snapshot));
    if (table == 0) {
        return;
    }

    out_snapshot->gateway_configured = table->gateway_configured;
    (void)memcpy(out_snapshot->gateway_cid, table->gateway_cid, ANYMSG_CID_LENGTH);
    out_snapshot->statistics = table->statistics;
    for (i = 0u; i < RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY; ++i) {
        out_snapshot->entries[i] = table->entries[i];
        if (table->entries[i].in_use) {
            out_snapshot->entry_count = out_snapshot->entry_count + 1u;
        }
    }
}

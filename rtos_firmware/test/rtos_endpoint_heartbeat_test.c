/**
 * @file rtos_endpoint_heartbeat_test.c
 * @brief P3 endpoint heartbeat 状态机 host 单测。
 * @author Yukikaze
 */
#include "rtos_endpoint_heartbeat.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

/**
 * @brief 构造 CID。
 *
 * @param first 首字节。
 * @param second 第二字节。
 * @param out_cid 输出 CID。
 */
static void make_cid(uint8_t first,
                     uint8_t second,
                     uint8_t out_cid[ANYMSG_CID_LENGTH])
{
    (void)memset(out_cid, 0, ANYMSG_CID_LENGTH);
    out_cid[0] = first;
    out_cid[1] = second;
}

/**
 * @brief 验证 endpoint ONLINE/WARN/OFFLINE/恢复。
 *
 * @return 0 表示通过。
 */
static int test_endpoint_state_transitions(void)
{
    rtos_endpoint_heartbeat_table_t table;       /**< 心跳表。 */
    rtos_endpoint_heartbeat_snapshot_t snapshot; /**< 快照。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH];       /**< source CID。 */
    uint8_t gateway_cid[ANYMSG_CID_LENGTH];      /**< gateway CID。 */

    rtos_endpoint_heartbeat_init(&table);
    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, gateway_cid);
    CHECK(rtos_endpoint_heartbeat_set_gateway(&table, gateway_cid) == UNIFIED_OK);
    CHECK(rtos_endpoint_heartbeat_consume(&table,
                                          source_cid,
                                          gateway_cid,
                                          PUT_SHM_INTERFACE_CAN,
                                          100u,
                                          55u) == UNIFIED_OK);

    CHECK(rtos_endpoint_heartbeat_scan_timeouts(&table, 3099u) == 0u);
    CHECK(rtos_endpoint_heartbeat_scan_timeouts(&table, 3100u) == 1u);
    rtos_endpoint_heartbeat_get_snapshot(&table, &snapshot);
    CHECK(snapshot.entries[0].state == RTOS_ENDPOINT_HEARTBEAT_STATE_WARN);
    CHECK(snapshot.entries[0].timeout_count == 1u);

    CHECK(rtos_endpoint_heartbeat_scan_timeouts(&table, 5100u) == 1u);
    rtos_endpoint_heartbeat_get_snapshot(&table, &snapshot);
    CHECK(snapshot.entries[0].state == RTOS_ENDPOINT_HEARTBEAT_STATE_OFFLINE);
    CHECK(snapshot.entries[0].timeout_count == 2u);

    CHECK(rtos_endpoint_heartbeat_consume(&table,
                                          source_cid,
                                          gateway_cid,
                                          PUT_SHM_INTERFACE_CAN,
                                          5200u,
                                          77u) == UNIFIED_OK);
    rtos_endpoint_heartbeat_get_snapshot(&table, &snapshot);
    CHECK(snapshot.entries[0].state == RTOS_ENDPOINT_HEARTBEAT_STATE_ONLINE);
    CHECK(snapshot.entries[0].rx_count == 2u);
    CHECK(snapshot.statistics.recover_count == 1u);
    CHECK(snapshot.statistics.timeout_count == 2u);

    return 0;
}

/**
 * @brief 验证非法 source、gateway 未配置和表满统计。
 *
 * @return 0 表示通过。
 */
static int test_endpoint_rejects_invalid_inputs(void)
{
    rtos_endpoint_heartbeat_table_t table;       /**< 心跳表。 */
    rtos_endpoint_heartbeat_snapshot_t snapshot; /**< 快照。 */
    uint8_t source_cid[ANYMSG_CID_LENGTH];       /**< source CID。 */
    uint8_t gateway_cid[ANYMSG_CID_LENGTH];      /**< gateway CID。 */
    uint32_t i;                                  /**< 循环下标。 */

    rtos_endpoint_heartbeat_init(&table);
    make_cid(ANYMSG_CID_CAN_MIN, 1u, source_cid);
    make_cid(ANYMSG_CID_RS485_MIN, 2u, gateway_cid);
    CHECK(rtos_endpoint_heartbeat_consume(&table,
                                          source_cid,
                                          gateway_cid,
                                          PUT_SHM_INTERFACE_CAN,
                                          1u,
                                          1u) == UNIFIED_ERR_IPC_NOT_READY);

    CHECK(rtos_endpoint_heartbeat_set_gateway(&table, gateway_cid) == UNIFIED_OK);
    make_cid(ANYMSG_CID_RESERVED_LOW_MIN, 1u, source_cid);
    CHECK(rtos_endpoint_heartbeat_consume(&table,
                                          source_cid,
                                          gateway_cid,
                                          PUT_SHM_INTERFACE_CAN,
                                          1u,
                                          1u) == UNIFIED_ERR_INVALID_ARG);

    rtos_endpoint_heartbeat_get_snapshot(&table, &snapshot);
    CHECK(snapshot.statistics.invalid_count == 2u);

    rtos_endpoint_heartbeat_init(&table);
    CHECK(rtos_endpoint_heartbeat_set_gateway(&table, gateway_cid) == UNIFIED_OK);
    for (i = 0u; i < RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY; ++i) {
        make_cid(ANYMSG_CID_CAN_MIN, (uint8_t)i, source_cid);
        CHECK(rtos_endpoint_heartbeat_consume(&table,
                                              source_cid,
                                              gateway_cid,
                                              PUT_SHM_INTERFACE_CAN,
                                              i,
                                              i) == UNIFIED_OK);
    }
    make_cid(ANYMSG_CID_ETHERNET_MIN, 1u, source_cid);
    CHECK(rtos_endpoint_heartbeat_consume(&table,
                                          source_cid,
                                          gateway_cid,
                                          PUT_SHM_INTERFACE_ETHERNET,
                                          100u,
                                          100u) == UNIFIED_ERR_IPC_QUEUE_FULL);
    rtos_endpoint_heartbeat_get_snapshot(&table, &snapshot);
    CHECK(snapshot.entry_count == RTOS_FIRMWARE_ENDPOINT_HEARTBEAT_CAPACITY);
    CHECK(snapshot.statistics.table_full_count == 1u);

    return 0;
}

int main(void)
{
    CHECK(test_endpoint_state_transitions() == 0);
    CHECK(test_endpoint_rejects_invalid_inputs() == 0);
    return 0;
}

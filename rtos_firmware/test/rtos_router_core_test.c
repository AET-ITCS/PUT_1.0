/**
 * @file rtos_router_core_test.c
 * @brief P1 路由核心主机端测试。
 * @author Yukikaze
 */
#include "rtos_router.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__,   \
                          #condition);                                                \
            return 1;                                                                 \
        }                                                                             \
    } while (0)

/**
 * @brief 模拟 sink 上下文。
 */
typedef struct {
    uint32_t now_ms;                       /**< 时间源当前值。 */
    uint32_t tx_count;                     /**< TX sink 调用次数。 */
    uint32_t reclaim_count;                /**< reclaim sink 调用次数。 */
    uint32_t tx_failures_before_success;   /**< 成功前模拟队列满的次数。 */
    bool tx_always_full;                   /**< 强制所有 TX 尝试返回队列满。 */
    rtos_route_output_t tx_outputs[128];   /**< 捕获到的 TX 输出。 */
    rtos_route_output_t reclaim_outputs[128]; /**< 捕获到的 reclaim 输出。 */
} mock_context_t;

/**
 * @brief 模拟时间源。
 *
 * @param user_context 模拟上下文。
 * @return 当前模拟时间。
 */
static uint32_t mock_time_source(void *user_context)
{
    mock_context_t *context; /**< 模拟上下文。 */

    context = (mock_context_t *)user_context;
    if (context == 0) {
        return 0u;
    }

    return context->now_ms;
}

/**
 * @brief 模拟 TX sink。
 *
 * @param output 路由输出。
 * @param user_context 模拟上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t mock_tx_sink(const rtos_route_output_t *output,
                                    void *user_context)
{
    mock_context_t *context; /**< 模拟上下文。 */

    context = (mock_context_t *)user_context;
    if ((context == 0) || (output == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (context->tx_count < 128u) {
        context->tx_outputs[context->tx_count] = *output;
    }
    context->tx_count = context->tx_count + 1u;

    if (context->tx_always_full) {
        return UNIFIED_ERR_IPC_QUEUE_FULL;
    }

    if (context->tx_failures_before_success > 0u) {
        context->tx_failures_before_success = context->tx_failures_before_success - 1u;
        return UNIFIED_ERR_IPC_QUEUE_FULL;
    }

    return UNIFIED_OK;
}

/**
 * @brief 模拟 reclaim sink。
 *
 * @param output 路由输出。
 * @param user_context 模拟上下文。
 * @return UNIFIED_OK 表示成功，否则返回公共错误码。
 */
static unified_error_t mock_reclaim_sink(const rtos_route_output_t *output,
                                         void *user_context)
{
    mock_context_t *context; /**< 模拟上下文。 */

    context = (mock_context_t *)user_context;
    if ((context == 0) || (output == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if (context->reclaim_count < 128u) {
        context->reclaim_outputs[context->reclaim_count] = *output;
    }
    context->reclaim_count = context->reclaim_count + 1u;

    return UNIFIED_OK;
}

/**
 * @brief 初始化路由器和模拟上下文。
 *
 * @param router 路由上下文。
 * @param context 模拟上下文。
 */
static void init_router(rtos_router_context_t *router, mock_context_t *context)
{
    rtos_router_sinks_t sinks; /**< 模拟 sink 回调集合。 */

    (void)memset(context, 0, sizeof(*context));
    (void)memset(&sinks, 0, sizeof(sinks));
    sinks.tx_sink = mock_tx_sink;
    sinks.reclaim_sink = mock_reclaim_sink;
    sinks.user_context = context;
    rtos_router_init(router, &sinks, mock_time_source, context);
    rtos_router_set_linux_epoch(router, 1u);
}

/**
 * @brief 构造一个合法路由输入。
 *
 * @param frame_id 帧 ID。
 * @param destination_first_byte 目的 CID 首字节。
 * @param type anyMSG type。
 * @param priority 优先级。
 * @return 构造完成的路由输入。
 */
static rtos_route_input_t make_input(uint32_t frame_id,
                                     uint8_t destination_first_byte,
                                     uint8_t type,
                                     uint8_t priority)
{
    rtos_route_input_t input; /**< 正在构造的路由输入。 */

    (void)memset(&input, 0, sizeof(input));
    input.frame_id = frame_id;
    input.source_interface = PUT_SHM_INTERFACE_CAN;
    input.source_cid[0] = ANYMSG_CID_CAN_MIN;
    input.source_cid[1] = 1u;
    input.destination_cid[0] = destination_first_byte;
    input.destination_cid[1] = 2u;
    input.type = type;
    input.priority = priority;
    input.trust = RTOS_ROUTE_TRUST_AUTH_OK;
    input.epoch = 1u;
    input.anymsg_header_valid = true;
    return input;
}

/**
 * @brief 验证所有固定 CID 段能路由到预期接口。
 *
 * @return 0 表示测试通过，非 0 表示失败。
 */
static int test_cid_routes(void)
{
    static const uint8_t cid_first_byte[PUT_SHM_INTERFACE_COUNT] = {
        ANYMSG_CID_CAN_MIN,
        ANYMSG_CID_ETHERNET_MIN,
        ANYMSG_CID_WIFI_MIN,
        ANYMSG_CID_BLUETOOTH_MIN,
        ANYMSG_CID_4G_MIN,
        ANYMSG_CID_RS485_MIN,
    };
    static const put_shm_interface_t expected_interface[PUT_SHM_INTERFACE_COUNT] = {
        PUT_SHM_INTERFACE_CAN,
        PUT_SHM_INTERFACE_ETHERNET,
        PUT_SHM_INTERFACE_WIFI,
        PUT_SHM_INTERFACE_BLUETOOTH,
        PUT_SHM_INTERFACE_4G,
        PUT_SHM_INTERFACE_RS485,
    };
    rtos_router_context_t router;   /**< 被测路由上下文。 */
    mock_context_t context;         /**< 模拟 sink 上下文。 */
    rtos_route_input_t input;       /**< 路由输入。 */
    uint32_t i;                     /**< 循环下标。 */

    for (i = 0u; i < PUT_SHM_INTERFACE_COUNT; ++i) {
        init_router(&router, &context);
        input = make_input(i, cid_first_byte[i], ANYMSG_TYPE_RAW_CAN, 2u);
        CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
        CHECK(rtos_router_drain(&router, 1u) == 1u);
        CHECK(context.tx_count == 1u);
        CHECK(context.reclaim_count == 0u);
        CHECK(context.tx_outputs[0].target_interface == expected_interface[i]);
    }

    return 0;
}

/**
 * @brief 验证保留 CID 段不会进入 TX。
 *
 * @return 0 表示测试通过，非 0 表示失败。
 */
static int test_no_route_reclaim(void)
{
    rtos_router_context_t router; /**< 被测路由上下文。 */
    mock_context_t context;       /**< 模拟 sink 上下文。 */
    rtos_route_input_t input;     /**< 路由输入。 */

    init_router(&router, &context);
    input = make_input(1u, ANYMSG_CID_RESERVED_LOW_MIN, ANYMSG_TYPE_RAW_CAN, 2u);
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    CHECK(context.tx_count == 0u);
    CHECK(context.reclaim_count == 1u);
    CHECK(context.reclaim_outputs[0].reclaim_reason == PUT_SHM_RECLAIM_REASON_NO_ROUTE);

    input = make_input(2u, ANYMSG_CID_RESERVED_HIGH_MIN, ANYMSG_TYPE_RAW_CAN, 2u);
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    CHECK(context.tx_count == 0u);
    CHECK(context.reclaim_count == 2u);
    CHECK(context.reclaim_outputs[1].reclaim_reason == PUT_SHM_RECLAIM_REASON_NO_ROUTE);

    return 0;
}

/**
 * @brief 验证非法 header/type/priority/trust 路径按非法帧回收。
 *
 * @return 0 表示测试通过，非 0 表示失败。
 */
static int test_invalid_inputs(void)
{
    rtos_router_context_t router;          /**< 被测路由上下文。 */
    mock_context_t context;                /**< 模拟 sink 上下文。 */
    rtos_route_input_t input;              /**< 路由输入。 */
    rtos_router_statistics_t statistics;   /**< 路由统计。 */

    init_router(&router, &context);

    input = make_input(1u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RESERVED_MIDDLE_MIN, 2u);
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    input = make_input(2u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 4u);
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    input = make_input(3u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 2u);
    input.anymsg_header_valid = false;
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    input = make_input(4u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 2u);
    input.trust = RTOS_ROUTE_TRUST_AUTH_FAILED;
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    input = make_input(5u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 2u);
    input.trust = RTOS_ROUTE_TRUST_INTEGRITY_FAILED;
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    input = make_input(6u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 2u);
    input.trust = RTOS_ROUTE_TRUST_REPLAY_DROPPED;
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);

    CHECK(context.tx_count == 0u);
    CHECK(context.reclaim_count == 6u);
    CHECK(context.reclaim_outputs[0].reclaim_reason == PUT_SHM_RECLAIM_REASON_INVALID_FRAME);
    CHECK(context.reclaim_outputs[5].reclaim_reason == PUT_SHM_RECLAIM_REASON_INVALID_FRAME);

    rtos_router_get_statistics(&router, &statistics);
    CHECK(statistics.invalid_type_count == 1u);
    CHECK(statistics.invalid_priority_count == 1u);
    CHECK(statistics.invalid_header_count == 1u);
    CHECK(statistics.auth_failed_count == 1u);
    CHECK(statistics.integrity_failed_count == 1u);
    CHECK(statistics.replay_dropped_count == 1u);
    CHECK(statistics.drop_reason_count[PUT_SHM_RECLAIM_REASON_INVALID_FRAME] == 6u);

    return 0;
}

/**
 * @brief 验证 TTL 和 epoch 检查。
 *
 * @return 0 表示测试通过，非 0 表示失败。
 */
static int test_ttl_and_epoch(void)
{
    rtos_router_context_t router; /**< 被测路由上下文。 */
    mock_context_t context;       /**< 模拟 sink 上下文。 */
    rtos_route_input_t input;     /**< 路由输入。 */

    init_router(&router, &context);
    context.now_ms = 100u;
    input = make_input(1u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 2u);
    input.receive_time_ms = 80u;
    input.ttl = 10u;
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    CHECK(context.reclaim_count == 1u);
    CHECK(context.reclaim_outputs[0].reclaim_reason == PUT_SHM_RECLAIM_REASON_TTL_EXPIRED);

    input = make_input(2u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 2u);
    input.receive_time_ms = 0u;
    input.ttl = 0u;
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    CHECK(rtos_router_drain(&router, 1u) == 1u);
    CHECK(context.tx_count == 1u);

    input = make_input(3u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 2u);
    input.receive_time_ms = 100u;
    input.ttl = 5u;
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    context.now_ms = 106u;
    CHECK(rtos_router_drain(&router, 1u) == 1u);
    CHECK(context.reclaim_count == 2u);
    CHECK(context.reclaim_outputs[1].reclaim_reason == PUT_SHM_RECLAIM_REASON_TTL_EXPIRED);

    input = make_input(4u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 2u);
    input.epoch = 2u;
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    CHECK(context.reclaim_count == 3u);
    CHECK(context.reclaim_outputs[2].reclaim_reason ==
          PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH);

    return 0;
}

/**
 * @brief 验证心跳消费和 gateway CID 行为。
 *
 * @return 0 表示测试通过，非 0 表示失败。
 */
static int test_heartbeat_consume(void)
{
    rtos_router_context_t router;                      /**< 被测路由上下文。 */
    mock_context_t context;                            /**< 模拟 sink 上下文。 */
    rtos_route_input_t input;                          /**< 路由输入。 */
    rtos_endpoint_heartbeat_snapshot_t heartbeat;      /**< 心跳快照。 */
    uint8_t gateway_cid[ANYMSG_CID_LENGTH] = {0x01u, 0x02u, 0x03u, 0x04u};

    init_router(&router, &context);
    input = make_input(1u, gateway_cid[0], ANYMSG_TYPE_ENDPOINT_TO_GATEWAY_HEARTBEAT, 0u);
    (void)memcpy(input.destination_cid, gateway_cid, ANYMSG_CID_LENGTH);
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    CHECK(context.tx_count == 0u);
    CHECK(context.reclaim_count == 1u);
    CHECK(context.reclaim_outputs[0].reclaim_reason ==
          PUT_SHM_RECLAIM_REASON_HEARTBEAT_CONSUMED);
    rtos_endpoint_heartbeat_get_snapshot(&router.endpoint_heartbeat, &heartbeat);
    CHECK(heartbeat.entry_count == 0u);

    init_router(&router, &context);
    CHECK(rtos_endpoint_heartbeat_set_gateway(&router.endpoint_heartbeat, gateway_cid) ==
          UNIFIED_OK);
    input = make_input(2u, gateway_cid[0], ANYMSG_TYPE_ENDPOINT_TO_GATEWAY_HEARTBEAT, 0u);
    (void)memcpy(input.destination_cid, gateway_cid, ANYMSG_CID_LENGTH);
    input.frame_local_time = 55u;
    context.now_ms = 77u;
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    CHECK(context.tx_count == 0u);
    CHECK(context.reclaim_count == 1u);
    rtos_endpoint_heartbeat_get_snapshot(&router.endpoint_heartbeat, &heartbeat);
    CHECK(heartbeat.entry_count == 1u);
    CHECK(heartbeat.entries[0].rx_count == 1u);
    CHECK(heartbeat.entries[0].last_rtos_time_ms == 77u);
    CHECK(heartbeat.entries[0].last_frame_local_time == 55u);

    return 0;
}

/**
 * @brief 验证 TX 队列满重试策略和最终 reclaim。
 *
 * @return 0 表示测试通过，非 0 表示失败。
 */
static int test_tx_congestion(void)
{
    rtos_router_context_t router;        /**< 被测路由上下文。 */
    mock_context_t context;              /**< 模拟 sink 上下文。 */
    rtos_route_input_t input;            /**< 路由输入。 */
    rtos_router_statistics_t statistics; /**< 路由统计。 */

    init_router(&router, &context);
    context.tx_failures_before_success = 2u;
    input = make_input(1u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 0u);
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    CHECK(rtos_router_drain(&router, 1u) == 1u);
    CHECK(context.tx_count == 3u);
    CHECK(context.reclaim_count == 0u);
    CHECK(context.tx_outputs[2].retry_count == 2u);
    rtos_router_get_statistics(&router, &statistics);
    CHECK(statistics.tx_retry_count == 2u);
    CHECK(statistics.routed_count == 1u);

    init_router(&router, &context);
    context.tx_always_full = true;
    input = make_input(2u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 0u);
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    CHECK(rtos_router_drain(&router, 1u) == 1u);
    CHECK(context.tx_count == 4u);
    CHECK(context.reclaim_count == 1u);
    CHECK(context.reclaim_outputs[0].retry_count == 3u);
    CHECK(context.reclaim_outputs[0].reclaim_reason == PUT_SHM_RECLAIM_REASON_QUEUE_FULL);

    init_router(&router, &context);
    context.tx_always_full = true;
    input = make_input(3u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 3u);
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    CHECK(rtos_router_drain(&router, 1u) == 1u);
    CHECK(context.tx_count == 1u);
    CHECK(context.reclaim_count == 1u);
    CHECK(context.reclaim_outputs[0].retry_count == 0u);

    return 0;
}

/**
 * @brief 验证路由调度器的 priority 顺序。
 *
 * @return 0 表示测试通过，非 0 表示失败。
 */
static int test_router_priority_order(void)
{
    rtos_router_context_t router; /**< 被测路由上下文。 */
    mock_context_t context;       /**< 模拟 sink 上下文。 */
    rtos_route_input_t input;     /**< 路由输入。 */
    uint32_t i;                   /**< 循环下标。 */

    init_router(&router, &context);
    for (i = 0u; i < 17u; ++i) {
        input = make_input(i, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 0u);
        CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    }
    input = make_input(100u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 2u);
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);
    input = make_input(200u, ANYMSG_CID_RS485_MIN, ANYMSG_TYPE_RAW_CAN, 3u);
    CHECK(rtos_router_submit(&router, &input) == UNIFIED_OK);

    CHECK(rtos_router_drain(&router, 19u) == 19u);
    for (i = 0u; i < RTOS_FIRMWARE_PRIORITY_0_QUOTA; ++i) {
        CHECK(context.tx_outputs[i].priority == 0u);
    }
    CHECK(context.tx_outputs[RTOS_FIRMWARE_PRIORITY_0_QUOTA].priority == 2u);
    CHECK(context.tx_outputs[RTOS_FIRMWARE_PRIORITY_0_QUOTA + 1u].priority == 3u);
    CHECK(context.tx_outputs[RTOS_FIRMWARE_PRIORITY_0_QUOTA + 2u].priority == 0u);

    return 0;
}

/**
 * @brief 验证 route table CRC 错误时保留旧表。
 *
 * @return 0 表示测试通过。
 */
static int test_route_table_crc_rejects_bad_update(void)
{
    rtos_router_context_t router;       /**< 被测路由上下文。 */
    mock_context_t context;             /**< 模拟 sink 上下文。 */
    rtos_route_table_snapshot_t table;  /**< route table 快照。 */
    rtos_route_table_snapshot_t saved;  /**< 原 route table 快照。 */
    rtos_router_statistics_t statistics; /**< router 统计。 */

    init_router(&router, &context);
    rtos_router_get_route_table(&router, &saved);
    table = saved;
    table.active_route_epoch = table.active_route_epoch + 1u;
    table.crc16 = 0x1234u;
    CHECK(rtos_router_set_route_table(&router, &table) == UNIFIED_ERR_CRC);
    rtos_router_get_route_table(&router, &table);
    CHECK(table.active_route_epoch == saved.active_route_epoch);
    rtos_router_get_statistics(&router, &statistics);
    CHECK(statistics.route_table_crc_error_count == 1u);

    table = saved;
    table.active_route_epoch = table.active_route_epoch + 1u;
    table.crc16 = rtos_router_route_table_calculate_crc(&table);
    CHECK(rtos_router_set_route_table(&router, &table) == UNIFIED_OK);
    rtos_router_get_route_table(&router, &saved);
    CHECK(saved.active_route_epoch == table.active_route_epoch);

    return 0;
}

/**
 * @brief P1 路由核心主机端测试入口。
 *
 * @return 0 表示全部测试通过，非 0 表示失败。
 */
int main(void)
{
    CHECK(test_cid_routes() == 0);
    CHECK(test_no_route_reclaim() == 0);
    CHECK(test_invalid_inputs() == 0);
    CHECK(test_ttl_and_epoch() == 0);
    CHECK(test_heartbeat_consume() == 0);
    CHECK(test_tx_congestion() == 0);
    CHECK(test_router_priority_order() == 0);
    CHECK(test_route_table_crc_rejects_bad_update() == 0);
    CHECK(rtos_router_adapter_p1_boundary_check() == UNIFIED_OK);

    return 0;
}

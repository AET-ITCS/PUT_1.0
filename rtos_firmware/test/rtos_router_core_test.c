/**
 * @file rtos_router_core_test.c
 * @brief P1 router core host tests.
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
 * @brief Mock sink context.
 */
typedef struct {
    uint32_t now_ms;                       /**< Time source value. */
    uint32_t tx_count;                     /**< TX sink call count. */
    uint32_t reclaim_count;                /**< Reclaim sink call count. */
    uint32_t tx_failures_before_success;   /**< Queue-full failures before success. */
    bool tx_always_full;                   /**< Force all TX attempts to queue full. */
    rtos_route_output_t tx_outputs[128];   /**< Captured TX outputs. */
    rtos_route_output_t reclaim_outputs[128]; /**< Captured reclaim outputs. */
} mock_context_t;

/**
 * @brief Mock time source.
 */
static uint32_t mock_time_source(void *user_context)
{
    mock_context_t *context; /**< Mock context. */

    context = (mock_context_t *)user_context;
    if (context == 0) {
        return 0u;
    }

    return context->now_ms;
}

/**
 * @brief Mock TX sink.
 */
static unified_error_t mock_tx_sink(const rtos_route_output_t *output,
                                    void *user_context)
{
    mock_context_t *context; /**< Mock context. */

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
 * @brief Mock reclaim sink.
 */
static unified_error_t mock_reclaim_sink(const rtos_route_output_t *output,
                                         void *user_context)
{
    mock_context_t *context; /**< Mock context. */

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
 * @brief Initialize router and mock context.
 */
static void init_router(rtos_router_context_t *router, mock_context_t *context)
{
    rtos_router_sinks_t sinks; /**< Mock sink callbacks. */

    (void)memset(context, 0, sizeof(*context));
    (void)memset(&sinks, 0, sizeof(sinks));
    sinks.tx_sink = mock_tx_sink;
    sinks.reclaim_sink = mock_reclaim_sink;
    sinks.user_context = context;
    rtos_router_init(router, &sinks, mock_time_source, context);
    rtos_router_set_linux_epoch(router, 1u);
}

/**
 * @brief Build a valid route input.
 */
static rtos_route_input_t make_input(uint32_t frame_id,
                                     uint8_t destination_first_byte,
                                     uint8_t type,
                                     uint8_t priority)
{
    rtos_route_input_t input; /**< Route input under construction. */

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
 * @brief Verify all fixed CID segments route to the expected interface.
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
    rtos_router_context_t router;   /**< Router under test. */
    mock_context_t context;         /**< Mock sink context. */
    rtos_route_input_t input;       /**< Route input. */
    uint32_t i;                     /**< Loop index. */

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
 * @brief Verify reserved CID ranges do not enter TX.
 */
static int test_no_route_reclaim(void)
{
    rtos_router_context_t router; /**< Router under test. */
    mock_context_t context;       /**< Mock sink context. */
    rtos_route_input_t input;     /**< Route input. */

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
 * @brief Verify invalid header/type/priority/trust paths reclaim as invalid frame.
 */
static int test_invalid_inputs(void)
{
    rtos_router_context_t router;          /**< Router under test. */
    mock_context_t context;                /**< Mock sink context. */
    rtos_route_input_t input;              /**< Route input. */
    rtos_router_statistics_t statistics;   /**< Router statistics. */

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
 * @brief Verify TTL and epoch checks.
 */
static int test_ttl_and_epoch(void)
{
    rtos_router_context_t router; /**< Router under test. */
    mock_context_t context;       /**< Mock sink context. */
    rtos_route_input_t input;     /**< Route input. */

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
 * @brief Verify heartbeat consume and gateway CID behavior.
 */
static int test_heartbeat_consume(void)
{
    rtos_router_context_t router;                      /**< Router under test. */
    mock_context_t context;                            /**< Mock sink context. */
    rtos_route_input_t input;                          /**< Route input. */
    rtos_endpoint_heartbeat_snapshot_t heartbeat;      /**< Heartbeat snapshot. */
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
 * @brief Verify TX queue-full retry policy and final reclaim.
 */
static int test_tx_congestion(void)
{
    rtos_router_context_t router;        /**< Router under test. */
    mock_context_t context;              /**< Mock sink context. */
    rtos_route_input_t input;            /**< Route input. */
    rtos_router_statistics_t statistics; /**< Router statistics. */

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
 * @brief Verify priority ordering through the router scheduler.
 */
static int test_router_priority_order(void)
{
    rtos_router_context_t router; /**< Router under test. */
    mock_context_t context;       /**< Mock sink context. */
    rtos_route_input_t input;     /**< Route input. */
    uint32_t i;                   /**< Loop index. */

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

int main(void)
{
    CHECK(test_cid_routes() == 0);
    CHECK(test_no_route_reclaim() == 0);
    CHECK(test_invalid_inputs() == 0);
    CHECK(test_ttl_and_epoch() == 0);
    CHECK(test_heartbeat_consume() == 0);
    CHECK(test_tx_congestion() == 0);
    CHECK(test_router_priority_order() == 0);
    CHECK(rtos_router_adapter_p1_boundary_check() == UNIFIED_OK);

    return 0;
}

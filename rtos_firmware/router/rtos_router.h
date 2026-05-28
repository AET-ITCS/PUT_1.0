/**
 * @file rtos_router.h
 * @brief rtos_firmware P1 route core interface.
 * @author Yukikaze
 */
#ifndef RTOS_ROUTER_H
#define RTOS_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

#include "error_code.h"
#include "rtos_endpoint_heartbeat.h"
#include "rtos_priority_queue.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief P1 route input trust state.
 */
typedef enum {
    RTOS_ROUTE_TRUST_AUTH_OK = 0,
    RTOS_ROUTE_TRUST_INTERNAL_TRUSTED = 1,
    RTOS_ROUTE_TRUST_AUTH_FAILED = 2,
    RTOS_ROUTE_TRUST_INTEGRITY_FAILED = 3,
    RTOS_ROUTE_TRUST_REPLAY_DROPPED = 4,
} rtos_route_trust_t;

/**
 * @brief P1 route output kind.
 */
typedef enum {
    RTOS_ROUTE_OUTPUT_NONE = 0,
    RTOS_ROUTE_OUTPUT_TX = 1,
    RTOS_ROUTE_OUTPUT_RECLAIM = 2,
} rtos_route_output_kind_t;

/**
 * @brief Trusted route input, decoupled from put_shm_descriptor_t.
 */
typedef struct {
    uint32_t frame_id;                      /**< Trusted Frame Pool block ID. */
    put_shm_interface_t source_interface;   /**< Source RX interface. */
    uint8_t source_cid[ANYMSG_CID_LENGTH];  /**< anyMSG source CID. */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< anyMSG destination CID. */
    uint8_t type;                           /**< anyMSG type. */
    uint8_t priority;                       /**< Scheduler priority. */
    uint8_t ttl;                            /**< TTL in milliseconds; 0 disables TTL. */
    rtos_route_trust_t trust;               /**< P1 trust state. */
    uint32_t epoch;                         /**< Linux epoch carried by the input. */
    uint32_t flags;                         /**< Route input flags. */
    uint32_t receive_time_ms;               /**< Time when P1 received this route input. */
    uint32_t frame_local_time;              /**< anyMSG local_time snapshot. */
    bool anymsg_header_valid;               /**< false simulates static anyMSG header failure. */
} rtos_route_input_t;

/**
 * @brief Router output for mock TX and reclaim sinks.
 */
typedef struct {
    rtos_route_output_kind_t kind;          /**< TX or reclaim output. */
    uint32_t frame_id;                      /**< Frame reference. */
    put_shm_interface_t source_interface;   /**< Original source interface. */
    put_shm_interface_t target_interface;   /**< Target interface or last route target. */
    uint8_t source_cid[ANYMSG_CID_LENGTH];  /**< Source CID. */
    uint8_t destination_cid[ANYMSG_CID_LENGTH]; /**< Destination CID. */
    uint8_t type;                           /**< anyMSG type. */
    uint8_t priority;                       /**< Priority. */
    uint8_t retry_count;                    /**< TX retry count. */
    uint32_t epoch;                         /**< Linux epoch. */
    uint32_t flags;                         /**< Output flags. */
    uint32_t latency_ms;                    /**< receive_time_ms -> output latency. */
    put_shm_reclaim_reason_t reclaim_reason; /**< Reclaim reason when kind is reclaim. */
} rtos_route_output_t;

/**
 * @brief Route table snapshot used by P1 fixed CID routing.
 */
typedef struct {
    uint32_t route_version;                 /**< Route table version. */
    uint32_t active_route_epoch;            /**< Active route epoch. */
    bool valid;                             /**< Route table validity flag. */
    put_shm_interface_t cid_segment_targets[ANYMSG_CID_SEGMENT_RESERVED_HIGH + 1u];
    uint16_t crc16;                         /**< Reserved for later control area CRC. */
} rtos_route_table_snapshot_t;

/**
 * @brief P1 router statistics snapshot.
 */
typedef struct {
    uint32_t submitted_count;
    uint32_t enqueued_count;
    uint32_t routed_count;
    uint32_t reclaimed_count;
    uint32_t heartbeat_consumed_count;
    uint32_t queue_evicted_count;
    uint32_t tx_retry_count;
    uint32_t drop_reason_count[PUT_SHM_RECLAIM_REASON_QUEUE_FULL + 1u];
    uint32_t routed_by_interface[PUT_SHM_INTERFACE_COUNT];
    uint32_t routed_by_priority[RTOS_FIRMWARE_PRIORITY_COUNT];
    uint32_t auth_failed_count;
    uint32_t integrity_failed_count;
    uint32_t replay_dropped_count;
    uint32_t invalid_priority_count;
    uint32_t invalid_type_count;
    uint32_t invalid_header_count;
} rtos_router_statistics_t;

/**
 * @brief Router time source.
 */
typedef uint32_t (*rtos_router_time_source_t)(void *user_context);

/**
 * @brief Mockable TX sink.
 */
typedef unified_error_t (*rtos_router_tx_sink_t)(const rtos_route_output_t *output,
                                                 void *user_context);

/**
 * @brief Mockable reclaim sink.
 */
typedef unified_error_t (*rtos_router_reclaim_sink_t)(const rtos_route_output_t *output,
                                                      void *user_context);

/**
 * @brief P1 router sink callbacks.
 */
typedef struct {
    rtos_router_tx_sink_t tx_sink;
    rtos_router_reclaim_sink_t reclaim_sink;
    void *user_context;
} rtos_router_sinks_t;

/**
 * @brief P1 router context.
 */
typedef struct {
    rtos_priority_queue_t queue;
    rtos_endpoint_heartbeat_table_t endpoint_heartbeat;
    rtos_route_table_snapshot_t route_table;
    rtos_router_statistics_t statistics;
    rtos_router_sinks_t sinks;
    rtos_router_time_source_t time_source;
    void *time_context;
    uint32_t current_linux_epoch;
} rtos_router_context_t;

/**
 * @brief Initialize router context.
 */
void rtos_router_init(rtos_router_context_t *router,
                      const rtos_router_sinks_t *sinks,
                      rtos_router_time_source_t time_source,
                      void *time_context);

/**
 * @brief Set the Linux epoch accepted by P1 epoch checks.
 */
void rtos_router_set_linux_epoch(rtos_router_context_t *router, uint32_t linux_epoch);

/**
 * @brief Submit one trusted route input into P1 routing.
 */
unified_error_t rtos_router_submit(rtos_router_context_t *router,
                                   const rtos_route_input_t *input);

/**
 * @brief Schedule one queued item to TX or reclaim.
 */
unified_error_t rtos_router_schedule_once(rtos_router_context_t *router,
                                          rtos_route_output_t *out_output);

/**
 * @brief Schedule up to budget queued items.
 *
 * @return Number of items consumed from the local scheduler queue.
 */
uint32_t rtos_router_drain(rtos_router_context_t *router, uint32_t budget);

/**
 * @brief Read route table snapshot.
 */
void rtos_router_get_route_table(const rtos_router_context_t *router,
                                 rtos_route_table_snapshot_t *out_table);

/**
 * @brief Replace route table snapshot.
 */
unified_error_t rtos_router_set_route_table(rtos_router_context_t *router,
                                            const rtos_route_table_snapshot_t *table);

/**
 * @brief Fill a default P1 CID route table.
 */
void rtos_router_route_table_default(rtos_route_table_snapshot_t *out_table);

/**
 * @brief Lookup a target interface for a destination CID first byte.
 */
unified_error_t rtos_router_route_table_lookup(
    const rtos_route_table_snapshot_t *table,
    uint8_t destination_cid_first_byte,
    put_shm_interface_t *out_interface);

/**
 * @brief Read router statistics.
 */
void rtos_router_get_statistics(const rtos_router_context_t *router,
                                rtos_router_statistics_t *out_statistics);

/**
 * @brief Return true when a P1 type is allowed to route or be consumed.
 */
bool rtos_router_type_is_valid(uint8_t type);

/**
 * @brief Return true when a P1 trust state may enter routing.
 */
bool rtos_router_trust_is_routable(rtos_route_trust_t trust);

/**
 * @brief Return true when a P1 TTL has expired.
 */
bool rtos_router_ttl_is_expired(uint32_t now_ms,
                                uint32_t receive_time_ms,
                                uint8_t ttl_ms);

/**
 * @brief P1 adapter boundary symbol; descriptor adaptation is implemented in P2.
 */
unified_error_t rtos_router_adapter_p1_boundary_check(void);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_ROUTER_H */

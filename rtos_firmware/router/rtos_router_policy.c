/**
 * @file rtos_router_policy.c
 * @brief P1 路由校验和策略 helper 实现。
 * @author Yukikaze
 */
#include "rtos_router.h"

/**
 * @brief 判断 anyMSG type 是否允许在 P1 中转发或消费。
 *
 * @param type anyMSG type。
 * @return true 表示允许，false 表示非法或保留。
 */
bool rtos_router_type_is_valid(uint8_t type)
{
    if (type <= ANYMSG_TYPE_NETWORK_AUTH_MAX) {
        return true;
    }

    if ((type >= ANYMSG_TYPE_SERVICE_INDEX_MIN) &&
        (type <= ANYMSG_TYPE_SERVICE_INDEX_MAX)) {
        return true;
    }

    if ((type >= ANYMSG_TYPE_TUNNEL_PORT_MAPPING_MIN) &&
        (type <= ANYMSG_TYPE_TUNNEL_PORT_MAPPING_MAX)) {
        return true;
    }

    if ((type >= ANYMSG_TYPE_MODBUS_RTU) && (type <= ANYMSG_TYPE_CANOPEN)) {
        return true;
    }

    return false;
}

/**
 * @brief 判断可信状态是否允许进入路由。
 *
 * @param trust P1 路由输入可信状态。
 * @return true 表示可路由，false 表示应丢弃。
 */
bool rtos_router_trust_is_routable(rtos_route_trust_t trust)
{
    return (trust == RTOS_ROUTE_TRUST_AUTH_OK) ||
           (trust == RTOS_ROUTE_TRUST_INTERNAL_TRUSTED);
}

/**
 * @brief 判断 P1 TTL 是否已经过期。
 *
 * @param now_ms 当前时间，单位毫秒。
 * @param receive_time_ms 输入接收时间，单位毫秒。
 * @param ttl_ms TTL 毫秒值，0 表示禁用检查。
 * @return true 表示已过期，false 表示未过期。
 */
bool rtos_router_ttl_is_expired(uint32_t now_ms,
                                uint32_t receive_time_ms,
                                uint8_t ttl_ms)
{
    uint32_t age_ms; /**< 无符号差值用于兼容单调时间回绕。 */

    if (ttl_ms == 0u) {
        return false;
    }

    age_ms = now_ms - receive_time_ms;
    return age_ms >= (uint32_t)ttl_ms;
}

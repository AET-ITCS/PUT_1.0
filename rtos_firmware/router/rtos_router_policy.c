/**
 * @file rtos_router_policy.c
 * @brief P1 router validation and policy helpers.
 * @author Yukikaze
 */
#include "rtos_router.h"

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

bool rtos_router_trust_is_routable(rtos_route_trust_t trust)
{
    return (trust == RTOS_ROUTE_TRUST_AUTH_OK) ||
           (trust == RTOS_ROUTE_TRUST_INTERNAL_TRUSTED);
}

bool rtos_router_ttl_is_expired(uint32_t now_ms,
                                uint32_t receive_time_ms,
                                uint8_t ttl_ms)
{
    uint32_t age_ms; /**< Unsigned delta handles monotonic wrap. */

    if (ttl_ms == 0u) {
        return false;
    }

    age_ms = now_ms - receive_time_ms;
    return age_ms >= (uint32_t)ttl_ms;
}

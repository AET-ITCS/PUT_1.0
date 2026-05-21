/* 以太网 UDP CAN direct 网关帧解析实现。 */
#include "ethernet_udp.h"

#include "can_direct_frame.h"

unified_error_t ethernet_udp_parse_frame(const uint8_t *buffer,
                                         size_t length,
                                         protocol_parsed_msg_t *out_msg)
{
    return can_direct_parse_frame(buffer, length, PROTOCOL_TYPE_ETHERNET, out_msg);
}

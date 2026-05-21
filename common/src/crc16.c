/* CRC16 公共实现：计算统一帧和 CAN direct 网关帧使用的 CRC-16/CCITT-FALSE。 */
#include "crc16.h"

uint16_t unified_crc16_ccitt_false(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFu;

    if ((data == 0) && (length != 0u)) {
        return 0u;
    }

    for (size_t i = 0u; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8u;
        for (uint8_t bit = 0u; bit < 8u; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1u) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1u);
            }
        }
    }

    return crc;
}

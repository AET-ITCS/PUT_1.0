/* CRC16 公共接口：提供大核、小核和测试共用的 CRC-16/CCITT-FALSE 计算函数。 */
#ifndef CRC16_H
#define CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 计算 CRC-16/CCITT-FALSE。
 *
 * 参数：poly=0x1021, init=0xFFFF, refin=false, refout=false, xorout=0x0000。
 */
uint16_t unified_crc16_ccitt_false(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* CRC16_H */

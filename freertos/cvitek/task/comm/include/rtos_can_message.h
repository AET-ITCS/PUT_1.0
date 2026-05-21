/* FreeRTOS comm CAN message：小核 CAN 层稳定内部消息，不绑定大小核上层协议。 */
#ifndef RTOS_CAN_MESSAGE_H
#define RTOS_CAN_MESSAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTOS_CAN_CLASSIC_DATA_MAX_LEN 8u
#define RTOS_CAN_STANDARD_ID_MAX 0x7FFu
#define RTOS_CAN_EXTENDED_ID_MAX 0x1FFFFFFFu

typedef enum {
    RTOS_CAN_FLAG_NONE = 0x00u,
    RTOS_CAN_FLAG_EXTENDED_ID = (uint8_t)(1u << 0),
} rtos_can_flag_t;

typedef struct {
    uint32_t can_id;
    uint8_t can_dlc;
    uint8_t can_flags;
    uint8_t can_data[RTOS_CAN_CLASSIC_DATA_MAX_LEN];
} rtos_can_message_t;

#ifdef __cplusplus
}
#endif

#endif /* RTOS_CAN_MESSAGE_H */

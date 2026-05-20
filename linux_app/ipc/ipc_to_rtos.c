/* 大核到小核发送 stub：先打印 unified_frame_t，等待共享内存模块接入。 */
#include "ipc_to_rtos.h"

#include <inttypes.h>
#include <stdio.h>

unified_error_t ipc_to_rtos_send(const unified_frame_t *frame)
{
    if (frame == NULL) {
        return UNIFIED_ERR_NULL;
    }

    printf("[ipc_stub] seq=%" PRIu32 " src_proto=0x%02X vehicle=0x%02X can_id=0x%" PRIX32
           " dlc=%u flags=0x%02X crc=0x%04X data=",
           frame->sequence,
           frame->source_protocol,
           frame->vehicle_type,
           frame->can_id,
           frame->can_dlc,
           frame->can_flags,
           frame->crc16);

    for (uint8_t i = 0u; i < frame->can_dlc; ++i) {
        printf("%02X", frame->can_data[i]);
        if ((uint8_t)(i + 1u) < frame->can_dlc) {
            putchar(' ');
        }
    }

    putchar('\n');
    fflush(stdout);
    return UNIFIED_OK;
}

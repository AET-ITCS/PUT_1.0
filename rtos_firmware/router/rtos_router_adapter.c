/**
 * @file rtos_router_adapter.c
 * @brief 共享内存 descriptor 到路由输入的适配实现。
 * @author Yukikaze
 */
#include "rtos_router.h"

#include <string.h>

/**
 * @brief 按 little-endian 读取 16 位字段。
 *
 * @param bytes 原始字节。
 * @return 主机序 16 位值。
 */
static uint16_t read_le16(const uint8_t bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
}

/**
 * @brief 按 little-endian 读取 32 位字段。
 *
 * @param bytes 原始字节。
 * @return 主机序 32 位值。
 */
static uint32_t read_le32(const uint8_t bytes[4])
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) |
           ((uint32_t)bytes[3] << 24u);
}

/**
 * @brief 判断 descriptor 元数据是否与 anyMSG header 一致。
 *
 * @param descriptor descriptor 元数据。
 * @param header anyMSG header。
 * @return true 表示一致，false 表示不一致。
 */
static bool descriptor_matches_header(const put_shm_descriptor_t *descriptor,
                                      const anymsg_header_t *header)
{
    if ((descriptor == 0) || (header == 0)) {
        return false;
    }

    if (memcmp(descriptor->source_cid,
               header->source_cid,
               ANYMSG_CID_LENGTH) != 0) {
        return false;
    }

    if (memcmp(descriptor->destination_cid,
               header->destination_cid,
               ANYMSG_CID_LENGTH) != 0) {
        return false;
    }

    return descriptor->type == header->type;
}

/**
 * @brief 判断 descriptor 是否包含指定 flag。
 *
 * @param descriptor descriptor 元数据。
 * @param flag 待检查 flag。
 * @return true 表示 flag 已置位。
 */
static bool descriptor_has_flag(const put_shm_descriptor_t *descriptor, uint32_t flag)
{
    if (descriptor == 0) {
        return false;
    }

    return (descriptor->flags & flag) != 0u;
}

/**
 * @brief 判断来源接口是否属于外部入口。
 *
 * @param interface_id 来源接口 ID。
 * @return true 表示 Ethernet/Wi-Fi/Bluetooth/4G 外部入口。
 */
static bool source_interface_is_external(uint8_t interface_id)
{
    return (interface_id == (uint8_t)PUT_SHM_INTERFACE_ETHERNET) ||
           (interface_id == (uint8_t)PUT_SHM_INTERFACE_WIFI) ||
           (interface_id == (uint8_t)PUT_SHM_INTERFACE_BLUETOOTH) ||
           (interface_id == (uint8_t)PUT_SHM_INTERFACE_4G);
}

/**
 * @brief 判断目的 CID 是否落入控制类物理接口段。
 *
 * @param destination_cid anyMSG destination CID。
 * @return true 表示目标为 CAN 或 RS485 CID 段。
 */
static bool destination_cid_is_control_path(const uint8_t destination_cid[ANYMSG_CID_LENGTH])
{
    anymsg_cid_segment_t segment; /**< 目的 CID 地址段。 */

    if (destination_cid == 0) {
        return false;
    }

    segment = anymsg_cid_segment_from_first_byte(destination_cid[0]);
    return (segment == ANYMSG_CID_SEGMENT_CAN) ||
           (segment == ANYMSG_CID_SEGMENT_RS485);
}

/**
 * @brief 判断 descriptor 是否需要 CONTROL_ALLOWED。
 *
 * @param descriptor descriptor 元数据。
 * @return true 表示外部入口正在请求高优先级或控制接口路径。
 */
static bool descriptor_requires_control_allowed(const put_shm_descriptor_t *descriptor)
{
    if (descriptor == 0) {
        return false;
    }

    if (!source_interface_is_external(descriptor->source_interface)) {
        return false;
    }

    return (descriptor->priority <= 1u) ||
           destination_cid_is_control_path(descriptor->destination_cid);
}

/**
 * @brief 将 descriptor flags 映射为 RTOS 路由可信状态。
 *
 * @param descriptor descriptor 元数据。
 * @return 路由可信状态。
 */
static rtos_route_trust_t descriptor_trust_from_flags(const put_shm_descriptor_t *descriptor)
{
    if (descriptor == 0) {
        return RTOS_ROUTE_TRUST_AUTH_FAILED;
    }

    if (descriptor_has_flag(descriptor, PUT_SHM_DESCRIPTOR_FLAG_INTERNAL_TRUSTED)) {
        /* 内部可信入口由 Linux 明确标记，允许绕过外部入口认证三件套。 */
        return RTOS_ROUTE_TRUST_INTERNAL_TRUSTED;
    }

    if (!descriptor_has_flag(descriptor, PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK)) {
        /* 外部入口未完成鉴权时不得进入 Router Scheduler。 */
        return RTOS_ROUTE_TRUST_AUTH_FAILED;
    }

    if (!descriptor_has_flag(descriptor, PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK)) {
        /* 外部入口完整性检查失败时不得路由。 */
        return RTOS_ROUTE_TRUST_INTEGRITY_FAILED;
    }

    if (!descriptor_has_flag(descriptor, PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK)) {
        /* 外部入口重放检查失败时不得路由。 */
        return RTOS_ROUTE_TRUST_REPLAY_DROPPED;
    }

    if (descriptor_requires_control_allowed(descriptor) &&
        !descriptor_has_flag(descriptor, PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED)) {
        /* 外部高优先级或 CAN/RS485 控制路径需要额外授权。 */
        return RTOS_ROUTE_TRUST_AUTH_FAILED;
    }

    return RTOS_ROUTE_TRUST_AUTH_OK;
}

/**
 * @brief 校验 Frame Pool 中的 anyMSG header。
 *
 * @param descriptor descriptor 元数据。
 * @param frame 完整 anyMSG 起始地址。
 * @param frame_length 完整 anyMSG 字节数。
 * @param out_header 输出 header 指针。
 * @return true 表示基础校验通过，false 表示应按 invalid frame 回收。
 */
static bool validate_anymsg_header(const put_shm_descriptor_t *descriptor,
                                   const uint8_t *frame,
                                   uint16_t frame_length,
                                   const anymsg_header_t **out_header)
{
    const anymsg_header_t *header; /**< anyMSG header 视图。 */
    uint16_t msg_length;           /**< little-endian 完整帧长度。 */
    uint16_t payload_length;       /**< little-endian payload 长度。 */

    if ((descriptor == 0) || (frame == 0) || (out_header == 0) ||
        (frame_length < ANYMSG_HEADER_SIZE)) {
        return false;
    }

    header = (const anymsg_header_t *)frame;
    *out_header = header;
    msg_length = read_le16(header->msg_length);
    payload_length = read_le16(header->payload_length);

    if (anymsg_validate_normalized_lengths(msg_length,
                                           payload_length,
                                           frame_length) != UNIFIED_OK) {
        return false;
    }

    if (anymsg_validate_header_static_fields(header) != UNIFIED_OK) {
        return false;
    }

    if (!descriptor_matches_header(descriptor, header)) {
        return false;
    }

    return rtos_router_type_is_valid(header->type);
}

/**
 * @brief 检查 P1 适配层边界符号是否可链接。
 *
 * @return UNIFIED_OK 表示 P1 边界占位可用。
 */
unified_error_t rtos_router_adapter_p1_boundary_check(void)
{
    return UNIFIED_OK;
}

/**
 * @brief 将可信 RX descriptor 和 Frame Pool 中的 anyMSG 转换为路由输入。
 *
 * descriptor 级 CRC、Frame Pool 边界和接口一致性由 IPC 出队 API 保证；
 * 本函数只处理已经出队的可信 frame reference。
 *
 * @param ipc IPC 上下文。
 * @param descriptor 已通过 IPC 出队校验的 RX descriptor。
 * @param now_ms 当前 RTOS 时间，单位毫秒。
 * @param out_input 输出 route input。
 * @return UNIFIED_OK 表示转换完成，否则返回公共错误码。
 */
unified_error_t rtos_router_adapter_descriptor_to_input(
    const rtos_shm_ipc_t *ipc,
    const put_shm_descriptor_t *descriptor,
    uint32_t now_ms,
    rtos_route_input_t *out_input)
{
    const uint8_t *frame;          /**< Frame Pool 中的完整 anyMSG。 */
    uint16_t frame_length;         /**< 完整 anyMSG 字节数。 */
    const anymsg_header_t *header; /**< anyMSG header 视图。 */
    bool header_valid;             /**< anyMSG 基础校验结果。 */
    unified_error_t result;        /**< IPC 读取结果。 */

    if ((ipc == 0) || (descriptor == 0) || (out_input == 0)) {
        return UNIFIED_ERR_NULL;
    }

    frame = 0;
    frame_length = 0u;
    header = 0;
    result = rtos_shm_ipc_get_frame_const(ipc, descriptor, &frame, &frame_length);
    if (result != UNIFIED_OK) {
        return result;
    }

    header_valid = validate_anymsg_header(descriptor, frame, frame_length, &header);

    (void)memset(out_input, 0, sizeof(*out_input));
    out_input->frame_id = descriptor->frame_id;
    out_input->source_interface = (put_shm_interface_t)descriptor->source_interface;
    (void)memcpy(out_input->source_cid, descriptor->source_cid, ANYMSG_CID_LENGTH);
    (void)memcpy(out_input->destination_cid,
                 descriptor->destination_cid,
                 ANYMSG_CID_LENGTH);
    out_input->type = (header != 0) ? header->type : descriptor->type;
    out_input->priority = descriptor->priority;
    out_input->ttl = descriptor->ttl;
    out_input->trust = descriptor_trust_from_flags(descriptor);
    out_input->epoch = descriptor->epoch;
    out_input->flags = descriptor->flags;
    out_input->receive_time_ms = now_ms;
    out_input->frame_local_time = (header != 0) ? read_le32(header->local_time) : 0u;
    out_input->anymsg_header_valid = header_valid;

    return UNIFIED_OK;
}

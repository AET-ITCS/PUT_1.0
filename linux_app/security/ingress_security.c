/**
 * @file ingress_security.c
 * @brief Linux 外部入口 anyMSG 可信性评估实现。
 * @author Yukikaze
 */
#include "ingress_security.h"

#include <string.h>

#include "anymsg_frame.h"

/**
 * @brief 读取 little-endian 16 位字段。
 *
 * @param bytes 输入字节。
 * @return 16 位值。
 */
static uint16_t read_le16(const uint8_t bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
}

/**
 * @brief 读取 little-endian 32 位字段。
 *
 * @param bytes 输入字节。
 * @return 32 位值。
 */
static uint32_t read_le32(const uint8_t bytes[4])
{
    return ((uint32_t)bytes[0]) |
           ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) |
           ((uint32_t)bytes[3] << 24u);
}

/**
 * @brief 判断接口是否是需要入口安全评估的外部接口。
 *
 * @param interface_id 接口 ID。
 * @return true 表示外部入口。
 */
static bool is_external_interface(put_shm_interface_t interface_id)
{
    return (interface_id == PUT_SHM_INTERFACE_ETHERNET) ||
           (interface_id == PUT_SHM_INTERFACE_WIFI) ||
           (interface_id == PUT_SHM_INTERFACE_BLUETOOTH) ||
           (interface_id == PUT_SHM_INTERFACE_4G);
}

/**
 * @brief 判断目的 CID 是否落入控制接口段。
 *
 * @param header anyMSG header。
 * @return true 表示目标为 CAN 或 RS485。
 */
static bool destination_is_control_path(const anymsg_header_t *header)
{
    anymsg_cid_segment_t segment; /**< 目的 CID 地址段。 */

    if (header == 0) {
        return false;
    }

    segment = anymsg_cid_segment_from_first_byte(
        anymsg_destination_cid_first_byte(header));
    return (segment == ANYMSG_CID_SEGMENT_CAN) ||
           (segment == ANYMSG_CID_SEGMENT_RS485);
}

/**
 * @brief 校验 verify_string 的指定片段。
 *
 * @param header anyMSG header。
 * @param offset verify_string 内偏移。
 * @param expected 期望字节。
 * @param expected_length 期望长度。
 * @return true 表示片段匹配。
 */
static bool verify_slice_matches(const anymsg_header_t *header,
                                 size_t offset,
                                 const uint8_t *expected,
                                 size_t expected_length)
{
    if ((header == 0) || (expected == 0) || (expected_length == 0u)) {
        return false;
    }

    if ((offset + expected_length) > ANYMSG_VERIFY_STRING_LENGTH) {
        return false;
    }

    return memcmp(&header->verify_string[offset], expected, expected_length) == 0;
}

/**
 * @brief 校验 anyMSG 基础长度和静态字段。
 *
 * @param input 安全评估输入。
 * @param out_header 输出 anyMSG header。
 * @return UNIFIED_OK 表示可继续安全评估。
 */
static unified_error_t parse_frame_header(const ingress_security_input_t *input,
                                          const anymsg_header_t **out_header)
{
    const anymsg_header_t *header; /**< anyMSG header。 */
    uint16_t msg_length;           /**< 完整帧长度。 */
    uint16_t payload_length;       /**< payload 长度。 */
    unified_error_t result;        /**< 校验结果。 */

    if ((input == 0) || (out_header == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if ((input->frame == 0) ||
        (input->frame_length < ANYMSG_HEADER_SIZE) ||
        (input->frame_length > PUT_SHM_FRAME_POOL_BLOCK_SIZE)) {
        return UNIFIED_ERR_LENGTH;
    }

    header = (const anymsg_header_t *)input->frame;
    msg_length = read_le16(header->msg_length);
    payload_length = read_le16(header->payload_length);
    result = anymsg_validate_normalized_lengths(msg_length,
                                                payload_length,
                                                input->frame_length);
    if (result != UNIFIED_OK) {
        return result;
    }

    result = anymsg_validate_header_static_fields(header);
    if (result != UNIFIED_OK) {
        return result;
    }

    *out_header = header;
    return UNIFIED_OK;
}

/**
 * @brief 检查 replay/sequence 规则并在通过时更新状态。
 *
 * @param policy 安全策略。
 * @param input 安全评估输入。
 * @param header anyMSG header。
 * @return true 表示 replay 检查通过。
 */
static bool replay_check_and_update(ingress_security_policy_t *policy,
                                    const ingress_security_input_t *input,
                                    const anymsg_header_t *header)
{
    uint32_t interface_index; /**< 接口数组下标。 */
    uint32_t sequence;        /**< 当前帧 local_time/sequence。 */

    if ((policy == 0) || (input == 0) || (header == 0)) {
        return false;
    }

    interface_index = (uint32_t)input->source_interface;
    if (interface_index >= PUT_SHM_INTERFACE_COUNT) {
        return false;
    }

    sequence = read_le32(header->local_time);
    if ((policy->max_age_ms != 0u) && (input->now_ms != 0u)) {
        if ((input->now_ms < sequence) ||
            ((input->now_ms - sequence) > policy->max_age_ms)) {
            /* 时间窗失败时不能更新 last_sequence。 */
            return false;
        }
    }

    if (policy->sequence_seen[interface_index] &&
        (sequence <= policy->last_sequence[interface_index])) {
        /* 重放或乱序帧不能更新 last_sequence。 */
        return false;
    }

    policy->last_sequence[interface_index] = sequence;
    policy->sequence_seen[interface_index] = true;
    return true;
}

void ingress_security_policy_init(ingress_security_policy_t *policy)
{
    if (policy == 0) {
        return;
    }

    (void)memset(policy, 0, sizeof(*policy));
}

unified_error_t ingress_security_policy_set_mock_credentials(
    ingress_security_policy_t *policy,
    const uint8_t *auth_token,
    size_t auth_token_length,
    const uint8_t *integrity_tag,
    size_t integrity_tag_length)
{
    if (policy == 0) {
        return UNIFIED_ERR_NULL;
    }

    if ((auth_token_length > INGRESS_SECURITY_AUTH_TOKEN_MAX_LEN) ||
        (integrity_tag_length > INGRESS_SECURITY_INTEGRITY_TAG_MAX_LEN)) {
        return UNIFIED_ERR_LENGTH;
    }

    if ((auth_token_length != 0u) && (auth_token == 0)) {
        return UNIFIED_ERR_NULL;
    }

    if ((integrity_tag_length != 0u) && (integrity_tag == 0)) {
        return UNIFIED_ERR_NULL;
    }

    (void)memset(policy->auth_token, 0, sizeof(policy->auth_token));
    (void)memset(policy->integrity_tag, 0, sizeof(policy->integrity_tag));
    if (auth_token_length != 0u) {
        (void)memcpy(policy->auth_token, auth_token, auth_token_length);
    }
    if (integrity_tag_length != 0u) {
        (void)memcpy(policy->integrity_tag, integrity_tag, integrity_tag_length);
    }
    policy->auth_token_length = auth_token_length;
    policy->integrity_tag_length = integrity_tag_length;
    return UNIFIED_OK;
}

unified_error_t ingress_security_evaluate(ingress_security_policy_t *policy,
                                          const ingress_security_input_t *input,
                                          uint32_t *out_trust_flags)
{
    const anymsg_header_t *header; /**< anyMSG header。 */
    uint32_t trust_flags;          /**< 本次计算出的 trust flags。 */
    unified_error_t result;        /**< 解析或校验结果。 */

    if ((input == 0) || (out_trust_flags == 0)) {
        return UNIFIED_ERR_NULL;
    }

    *out_trust_flags = 0u;
    if ((policy == 0) || !policy->enabled) {
        /* 未配置策略时保持默认不可信，不阻止 IPC 入队。 */
        return UNIFIED_OK;
    }

    if (!is_external_interface(input->source_interface)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    result = parse_frame_header(input, &header);
    if (result != UNIFIED_OK) {
        return result;
    }

    (void)input->source_ipv4_be;
    (void)input->source_port;
    trust_flags = 0u;

    if (!policy->require_authentication ||
        verify_slice_matches(header,
                             INGRESS_SECURITY_AUTH_TOKEN_OFFSET,
                             policy->auth_token,
                             policy->auth_token_length)) {
        trust_flags |= PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK;
    }

    if (!policy->require_integrity ||
        verify_slice_matches(header,
                             INGRESS_SECURITY_INTEGRITY_TAG_OFFSET,
                             policy->integrity_tag,
                             policy->integrity_tag_length)) {
        trust_flags |= PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK;
    }

    if (!policy->require_replay_protection ||
        replay_check_and_update(policy, input, header)) {
        trust_flags |= PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK;
    }

    if (policy->allow_control_path && destination_is_control_path(header)) {
        trust_flags |= PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED;
    }

    *out_trust_flags = trust_flags & PUT_SHM_DESCRIPTOR_TRUST_FLAG_MASK;
    return UNIFIED_OK;
}

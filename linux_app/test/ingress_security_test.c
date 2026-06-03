/**
 * @file ingress_security_test.c
 * @brief Linux 外部入口安全策略 host 单测。
 * @author Yukikaze
 */
#include "ingress_security.h"

#include <stdio.h>
#include <string.h>

#include "anymsg_frame.h"

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

/** @brief 测试鉴权 token。 */
static const uint8_t k_auth_token[INGRESS_SECURITY_AUTH_TOKEN_MAX_LEN] = {
    'A', 'U', 'T', 'H', 'O', 'K', '0', '1',
};

/** @brief 测试完整性 tag。 */
static const uint8_t k_integrity_tag[INGRESS_SECURITY_INTEGRITY_TAG_MAX_LEN] = {
    'I', 'N', 'T', 'O', 'K', '0', '0', '1',
};

/**
 * @brief 写入 little-endian 16 位字段。
 *
 * @param bytes 输出字节。
 * @param value 输入值。
 */
static void write_le16(uint8_t bytes[2], uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xFFu);
}

/**
 * @brief 写入 little-endian 32 位字段。
 *
 * @param bytes 输出字节。
 * @param value 输入值。
 */
static void write_le32(uint8_t bytes[4], uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16u) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24u) & 0xFFu);
}

/**
 * @brief 构造测试 anyMSG。
 *
 * @param frame 输出帧。
 * @param source_first source CID 首字节。
 * @param destination_first destination CID 首字节。
 * @param sequence local_time/sequence。
 * @param valid_auth 是否写入正确 token。
 * @param valid_integrity 是否写入正确 tag。
 * @return 完整帧长度。
 */
static size_t make_anymsg(uint8_t *frame,
                          uint8_t source_first,
                          uint8_t destination_first,
                          uint32_t sequence,
                          bool valid_auth,
                          bool valid_integrity)
{
    anymsg_header_t *header; /**< anyMSG header。 */
    uint16_t length;         /**< 完整帧长度。 */

    length = ANYMSG_HEADER_SIZE;
    (void)memset(frame, 0, PUT_SHM_FRAME_POOL_BLOCK_SIZE);
    header = (anymsg_header_t *)frame;
    write_le16(header->msg_length, length);
    write_le16(header->payload_length, 0u);
    write_le32(header->local_time, sequence);
    header->source_cid[0] = source_first;
    header->destination_cid[0] = destination_first;
    header->type = ANYMSG_TYPE_RAW_CAN;
    if (valid_auth) {
        (void)memcpy(&header->verify_string[INGRESS_SECURITY_AUTH_TOKEN_OFFSET],
                     k_auth_token,
                     sizeof(k_auth_token));
    }
    if (valid_integrity) {
        (void)memcpy(&header->verify_string[INGRESS_SECURITY_INTEGRITY_TAG_OFFSET],
                     k_integrity_tag,
                     sizeof(k_integrity_tag));
    }

    return length;
}

/**
 * @brief 构造启用所有检查的策略。
 *
 * @param policy 输出策略。
 * @param allow_control_path 是否允许控制路径。
 * @return 0 表示成功。
 */
static int make_policy(ingress_security_policy_t *policy, bool allow_control_path)
{
    ingress_security_policy_init(policy);
    policy->enabled = true;
    policy->require_authentication = true;
    policy->require_integrity = true;
    policy->require_replay_protection = true;
    policy->allow_control_path = allow_control_path;
    policy->max_age_ms = 0u;
    CHECK(ingress_security_policy_set_mock_credentials(policy,
                                                       k_auth_token,
                                                       sizeof(k_auth_token),
                                                       k_integrity_tag,
                                                       sizeof(k_integrity_tag)) ==
          UNIFIED_OK);
    return 0;
}

/**
 * @brief 评估一帧测试 anyMSG。
 *
 * @param policy 策略。
 * @param frame 完整帧。
 * @param frame_len 帧长度。
 * @param out_flags 输出 flags。
 * @return 0 表示成功。
 */
static int evaluate_wifi_frame(ingress_security_policy_t *policy,
                               const uint8_t *frame,
                               size_t frame_len,
                               uint32_t *out_flags)
{
    ingress_security_input_t input; /**< 安全输入。 */

    (void)memset(&input, 0, sizeof(input));
    input.source_interface = PUT_SHM_INTERFACE_WIFI;
    input.frame = frame;
    input.frame_length = frame_len;
    input.now_ms = 0u;
    CHECK(ingress_security_evaluate(policy, &input, out_flags) == UNIFIED_OK);
    return 0;
}

/**
 * @brief 默认空策略不授予任何 trust flag。
 *
 * @return 0 表示通过。
 */
static int test_disabled_policy_outputs_zero(void)
{
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE]; /**< 测试帧。 */
    uint32_t flags;                               /**< 输出 flags。 */
    size_t frame_len;                             /**< 帧长度。 */

    frame_len = make_anymsg(frame, ANYMSG_CID_WIFI_MIN, ANYMSG_CID_CAN_MIN,
                            1u, true, true);
    CHECK(evaluate_wifi_frame(0, frame, frame_len, &flags) == 0);
    CHECK(flags == 0u);
    return 0;
}

/**
 * @brief 合法外部控制帧获得全部 trust flags。
 *
 * @return 0 表示通过。
 */
static int test_valid_control_frame_gets_all_flags(void)
{
    ingress_security_policy_t policy;             /**< 安全策略。 */
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE]; /**< 测试帧。 */
    uint32_t flags;                               /**< 输出 flags。 */
    size_t frame_len;                             /**< 帧长度。 */
    uint32_t expected;                            /**< 期望 flags。 */

    CHECK(make_policy(&policy, true) == 0);
    frame_len = make_anymsg(frame, ANYMSG_CID_WIFI_MIN, ANYMSG_CID_CAN_MIN,
                            10u, true, true);
    CHECK(evaluate_wifi_frame(&policy, frame, frame_len, &flags) == 0);
    expected = PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK |
               PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK |
               PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK |
               PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED;
    CHECK((flags & PUT_SHM_DESCRIPTOR_TRUST_FLAG_MASK) == expected);
    return 0;
}

/**
 * @brief 完整性 tag 失败时不授予 INTEGRITY_OK。
 *
 * @return 0 表示通过。
 */
static int test_integrity_failure_drops_integrity_flag(void)
{
    ingress_security_policy_t policy;             /**< 安全策略。 */
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE]; /**< 测试帧。 */
    uint32_t flags;                               /**< 输出 flags。 */
    size_t frame_len;                             /**< 帧长度。 */

    CHECK(make_policy(&policy, true) == 0);
    frame_len = make_anymsg(frame, ANYMSG_CID_WIFI_MIN, ANYMSG_CID_CAN_MIN,
                            11u, true, false);
    CHECK(evaluate_wifi_frame(&policy, frame, frame_len, &flags) == 0);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK) != 0u);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK) == 0u);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK) != 0u);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED) != 0u);
    return 0;
}

/**
 * @brief 重复 sequence 被判为 replay。
 *
 * @return 0 表示通过。
 */
static int test_replay_failure_drops_replay_flag(void)
{
    ingress_security_policy_t policy;             /**< 安全策略。 */
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE]; /**< 测试帧。 */
    uint32_t flags;                               /**< 输出 flags。 */
    size_t frame_len;                             /**< 帧长度。 */

    CHECK(make_policy(&policy, true) == 0);
    frame_len = make_anymsg(frame, ANYMSG_CID_WIFI_MIN, ANYMSG_CID_CAN_MIN,
                            12u, true, true);
    CHECK(evaluate_wifi_frame(&policy, frame, frame_len, &flags) == 0);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK) != 0u);

    CHECK(evaluate_wifi_frame(&policy, frame, frame_len, &flags) == 0);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK) != 0u);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK) != 0u);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK) == 0u);
    return 0;
}

/**
 * @brief 策略不允许控制路径时不授予 CONTROL_ALLOWED。
 *
 * @return 0 表示通过。
 */
static int test_control_path_requires_policy_allow(void)
{
    ingress_security_policy_t policy;             /**< 安全策略。 */
    uint8_t frame[PUT_SHM_FRAME_POOL_BLOCK_SIZE]; /**< 测试帧。 */
    uint32_t flags;                               /**< 输出 flags。 */
    size_t frame_len;                             /**< 帧长度。 */

    CHECK(make_policy(&policy, false) == 0);
    frame_len = make_anymsg(frame, ANYMSG_CID_WIFI_MIN, ANYMSG_CID_CAN_MIN,
                            13u, true, true);
    CHECK(evaluate_wifi_frame(&policy, frame, frame_len, &flags) == 0);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_AUTH_OK) != 0u);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_INTEGRITY_OK) != 0u);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_REPLAY_OK) != 0u);
    CHECK((flags & PUT_SHM_DESCRIPTOR_FLAG_CONTROL_ALLOWED) == 0u);
    return 0;
}

int main(void)
{
    CHECK(test_disabled_policy_outputs_zero() == 0);
    CHECK(test_valid_control_frame_gets_all_flags() == 0);
    CHECK(test_integrity_failure_drops_integrity_flag() == 0);
    CHECK(test_replay_failure_drops_replay_flag() == 0);
    CHECK(test_control_path_requires_policy_allow() == 0);
    puts("ingress_security_test: OK");
    return 0;
}

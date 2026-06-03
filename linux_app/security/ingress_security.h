/**
 * @file ingress_security.h
 * @brief Linux 外部入口 anyMSG 可信性评估接口。
 * @author Yukikaze
 */
#ifndef INGRESS_SECURITY_H
#define INGRESS_SECURITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_code.h"
#include "shared_memory_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief mock 鉴权 token 在 verify_string 中的起始偏移。 */
#define INGRESS_SECURITY_AUTH_TOKEN_OFFSET 0u

/** @brief mock 完整性 tag 在 verify_string 中的起始偏移。 */
#define INGRESS_SECURITY_INTEGRITY_TAG_OFFSET 8u

/** @brief mock 鉴权 token 最大长度。 */
#define INGRESS_SECURITY_AUTH_TOKEN_MAX_LEN 8u

/** @brief mock 完整性 tag 最大长度。 */
#define INGRESS_SECURITY_INTEGRITY_TAG_MAX_LEN 8u

/**
 * @brief Linux 外部入口安全策略。
 */
typedef struct {
    bool enabled;                                      /**< 是否启用该安全策略。 */
    bool require_authentication;                       /**< 是否要求鉴权 token 匹配。 */
    bool require_integrity;                            /**< 是否要求完整性 tag 匹配。 */
    bool require_replay_protection;                    /**< 是否要求 local_time 单调递增。 */
    bool allow_control_path;                           /**< 是否允许外部入口访问 CAN/RS485 控制路径。 */
    uint8_t auth_token[INGRESS_SECURITY_AUTH_TOKEN_MAX_LEN]; /**< 期望鉴权 token。 */
    size_t auth_token_length;                          /**< 鉴权 token 实际长度。 */
    uint8_t integrity_tag[INGRESS_SECURITY_INTEGRITY_TAG_MAX_LEN]; /**< 期望完整性 tag。 */
    size_t integrity_tag_length;                       /**< 完整性 tag 实际长度。 */
    uint32_t max_age_ms;                               /**< 最大时间窗，0 表示不检查 now_ms。 */
    uint32_t last_sequence[PUT_SHM_INTERFACE_COUNT];   /**< 每接口最近通过的 local_time/sequence。 */
    bool sequence_seen[PUT_SHM_INTERFACE_COUNT];       /**< 每接口是否已经见过 sequence。 */
} ingress_security_policy_t;

/**
 * @brief Linux 外部入口安全评估输入。
 */
typedef struct {
    put_shm_interface_t source_interface; /**< 来源物理接口。 */
    const uint8_t *frame;                 /**< 完整 anyMSG 起始地址。 */
    size_t frame_length;                  /**< 完整 anyMSG 字节数。 */
    uint32_t now_ms;                      /**< 当前 Linux 时间，单位毫秒；0 表示不参与时间窗判断。 */
    uint32_t source_ipv4_be;              /**< 来源 IPv4 地址，网络字节序；当前 mock 策略可忽略。 */
    uint16_t source_port;                 /**< 来源端口；当前 mock 策略可忽略。 */
} ingress_security_input_t;

/**
 * @brief 初始化入口安全策略为默认拒绝授权状态。
 *
 * @param policy 安全策略。
 */
void ingress_security_policy_init(ingress_security_policy_t *policy);

/**
 * @brief 配置 mock token 和完整性 tag。
 *
 * @param policy 安全策略。
 * @param auth_token 鉴权 token，可为 NULL。
 * @param auth_token_length 鉴权 token 长度。
 * @param integrity_tag 完整性 tag，可为 NULL。
 * @param integrity_tag_length 完整性 tag 长度。
 * @return UNIFIED_OK 表示配置成功，否则返回公共错误码。
 */
unified_error_t ingress_security_policy_set_mock_credentials(
    ingress_security_policy_t *policy,
    const uint8_t *auth_token,
    size_t auth_token_length,
    const uint8_t *integrity_tag,
    size_t integrity_tag_length);

/**
 * @brief 评估外部入口 anyMSG 并输出 descriptor trust flags。
 *
 * @param policy 安全策略；NULL 或 disabled 时输出 0。
 * @param input 安全评估输入。
 * @param out_trust_flags 输出 PUT_SHM_DESCRIPTOR_FLAG_* 组合。
 * @return UNIFIED_OK 表示评估完成，否则返回公共错误码。
 */
unified_error_t ingress_security_evaluate(ingress_security_policy_t *policy,
                                          const ingress_security_input_t *input,
                                          uint32_t *out_trust_flags);

#ifdef __cplusplus
}
#endif

#endif /* INGRESS_SECURITY_H */

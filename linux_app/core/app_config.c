/* linux_app 简单 INI 配置解析实现。 */
#include "app_config.h"

#include <ctype.h>
#include <errno.h>
#include <arpa/inet.h>
#include <linux/serial.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LINE_MAX 256u

static char *trim(char *text)
{
    char *end;

    if (text == NULL) {
        return NULL;
    }

    while (isspace((unsigned char)*text)) {
        ++text;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1u;
    while ((end > text) && isspace((unsigned char)*end)) {
        *end = '\0';
        --end;
    }

    return text;
}

static void strip_inline_comment(char *text)
{
    bool in_quote = false;

    if (text == NULL) {
        return;
    }

    for (char *p = text; *p != '\0'; ++p) {
        if (*p == '"') {
            in_quote = !in_quote;
            continue;
        }

        if (!in_quote && ((*p == '#') || (*p == ';'))) {
            *p = '\0';
            return;
        }
    }
}

static void unquote(char *text)
{
    size_t len;

    if (text == NULL) {
        return;
    }

    len = strlen(text);
    if ((len >= 2u) && (text[0] == '"') && (text[len - 1u] == '"')) {
        memmove(text, text + 1, len - 2u);
        text[len - 2u] = '\0';
    }
}

static int parse_bool(const char *text, bool *out_value)
{
    if ((text == NULL) || (out_value == NULL)) {
        return -1;
    }

    if ((strcmp(text, "true") == 0) || (strcmp(text, "1") == 0) ||
        (strcmp(text, "yes") == 0) || (strcmp(text, "on") == 0)) {
        *out_value = true;
        return 0;
    }

    if ((strcmp(text, "false") == 0) || (strcmp(text, "0") == 0) ||
        (strcmp(text, "no") == 0) || (strcmp(text, "off") == 0)) {
        *out_value = false;
        return 0;
    }

    return -1;
}

static int parse_u32(const char *text, uint32_t *out_value)
{
    char *end = NULL;
    unsigned long value;

    if ((text == NULL) || (out_value == NULL)) {
        return -1;
    }

    errno = 0;
    value = strtoul(text, &end, 0);
    if ((errno != 0) || (end == text) || (*end != '\0') || (value > UINT32_MAX)) {
        return -1;
    }

    *out_value = (uint32_t)value;
    return 0;
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if ((dst == NULL) || (dst_size == 0u)) {
        return;
    }

    (void)snprintf(dst, dst_size, "%s", (src == NULL) ? "" : src);
}

static bool is_valid_ipv4_address(const char *text)
{
    struct in_addr addr;

    if ((text == NULL) || (text[0] == '\0')) {
        return false;
    }

    return inet_pton(AF_INET, text, &addr) == 1;
}

static bool is_supported_rs485_baudrate(uint32_t baudrate)
{
    switch (baudrate) {
    case 9600u:
    case 19200u:
    case 38400u:
    case 57600u:
    case 115200u:
        return true;
    default:
        return false;
    }
}

static bool is_valid_rs485_flags(uint32_t flags)
{
    if (flags == 0u) {
        return true;
    }

    return (flags & (uint32_t)SER_RS485_ENABLED) != 0u;
}

static int parse_wifi_cid(const char *text, uint8_t cid[ANYMSG_CID_LENGTH])
{
    uint32_t value;

    if ((text == NULL) || (cid == NULL) || (parse_u32(text, &value) != 0)) {
        return -1;
    }

    cid[0] = (uint8_t)((value >> 24u) & 0xFFu);
    cid[1] = (uint8_t)((value >> 16u) & 0xFFu);
    cid[2] = (uint8_t)((value >> 8u) & 0xFFu);
    cid[3] = (uint8_t)(value & 0xFFu);

    return (anymsg_cid_segment_from_first_byte(cid[0]) == ANYMSG_CID_SEGMENT_WIFI) ? 0 : -1;
}

static int parse_four_g_cid(const char *text, uint8_t cid[ANYMSG_CID_LENGTH])
{
    uint32_t value;

    if ((text == NULL) || (cid == NULL) || (parse_u32(text, &value) != 0)) {
        return -1;
    }

    cid[0] = (uint8_t)((value >> 24u) & 0xFFu);
    cid[1] = (uint8_t)((value >> 16u) & 0xFFu);
    cid[2] = (uint8_t)((value >> 8u) & 0xFFu);
    cid[3] = (uint8_t)(value & 0xFFu);

    return (anymsg_cid_segment_from_first_byte(cid[0]) == ANYMSG_CID_SEGMENT_4G) ? 0 : -1;
}

static int parse_wifi_tx_peer(app_config_t *config, const char *value)
{
    char line[CONFIG_LINE_MAX];
    char *cid_text;
    char *ip_text;
    char *port_text;
    char *comma_a;
    char *comma_b;
    wifi_tx_peer_t peer;
    struct in_addr addr;
    uint32_t port_value;

    if ((config == NULL) || (value == NULL) || (strlen(value) >= sizeof(line)) ||
        (config->wifi_tx_peer_count >= WIFI_TX_PEER_MAX)) {
        return -1;
    }

    copy_string(line, sizeof(line), value);
    comma_a = strchr(line, ',');
    if (comma_a == NULL) {
        return -1;
    }
    *comma_a = '\0';

    comma_b = strchr(comma_a + 1, ',');
    if ((comma_b == NULL) || (strchr(comma_b + 1, ',') != NULL)) {
        return -1;
    }
    *comma_b = '\0';

    cid_text = trim(line);
    ip_text = trim(comma_a + 1);
    port_text = trim(comma_b + 1);
    if ((cid_text == NULL) || (cid_text[0] == '\0') ||
        (ip_text == NULL) || (ip_text[0] == '\0') ||
        (port_text == NULL) || (port_text[0] == '\0')) {
        return -1;
    }

    memset(&peer, 0, sizeof(peer));
    if (parse_wifi_cid(cid_text, peer.destination_cid) != 0) {
        return -1;
    }
    if ((inet_pton(AF_INET, ip_text, &addr) != 1) || (addr.s_addr == 0u)) {
        return -1;
    }
    if ((parse_u32(port_text, &port_value) != 0) || (port_value > UINT16_MAX)) {
        return -1;
    }

    peer.ipv4_addr_be = addr.s_addr;
    peer.port = (uint16_t)port_value;
    config->wifi_tx_peers[config->wifi_tx_peer_count] = peer;
    config->wifi_tx_peer_count++;
    return 0;
}

static int parse_four_g_tx_peer(app_config_t *config, const char *value)
{
    char line[CONFIG_LINE_MAX];
    char *cid_text;
    char *ip_text;
    char *port_text;
    char *comma_a;
    char *comma_b;
    four_g_tx_peer_t peer;
    struct in_addr addr;
    uint32_t port_value;

    if ((config == NULL) || (value == NULL) || (strlen(value) >= sizeof(line)) ||
        (config->four_g_tx_peer_count >= FOUR_G_TX_PEER_MAX)) {
        return -1;
    }

    copy_string(line, sizeof(line), value);
    comma_a = strchr(line, ',');
    if (comma_a == NULL) {
        return -1;
    }
    *comma_a = '\0';

    comma_b = strchr(comma_a + 1, ',');
    if ((comma_b == NULL) || (strchr(comma_b + 1, ',') != NULL)) {
        return -1;
    }
    *comma_b = '\0';

    cid_text = trim(line);
    ip_text = trim(comma_a + 1);
    port_text = trim(comma_b + 1);
    if ((cid_text == NULL) || (cid_text[0] == '\0') ||
        (ip_text == NULL) || (ip_text[0] == '\0') ||
        (port_text == NULL) || (port_text[0] == '\0')) {
        return -1;
    }

    memset(&peer, 0, sizeof(peer));
    if (parse_four_g_cid(cid_text, peer.destination_cid) != 0) {
        return -1;
    }
    if ((inet_pton(AF_INET, ip_text, &addr) != 1) || (addr.s_addr == 0u)) {
        return -1;
    }
    if ((parse_u32(port_text, &port_value) != 0) || (port_value > UINT16_MAX)) {
        return -1;
    }

    peer.ipv4_addr_be = addr.s_addr;
    peer.port = (uint16_t)port_value;
    config->four_g_tx_peers[config->four_g_tx_peer_count] = peer;
    config->four_g_tx_peer_count++;
    return 0;
}

void app_config_set_defaults(app_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->status_enabled = true;
    copy_string(config->status_dir, sizeof(config->status_dir), "/run/put/status");
    config->can_enabled = true;
    copy_string(config->can_ifname, sizeof(config->can_ifname), APP_CONFIG_CAN_DEFAULT_IFNAME);
    config->can_bitrate = APP_CONFIG_CAN_DEFAULT_BITRATE;
    config->can_tx_can_id = APP_CONFIG_CAN_DEFAULT_TX_CAN_ID;
    config->can_rx_filter_id = APP_CONFIG_CAN_DEFAULT_RX_FILTER_ID;
    config->can_rx_filter_mask = APP_CONFIG_CAN_DEFAULT_RX_FILTER_MASK;
    config->can_extended_id = false;
    config->can_reassembly_timeout_ms = APP_CONFIG_CAN_DEFAULT_REASSEMBLY_TIMEOUT_MS;
    config->ethernet_enabled = true;
    config->ethernet_udp_enabled = APP_CONFIG_ETHERNET_DEFAULT_UDP_ENABLED;
    config->ethernet_tcp_enabled = APP_CONFIG_ETHERNET_DEFAULT_TCP_ENABLED;
    copy_string(config->ethernet_bind_addr,
                sizeof(config->ethernet_bind_addr),
                APP_CONFIG_ETHERNET_DEFAULT_BIND_ADDR);
    config->ethernet_port = (uint16_t)APP_CONFIG_ETHERNET_DEFAULT_PORT;
    copy_string(config->ethernet_tx_peer_addr,
                sizeof(config->ethernet_tx_peer_addr),
                APP_CONFIG_ETHERNET_DEFAULT_TX_PEER_ADDR);
    config->ethernet_tx_peer_port = (uint16_t)APP_CONFIG_ETHERNET_DEFAULT_TX_PEER_PORT;
    config->wifi_enabled = true;
    config->wifi_udp_enabled = APP_CONFIG_WIFI_DEFAULT_UDP_ENABLED;
    config->wifi_tcp_enabled = APP_CONFIG_WIFI_DEFAULT_TCP_ENABLED;
    copy_string(config->wifi_bind_addr,
                sizeof(config->wifi_bind_addr),
                APP_CONFIG_WIFI_DEFAULT_BIND_ADDR);
    config->wifi_port = (uint16_t)APP_CONFIG_WIFI_DEFAULT_PORT;
    copy_string(config->wifi_tx_peer_addr,
                sizeof(config->wifi_tx_peer_addr),
                APP_CONFIG_WIFI_DEFAULT_TX_PEER_ADDR);
    config->wifi_tx_peer_port = (uint16_t)APP_CONFIG_WIFI_DEFAULT_TX_PEER_PORT;
    config->bluetooth_enabled = false;
    config->bluetooth_channel = 1u;
    config->four_g_enabled = false;
    copy_string(config->four_g_ifname,
                sizeof(config->four_g_ifname),
                APP_CONFIG_FOUR_G_DEFAULT_IFNAME);
    config->four_g_bind_to_device = APP_CONFIG_FOUR_G_DEFAULT_BIND_TO_DEVICE;
    config->four_g_udp_enabled = APP_CONFIG_FOUR_G_DEFAULT_UDP_ENABLED;
    config->four_g_tcp_enabled = APP_CONFIG_FOUR_G_DEFAULT_TCP_ENABLED;
    copy_string(config->four_g_bind_addr,
                sizeof(config->four_g_bind_addr),
                APP_CONFIG_FOUR_G_DEFAULT_BIND_ADDR);
    config->four_g_port = (uint16_t)APP_CONFIG_FOUR_G_DEFAULT_PORT;
    copy_string(config->four_g_tx_peer_addr,
                sizeof(config->four_g_tx_peer_addr),
                APP_CONFIG_FOUR_G_DEFAULT_TX_PEER_ADDR);
    config->four_g_tx_peer_port = (uint16_t)APP_CONFIG_FOUR_G_DEFAULT_TX_PEER_PORT;
    config->rs485_enabled = true;
    copy_string(config->rs485_uart_device,
                sizeof(config->rs485_uart_device),
                APP_CONFIG_RS485_DEFAULT_UART_DEVICE);
    config->rs485_baudrate = APP_CONFIG_RS485_DEFAULT_BAUDRATE;
    config->rs485_flags = (uint32_t)(SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND);
    config->max_packets = 0u;
}

static unified_error_t apply_key_value(app_config_t *config,
                                       const char *section,
                                       const char *key,
                                       const char *value)
{
    bool bool_value;
    uint32_t u32_value;

    if ((config == NULL) || (section == NULL) || (key == NULL) || (value == NULL)) {
        return UNIFIED_ERR_NULL;
    }

    if (strcmp(section, "status") == 0) {
        if (strcmp(key, "enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->status_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "dir") == 0) {
            if (value[0] == '\0') {
                return UNIFIED_ERR_INVALID_ARG;
            }
            copy_string(config->status_dir, sizeof(config->status_dir), value);
            return UNIFIED_OK;
        }
    } else if (strcmp(section, "can") == 0) {
        if (strcmp(key, "enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->can_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "ifname") == 0) {
            if (value[0] == '\0') {
                return UNIFIED_ERR_INVALID_ARG;
            }
            copy_string(config->can_ifname, sizeof(config->can_ifname), value);
            return UNIFIED_OK;
        }

        if (strcmp(key, "bitrate") == 0) {
            if ((parse_u32(value, &u32_value) != 0) || (u32_value == 0u)) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->can_bitrate = u32_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "rx_filter_id") == 0) {
            if (parse_u32(value, &u32_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->can_rx_filter_id = u32_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "tx_can_id") == 0) {
            if (parse_u32(value, &u32_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->can_tx_can_id = u32_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "rx_filter_mask") == 0) {
            if (parse_u32(value, &u32_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->can_rx_filter_mask = u32_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "extended_id") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->can_extended_id = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "reassembly_timeout_ms") == 0) {
            if (parse_u32(value, &u32_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->can_reassembly_timeout_ms = u32_value;
            return UNIFIED_OK;
        }
    } else if (strcmp(section, "ethernet") == 0) {
        if (strcmp(key, "enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->ethernet_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "udp_enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->ethernet_udp_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "tcp_enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->ethernet_tcp_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "bind_addr") == 0) {
            if (value[0] == '\0') {
                return UNIFIED_ERR_INVALID_ARG;
            }
            copy_string(config->ethernet_bind_addr, sizeof(config->ethernet_bind_addr), value);
            return UNIFIED_OK;
        }

        if (strcmp(key, "port") == 0) {
            if ((parse_u32(value, &u32_value) != 0) || (u32_value == 0u) || (u32_value > UINT16_MAX)) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->ethernet_port = (uint16_t)u32_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "tx_peer_addr") == 0) {
            copy_string(config->ethernet_tx_peer_addr,
                        sizeof(config->ethernet_tx_peer_addr),
                        value);
            return UNIFIED_OK;
        }

        if (strcmp(key, "tx_peer_port") == 0) {
            if ((parse_u32(value, &u32_value) != 0) || (u32_value == 0u) || (u32_value > UINT16_MAX)) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->ethernet_tx_peer_port = (uint16_t)u32_value;
            return UNIFIED_OK;
        }
    } else if (strcmp(section, "wifi") == 0) {
        if (strcmp(key, "enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->wifi_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "udp_enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->wifi_udp_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "tcp_enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->wifi_tcp_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "bind_addr") == 0) {
            if (value[0] == '\0') {
                return UNIFIED_ERR_INVALID_ARG;
            }
            copy_string(config->wifi_bind_addr, sizeof(config->wifi_bind_addr), value);
            return UNIFIED_OK;
        }

        if (strcmp(key, "port") == 0) {
            if ((parse_u32(value, &u32_value) != 0) || (u32_value == 0u) || (u32_value > UINT16_MAX)) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->wifi_port = (uint16_t)u32_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "tx_peer_addr") == 0) {
            copy_string(config->wifi_tx_peer_addr,
                        sizeof(config->wifi_tx_peer_addr),
                        value);
            return UNIFIED_OK;
        }

        if (strcmp(key, "tx_peer_port") == 0) {
            if ((parse_u32(value, &u32_value) != 0) || (u32_value == 0u) || (u32_value > UINT16_MAX)) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->wifi_tx_peer_port = (uint16_t)u32_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "tx_peer") == 0) {
            if (parse_wifi_tx_peer(config, value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            return UNIFIED_OK;
        }
    } else if ((strcmp(section, "4g") == 0) || (strcmp(section, "four_g") == 0)) {
        if (strcmp(key, "enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->four_g_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "ifname") == 0) {
            if (value[0] == '\0') {
                return UNIFIED_ERR_INVALID_ARG;
            }
            copy_string(config->four_g_ifname, sizeof(config->four_g_ifname), value);
            return UNIFIED_OK;
        }

        if (strcmp(key, "bind_to_device") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->four_g_bind_to_device = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "udp_enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->four_g_udp_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "tcp_enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->four_g_tcp_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "bind_addr") == 0) {
            if (value[0] == '\0') {
                return UNIFIED_ERR_INVALID_ARG;
            }
            copy_string(config->four_g_bind_addr, sizeof(config->four_g_bind_addr), value);
            return UNIFIED_OK;
        }

        if (strcmp(key, "port") == 0) {
            if ((parse_u32(value, &u32_value) != 0) || (u32_value == 0u) || (u32_value > UINT16_MAX)) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->four_g_port = (uint16_t)u32_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "tx_peer_addr") == 0) {
            copy_string(config->four_g_tx_peer_addr,
                        sizeof(config->four_g_tx_peer_addr),
                        value);
            return UNIFIED_OK;
        }

        if (strcmp(key, "tx_peer_port") == 0) {
            if ((parse_u32(value, &u32_value) != 0) || (u32_value == 0u) || (u32_value > UINT16_MAX)) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->four_g_tx_peer_port = (uint16_t)u32_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "tx_peer") == 0) {
            if (parse_four_g_tx_peer(config, value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            return UNIFIED_OK;
        }
    } else if (strcmp(section, "bluetooth") == 0) {
        if (strcmp(key, "enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->bluetooth_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "channel") == 0) {
            if ((parse_u32(value, &u32_value) != 0) || (u32_value == 0u) || (u32_value > UINT8_MAX)) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->bluetooth_channel = (uint8_t)u32_value;
            return UNIFIED_OK;
        }
    } else if (strcmp(section, "rs485") == 0) {
        if (strcmp(key, "enabled") == 0) {
            if (parse_bool(value, &bool_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->rs485_enabled = bool_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "uart_device") == 0) {
            copy_string(config->rs485_uart_device, sizeof(config->rs485_uart_device), value);
            return UNIFIED_OK;
        }

        if (strcmp(key, "baudrate") == 0) {
            if (parse_u32(value, &u32_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->rs485_baudrate = u32_value;
            return UNIFIED_OK;
        }

        if (strcmp(key, "rs485_flags") == 0) {
            if (parse_u32(value, &u32_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->rs485_flags = u32_value;
            return UNIFIED_OK;
        }
    } else if (strcmp(section, "runtime") == 0) {
        if (strcmp(key, "max_packets") == 0) {
            if (parse_u32(value, &u32_value) != 0) {
                return UNIFIED_ERR_INVALID_ARG;
            }
            config->max_packets = u32_value;
            return UNIFIED_OK;
        }
    }

    return UNIFIED_OK;
}

unified_error_t app_config_load_file(app_config_t *config, const char *path)
{
    FILE *fp;
    char line[CONFIG_LINE_MAX];
    char section[64] = "";
    unsigned int line_no = 0u;

    if ((config == NULL) || (path == NULL) || (path[0] == '\0')) {
        return UNIFIED_ERR_NULL;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *work;
        char *eq;
        char *key;
        char *value;
        size_t len;

        line_no++;
        strip_inline_comment(line);
        work = trim(line);

        if ((work == NULL) || (work[0] == '\0')) {
            continue;
        }

        len = strlen(work);
        if ((work[0] == '[') && (len >= 3u) && (work[len - 1u] == ']')) {
            work[len - 1u] = '\0';
            copy_string(section, sizeof(section), trim(work + 1));
            continue;
        }

        eq = strchr(work, '=');
        if ((eq == NULL) || (section[0] == '\0')) {
            fprintf(stderr, "invalid config line %u in %s\n", line_no, path);
            (void)fclose(fp);
            return UNIFIED_ERR_INVALID_ARG;
        }

        *eq = '\0';
        key = trim(work);
        value = trim(eq + 1);
        unquote(value);

        if (apply_key_value(config, section, key, value) != UNIFIED_OK) {
            fprintf(stderr, "invalid config value at line %u in %s\n", line_no, path);
            (void)fclose(fp);
            return UNIFIED_ERR_INVALID_ARG;
        }
    }

    if (ferror(fp)) {
        (void)fclose(fp);
        return UNIFIED_ERR_INVALID_ARG;
    }

    (void)fclose(fp);
    return app_config_validate(config);
}

unified_error_t app_config_validate(const app_config_t *config)
{
    uint32_t can_id_max;

    if (config == NULL) {
        return UNIFIED_ERR_NULL;
    }

    if (config->status_enabled && (config->status_dir[0] == '\0')) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (config->can_enabled) {
        can_id_max = config->can_extended_id ? 0x1FFFFFFFu : 0x7FFu;
        if ((config->can_ifname[0] == '\0') ||
            (config->can_bitrate == 0u) ||
            (config->can_tx_can_id > can_id_max) ||
            (config->can_rx_filter_id > can_id_max) ||
            (config->can_rx_filter_mask > can_id_max) ||
            (config->can_reassembly_timeout_ms < 100u) ||
            (config->can_reassembly_timeout_ms > 5000u)) {
            return UNIFIED_ERR_INVALID_ARG;
        }
    }

    if (config->ethernet_enabled) {
        if ((config->ethernet_bind_addr[0] == '\0') || (config->ethernet_port == 0u)) {
            return UNIFIED_ERR_INVALID_ARG;
        }
        if (!config->ethernet_udp_enabled && !config->ethernet_tcp_enabled) {
            return UNIFIED_ERR_INVALID_ARG;
        }
        if (config->ethernet_tx_peer_addr[0] != '\0') {
            if ((config->ethernet_tx_peer_port == 0u) ||
                !is_valid_ipv4_address(config->ethernet_tx_peer_addr)) {
                return UNIFIED_ERR_INVALID_ARG;
            }
        }
    }

    if (config->wifi_enabled) {
        if ((config->wifi_bind_addr[0] == '\0') || (config->wifi_port == 0u)) {
            return UNIFIED_ERR_INVALID_ARG;
        }
        if (!config->wifi_udp_enabled && !config->wifi_tcp_enabled) {
            return UNIFIED_ERR_INVALID_ARG;
        }
        if (config->wifi_tx_peer_addr[0] != '\0') {
            if ((config->wifi_tx_peer_port == 0u) ||
                !is_valid_ipv4_address(config->wifi_tx_peer_addr)) {
                return UNIFIED_ERR_INVALID_ARG;
            }
        }
    }

    if (config->wifi_tx_peer_count > WIFI_TX_PEER_MAX) {
        return UNIFIED_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < config->wifi_tx_peer_count; ++i) {
        const wifi_tx_peer_t *peer = &config->wifi_tx_peers[i];

        if ((anymsg_cid_segment_from_first_byte(peer->destination_cid[0]) !=
             ANYMSG_CID_SEGMENT_WIFI) ||
            (peer->ipv4_addr_be == 0u) ||
            ((peer->port == 0u) && (config->wifi_port == 0u))) {
            return UNIFIED_ERR_INVALID_ARG;
        }
    }

    if (config->bluetooth_enabled && (config->bluetooth_channel == 0u)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (config->four_g_enabled) {
        if ((config->four_g_ifname[0] == '\0') ||
            (config->four_g_bind_addr[0] == '\0') ||
            (config->four_g_port == 0u)) {
            return UNIFIED_ERR_INVALID_ARG;
        }
        if (!config->four_g_udp_enabled && !config->four_g_tcp_enabled) {
            return UNIFIED_ERR_INVALID_ARG;
        }
        if (config->four_g_tx_peer_addr[0] != '\0') {
            if ((config->four_g_tx_peer_port == 0u) ||
                !is_valid_ipv4_address(config->four_g_tx_peer_addr)) {
                return UNIFIED_ERR_INVALID_ARG;
            }
        }
    }

    if (config->four_g_tx_peer_count > FOUR_G_TX_PEER_MAX) {
        return UNIFIED_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < config->four_g_tx_peer_count; ++i) {
        const four_g_tx_peer_t *peer = &config->four_g_tx_peers[i];

        if ((anymsg_cid_segment_from_first_byte(peer->destination_cid[0]) !=
             ANYMSG_CID_SEGMENT_4G) ||
            (peer->ipv4_addr_be == 0u) ||
            ((peer->port == 0u) && (config->four_g_port == 0u))) {
            return UNIFIED_ERR_INVALID_ARG;
        }
    }

    if (config->rs485_enabled) {
        if ((config->rs485_uart_device[0] == '\0') ||
            !is_supported_rs485_baudrate(config->rs485_baudrate) ||
            !is_valid_rs485_flags(config->rs485_flags)) {
            return UNIFIED_ERR_INVALID_ARG;
        }
    }

    return UNIFIED_OK;
}

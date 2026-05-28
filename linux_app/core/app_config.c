/* linux_app 简单 INI 配置解析实现。 */
#include "app_config.h"

#include <ctype.h>
#include <errno.h>
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

void app_config_set_defaults(app_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->status_enabled = true;
    copy_string(config->status_dir, sizeof(config->status_dir), "/run/put/status");
    config->ethernet_enabled = true;
    config->ethernet_udp_enabled = APP_CONFIG_ETHERNET_DEFAULT_UDP_ENABLED;
    config->ethernet_tcp_enabled = APP_CONFIG_ETHERNET_DEFAULT_TCP_ENABLED;
    copy_string(config->ethernet_bind_addr,
                sizeof(config->ethernet_bind_addr),
                APP_CONFIG_ETHERNET_DEFAULT_BIND_ADDR);
    config->ethernet_port = (uint16_t)APP_CONFIG_ETHERNET_DEFAULT_PORT;
    config->wifi_enabled = false;
    config->wifi_udp_enabled = APP_CONFIG_WIFI_DEFAULT_UDP_ENABLED;
    config->wifi_tcp_enabled = APP_CONFIG_WIFI_DEFAULT_TCP_ENABLED;
    copy_string(config->wifi_bind_addr,
                sizeof(config->wifi_bind_addr),
                APP_CONFIG_WIFI_DEFAULT_BIND_ADDR);
    config->wifi_port = (uint16_t)APP_CONFIG_WIFI_DEFAULT_PORT;
    config->bluetooth_enabled = false;
    config->bluetooth_channel = 1u;
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
    if (config == NULL) {
        return UNIFIED_ERR_NULL;
    }

    if (config->status_enabled && (config->status_dir[0] == '\0')) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (config->ethernet_enabled) {
        if ((config->ethernet_bind_addr[0] == '\0') || (config->ethernet_port == 0u)) {
            return UNIFIED_ERR_INVALID_ARG;
        }
        if (!config->ethernet_udp_enabled && !config->ethernet_tcp_enabled) {
            return UNIFIED_ERR_INVALID_ARG;
        }
    }

    if (config->wifi_enabled) {
        if ((config->wifi_bind_addr[0] == '\0') || (config->wifi_port == 0u)) {
            return UNIFIED_ERR_INVALID_ARG;
        }
        if (!config->wifi_udp_enabled && !config->wifi_tcp_enabled) {
            return UNIFIED_ERR_INVALID_ARG;
        }
    }

    if (config->bluetooth_enabled && (config->bluetooth_channel == 0u)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

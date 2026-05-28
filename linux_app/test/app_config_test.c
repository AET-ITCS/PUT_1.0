#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            (void)fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, \
                          #condition);                                              \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static int test_default_ethernet_config(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    CHECK(!config.can_enabled);
    CHECK(strcmp(config.can_ifname, "can0") == 0);
    CHECK(config.can_bitrate == 500000u);
    CHECK(config.can_rx_filter_id == 0x320u);
    CHECK(config.can_rx_filter_mask == 0x7FFu);
    CHECK(!config.can_extended_id);
    CHECK(config.can_reassembly_timeout_ms == 500u);
    CHECK(config.ethernet_enabled);
    CHECK(config.ethernet_udp_enabled);
    CHECK(config.ethernet_tcp_enabled);
    CHECK(strcmp(config.ethernet_bind_addr, "0.0.0.0") == 0);
    CHECK(config.ethernet_port == 5000u);
    CHECK(app_config_validate(&config) == UNIFIED_OK);
    return 0;
}

static int test_load_can_config(void)
{
    app_config_t config;
    char path[128];
    FILE *fp;

    (void)snprintf(path, sizeof(path), "/tmp/put_app_config_can_test_%ld.ini", (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[can]\n", fp);
    fputs("enabled = true\n", fp);
    fputs("ifname = \"vcan0\"\n", fp);
    fputs("bitrate = 250000\n", fp);
    fputs("rx_filter_id = 0x123456\n", fp);
    fputs("rx_filter_mask = 0x1FFFFFFF\n", fp);
    fputs("extended_id = true\n", fp);
    fputs("reassembly_timeout_ms = 1000\n", fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_OK);
    CHECK(config.can_enabled);
    CHECK(strcmp(config.can_ifname, "vcan0") == 0);
    CHECK(config.can_bitrate == 250000u);
    CHECK(config.can_rx_filter_id == 0x123456u);
    CHECK(config.can_rx_filter_mask == 0x1FFFFFFFu);
    CHECK(config.can_extended_id);
    CHECK(config.can_reassembly_timeout_ms == 1000u);
    (void)remove(path);
    return 0;
}

static int test_load_ethernet_config(void)
{
    app_config_t config;
    char path[128];
    FILE *fp;

    (void)snprintf(path, sizeof(path), "/tmp/put_app_config_test_%ld.ini", (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[status]\n", fp);
    fputs("enabled = false\n", fp);
    fputs("dir = \"/tmp/put_status\"\n", fp);
    fputs("[ethernet]\n", fp);
    fputs("enabled = false\n", fp);
    fputs("udp_enabled = false\n", fp);
    fputs("tcp_enabled = true\n", fp);
    fputs("bind_addr = \"127.0.0.1\"\n", fp);
    fputs("port = 6000\n", fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_OK);
    CHECK(!config.status_enabled);
    CHECK(!config.ethernet_enabled);
    CHECK(!config.ethernet_udp_enabled);
    CHECK(config.ethernet_tcp_enabled);
    CHECK(strcmp(config.ethernet_bind_addr, "127.0.0.1") == 0);
    CHECK(config.ethernet_port == 6000u);
    (void)remove(path);
    return 0;
}

static int test_reject_invalid_ethernet_port(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.ethernet_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

static int test_reject_ethernet_with_no_transport(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.ethernet_udp_enabled = false;
    config.ethernet_tcp_enabled = false;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

static int test_reject_invalid_can_config(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.can_enabled = true;
    config.can_rx_filter_id = 0x800u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.can_enabled = true;
    config.can_reassembly_timeout_ms = 99u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.can_enabled = true;
    config.can_ifname[0] = '\0';
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

int main(void)
{
    if (test_default_ethernet_config() != 0) {
        return 1;
    }
    if (test_load_can_config() != 0) {
        return 1;
    }
    if (test_load_ethernet_config() != 0) {
        return 1;
    }
    if (test_reject_invalid_ethernet_port() != 0) {
        return 1;
    }
    if (test_reject_ethernet_with_no_transport() != 0) {
        return 1;
    }
    if (test_reject_invalid_can_config() != 0) {
        return 1;
    }
    puts("app_config_test: OK");
    return 0;
}

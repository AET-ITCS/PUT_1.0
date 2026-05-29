#include "app_config.h"

#include <arpa/inet.h>
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
    CHECK(config.can_tx_can_id == 0x321u);
    CHECK(config.can_rx_filter_id == 0x320u);
    CHECK(config.can_rx_filter_mask == 0x7FFu);
    CHECK(!config.can_extended_id);
    CHECK(config.can_reassembly_timeout_ms == 500u);
    CHECK(config.ethernet_enabled);
    CHECK(config.ethernet_udp_enabled);
    CHECK(config.ethernet_tcp_enabled);
    CHECK(strcmp(config.ethernet_bind_addr, "0.0.0.0") == 0);
    CHECK(config.ethernet_port == 5000u);
    CHECK(strcmp(config.ethernet_tx_peer_addr, "") == 0);
    CHECK(config.ethernet_tx_peer_port == 5000u);
    CHECK(!config.wifi_enabled);
    CHECK(config.wifi_udp_enabled);
    CHECK(config.wifi_tcp_enabled);
    CHECK(strcmp(config.wifi_bind_addr, "0.0.0.0") == 0);
    CHECK(config.wifi_port == 5001u);
    CHECK(strcmp(config.wifi_tx_peer_addr, "") == 0);
    CHECK(config.wifi_tx_peer_port == 5001u);
    CHECK(config.wifi_tx_peer_count == 0u);
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
    fputs("tx_can_id = 0x123457\n", fp);
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
    CHECK(config.can_tx_can_id == 0x123457u);
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
    struct in_addr expected_addr;

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
    fputs("tx_peer_addr = \"127.0.0.10\"\n", fp);
    fputs("tx_peer_port = 7000\n", fp);
    fputs("[wifi]\n", fp);
    fputs("enabled = true\n", fp);
    fputs("udp_enabled = true\n", fp);
    fputs("tcp_enabled = false\n", fp);
    fputs("bind_addr = \"127.0.0.2\"\n", fp);
    fputs("port = 6001\n", fp);
    fputs("tx_peer_addr = \"127.0.0.11\"\n", fp);
    fputs("tx_peer_port = 7001\n", fp);
    fputs("tx_peer = \"0x61000001,192.168.1.50,5001\"\n", fp);
    fputs("tx_peer = \"0x62000002,127.0.0.1,0\"\n", fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_OK);
    CHECK(!config.status_enabled);
    CHECK(!config.ethernet_enabled);
    CHECK(!config.ethernet_udp_enabled);
    CHECK(config.ethernet_tcp_enabled);
    CHECK(strcmp(config.ethernet_bind_addr, "127.0.0.1") == 0);
    CHECK(config.ethernet_port == 6000u);
    CHECK(strcmp(config.ethernet_tx_peer_addr, "127.0.0.10") == 0);
    CHECK(config.ethernet_tx_peer_port == 7000u);
    CHECK(config.wifi_enabled);
    CHECK(config.wifi_udp_enabled);
    CHECK(!config.wifi_tcp_enabled);
    CHECK(strcmp(config.wifi_bind_addr, "127.0.0.2") == 0);
    CHECK(config.wifi_port == 6001u);
    CHECK(strcmp(config.wifi_tx_peer_addr, "127.0.0.11") == 0);
    CHECK(config.wifi_tx_peer_port == 7001u);
    CHECK(config.wifi_tx_peer_count == 2u);
    CHECK(config.wifi_tx_peers[0].destination_cid[0] == 0x61u);
    CHECK(config.wifi_tx_peers[0].destination_cid[1] == 0x00u);
    CHECK(config.wifi_tx_peers[0].destination_cid[2] == 0x00u);
    CHECK(config.wifi_tx_peers[0].destination_cid[3] == 0x01u);
    CHECK(config.wifi_tx_peers[0].port == 5001u);
    CHECK(inet_pton(AF_INET, "192.168.1.50", &expected_addr) == 1);
    CHECK(config.wifi_tx_peers[0].ipv4_addr_be == expected_addr.s_addr);
    CHECK(config.wifi_tx_peers[1].destination_cid[0] == 0x62u);
    CHECK(config.wifi_tx_peers[1].destination_cid[3] == 0x02u);
    CHECK(config.wifi_tx_peers[1].port == 0u);
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
    config.can_tx_can_id = 0x800u;
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

static int test_reject_wifi_with_no_transport(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.wifi_enabled = true;
    config.wifi_udp_enabled = false;
    config.wifi_tcp_enabled = false;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

static int test_reject_invalid_tx_peer(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.ethernet_enabled = true;
    (void)snprintf(config.ethernet_tx_peer_addr,
                   sizeof(config.ethernet_tx_peer_addr),
                   "%s",
                   "not-an-ip");
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.ethernet_enabled = true;
    (void)snprintf(config.ethernet_tx_peer_addr,
                   sizeof(config.ethernet_tx_peer_addr),
                   "%s",
                   "127.0.0.1");
    config.ethernet_tx_peer_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.wifi_enabled = true;
    (void)snprintf(config.wifi_tx_peer_addr,
                   sizeof(config.wifi_tx_peer_addr),
                   "%s",
                   "not-an-ip");
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);

    app_config_set_defaults(&config);
    config.wifi_enabled = true;
    (void)snprintf(config.wifi_tx_peer_addr,
                   sizeof(config.wifi_tx_peer_addr),
                   "%s",
                   "127.0.0.1");
    config.wifi_tx_peer_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_ERR_INVALID_ARG);
    return 0;
}

static int expect_invalid_wifi_peer_line(const char *line)
{
    app_config_t config;
    char path[128];
    FILE *fp;

    (void)snprintf(path,
                   sizeof(path),
                   "/tmp/put_app_config_bad_wifi_peer_%ld.ini",
                   (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[wifi]\n", fp);
    fputs(line, fp);
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_ERR_INVALID_ARG);
    (void)remove(path);
    return 0;
}

static int test_reject_invalid_wifi_tx_peer(void)
{
    app_config_t config;
    char path[128];
    FILE *fp;

    CHECK(expect_invalid_wifi_peer_line("tx_peer = \"0x20000001,127.0.0.1,5001\"\n") == 0);
    CHECK(expect_invalid_wifi_peer_line("tx_peer = \"0x61000001,not-an-ip,5001\"\n") == 0);
    CHECK(expect_invalid_wifi_peer_line("tx_peer = \"0x61000001,127.0.0.1,65536\"\n") == 0);

    (void)snprintf(path,
                   sizeof(path),
                   "/tmp/put_app_config_many_wifi_peer_%ld.ini",
                   (long)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[wifi]\n", fp);
    for (uint32_t i = 0u; i <= WIFI_TX_PEER_MAX; ++i) {
        fprintf(fp, "tx_peer = \"0x%02X0000%02X,127.0.0.1,5001\"\n",
                (unsigned)(0x60u + (i & 0x0Fu)),
                (unsigned)i);
    }
    CHECK(fclose(fp) == 0);

    app_config_set_defaults(&config);
    CHECK(app_config_load_file(&config, path) == UNIFIED_ERR_INVALID_ARG);
    (void)remove(path);
    return 0;
}

static int test_ignore_tx_peer_when_interface_disabled(void)
{
    app_config_t config;

    app_config_set_defaults(&config);
    config.ethernet_enabled = false;
    (void)snprintf(config.ethernet_tx_peer_addr,
                   sizeof(config.ethernet_tx_peer_addr),
                   "%s",
                   "not-an-ip");
    config.ethernet_tx_peer_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_OK);

    app_config_set_defaults(&config);
    config.wifi_enabled = false;
    (void)snprintf(config.wifi_tx_peer_addr,
                   sizeof(config.wifi_tx_peer_addr),
                   "%s",
                   "not-an-ip");
    config.wifi_tx_peer_port = 0u;
    CHECK(app_config_validate(&config) == UNIFIED_OK);
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
    if (test_reject_wifi_with_no_transport() != 0) {
        return 1;
    }
    if (test_reject_invalid_tx_peer() != 0) {
        return 1;
    }
    if (test_reject_invalid_wifi_tx_peer() != 0) {
        return 1;
    }
    if (test_ignore_tx_peer_when_interface_disabled() != 0) {
        return 1;
    }
    puts("app_config_test: OK");
    return 0;
}

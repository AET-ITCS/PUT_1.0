/* linux_app 入口：解析启动参数并启动大核 UDP 协议转换主循环。 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_packer.h"
#include "protocol_manager.h"
#include "bluetooth_server.h"

#define DEFAULT_UDP_PORT 5000u

static void print_usage(const char *program)
{
    printf("Usage: %s [--udp-port PORT] [--max-packets N] [--status-dir DIR] [--disable-status]\n", program);
    printf("  --udp-port PORT   UDP listen port, default 5000\n");
    printf("  --max-packets N   stop after N packets, default 0 means forever\n");
    printf("  --status-dir DIR  write Web snapshots to DIR, default /run/put/status\n");
    printf("  --disable-status  do not write Web status snapshot files\n");
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

int main(int argc, char **argv)
{
    uint32_t port = DEFAULT_UDP_PORT;
    uint32_t max_packets = 0u;
    const char *status_dir = NULL;
    bool status_enabled = true;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }

        if (strcmp(argv[i], "--udp-port") == 0) {
            if ((i + 1) >= argc || parse_u32(argv[++i], &port) != 0 || port == 0u || port > UINT16_MAX) {
                fprintf(stderr, "invalid --udp-port\n");
                return EXIT_FAILURE;
            }
            continue;
        }

        if (strcmp(argv[i], "--max-packets") == 0) {
            if ((i + 1) >= argc || parse_u32(argv[++i], &max_packets) != 0) {
                fprintf(stderr, "invalid --max-packets\n");
                return EXIT_FAILURE;
            }
            continue;
        }

        if (strcmp(argv[i], "--status-dir") == 0) {
            if ((i + 1) >= argc || argv[i + 1][0] == '\0') {
                fprintf(stderr, "invalid --status-dir\n");
                return EXIT_FAILURE;
            }
            status_dir = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--disable-status") == 0) {
            status_enabled = false;
            continue;
        }

        fprintf(stderr, "unknown argument: %s\n", argv[i]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    frame_packer_init(1u);

    // 异步启动蓝牙 RFCOMM SPP 服务子线程
    if (bluetooth_server_start(status_dir, status_enabled) != 0) {
        fprintf(stderr, "Warning: failed to start bluetooth server thread\n");
    }

    unified_error_t err = protocol_manager_run_udp_with_status((uint16_t)port, max_packets, status_dir, status_enabled);

    // 程序退出前，优雅停止蓝牙服务并销毁/回收子线程资源
    bluetooth_server_stop();

    return (err == UNIFIED_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}

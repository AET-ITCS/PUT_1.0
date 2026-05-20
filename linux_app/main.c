/* linux_app 入口：解析启动参数并启动大核 UDP 协议转换主循环。 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_packer.h"
#include "protocol_manager.h"

#define DEFAULT_UDP_PORT 5000u

static void print_usage(const char *program)
{
    printf("Usage: %s [--udp-port PORT] [--max-packets N]\n", program);
    printf("  --udp-port PORT   UDP listen port, default 5000\n");
    printf("  --max-packets N   stop after N packets, default 0 means forever\n");
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

        fprintf(stderr, "unknown argument: %s\n", argv[i]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    frame_packer_init(1u);
    return (protocol_manager_run_udp((uint16_t)port, max_packets) == UNIFIED_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}

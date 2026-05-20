/* 协议管理实现：监听以太网 UDP 数据并串联解析、打包、发送流程。 */
#define _POSIX_C_SOURCE 200809L

#include "protocol_manager.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ethernet_udp.h"
#include "frame_packer.h"
#include "ipc_to_rtos.h"

static void log_error(const char *stage, unified_error_t err)
{
    fprintf(stderr, "[%s] error=%d\n", stage, (int)err);
}

unified_error_t protocol_manager_run_udp(uint16_t port, uint32_t max_packets)
{
    int fd;
    struct sockaddr_in addr;
    uint32_t packet_count = 0u;

    if (port == 0u) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return UNIFIED_ERR_INVALID_ARG;
    }

    printf("linux_app UDP protocol manager listening on 0.0.0.0:%u\n", (unsigned)port);

    while ((max_packets == 0u) || (packet_count < max_packets)) {
        uint8_t buffer[256];
        ssize_t received;
        protocol_parsed_msg_t parsed_msg;
        unified_frame_t frame;
        unified_error_t err;

        received = recvfrom(fd, buffer, sizeof(buffer), 0, NULL, NULL);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recvfrom");
            close(fd);
            return UNIFIED_ERR_INVALID_ARG;
        }

        packet_count++;

        err = ethernet_udp_parse_frame(buffer, (size_t)received, &parsed_msg);
        if (err != UNIFIED_OK) {
            log_error("ethernet_udp_parse_frame", err);
            continue;
        }

        err = frame_packer_pack(&parsed_msg, &frame);
        if (err != UNIFIED_OK) {
            log_error("frame_packer_pack", err);
            continue;
        }

        err = ipc_to_rtos_send(&frame);
        if (err != UNIFIED_OK) {
            log_error("ipc_to_rtos_send", err);
            continue;
        }
    }

    close(fd);
    return UNIFIED_OK;
}

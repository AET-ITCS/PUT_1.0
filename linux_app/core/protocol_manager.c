/* 协议管理实现：监听以太网 UDP 数据并串联解析、打包、发送流程。 */
#define _POSIX_C_SOURCE 200809L

#include "protocol_manager.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ethernet_status.h"
#include "ethernet_udp.h"
#include "frame_packer.h"
#include "ipc_to_rtos.h"

static void log_error(const char *stage, unified_error_t err)
{
    fprintf(stderr, "[%s] error=%d\n", stage, (int)err);
}

static void write_status_snapshot(ethernet_status_t *status, bool *warned)
{
    if ((status == NULL) || !status->enabled) {
        return;
    }

    if ((ethernet_status_write_all(status) != 0) && (warned != NULL) && !(*warned)) {
        fprintf(stderr,
                "[ethernet_status] failed to write status snapshots under %s; "
                "use --status-dir or --disable-status if needed\n",
                status->status_dir);
        *warned = true;
    }
}

unified_error_t protocol_manager_run_udp(uint16_t port, uint32_t max_packets)
{
    return protocol_manager_run_udp_with_status(port, max_packets, NULL, true);
}

unified_error_t protocol_manager_run_udp_with_status(uint16_t port,
                                                     uint32_t max_packets,
                                                     const char *status_dir,
                                                     bool status_enabled)
{
    int fd;
    struct sockaddr_in addr;
    uint32_t packet_count = 0u;
    ethernet_status_t status;
    bool status_warning_printed = false;

    if (port == 0u) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    ethernet_status_init(&status, status_dir, port, status_enabled);
    write_status_snapshot(&status, &status_warning_printed);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        ethernet_status_record_error(&status, "socket", UNIFIED_ERR_INVALID_ARG);
        write_status_snapshot(&status, &status_warning_printed);
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        ethernet_status_record_error(&status, "bind", UNIFIED_ERR_INVALID_ARG);
        write_status_snapshot(&status, &status_warning_printed);
        close(fd);
        return UNIFIED_ERR_INVALID_ARG;
    }

    printf("linux_app UDP protocol manager listening on 0.0.0.0:%u\n", (unsigned)port);
    ethernet_status_mark_listening(&status);
    write_status_snapshot(&status, &status_warning_printed);

    while ((max_packets == 0u) || (packet_count < max_packets)) {
        uint8_t buffer[256];
        ssize_t received;
        protocol_parsed_msg_t parsed_msg;
        unified_frame_t frame;
        unified_error_t err;
        fd_set readfds;
        struct timeval timeout;
        int ready;

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        ready = select(fd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            ethernet_status_record_error(&status, "select", UNIFIED_ERR_INVALID_ARG);
            write_status_snapshot(&status, &status_warning_printed);
            close(fd);
            return UNIFIED_ERR_INVALID_ARG;
        }

        if (ready == 0) {
            write_status_snapshot(&status, &status_warning_printed);
            continue;
        }

        received = recvfrom(fd, buffer, sizeof(buffer), 0, NULL, NULL);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recvfrom");
            ethernet_status_record_error(&status, "recvfrom", UNIFIED_ERR_INVALID_ARG);
            write_status_snapshot(&status, &status_warning_printed);
            close(fd);
            return UNIFIED_ERR_INVALID_ARG;
        }

        packet_count++;
        ethernet_status_record_rx(&status, (size_t)received);

        err = ethernet_udp_parse_frame(buffer, (size_t)received, &parsed_msg);
        if (err != UNIFIED_OK) {
            log_error("ethernet_udp_parse_frame", err);
            ethernet_status_record_error(&status, "ethernet_udp_parse_frame", err);
            write_status_snapshot(&status, &status_warning_printed);
            continue;
        }

        err = frame_packer_pack(&parsed_msg, &frame);
        if (err != UNIFIED_OK) {
            log_error("frame_packer_pack", err);
            ethernet_status_record_error(&status, "frame_packer_pack", err);
            write_status_snapshot(&status, &status_warning_printed);
            continue;
        }

        err = ipc_to_rtos_send(&frame);
        if (err != UNIFIED_OK) {
            log_error("ipc_to_rtos_send", err);
            ethernet_status_record_error(&status, "ipc_to_rtos_send", err);
            write_status_snapshot(&status, &status_warning_printed);
            continue;
        }

        ethernet_status_record_tx_ok(&status);
        write_status_snapshot(&status, &status_warning_printed);
    }

    ethernet_status_mark_stopped(&status, "UDP listener stopped after reaching max packet limit");
    write_status_snapshot(&status, &status_warning_printed);
    close(fd);
    return UNIFIED_OK;
}

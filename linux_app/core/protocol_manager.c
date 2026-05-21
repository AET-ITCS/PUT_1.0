/* 协议管理实现：多协议线程接收，统一解析、打包、发送和状态统计。 */
#define _POSIX_C_SOURCE 200809L

#include "protocol_manager.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "ethernet_udp.h"
#include "frame_packer.h"
#include "ipc_to_rtos.h"
#include "rs485_debug.h"
#include "status_collector.h"

typedef struct {
    const app_config_t *config;
    status_collector_t status;
    pthread_mutex_t pipeline_mutex;
} protocol_manager_context_t;

typedef struct {
    protocol_manager_context_t *ctx;
    status_module_id_t module_id;
} worker_context_t;

static void log_error(const char *stage, unified_error_t err)
{
    fprintf(stderr, "[%s] error=%d\n", stage, (int)err);
}

static void write_status_snapshot(protocol_manager_context_t *ctx, bool *warned)
{
    if ((ctx == NULL) || !ctx->config->status_enabled) {
        return;
    }

    if ((status_collector_write_all(&ctx->status) != 0) && (warned != NULL) && !(*warned)) {
        fprintf(stderr,
                "[status_collector] failed to write status snapshots under %s; "
                "use --status-dir or --disable-status if needed\n",
                ctx->config->status_dir);
        *warned = true;
    }
}

static unified_error_t process_parsed_message(protocol_manager_context_t *ctx,
                                              status_module_id_t module_id,
                                              const protocol_parsed_msg_t *parsed_msg)
{
    unified_frame_t frame;
    unified_error_t err;

    (void)pthread_mutex_lock(&ctx->pipeline_mutex);

    err = frame_packer_pack(parsed_msg, &frame);
    if (err != UNIFIED_OK) {
        (void)pthread_mutex_unlock(&ctx->pipeline_mutex);
        log_error("frame_packer_pack", err);
        status_collector_record_error(&ctx->status, module_id, "frame_packer_pack", err);
        return err;
    }

    err = ipc_to_rtos_send(&frame);
    if (err != UNIFIED_OK) {
        (void)pthread_mutex_unlock(&ctx->pipeline_mutex);
        log_error("ipc_to_rtos_send", err);
        status_collector_record_error(&ctx->status, module_id, "ipc_to_rtos_send", err);
        return err;
    }

    (void)pthread_mutex_unlock(&ctx->pipeline_mutex);
    status_collector_record_tx_ok(&ctx->status, module_id);
    return UNIFIED_OK;
}

static void *udp_worker(void *arg)
{
    worker_context_t *worker = (worker_context_t *)arg;
    protocol_manager_context_t *ctx = worker->ctx;
    int fd;
    struct sockaddr_in addr;
    uint32_t packet_count = 0u;
    bool status_warning_printed = false;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        status_collector_record_error(&ctx->status, STATUS_MODULE_ETHERNET, "socket", UNIFIED_ERR_INVALID_ARG);
        write_status_snapshot(ctx, &status_warning_printed);
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ctx->config->ethernet_udp_port);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        status_collector_record_error(&ctx->status, STATUS_MODULE_ETHERNET, "bind", UNIFIED_ERR_INVALID_ARG);
        status_collector_mark_stopped(&ctx->status, STATUS_MODULE_ETHERNET, "UDP bind failed");
        write_status_snapshot(ctx, &status_warning_printed);
        close(fd);
        return NULL;
    }

    printf("linux_app UDP worker listening on 0.0.0.0:%u\n", (unsigned)ctx->config->ethernet_udp_port);
    status_collector_mark_running(&ctx->status, STATUS_MODULE_ETHERNET);
    write_status_snapshot(ctx, &status_warning_printed);

    while ((ctx->config->max_packets == 0u) || (packet_count < ctx->config->max_packets)) {
        uint8_t buffer[256];
        ssize_t received;
        protocol_parsed_msg_t parsed_msg;
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
            status_collector_record_error(&ctx->status, STATUS_MODULE_ETHERNET, "select", UNIFIED_ERR_INVALID_ARG);
            break;
        }

        if (ready == 0) {
            write_status_snapshot(ctx, &status_warning_printed);
            continue;
        }

        received = recvfrom(fd, buffer, sizeof(buffer), 0, NULL, NULL);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recvfrom");
            status_collector_record_error(&ctx->status, STATUS_MODULE_ETHERNET, "recvfrom", UNIFIED_ERR_INVALID_ARG);
            break;
        }

        packet_count++;
        status_collector_record_rx(&ctx->status, STATUS_MODULE_ETHERNET, (size_t)received);

        err = ethernet_udp_parse_frame(buffer, (size_t)received, &parsed_msg);
        if (err != UNIFIED_OK) {
            log_error("ethernet_udp_parse_frame", err);
            status_collector_record_error(&ctx->status,
                                          STATUS_MODULE_ETHERNET,
                                          "ethernet_udp_parse_frame",
                                          err);
            write_status_snapshot(ctx, &status_warning_printed);
            continue;
        }

        (void)process_parsed_message(ctx, STATUS_MODULE_ETHERNET, &parsed_msg);
        write_status_snapshot(ctx, &status_warning_printed);
    }

    status_collector_mark_stopped(&ctx->status, STATUS_MODULE_ETHERNET, "UDP worker stopped");
    write_status_snapshot(ctx, &status_warning_printed);
    close(fd);
    return NULL;
}

static speed_t baud_to_speed(uint32_t baud)
{
    switch (baud) {
    case 9600u:
        return B9600;
    case 19200u:
        return B19200;
    case 38400u:
        return B38400;
    case 57600u:
        return B57600;
    case 115200u:
        return B115200;
#ifdef B230400
    case 230400u:
        return B230400;
#endif
#ifdef B460800
    case 460800u:
        return B460800;
#endif
#ifdef B921600
    case 921600u:
        return B921600;
#endif
    default:
        return (speed_t)0;
    }
}

static unified_error_t configure_serial_raw(int fd, uint32_t baud)
{
    struct termios tio;
    speed_t speed = baud_to_speed(baud);

    if (speed == (speed_t)0) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (tcgetattr(fd, &tio) != 0) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    tio.c_iflag = (tcflag_t)0;
    tio.c_oflag = (tcflag_t)0;
    tio.c_lflag = (tcflag_t)0;
    tio.c_cflag &= (tcflag_t)~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
    tio.c_cflag &= (tcflag_t)~CRTSCTS;
#endif
    tio.c_cflag |= (tcflag_t)(CS8 | CLOCAL | CREAD);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    if ((cfsetispeed(&tio, speed) != 0) || (cfsetospeed(&tio, speed) != 0)) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    return UNIFIED_OK;
}

static const char *rs485_protocol_name(app_rs485_protocol_t protocol)
{
    switch (protocol) {
    case APP_RS485_PROTOCOL_CAN_DIRECT:
        return "can_direct";
    default:
        return "unknown";
    }
}

static void handle_rs485_can_direct_frame(protocol_manager_context_t *ctx,
                                          const uint8_t frame_buf[RS485_DEBUG_FRAME_LENGTH],
                                          bool *status_warning_printed)
{
    protocol_parsed_msg_t parsed_msg;
    unified_error_t err;

    status_collector_record_rx(&ctx->status, STATUS_MODULE_RS485, RS485_DEBUG_FRAME_LENGTH);
    err = rs485_debug_parse_frame(frame_buf, RS485_DEBUG_FRAME_LENGTH, &parsed_msg);
    if (err != UNIFIED_OK) {
        log_error("rs485_can_direct_parse_frame", err);
        status_collector_record_error(&ctx->status,
                                      STATUS_MODULE_RS485,
                                      "rs485_can_direct_parse_frame",
                                      err);
        write_status_snapshot(ctx, status_warning_printed);
        return;
    }

    (void)process_parsed_message(ctx, STATUS_MODULE_RS485, &parsed_msg);
    write_status_snapshot(ctx, status_warning_printed);
}

static void *rs485_worker(void *arg)
{
    worker_context_t *worker = (worker_context_t *)arg;
    protocol_manager_context_t *ctx = worker->ctx;
    int fd;
    uint32_t frame_count = 0u;
    bool status_warning_printed = false;
    rs485_debug_sync_t sync;

    rs485_debug_sync_init(&sync);

    fd = open(ctx->config->rs485_dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open rs485");
        status_collector_record_error(&ctx->status, STATUS_MODULE_RS485, "open_rs485", UNIFIED_ERR_INVALID_ARG);
        status_collector_mark_stopped(&ctx->status, STATUS_MODULE_RS485, "RS485 device open failed");
        write_status_snapshot(ctx, &status_warning_printed);
        return NULL;
    }

    if (configure_serial_raw(fd, ctx->config->rs485_baud) != UNIFIED_OK) {
        fprintf(stderr, "failed to configure RS485 device %s baud=%u\n",
                ctx->config->rs485_dev,
                (unsigned)ctx->config->rs485_baud);
        status_collector_record_error(&ctx->status, STATUS_MODULE_RS485, "configure_serial_raw", UNIFIED_ERR_INVALID_ARG);
        status_collector_mark_stopped(&ctx->status, STATUS_MODULE_RS485, "RS485 serial config failed");
        write_status_snapshot(ctx, &status_warning_printed);
        close(fd);
        return NULL;
    }

    printf("linux_app RS485 worker reading %s baud=%u protocol=%s\n",
           ctx->config->rs485_dev,
           (unsigned)ctx->config->rs485_baud,
           rs485_protocol_name(ctx->config->rs485_protocol));
    status_collector_mark_running(&ctx->status, STATUS_MODULE_RS485);
    write_status_snapshot(ctx, &status_warning_printed);

    while ((ctx->config->max_packets == 0u) || (frame_count < ctx->config->max_packets)) {
        uint8_t buffer[128];
        ssize_t nread;
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
            perror("select rs485");
            status_collector_record_error(&ctx->status, STATUS_MODULE_RS485, "select_rs485", UNIFIED_ERR_INVALID_ARG);
            break;
        }

        if (ready == 0) {
            write_status_snapshot(ctx, &status_warning_printed);
            continue;
        }

        nread = read(fd, buffer, sizeof(buffer));
        if (nread < 0) {
            if ((errno == EINTR) || (errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                continue;
            }
            perror("read rs485");
            status_collector_record_error(&ctx->status, STATUS_MODULE_RS485, "read_rs485", UNIFIED_ERR_INVALID_ARG);
            break;
        }

        for (ssize_t i = 0; i < nread; ++i) {
            uint8_t frame_buf[RS485_DEBUG_FRAME_LENGTH];
            if (!rs485_debug_sync_feed(&sync, buffer[i], frame_buf)) {
                continue;
            }
            frame_count++;
            handle_rs485_can_direct_frame(ctx, frame_buf, &status_warning_printed);
            if ((ctx->config->max_packets != 0u) && (frame_count >= ctx->config->max_packets)) {
                break;
            }
        }
    }

    status_collector_mark_stopped(&ctx->status, STATUS_MODULE_RS485, "RS485 worker stopped");
    write_status_snapshot(ctx, &status_warning_printed);
    close(fd);
    return NULL;
}

static void configure_status_modules(protocol_manager_context_t *ctx)
{
    char detail[128];

    status_collector_configure_module(&ctx->status,
                                      STATUS_MODULE_4G,
                                      false,
                                      false,
                                      "4g",
                                      "4G protocol module is not implemented yet");
    status_collector_configure_module(&ctx->status,
                                      STATUS_MODULE_WIFI,
                                      false,
                                      false,
                                      "wifi",
                                      "WiFi protocol module is not implemented yet");
    status_collector_configure_module(&ctx->status,
                                      STATUS_MODULE_BLUETOOTH,
                                      false,
                                      false,
                                      "bluetooth",
                                      "Bluetooth protocol module is not implemented yet");

    (void)snprintf(detail, sizeof(detail), "udp port=%u", (unsigned)ctx->config->ethernet_udp_port);
    status_collector_configure_module(&ctx->status,
                                      STATUS_MODULE_ETHERNET,
                                      true,
                                      ctx->config->ethernet_udp_enabled,
                                      "can_direct",
                                      detail);

    (void)snprintf(detail,
                   sizeof(detail),
                   "dev=%.72s baud=%u",
                   ctx->config->rs485_dev,
                   (unsigned)ctx->config->rs485_baud);
    status_collector_configure_module(&ctx->status,
                                      STATUS_MODULE_RS485,
                                      true,
                                      ctx->config->rs485_enabled,
                                      rs485_protocol_name(ctx->config->rs485_protocol),
                                      detail);
}

unified_error_t protocol_manager_run(const app_config_t *config)
{
    protocol_manager_context_t ctx;
    pthread_t udp_thread;
    pthread_t rs485_thread;
    worker_context_t udp_worker_ctx;
    worker_context_t rs485_worker_ctx;
    bool udp_started = false;
    bool rs485_started = false;
    bool status_warning_printed = false;
    int enabled_count = 0;

    if (config == NULL) {
        return UNIFIED_ERR_NULL;
    }

    if (app_config_validate(config) != UNIFIED_OK) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.config = config;
    status_collector_init(&ctx.status, config->status_dir, config->status_enabled);
    (void)pthread_mutex_init(&ctx.pipeline_mutex, NULL);
    configure_status_modules(&ctx);
    write_status_snapshot(&ctx, &status_warning_printed);

    if (config->ethernet_udp_enabled) {
        udp_worker_ctx.ctx = &ctx;
        udp_worker_ctx.module_id = STATUS_MODULE_ETHERNET;
        if (pthread_create(&udp_thread, NULL, udp_worker, &udp_worker_ctx) != 0) {
            status_collector_record_error(&ctx.status, STATUS_MODULE_ETHERNET, "pthread_create_udp", UNIFIED_ERR_INVALID_ARG);
        } else {
            udp_started = true;
            enabled_count++;
        }
    }

    if (config->rs485_enabled) {
        rs485_worker_ctx.ctx = &ctx;
        rs485_worker_ctx.module_id = STATUS_MODULE_RS485;
        if (pthread_create(&rs485_thread, NULL, rs485_worker, &rs485_worker_ctx) != 0) {
            status_collector_record_error(&ctx.status, STATUS_MODULE_RS485, "pthread_create_rs485", UNIFIED_ERR_INVALID_ARG);
        } else {
            rs485_started = true;
            enabled_count++;
        }
    }

    if (enabled_count == 0) {
        fprintf(stderr, "no protocol module enabled\n");
        write_status_snapshot(&ctx, &status_warning_printed);
        status_collector_destroy(&ctx.status);
        (void)pthread_mutex_destroy(&ctx.pipeline_mutex);
        return UNIFIED_ERR_INVALID_ARG;
    }

    if (udp_started) {
        (void)pthread_join(udp_thread, NULL);
    }

    if (rs485_started) {
        (void)pthread_join(rs485_thread, NULL);
    }

    write_status_snapshot(&ctx, &status_warning_printed);
    status_collector_destroy(&ctx.status);
    (void)pthread_mutex_destroy(&ctx.pipeline_mutex);
    return UNIFIED_OK;
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
    app_config_t config;

    if (port == 0u) {
        return UNIFIED_ERR_INVALID_ARG;
    }

    app_config_set_defaults(&config);
    config.ethernet_udp_enabled = true;
    config.ethernet_udp_port = port;
    config.rs485_enabled = false;
    config.max_packets = max_packets;
    config.status_enabled = status_enabled;
    if ((status_dir != NULL) && (status_dir[0] != '\0')) {
        (void)snprintf(config.status_dir, sizeof(config.status_dir), "%s", status_dir);
    }

    return protocol_manager_run(&config);
}

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "rs485_adapter.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "anymsg_frame.h"
#include "shared_memory_ipc.h"

/* 重试打开串口的延迟时间（毫秒） */
#define RS485_ADAPTER_RETRY_DELAY_MS 1000u
/* IPC raw数据的帧类型定义 (anyMSG 协议规定) */
#define RS485_ANYMSG_TYPE_RAW 0x1Fu

/**
 * @brief RS485 适配器内部全局状态结构体
 * 
 * 包含互斥锁、串口描述符、后台线程控制以及当前的配置和状态统计。
 */
typedef struct {
    pthread_mutex_t lock;               /* 用于多线程保护内部状态的互斥锁 */
    int fd;                             /* 当前打开的串口设备文件描述符 */
    pthread_t thread;                   /* 接收线程的句柄 */
    bool thread_started;                /* 线程是否已经成功启动 */
    bool stop_requested;                /* 外部是否请求停止线程的标志 */
    rs485_adapter_config_t config;      /* 模块的当前配置副本 */
    rs485_status_t status;              /* 运行时状态统计信息 */
} rs485_adapter_state_t;

/* 全局唯一的RS485适配器状态实例 */
static rs485_adapter_state_t g_rs485_state = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .fd = -1,
};

/**
 * @brief 获取当前的单调时间（毫秒）
 * 
 * 使用 CLOCK_MONOTONIC，防止受系统时间手动修改的影响，用于性能及超时计算。
 * 
 * @return uint64_t 毫秒时间戳，失败返回 0
 */
static uint64_t now_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return ((uint64_t)ts.tv_sec * 1000ull) + ((uint64_t)ts.tv_nsec / 1000000ull);
}

/**
 * @brief 毫秒级线程睡眠函数
 * 
 * 封装 nanosleep，防止被信号中断(EINTR)，确保休眠精度与可靠性。
 * 
 * @param duration_ms 睡眠的时长（毫秒）
 */
static void sleep_ms(uint32_t duration_ms)
{
    struct timespec req;
    struct timespec rem;
    req.tv_sec = (time_t)(duration_ms / 1000u);
    req.tv_nsec = (long)((duration_ms % 1000u) * 1000000u);
    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) {
            break;
        }
        req = rem;
    }
}

/**
 * @brief 判断文本中是否包含指定子串
 * 
 * 用于辅助判断错误发生的阶段或错误源。
 */
static bool string_has(const char *text, const char *needle)
{
    return (text != 0) && (needle != 0) && (strstr(text, needle) != 0);
}

/**
 * @brief 记录并上报错误信息
 * 
 * 递增不同类型的错误计数器，更新最后错误时间，并将错误记录至全局状态收集器。
 * 
 * @param status 指向状态统计结构体的指针
 * @param collector 指向全局状态收集器的指针
 * @param stage 错误发生的阶段/位置（如 "uart_read", "ipc_commit"）
 * @param err 统一错误码
 */
static void record_error(rs485_status_t *status,
                         status_collector_t *collector,
                         const char *stage,
                         unified_error_t err)
{
    uint64_t now_ms;

    now_ms = now_monotonic_ms();
    if (status != 0) {
        status->error_count++;
        status->last_error_ms = now_ms;
        status->updated_at_ms = now_ms;
        (void)snprintf(status->last_error_stage,
                       sizeof(status->last_error_stage),
                       "%s",
                       (stage == 0) ? "rs485_unknown" : stage);
        (void)snprintf(status->last_error_message,
                       sizeof(status->last_error_message),
                       "%s failed with error=%d",
                       (stage == 0) ? "rs485_unknown" : stage,
                       (int)err);

        /* 根据 stage 名称对错误进行精细化归类 */
        if (string_has(stage, "decode") || string_has(stage, "parse")) {
            status->decode_error_count++;
        } else if (string_has(stage, "crc")) {
            status->crc_error_count++;
        } else if (string_has(stage, "send")) {
            status->send_fail_count++;
        } else if (string_has(stage, "uart") || string_has(stage, "open")) {
            status->interface_offline_count++;
        }
        if (string_has(stage, "ipc")) {
            status->ipc_error_count++;
        }
    }

    if (collector != 0) {
        status_collector_record_error(collector, STATUS_MODULE_RS485, stage, err);
    }
}

/**
 * @brief 记录成功的接收操作统计
 * 
 * @param status 指向状态统计结构体的指针
 * @param collector 指向全局状态收集器的指针
 * @param bytes 接收到的数据字节数
 */
static void record_rx_ok(rs485_status_t *status,
                         status_collector_t *collector,
                         size_t bytes)
{
    uint64_t now_ms;
    now_ms = now_monotonic_ms();
    if (status != 0) {
        status->rx_frames++;
        status->rx_bytes += (uint64_t)bytes;
        status->last_rx_ms = now_ms;
        status->updated_at_ms = now_ms;
    }
    if (collector != 0) {
        status_collector_record_rx(collector, STATUS_MODULE_RS485, bytes);
    }
}

/**
 * @brief 记录成功的发送操作统计
 * 
 * @param status 指向状态统计结构体的指针
 * @param collector 指向全局状态收集器的指针
 * @param bytes 发送的数据字节数
 */
static void record_tx_ok(rs485_status_t *status,
                         status_collector_t *collector,
                         size_t bytes)
{
    uint64_t now_ms;
    now_ms = now_monotonic_ms();
    if (status != 0) {
        status->tx_frames++;
        status->tx_bytes += (uint64_t)bytes;
        status->last_tx_ms = now_ms;
        status->updated_at_ms = now_ms;
    }
    if (collector != 0) {
        status_collector_record_tx_ok(collector, STATUS_MODULE_RS485, bytes);
    }
}

/**
 * @brief 以小端序(Little Endian)方式写入2字节的16位无符号数
 */
static void write_le16(uint8_t bytes[2], uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xFFu);
}

/**
 * @brief 以小端序(Little Endian)方式读取2字节的16位无符号数
 */
static uint16_t read_le16(const uint8_t bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
}

/**
 * @brief 根据目标的 CID (Component ID) 前缀推断路由目标接口类型
 * 
 * 跨核 IPC 路由需要该 Hint 来将数据投递到对应的 RTOS/Linux 核上的物理网络层。
 * 
 * @param cid 目的 CID 数组
 * @return put_shm_interface_t 对应的共享内存物理接口标识
 */
static put_shm_interface_t route_hint_from_destination_cid(const uint8_t cid[ANYMSG_CID_LENGTH])
{
    switch (anymsg_cid_segment_from_first_byte(cid[0])) {
    case ANYMSG_CID_SEGMENT_CAN:
        return PUT_SHM_INTERFACE_CAN;
    case ANYMSG_CID_SEGMENT_ETHERNET:
        return PUT_SHM_INTERFACE_ETHERNET;
    case ANYMSG_CID_SEGMENT_WIFI:
        return PUT_SHM_INTERFACE_WIFI;
    case ANYMSG_CID_SEGMENT_BLUETOOTH:
        return PUT_SHM_INTERFACE_BLUETOOTH;
    case ANYMSG_CID_SEGMENT_4G:
        return PUT_SHM_INTERFACE_4G;
    case ANYMSG_CID_SEGMENT_RS485:
        return PUT_SHM_INTERFACE_RS485;
    case ANYMSG_CID_SEGMENT_RESERVED_LOW:
    case ANYMSG_CID_SEGMENT_RESERVED_HIGH:
    default:
        return PUT_SHM_INTERFACE_RS485;
    }
}

/**
 * @brief 设置配置的默认参数值
 */
void rs485_adapter_config_set_defaults(rs485_adapter_config_t *config)
{
    if (config == 0) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->enabled = false;
    (void)snprintf(config->uart_device, sizeof(config->uart_device), "%s", RS485_ADAPTER_DEFAULT_DEVICE);
    config->baudrate = RS485_ADAPTER_DEFAULT_BAUDRATE;
    /* 默认启用RS485，并在发送完毕后自动拉低RTS释放总线控制权（硬件自动流控） */
    config->rs485_flags = SER_RS485_ENABLED | SER_RS485_RTS_AFTER_SEND;
}

/**
 * @brief 将接收到的 RS485 裸数据打包成 anyMSG 帧并提交至共享内存 IPC
 * 
 * 该功能在不需要对 Modbus 协议做深度解析的极简方案(方案B)中应用。
 * 它直接申请 IPC RX 帧缓存，封装 anyMSG 头部（设置为原始RS485消息类型 0x1F），然后写入接收队列中。
 * 
 * @param state 适配器全局状态指针
 * @param payload 从物理串口中读取的原始数据载荷
 * @param payload_len 原始数据载荷的长度
 * @return unified_error_t 成功返回 UNIFIED_OK，失败返回具体错误码
 */
static unified_error_t rs485_adapter_submit_to_ipc(rs485_adapter_state_t *state,
                                                   const uint8_t *payload,
                                                   size_t payload_len)
{
    uint32_t frame_id;
    uint8_t *frame_buffer;
    uint16_t frame_capacity;
    unified_error_t err;
    anymsg_header_t *header;
    uint16_t msg_length;

    if ((state == 0) || (state->config.ipc == 0) || (payload == 0) || (payload_len == 0u)) {
        return UNIFIED_ERR_NULL;
    }

    if (payload_len > ANYMSG_MAX_PAYLOAD_LENGTH) {
        return UNIFIED_ERR_LENGTH;
    }

    /* 1. 向共享内存分配一个空的帧缓冲区 */
    err = linux_shm_frame_alloc(state->config.ipc,
                                PUT_SHM_INTERFACE_RS485,
                                &frame_id,
                                &frame_buffer,
                                &frame_capacity);
    if (err != UNIFIED_OK) {
        return err;
    }

    /* 计算包含 oneMSG/anyMSG 头部的完整消息长度 */
    msg_length = (uint16_t)(ANYMSG_HEADER_SIZE + payload_len);
    if (msg_length > frame_capacity) {
        (void)linux_shm_frame_release(state->config.ipc, frame_id, PUT_SHM_RECLAIM_REASON_INVALID_FRAME);
        return UNIFIED_ERR_LENGTH;
    }

    /* 2. 构造 anyMSG 帧头部信息 */
    header = (anymsg_header_t *)frame_buffer;
    memset(header, 0, ANYMSG_HEADER_SIZE);
    write_le16(header->msg_length, msg_length);
    write_le16(header->payload_length, (uint16_t)payload_len);
    
    /* 默认源 CID 为 RS485 分段中的最小值 (0xC0) */
    header->source_cid[0] = ANYMSG_CID_RS485_MIN;
    /* 默认目的 CID 为保留段 */
    header->destination_cid[0] = ANYMSG_CID_RESERVED_LOW_MIN;
    header->type = RS485_ANYMSG_TYPE_RAW;

    /* 3. 拷贝裸数据到 payload 区域 */
    memcpy(frame_buffer + ANYMSG_HEADER_SIZE, payload, payload_len);

    /* 4. 提交该帧到共享内存接收环形队列 */
    err = linux_shm_frame_commit_rx(state->config.ipc,
                                    frame_id,
                                    msg_length,
                                    PUT_SHM_INTERFACE_RS485,
                                    route_hint_from_destination_cid(header->destination_cid),
                                    header->source_cid,
                                    header->destination_cid,
                                    header->type,
                                    0u,
                                    0u,
                                    state->config.linux_epoch,
                                    0u);
    if (err != UNIFIED_OK) {
        /* 如果队列满了提交失败，则必须主动释放释放占用的缓冲区 */
        (void)linux_shm_frame_release(state->config.ipc, frame_id, PUT_SHM_RECLAIM_REASON_QUEUE_FULL);
        return err;
    }

    return UNIFIED_OK;
}

/**
 * @brief 安全地读取线程停止请求标志
 */
static bool should_stop(rs485_adapter_state_t *state)
{
    bool stop_requested;
    (void)pthread_mutex_lock(&state->lock);
    stop_requested = state->stop_requested;
    (void)pthread_mutex_unlock(&state->lock);
    return stop_requested;
}

/**
 * @brief 安全地更新串口状态和在线标志
 */
static void set_socket_state(rs485_adapter_state_t *state,
                             int fd,
                             bool interface_online)
{
    (void)pthread_mutex_lock(&state->lock);
    state->fd = fd;
    state->status.interface_online = interface_online;
    state->status.updated_at_ms = now_monotonic_ms();
    (void)pthread_mutex_unlock(&state->lock);
}

/**
 * @brief 安全地提取文件描述符，并将状态置为离线
 * 
 * 用于关闭操作，确保文件描述符的所有权单线程化，防止并发冲突。
 */
static int take_socket_fd(rs485_adapter_state_t *state)
{
    int fd;
    (void)pthread_mutex_lock(&state->lock);
    fd = state->fd;
    state->fd = -1;
    state->status.interface_online = false;
    state->status.updated_at_ms = now_monotonic_ms();
    (void)pthread_mutex_unlock(&state->lock);
    return fd;
}

/**
 * @brief 打开并初始化串口设备，启用硬件 RS485 自动流控及分包超时机制
 * 
 * 核心设计：
 * 1. 采用 VTIME=1 (100ms) 结合 VMIN=0 的阻塞读取机制。当总线空闲 100ms 时，read() 会自动返回，
 *    无需应用层用高 CPU 占用的轮询或复杂定时器去判定 Modbus 数据帧结束。
 * 2. 使用 ioctl(TIOCSRS485) 启用 Linux 驱动级别的自动硬件流控（控制DE/RE引脚）。
 * 
 * @param config 配置参数
 * @param status 状态存储
 * @param collector 状态记录器
 * @return int 成功返回文件描述符，失败返回 -1
 */
static int open_uart(const rs485_adapter_config_t *config, rs485_status_t *status, status_collector_t *collector)
{
    int fd;
    struct termios tio;
    struct serial_rs485 rs485conf;
    speed_t speed;

    /* 1. 非阻塞方式打开，防止 open 阶段发生意外挂起 */
    fd = open(config->uart_device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        record_error(status, collector, "rs485_uart_open", UNIFIED_ERR_IPC_NOT_READY);
        return -1;
    }

    /* 2. 串口基础 Termios 参数设置 (8位数据位，无校验，1位停止位，无软件流控) */
    memset(&tio, 0, sizeof(tio));
    cfmakeraw(&tio);
    tio.c_cflag = CS8 | CLOCAL | CREAD;
    
    switch (config->baudrate) {
    case 9600u: speed = B9600; break;
    case 19200u: speed = B19200; break;
    case 38400u: speed = B38400; break;
    case 57600u: speed = B57600; break;
    case 115200u: speed = B115200; break;
    default: speed = B115200; break;
    }
    
    cfsetospeed(&tio, speed);
    cfsetispeed(&tio, speed);
    
    /* 3. 配置 100ms 帧尾判断超时 (VTIME 的单位为 0.1秒，故 VTIME=1 代表 100ms) 
     *    当串口总线超过 100ms 没有新的字符到来，read() 马上返回已收到数据。 */
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;
    
    /* 4. 清除非阻塞 O_NONBLOCK 标志，从而令 read() 可以借助于 Termios 的 VTIME 起效 */
    fcntl(fd, F_SETFL, 0);

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        record_error(status, collector, "rs485_tcsetattr", UNIFIED_ERR_INVALID_ARG);
        (void)close(fd);
        return -1;
    }

    /* 5. 硬件流控设置，通过 ioctl 激活 Linux 驱动的 RS485 模式 */
    if (config->rs485_flags != 0u) {
        memset(&rs485conf, 0, sizeof(rs485conf));
        if (ioctl(fd, TIOCGRS485, &rs485conf) < 0) {
            record_error(status, collector, "rs485_ioctl_get", UNIFIED_ERR_INVALID_ARG);
        } else {
            rs485conf.flags |= config->rs485_flags;
            /* 保证发送完毕后 RTS 引脚能够准确指示接收/发送状态(DE低电平使能接收) */
            if ((rs485conf.flags & SER_RS485_RTS_AFTER_SEND) != 0u) {
                rs485conf.flags &= (uint32_t)~SER_RS485_RTS_ON_SEND;
            }
            if (ioctl(fd, TIOCSRS485, &rs485conf) < 0) {
                record_error(status, collector, "rs485_ioctl_set", UNIFIED_ERR_INVALID_ARG);
            }
        }
    }

    return fd;
}

/**
 * @brief 接收线程入口函数
 * 
 * 独立循环接收串口总线数据，在串口掉线或出错时自动重试，持续监听并将数据转发至 IPC。
 */
static void *rs485_rx_thread(void *arg)
{
    rs485_adapter_state_t *state;
    uint8_t rx_buffer[RS485_ADAPTER_MAX_PAYLOAD_SIZE];

    state = (rs485_adapter_state_t *)arg;
    state->status.running = true;
    state->status.updated_at_ms = now_monotonic_ms();

    /* 标记当前模块状态为运行中 */
    if (state->config.collector != 0) {
        status_collector_mark_running(state->config.collector, STATUS_MODULE_RS485);
    }

    while (!should_stop(state)) {
        int fd;

        /* 1. 打开串口设备并初始化 */
        fd = open_uart(&state->config, &state->status, state->config.collector);
        if (fd < 0) {
            set_socket_state(state, -1, false);
            sleep_ms(RS485_ADAPTER_RETRY_DELAY_MS);
            continue;
        }

        set_socket_state(state, fd, true);

        /* 2. 接收循环 */
        while (!should_stop(state)) {
            ssize_t received;
            unified_error_t err;

            /* 阻塞读取，直到收到数据或 100ms 总线静默超时 */
            received = read(fd, rx_buffer, sizeof(rx_buffer));
            if (received > 0) {
                /* 收到一帧完整的数据包，提交给共享内存 IPC，转发给其他核心 */
                err = rs485_adapter_submit_to_ipc(state, rx_buffer, (size_t)received);
                if (err == UNIFIED_OK) {
                    record_rx_ok(&state->status, state->config.collector, (size_t)received);
                } else {
                    record_error(&state->status, state->config.collector, "rs485_ipc_commit", err);
                }
            } else if (received < 0) {
                /* 被信号中断或重试的非致命错误直接忽略并继续 */
                if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) {
                    continue;
                }
                /* 严重的串口I/O错误，退出内循环准备重连 */
                record_error(&state->status, state->config.collector, "rs485_uart_read", UNIFIED_ERR_IPC_OFFLINE);
                break;
            }
        }

        /* 3. 清理当前的串口描述符 */
        fd = take_socket_fd(state);
        if (fd >= 0) {
            (void)close(fd);
        }
        if (!should_stop(state)) {
            sleep_ms(RS485_ADAPTER_RETRY_DELAY_MS);
        }
    }

    /* 4. 退出线程时的状态还原 */
    state->status.running = false;
    state->status.interface_online = false;
    state->status.updated_at_ms = now_monotonic_ms();
    if (state->config.collector != 0) {
        status_collector_mark_stopped(state->config.collector, STATUS_MODULE_RS485, "rs485 rx stopped");
    }
    return 0;
}

/**
 * @brief 启动适配器
 */
int rs485_adapter_start(const rs485_adapter_config_t *config)
{
    uint64_t now_ms;

    if ((config == 0) || (config->ipc == 0) || (config->linux_epoch == 0u) ||
        (config->uart_device[0] == '\0')) {
        return -1;
    }

    (void)pthread_mutex_lock(&g_rs485_state.lock);
    if (g_rs485_state.thread_started) {
        (void)pthread_mutex_unlock(&g_rs485_state.lock);
        return 0;
    }

    /* 深度拷贝外部传入的配置 */
    g_rs485_state.config = *config;
    g_rs485_state.stop_requested = false;
    g_rs485_state.fd = -1;
    memset(&g_rs485_state.status, 0, sizeof(g_rs485_state.status));
    
    now_ms = now_monotonic_ms();
    g_rs485_state.status.enabled = config->enabled;
    g_rs485_state.status.updated_at_ms = now_ms;
    (void)snprintf(g_rs485_state.status.ifname, sizeof(g_rs485_state.status.ifname), "%s", config->uart_device);

    (void)pthread_mutex_unlock(&g_rs485_state.lock);

    /* 配置全局状态收集器 */
    if (config->collector != 0) {
        status_collector_configure_module(config->collector,
                                          STATUS_MODULE_RS485,
                                          true,
                                          config->enabled,
                                          "RS485 (Raw)",
                                          config->uart_device);
    }

    /* 如果模块配置为启用，创建后台线程 */
    if (config->enabled) {
        if (pthread_create(&g_rs485_state.thread, 0, rs485_rx_thread, &g_rs485_state) != 0) {
            record_error(&g_rs485_state.status, config->collector, "rs485_pthread_create", UNIFIED_ERR_IPC_NOT_READY);
            return -1;
        }

        (void)pthread_mutex_lock(&g_rs485_state.lock);
        g_rs485_state.thread_started = true;
        (void)pthread_mutex_unlock(&g_rs485_state.lock);
    }

    return 0;
}

/**
 * @brief 停止适配器并回收资源
 */
void rs485_adapter_stop(void)
{
    int fd;
    bool thread_started;

    (void)pthread_mutex_lock(&g_rs485_state.lock);
    thread_started = g_rs485_state.thread_started;
    g_rs485_state.stop_requested = true;
    (void)pthread_mutex_unlock(&g_rs485_state.lock);

    /* 主动提取串口描述符并关闭，打断正在阻塞 read 的线程 */
    fd = take_socket_fd(&g_rs485_state);
    if (fd >= 0) {
        (void)close(fd);
    }

    /* 等待后台线程结束退出 */
    if (thread_started) {
        (void)pthread_join(g_rs485_state.thread, 0);
    }

    (void)pthread_mutex_lock(&g_rs485_state.lock);
    g_rs485_state.thread_started = false;
    (void)pthread_mutex_unlock(&g_rs485_state.lock);
}

/* 适配器虚函数表物理实现 */

/**
 * @brief 获取最大传输单元
 */
static size_t rs485_get_mtu(void *ctx)
{
    (void)ctx;
    return RS485_ADAPTER_MAX_PAYLOAD_SIZE;
}

/**
 * @brief 解包及验证从IPC接收的帧
 * 
 * 校验 anyMSG 包的静态字段及长度有效性，并提取出 source_cid/destination_cid 等字段。
 */
static int rs485_decode_rx(void *ctx,
                           const uint8_t *input,
                           size_t input_len,
                           adapter_rx_result_t *out)
{
    const anymsg_header_t *header;
    uint16_t msg_length;
    uint16_t payload_length;
    unified_error_t err;

    (void)ctx;
    if ((input == 0) || (out == 0) || (input_len < ANYMSG_HEADER_SIZE)) {
        return -1;
    }

    header = (const anymsg_header_t *)input;
    msg_length = read_le16(header->msg_length);
    payload_length = read_le16(header->payload_length);

    err = anymsg_validate_normalized_lengths(msg_length, payload_length, input_len);
    if (err != UNIFIED_OK) {
        return -1;
    }

    err = anymsg_validate_header_static_fields(header);
    if (err != UNIFIED_OK) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->data = input;
    out->len = input_len;
    out->msg_length = msg_length;
    out->payload_length = payload_length;
    memcpy(out->source_cid, header->source_cid, ANYMSG_CID_LENGTH);
    memcpy(out->destination_cid, header->destination_cid, ANYMSG_CID_LENGTH);
    out->type = header->type;
    return 0;
}

/**
 * @brief 重组分包（RS485 不做应用层分片，返回 -1）
 */
static int rs485_reassemble(void *ctx,
                            const adapter_fragment_t *fragment,
                            anymsg_buffer_t *out_complete_msg)
{
    (void)ctx;
    (void)fragment;
    (void)out_complete_msg;
    return -1; /* RS485 在接收线程中直接借助于 VTIME 来天然重构帧 */
}

/**
 * @brief 剥离 anyMSG 头，将裸载荷剥离出准备发送至物理串口
 * 
 * 方案B：Linux 在向物理 RS485 总线发送数据时，剥离 anyMSG 的帧头部，
 * 只把 raw 的数据载荷写入物理串口，实现纯净的透传。
 */
static int rs485_encapsulate(void *ctx,
                             const anymsg_buffer_t *msg,
                             adapter_tx_packet_t *out_packet)
{
    const anymsg_header_t *header;

    (void)ctx;
    if ((msg == 0) || (msg->data == 0) || (out_packet == 0)) {
        return -1;
    }

    if (msg->len < ANYMSG_HEADER_SIZE) {
        return -1;
    }

    header = (const anymsg_header_t *)msg->data;
    /* 剥离头部，将数据指针移至载荷首地址 */
    out_packet->data = msg->data + ANYMSG_HEADER_SIZE;
    /* 发送长度为 payload_length 的原始裸数据 */
    out_packet->len = (size_t)read_le16(header->payload_length);

    return 0;
}

/**
 * @brief 发送分片（RS485 裸数据传输不需要进行发送端分片）
 */
static int rs485_fragment_tx(void *ctx,
                             const anymsg_buffer_t *msg,
                             adapter_tx_packet_list_t *out_packets)
{
    (void)ctx;
    (void)msg;
    (void)out_packets;
    return -1; /* 透传模式无需应用层分片 */
}

/**
 * @brief 驱动物理串口发送裸数据包
 * 
 * 将已经剥除 `anyMSG` 帧头部的原始裸载荷写入已打开的串口。
 */
static int rs485_send(void *ctx, const adapter_tx_packet_t *packet)
{
    int fd;
    ssize_t written;

    (void)ctx;
    if ((packet == 0) || (packet->data == 0)) {
        return -1;
    }

    (void)pthread_mutex_lock(&g_rs485_state.lock);
    fd = g_rs485_state.fd;
    (void)pthread_mutex_unlock(&g_rs485_state.lock);

    if (fd < 0) {
        record_error(&g_rs485_state.status, g_rs485_state.config.collector, "rs485_send_offline", UNIFIED_ERR_IPC_OFFLINE);
        return -1;
    }

    /* 物理写入串口设备 */
    written = write(fd, packet->data, packet->len);
    if ((written < 0) || ((size_t)written != packet->len)) {
        record_error(&g_rs485_state.status, g_rs485_state.config.collector, "rs485_uart_write", UNIFIED_ERR_INVALID_ARG);
        return -1;
    }

    record_tx_ok(&g_rs485_state.status, g_rs485_state.config.collector, packet->len);
    return 0;
}

/**
 * @brief 获取当前的运行时状态副本
 */
static int rs485_status(void *ctx, void *out_status)
{
    rs485_status_t *status;

    (void)ctx;
    if (out_status == 0) {
        return -1;
    }

    status = (rs485_status_t *)out_status;
    (void)pthread_mutex_lock(&g_rs485_state.lock);
    *status = g_rs485_state.status;
    (void)pthread_mutex_unlock(&g_rs485_state.lock);

    return 0;
}

/* 导出全局 RS485 适配器实例及操作虚函数表 */
physical_interface_adapter_t rs485_adapter = {
    .name = "RS485",
    .interface_id = (uint8_t)PUT_SHM_INTERFACE_RS485,
    .get_mtu = rs485_get_mtu,
    .decode_rx = rs485_decode_rx,
    .reassemble = rs485_reassemble,
    .encapsulate = rs485_encapsulate,
    .fragment_tx = rs485_fragment_tx,
    .send = rs485_send,
    .status = rs485_status,
};

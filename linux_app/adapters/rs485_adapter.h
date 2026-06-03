#ifndef RS485_ADAPTER_H
#define RS485_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "physical_interface_adapter.h"
#include "status_collector.h"
#include "linux_shm_ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 默认的RS485串口设备文件路径 */
#define RS485_ADAPTER_DEFAULT_DEVICE "/dev/ttyS4"
/* 默认串口波特率 */
#define RS485_ADAPTER_DEFAULT_BAUDRATE 115200u
/* RS485适配器支持的最大数据载荷长度（字节） */
#define RS485_ADAPTER_MAX_PAYLOAD_SIZE 256u

/**
 * @brief RS485 适配器配置结构体
 * 
 * 存储初始化和运行RS485模块所需的参数，包括串口路径、波特率、流控标志位及IPC句柄等。
 */
typedef struct {
    bool enabled;                       /* 是否启用该适配器 */
    char uart_device[64];               /* 串口设备路径，例如 "/dev/ttyS4" */
    uint32_t baudrate;                  /* 波特率，例如 115200 */
    uint32_t rs485_flags;               /* struct serial_rs485 的流控标志位 (TIOCSRS485) */
    uint32_t linux_epoch;               /* Linux 系统的启动时间戳（用于消息帧计时） */
    linux_shm_ipc_t *ipc;               /* 共享内存 IPC 句柄，用于跨核数据交互 */
    status_collector_t *collector;      /* 全局状态收集器，用于监控和统计运行错误 */
} rs485_adapter_config_t;

/**
 * @brief RS485 运行时状态统计结构体
 * 
 * 用于保存RS485模块的各种计数器、状态指标以及最新发生的错误描述信息。
 */
typedef struct {
    bool enabled;                       /* 模块是否启用 */
    bool running;                       /* 接收线程是否正在运行 */
    bool interface_online;              /* 物理接口（串口设备描述符）是否在线/已成功打开 */
    uint64_t rx_frames;                 /* 成功接收的数据帧总数 */
    uint64_t tx_frames;                 /* 成功发送的数据帧总数 */
    uint64_t rx_bytes;                  /* 接收的总字节数 */
    uint64_t tx_bytes;                  /* 发送的总字节数 */
    uint64_t error_count;               /* 累计发生的错误总数 */
    uint64_t decode_error_count;        /* 帧解码错误次数 */
    uint64_t crc_error_count;           /* CRC校验错误次数 */
    uint64_t send_fail_count;           /* 串口发送失败次数 */
    uint64_t ipc_error_count;           /* IPC 交互失败次数 */
    uint64_t interface_offline_count;   /* 接口离线（打开或操作失败）次数 */
    uint64_t last_rx_ms;                /* 最后一次接收到数据的时间戳（毫秒） */
    uint64_t last_tx_ms;                /* 最后一次成功发送数据的时间戳（毫秒） */
    uint64_t last_error_ms;             /* 最后一次发生错误的时间戳（毫秒） */
    uint64_t updated_at_ms;             /* 状态最后一次更新的时间戳（毫秒） */
    char ifname[64];                    /* 接口名称（通常为串口设备路径） */
    char last_error_stage[64];          /* 最近一次错误的发生阶段/模块名 */
    char last_error_message[128];       /* 最近一次错误的信息描述 */
} rs485_status_t;

/* 声明全局的物理接口适配器实例，供外部注册使用 */
extern physical_interface_adapter_t rs485_adapter;

/**
 * @brief 设置配置结构体的默认值
 * 
 * 将配置结构体清零，并配置为禁用状态、默认串口设备 `/dev/ttyS4`、默认波特率 115200 以及基本的 RS485 流控标志。
 * 
 * @param config 指向待初始化的配置结构体的指针
 */
void rs485_adapter_config_set_defaults(rs485_adapter_config_t *config);

/**
 * @brief 启动 RS485 适配器的线程及相关资源
 * 
 * 根据配置初始化RS485相关内存状态，并在配置启用时创建后台接收（RX）线程。
 * 
 * @param config 指向初始化配置信息结构体的指针
 * @return int 成功返回 0，失败返回 -1
 */
int rs485_adapter_start(const rs485_adapter_config_t *config);

/**
 * @brief 停止 RS485 适配器的线程并释放相关资源
 * 
 * 向接收线程发送停止信号，关闭已打开的串口描述符，并等待线程退出以确保安全关闭。
 */
void rs485_adapter_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* RS485_ADAPTER_H */

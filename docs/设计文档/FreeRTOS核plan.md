# FreeRTOS 小核 CAN 转发设计方案

## 1. 设计定位

本方案描述 SG2002 小核 FreeRTOS 侧的实时 CAN 转发设计。当前大小核之间的统一协议暂不定义，后续协议出来前，本方案不固定共享内存 payload 格式、payload 长度、magic、version、CRC、frame type 或业务字段。

系统主线调整为：

```text
外部协议输入
    ↓
Linux 大核协议解析 / 业务映射
    ↓
后续统一协议封装（TBD）
    ↓
/dev/cvi-rtos-cmdqu + 共享内存 IPC payload（格式 TBD）
    ↓
FreeRTOS 协议适配层（TBD）
    ↓
rtos_can_message_t
    ↓
XL2515 CAN 控制器 + XL1050 CAN 收发器
    ↓
CAN Bus
```

反向链路为：

```text
CAN Bus
    ↓
XL2515 CAN 控制器 + XL1050 CAN 收发器
    ↓
rtos_can_message_t
    ↓
FreeRTOS 协议适配层（TBD）
    ↓
共享内存 IPC payload（格式 TBD）
    ↓
Linux 大核协议解析 / 状态整理 / Web 快照
```

小核当前稳定职责：

- 维护内部 CAN 消息 `rtos_can_message_t`。
- 将协议适配层输出的 CAN 消息排队并发送到 CAN 总线。
- 从 CAN 总线读取报文并交给后续协议适配层。
- 维护 CAN、IPC、队列、错误恢复和状态统计。

小核不解析 4G、WiFi、蓝牙、RS485、以太网、MQTT 等外部协议，也不在本阶段定义大小核 payload 格式。历史旧实验链路中的 `unified_frame_t` / `rtos_gateway_frame` 只作为参考，不作为当前正式接口。

## 2. 设计依据

### 2.1 协议边界

当前统一协议未定，因此本方案只锁定 FreeRTOS CAN 层和 IPC 通道边界：

| 层级 | 当前约束 |
| ---- | -------- |
| 共享内存 IPC | 只提供双向消息槽、通知、cache 同步和错误返回 |
| IPC payload | 暂不定义字段和长度，等待后续统一协议文档 |
| FreeRTOS 协议适配层 | 将 TBD payload 转为 `rtos_can_message_t`，反向将 CAN RX 转为 TBD payload |
| FreeRTOS CAN 层 | 只处理 `rtos_can_message_t` |

旧实验链路中曾使用 `unified_frame_t`、固定长度、magic/version/CRC 和 `rtos_gateway_frame` validator 验证过方向，但这些内容不再作为本方案的正式 ABI。

### 2.2 内部 CAN 消息

`rtos_can_message_t` 是小核 CAN 层当前稳定接口，定义在：

```text
freertos/cvitek/task/comm/include/rtos_can_message.h
```

当前字段：

```c
typedef struct {
    uint32_t can_id;
    uint8_t can_dlc;
    uint8_t can_flags;
    uint8_t can_data[RTOS_CAN_CLASSIC_DATA_MAX_LEN];
} rtos_can_message_t;
```

v1 只支持经典 CAN：

| 项目 | 当前取值 |
| ---- | -------- |
| 数据长度 | `0 ~ RTOS_CAN_CLASSIC_DATA_MAX_LEN`，当前为 8 |
| 标准帧 ID | `0x000 ~ RTOS_CAN_STANDARD_ID_MAX`，当前为 `0x7FF` |
| 扩展帧 ID | `0x00000000 ~ RTOS_CAN_EXTENDED_ID_MAX`，当前为 `0x1FFFFFFF` |
| 支持 flag | `RTOS_CAN_FLAG_NONE`、`RTOS_CAN_FLAG_EXTENDED_ID` |
| 暂不支持 | CAN FD、BRS、RTR |

协议适配层负责把未来统一协议中的业务字段、来源信息、序号、时间戳、校验等内容转换为 CAN 层需要的 ID、DLC、flag 和 data。

### 2.3 SG2002 小核资源

根据 `docs/手册/sg2002_trm_cn.pdf` 和 `docs/原理图/sg2002-diagram.webp`：

- SG2002 包含一个用于 RTOS 的 RISC-V C906 小核，频率约 700MHz。
- 小核可用于实时控制任务，适合承载 CAN 转发、状态统计和看门狗逻辑。
- SG2002 外设资源包含 SPI、UART、GPIO、Watchdog 等。
- `ap_mailbox` 可作为大小核间轻量通知机制的硬件基础。

### 2.4 CAN 硬件通道

根据 `docs/原理图/duo_iob_v1.11.pdf`：

- CAN 控制器为 `XL2515`，与 MCP2515 类似，通过 SPI 接入主控。
- CAN 收发器为 `XL1050`。
- `XL2515` 使用 16MHz 晶振。

| XL2515 信号 | SG2002 / IOB 信号 |
| ----------- | ----------------- |
| `CS#` | `SPI2_CSn` |
| `SCK` | `SPI2_SCK` |
| `SI` | `SPI2_TX` |
| `SO` | `SPI2_RX` |
| `INT#` | `GPIO14` |
| `RXCAN` | `RX_CAN` |
| `TXCAN` | `TX_CAN` |

`XL1050` 将 `TX_CAN/RX_CAN` 转换为物理 CAN 总线的 `CAN_H/CAN_L`。v1 的 CAN 驱动按外置 SPI CAN 控制器设计，不假设 SG2002 内部集成 CAN IP。

## 3. 推荐目录结构

后续实现小核代码时，建议使用以下目录：

```text
freertos/cvitek/task/comm/
├── include/
│   ├── rtos_can_message.h       # FreeRTOS CAN 层内部消息
│   ├── rtos_protocol_adapter.h  # 后续协议适配层接口，占位
│   ├── rtos_ipc.h               # cmdqu / mailbox / 共享内存封装
│   ├── rtos_can_forward.h       # CAN 转发核心接口
│   ├── rtos_can_task.h          # FreeRTOS 任务声明
│   ├── rtos_can_driver.h        # XL2515 / CAN 驱动抽象
│   ├── rtos_status.h            # 状态统计
│   └── rtos_config.h            # 队列长度、优先级、开关配置
│
└── src/riscv64/
    ├── comm_main.c              # 小核入口，调用 gateway_forward_init()
    ├── rtos_ipc.c               # 处理 Linux 发来的 cmdqu 命令
    ├── rtos_can_forward.c       # 小核 CAN 转发主逻辑
    ├── rtos_can_task.c          # CAN_TX_Task / CAN_RX_Task / Status_Task
    ├── rtos_can_driver.c        # XL2515 初始化、发送、接收、错误读取
    └── rtos_status.c            # 统计 tx/rx/drop/error
```

`common/include` 中后续只放真正大小核共用、已经锁定的接口。不把 FreeRTOS 私有任务、队列和驱动细节放入 `common`。

## 4. 总体架构

```text
                         Linux 大核
        外部协议解析 / 后续统一协议封装（TBD）
                            |
                            |
                    /dev/cvi-rtos-cmdqu
                            |
                     共享内存 IPC payload
                            |
                            v
+--------------------------------------------------+
|                FreeRTOS 小核                     |
|                                                  |
|  +------------------+                            |
|  | Gateway_IPC_Task | <--- cmdqu / mailbox       |
|  +------------------+                            |
|           |                                      |
|           v                                      |
|  +------------------+                            |
|  | Protocol Adapter |  payload TBD -> CAN msg    |
|  +------------------+                            |
|           |                                      |
|           v                                      |
|  +------------------+        +----------------+  |
|  | CAN TX Queue     | -----> | CAN_TX_Task    |  |
|  +------------------+        +----------------+  |
|                                      |           |
|                                      v           |
|                               XL2515 Driver      |
|                                      |           |
|                                      v           |
|                                  CAN Bus         |
|                                      ^           |
|                                      |           |
|  +------------------+        +----------------+  |
|  | CAN RX Queue     | <----- | CAN_RX_Task    |  |
|  +------------------+        +----------------+  |
|           |                                      |
|           v                                      |
|  +------------------+                            |
|  | Protocol Adapter |  CAN msg -> payload TBD    |
|  +------------------+                            |
|           |                                      |
|           v                                      |
|  +------------------+                            |
|  | Linux Return     | -----> cmdqu / IPC         |
|  +------------------+                            |
|                                                  |
|  +------------------+                            |
|  | Status_Task      |                            |
|  +------------------+                            |
+--------------------------------------------------+
```

核心原则：

- Linux 负责复杂协议和业务映射。
- FreeRTOS 负责实时性、CAN 收发和异常恢复。
- 共享内存只做双向消息传输，不解释 payload。
- cmdqu / mailbox 只做命令与通知，不承载大量数据。
- Web 不直接访问小核或共享内存；状态统一由 Linux 整理为 `/run/put/status/` 快照。

## 5. 大小核通信设计

### 5.1 通信职责

Linux 到 FreeRTOS：

- 通知小核有新的 IPC payload 可读取。
- 下发后续统一协议定义的业务消息或控制消息。
- 发送心跳和恢复握手。

FreeRTOS 到 Linux：

- 回传 CAN RX 对应的 IPC payload。
- 回传 TX 结果、错误事件、状态统计。
- 上报小核心跳。

Web 监控边界：

- FreeRTOS 不提供 Web API，也不面向浏览器或 Rust Web 后端开放接口。
- Web 需要展示的 CAN、IPC、错误统计都先通过 RTOS -> Linux 通道回传给大核。
- `linux_app` 接收小核状态后，整理为 `can_status.json`、`ipc_status.json`、`events.jsonl` 等快照文件。
- Web 后端只读取这些快照文件，不向小核下发启停 CAN、清空统计、设置 bitrate 等控制命令。

### 5.2 共享内存接口预留

当前共享内存模块还没有实现，本方案只预留对接边界，不负责实现共享内存内部细节。共享内存负责人后续可以选择 ring、mailbox、DMA 或其他机制，只要提供双向 payload 通道即可。

对本小核方案来说，共享内存负责人只需要满足：

- 提供 Linux -> RTOS 和 RTOS -> Linux 两个方向的消息通道。
- 完成写共享内存、cache 同步和通知对端的内部动作。
- 支持空、满、可读、可写等队列状态。
- 成功返回 `UNIFIED_OK`，失败返回 `unified_error_t` 或后续 IPC 专用错误码。
- 不要求大核业务层或小核 CAN 层知道共享内存内部是 ring、mailbox、DMA 还是其他结构。
- 不在共享内存层解释 payload 字段。

### 5.3 对接边界

| 对接方 | 只需要依赖 | 不需要关心 |
| ------ | ---------- | ---------- |
| 大核协议层 | 后续统一协议和 IPC 发送接口 | 小核 CAN 驱动寄存器 |
| 共享内存模块 | payload 字节和方向 | payload 业务语义 |
| 小核协议适配层 | TBD payload 和 `rtos_can_message_t` | 外部协议来源和 Web 展示 |
| 小核 CAN 层 | `rtos_can_message_t` | 大小核 payload 格式 |
| Web 模块 | `/run/put/status/` 状态快照 | 共享内存和小核内部接口 |

## 6. FreeRTOS CAN 层校验设计

CAN 层只校验 `rtos_can_message_t` 的 CAN 约束：

1. 指针非空。
2. `can_flags` 只允许 `RTOS_CAN_FLAG_NONE` 或 `RTOS_CAN_FLAG_EXTENDED_ID`。
3. 标准帧 `can_id <= RTOS_CAN_STANDARD_ID_MAX`。
4. 扩展帧 `can_id <= RTOS_CAN_EXTENDED_ID_MAX`。
5. `can_dlc <= RTOS_CAN_CLASSIC_DATA_MAX_LEN`。

CAN 层不校验：

- 大小核 payload 长度。
- payload 版本、校验和、消息类型。
- 协议类型、车身业务类型、来源 ID。
- Linux 序号或时间戳。

这些字段等待后续统一协议确定后，由协议适配层负责。

当前 CAN 转发入口：

```c
unified_error_t rtos_can_forward_submit_message(const rtos_can_message_t *message);
```

## 7. FreeRTOS 任务设计

### 7.1 任务与优先级

| 任务 | 优先级 | 职责 |
| ---- | -----: | ---- |
| `CAN_RX_Task` | 最高 | 响应 GPIO14/XL2515 中断，优先清空 RX0/RX1 硬件 buffer |
| `CAN_TX_Task` | 高 | 消费 CAN TX 队列，调用 XL2515 发送 CAN |
| `Gateway_IPC_Task` | 中高 | 处理 cmdqu 通知，读取共享内存 payload，交给协议适配层 |
| `Status_Task` | 中 | 周期上报统计、心跳和队列水位 |
| `Watchdog_Task` | 中 | 喂狗、检测任务心跳和总线异常 |

XL2515 / MCP2515 类外置 SPI CAN 控制器通常只有 2 个硬件 RX buffer。500kbps 高负载 CAN 总线上，如果 TX 任务持续占用 CPU 或 SPI，RX buffer 会很快溢出。因此 `CAN_RX_Task` 优先级必须高于 `CAN_TX_Task`，GPIO14 中断唤醒后必须优先读取硬件 RX buffer。

### 7.2 Gateway_IPC_Task

职责：

- 初始化 cmdqu / mailbox 接收。
- 等待 Linux doorbell。
- 批量读取 Linux -> RTOS 通道中可用 payload。
- 将 payload 交给后续协议适配层。
- 协议适配层若输出 `rtos_can_message_t`，投递到 CAN TX Queue。

队列满时：

- 丢弃最新 CAN 消息。
- 增加 `drop_queue_full`。
- 回传 queue full 状态。

### 7.3 CAN_TX_Task

职责：

- 等待 CAN TX Queue。
- 调用 `rtos_can_driver_send()`。
- 根据返回结果更新 `tx_ok`、`tx_fail`、`bus_error`、`spi_error` 等统计。
- 发送失败时按错误类型决定是否重试。
- 单次发送和重试不得连续长时间占用 SPI/CPU，必须允许最高优先级的 `CAN_RX_Task` 抢占。
- 进入 offline / fail-safe 状态后立即停止消费 CAN TX Queue，拒绝发送超时前残留的旧业务消息。

推荐重试策略：

| 错误 | 策略 |
| ---- | ---- |
| SPI 短暂忙 | 最多重试 2 次，每次重试前短暂让出 |
| TX buffer 满 | 等待一个短周期后重试，等待期间允许 RX 抢占 |
| CAN error passive | 继续发送并上报 |
| bus-off | 停止发送，复位 XL2515，恢复后再发送 |

### 7.4 CAN_RX_Task

职责：

- 初始化 GPIO14 为 XL2515 `INT#` 输入中断。
- 中断中只释放 semaphore，不在 ISR 中读 SPI。
- 最高优先级任务上下文中读取 XL2515 中断标志。
- 收到 GPIO14 中断后，应尽快 drain XL2515 的 RX0/RX1 硬件 buffer。
- 读取 RX buffer 后生成 `rtos_can_message_t`。
- 将 CAN RX 消息交给后续协议适配层，由适配层封装为 TBD payload 后回传 Linux。
- 检测 XL2515 RX overflow / overrun 标志，更新统计并作为高优先级告警回传 Linux。

### 7.5 Status_Task

职责：

- 每 1000ms 上报一次小核状态。
- 统计 CAN TX/RX、丢帧、DLC 错误、CAN ID 错误、队列满、SPI 错误、bus-off 次数。
- 上报 TX/RX 队列水位。
- 上报 Linux 心跳是否超时。
- 支持 Linux 主动查询。

状态上报只面向 Linux 大核。Web 页面展示的小核在线、CAN 总线状态、IPC 超时、队列水位和错误计数，都由 Linux 大核接收后转换为 `/run/put/status/` 下的只读快照文件。

### 7.6 Watchdog_Task

职责：

- 周期喂硬件看门狗。
- 检查关键任务 heartbeat。
- 检查 XL2515 是否长时间无响应。
- 检查 Linux 心跳超时。

异常处理：

- 单个任务 heartbeat 超时：记录错误，尝试软恢复相关模块。
- XL2515 无响应：复位 CAN 控制器并重新初始化。
- Linux 心跳超时：执行 fail-safe offline，停止发送路径，清空 CAN TX Queue，取消 XL2515 TX buffer，切换 CAN 控制器到 Listen-Only。

### 7.7 Linux fail-safe offline

Linux 心跳超时代表大核可能已经崩溃、重启或无法继续确认业务状态。此时小核不得继续发送超时前滞留的控制指令。

进入 fail-safe offline 时按以下顺序处理：

1. 设置 `linux_online = false` 和 `tx_enabled = false`。
2. `Gateway_IPC_Task` 停止接收新的 Linux 下发业务 payload，只保留恢复握手命令。
3. `CAN_TX_Task` 停止消费 CAN TX Queue。
4. 清空软件 CAN TX Queue，丢弃所有未发送业务消息。
5. 调用 CAN 驱动取消 XL2515 已请求发送的 TX buffer。
6. 清空 XL2515 TX buffer 和相关中断标志。
7. 将 XL2515 切换到 Listen-Only 模式。
8. 保留 `CAN_RX_Task`、`Status_Task` 和错误统计上报。

Linux 恢复后必须重新走 HELLO / READY 握手。握手完成前，小核保持 Listen-Only，不自动恢复发送；握手完成后清除 offline 状态，重新进入 Normal Mode，并只接受恢复后的新业务消息。

## 8. CAN 驱动设计

### 8.1 驱动边界

`rtos_can_driver` 只暴露抽象接口，不让业务层感知 XL2515 寄存器细节。

建议接口职责：

```text
rtos_can_driver_init()
rtos_can_driver_set_bitrate()
rtos_can_driver_send()
rtos_can_driver_read()
rtos_can_driver_get_error()
rtos_can_driver_abort_tx()
rtos_can_driver_clear_tx_buffers()
rtos_can_driver_set_listen_only()
rtos_can_driver_set_normal()
rtos_can_driver_reset()
```

`rtos_can_forward` 只调用这些接口，不直接读写 SPI。

### 8.2 XL2515 默认配置

| 项目 | v1 默认值 |
| ---- | --------- |
| 控制器 | XL2515 |
| 收发器 | XL1050 |
| 晶振 | 16MHz |
| SPI 控制器 | SPI2 |
| SPI 模式 | Motorola SPI mode 0 |
| SPI 数据宽度 | 8 bit |
| SPI 初始频率 | 1MHz |
| SPI 稳定运行频率 | 8MHz |
| CAN bitrate | 500kbps |
| CAN 模式 | 默认 Normal，fail-safe offline 时切换 Listen-Only |
| 调试模式 | 支持 Loopback 编译开关 |

初始化顺序：

1. 配置 SPI2 pinmux。
2. 配置 SPI2 控制器。
3. 配置 GPIO14 为输入中断。
4. 复位 XL2515。
5. 进入 Configuration Mode。
6. 设置 bitrate。
7. 设置 RX mask/filter，v1 默认接收全部。
8. 清空 TX/RX buffer 和中断标志。
9. 进入 Normal Mode。
10. 启动 CAN RX/TX 任务。

### 8.3 SPI2 管脚依据

SG2002 TRM 与 Duo256M 原理图中，SPI2 可复用在 SD1 引脚：

| SG2002 引脚 | SPI2 功能 |
| ----------- | --------- |
| `SD1_CLK` | `SPI2_SCK` |
| `SD1_CMD` | `SPI2_SDO` |
| `SD1_D0` | `SPI2_SDI` |
| `SD1_D3` | `SPI2_CS_X` |

IOB 原理图已将这些信号引到 XL2515，因此 FreeRTOS BSP 侧需要确保这些 pinmux 被配置为 SPI2，而不是默认 SD1 / PWR_SPINOR1 功能。

## 9. 状态与错误设计

### 9.1 统计项

小核维护以下统计：

| 统计项 | 含义 |
| ------ | ---- |
| `rx_from_linux` | 从 Linux 通道读取并成功交给 CAN 层处理的消息数 |
| `tx_to_can_ok` | 成功发送到 CAN 的消息数 |
| `tx_to_can_fail` | CAN 发送失败次数 |
| `rx_from_can` | CAN 总线收到的消息数 |
| `tx_to_linux` | 回传 Linux 的 payload 数 |
| `rx_overrun` | 小核处理不及时导致的 CAN RX 过载次数 |
| `xl2515_rx_overflow` | XL2515 RX0/RX1 硬件 buffer 溢出次数 |
| `drop_null` | 空 CAN 消息指针 |
| `drop_dlc` | CAN DLC 非法丢弃 |
| `drop_can_id` | CAN ID 非法丢弃 |
| `drop_flag` | CAN flag 不支持丢弃 |
| `drop_queue_full` | FreeRTOS 队列满丢弃 |
| `drop_ring_full` | 回传 IPC 通道满丢弃 |
| `spi_error` | SPI 读写错误 |
| `can_bus_off` | CAN bus-off 次数 |
| `can_error_passive` | CAN error passive 次数 |
| `linux_heartbeat_timeout` | Linux 心跳超时次数 |
| `linux_offline_enter` | 进入 Linux offline / fail-safe 次数 |
| `tx_queue_purged` | fail-safe 时清空软件 TX 队列的消息数 |
| `xl2515_tx_aborted` | fail-safe 时取消 XL2515 TX buffer 的次数 |
| `listen_only_enter` | 切入 Listen-Only 模式次数 |
| `linux_rehandshake_ok` | Linux 恢复握手成功次数 |

协议字段错误、payload 版本错误、payload 校验错误等统计项等待后续统一协议确定后，在协议适配层补充。

### 9.2 错误处理策略

| 场景 | 处理 |
| ---- | ---- |
| CAN message 为空 | 丢弃，计数，上报 |
| CAN DLC 越界 | 丢弃，计数，上报 |
| v1 不支持 CAN flag | 丢弃，计数，上报 |
| 标准/扩展 CAN ID 越界 | 丢弃，计数，上报 |
| Linux->RTOS 通道空 | 忽略本次通知 |
| CAN TX Queue 满 | 丢弃最新消息，计数，上报 |
| RTOS->Linux 通道满 | 丢弃回传 payload，保留统计，下一次状态上报体现 |
| SPI 超时 | 重试，仍失败则复位 XL2515 |
| XL2515 RX overflow | 立即 drain RX buffer，计数，并作为高优先级告警回传 Linux |
| CAN bus-off | 停止 TX，复位 XL2515，恢复后重新进入 Normal Mode |
| Linux 心跳超时 | 停止 TX，清空 TX 队列，取消 XL2515 TX buffer，切 Listen-Only |
| Linux 心跳恢复 | 先完成 HELLO / READY 重新握手，再恢复发送 |

这些统计和错误事件的权威来源是小核状态上报；Web 只展示 Linux 大核整理后的快照，不参与错误恢复决策。

## 10. 配置项

`rtos_config.h` 建议包含：

| 配置 | 默认值 |
| ---- | -----: |
| `RTOS_CAN_TX_QUEUE_LEN` | 32 |
| `RTOS_CAN_RX_QUEUE_LEN` | 32 |
| `RTOS_STATUS_PERIOD_MS` | 1000 |
| `RTOS_LINUX_HEARTBEAT_TIMEOUT_MS` | 3000 |
| `RTOS_CAN_BITRATE` | 500000 |
| `RTOS_XL2515_OSC_HZ` | 16000000 |
| `RTOS_SPI_INIT_HZ` | 1000000 |
| `RTOS_SPI_RUN_HZ` | 8000000 |
| `RTOS_CAN_TX_RETRY_MAX` | 2 |
| `RTOS_CAN_LOOPBACK_ENABLE` | 0 |
| `RTOS_FAIL_SAFE_LISTEN_ONLY_ENABLE` | 1 |
| `RTOS_LINUX_REHANDSHAKE_REQUIRED` | 1 |

这些默认值用于 v1 调通，后续可根据实测吞吐和稳定性调整。

## 11. 初始化流程

小核入口 `comm_main.c` 调用：

```text
gateway_forward_init()
```

推荐初始化顺序：

1. 初始化状态统计模块。
2. 初始化共享内存 IPC 元数据。
3. 初始化 cmdqu / mailbox。
4. 初始化 SPI2、GPIO14 和 XL2515。
5. 创建 CAN TX Queue 和 CAN RX Queue。
6. 创建 `Gateway_IPC_Task`。
7. 创建 `CAN_RX_Task`。
8. 创建 `CAN_TX_Task`。
9. 创建 `Status_Task`。
10. 创建 `Watchdog_Task`。
11. 向 Linux 回传 `RTOS_READY`。

如果 CAN 初始化失败：

- 小核仍启动 IPC 和 Status 任务。
- 状态中标记 `can_ready = false`。
- Linux 可通过后续协议命令触发重新初始化。

## 12. Linux 协作约束

Linux 大核侧需要遵守：

- 当前不要把旧实验链路当作正式大小核协议。
- 后续统一协议确定前，不在共享内存 ABI 中写死 payload 字段。
- 写共享内存后 flush cache，再通过 `/dev/cvi-rtos-cmdqu` 通知小核。
- Linux 重启或心跳恢复后，必须先发送 HELLO / READY 重新握手命令。
- 重新握手完成前，小核保持 Listen-Only，不接受普通业务下发消息。
- 重新握手完成后，Linux 必须重新下发仍然需要执行的业务指令；小核不会保留或重放超时前的旧 TX 队列内容。

## 13. 测试方案

### 13.1 FreeRTOS CAN 层测试

- `rtos_can_message_t` 合法标准帧能进入 CAN TX 并调用 driver。
- 合法扩展帧能进入 CAN TX 并调用 driver。
- 空指针被拒绝并计数。
- DLC 大于 `RTOS_CAN_CLASSIC_DATA_MAX_LEN` 被拒绝并计数。
- 标准帧 ID 大于 `RTOS_CAN_STANDARD_ID_MAX` 被拒绝并计数。
- 扩展帧 ID 大于 `RTOS_CAN_EXTENDED_ID_MAX` 被拒绝并计数。
- 不支持 flag 被拒绝并计数。
- CAN TX Queue 满时丢弃最新消息。
- driver 发送失败时记录 `tx_to_can_fail`。

### 13.2 共享 IPC 通道测试

- 空通道读取。
- 满通道写入。
- 读写索引回绕。
- 连续写满再读空。
- Linux 写入后小核读取的 cache 一致性验证。
- 小核写回后 Linux 读取的 cache 一致性验证。
- 不校验 payload 字段内容，payload 字段级测试等待统一协议确定。

### 13.3 FreeRTOS 任务测试

- cmdqu 通知一次读取一条 payload。
- cmdqu 通知一次批量读取多条 payload。
- 协议适配层输出非法 CAN 消息时不进入 CAN TX Queue。
- CAN TX Queue 满时丢弃最新消息。
- RX/TX 并发时，`CAN_RX_Task` 不被 `CAN_TX_Task` 饿死。
- Linux 心跳超时后，`CAN_TX_Task` 停止消费 TX 队列并拒绝发送旧消息。
- Linux 恢复握手完成前，普通业务 payload 不会重新打开 TX 路径。
- Status_Task 周期上报统计。
- Watchdog_Task 检测任务 heartbeat。

### 13.4 CAN 硬件测试

- XL2515 SPI 读写寄存器测试。
- XL2515 loopback 模式发送接收测试。
- Normal 模式接 CAN 分析仪发送测试。
- CAN 分析仪发送，小核 RX 并回传 Linux。
- 高负载 CAN RX 压力测试，确认小核及时 drain RX0/RX1 buffer。
- RX/TX 并发压力测试，确认不发生 RX 饿死。
- XL2515 RX overflow 检测测试。
- Linux 心跳超时测试，确认 TX Queue 被清空，XL2515 TX buffer 被取消，CAN 不再发出旧控制消息。
- Linux 重启恢复测试，确认 HELLO / READY 握手完成前不会自动恢复 Normal Mode。
- Listen-Only 测试，确认小核仍能 RX / 统计 / 上报状态，但不会主动 TX。
- TX 积压并发超时测试，Linux 超时时如果 TX 队列正有积压，确认残留控制指令全部丢弃并计数。
- bus-off 场景恢复测试。
- 拔掉 CAN_H/CAN_L 后错误统计测试。

### 13.5 集成测试

完整链路等待后续统一协议确定后补齐：

```text
RS485 / WiFi / MQTT / Ethernet 输入
    ↓
Linux parser
    ↓
统一协议 payload（TBD）
    ↓
/dev/cvi-rtos-cmdqu + shared memory IPC
    ↓
FreeRTOS protocol adapter
    ↓
rtos_can_message_t
    ↓
CAN_TX_Task
    ↓
XL2515 + XL1050
    ↓
CAN 分析仪 / 车身节点
```

验收条件：

- 合法业务消息能稳定发送到 CAN。
- 非法业务消息不会进入 CAN 总线。
- Linux 能收到小核状态和错误统计。
- CAN RX 能回传 Linux。
- Web 相关状态只通过 Linux 大核快照展示，不直接连接或控制 FreeRTOS 小核。
- 连续运行 24 小时无任务死锁、无异常重启、无无法解释的丢帧。

## 14. 分阶段实现路线

当前实现边界：

- 本轮只实现 FreeRTOS 小核侧 `freertos/cvitek/task/comm` 下的私有代码。
- `common/`、`linux_app/`、共享内存实现、Web、既有测试脚本和大核协议链路暂不修改。
- 小核 CAN 层只依赖 `rtos_can_message_t`，不绑定未来大小核统一协议。

### 阶段 1：协议边界降级

- 明确旧实验链路不是正式大小核 ABI。
- FreeRTOS CAN 层改为依赖 `rtos_can_message_t`。
- 共享内存 IPC 在本方案中只作为双向 payload 通道，不定义 payload。
- 旧 frame validator 可临时保留为 legacy 文件，但不作为主链路接口。

### 阶段 2：小核 CAN 最小链路

- 建立或整理 `freertos/cvitek/task/comm` 目录。
- 实现 `comm_main.c` 和 `gateway_forward_init()`，初始化状态统计、mock IPC、mock CAN driver 和软件 TX 队列。
- 实现 `rtos_can_forward_submit_message()`。
- `rtos_ipc` 只保留初始化和 mock 注入入口，不实现真实共享 ring、cache 同步或 cmdqu doorbell。
- `rtos_can_driver` 先提供私有抽象和 mock 计数实现；XL2515 SPI 寄存器、GPIO14 中断和真实 FreeRTOS task 调度留到后续阶段。
- 增加 FreeRTOS 侧 mock 构建/测试入口，用静态注入 `rtos_can_message_t` 验证合法转发和 DLC/flag/CAN ID 等非法路径。

### 阶段 3：XL2515 驱动

- 实现 SPI2 初始化。
- 实现 XL2515 reset / register read-write。
- 实现 bitrate 配置。
- 实现 loopback send / receive。
- 实现 normal mode 发送。

### 阶段 4：共享内存适配点预留与状态回传占位

- 预留 `rtos_ipc` 与共享内存模块的接入点。
- `Gateway_IPC_Task` 内部先从 mock queue 或测试入口取 payload。
- 等后续统一协议确定后，将 payload 交给协议适配层解析。
- 实现 GPIO14 中断。
- 实现 CAN_RX_Task。
- 定义 CAN RX、状态统计、错误事件的回传调用点。
- RTOS -> Linux 回传先保留状态结构和接口占位，不实现真实共享内存写回。
- 真实 RTOS -> Linux 通道由共享内存负责人确认后再接入。

### 阶段 5：稳定性与错误恢复

- 完成状态统计。
- 保持 `CAN_RX_Task` 最高优先级，防止 XL2515 RX buffer 溢出。
- 完成 bus-off 恢复。
- 完成 SPI 超时恢复。
- 完成 Linux heartbeat 超时处理，进入 fail-safe offline 并切换 Listen-Only。
- 完成长时间压力测试。

### 阶段 6：统一协议确定后联调

- 按后续统一协议实现 Linux 和 FreeRTOS 两侧协议适配层。
- 将 IPC payload 字段级校验放入协议适配层。
- 验证 Linux -> RTOS payload 到 `rtos_can_message_t` 的转换。
- 验证 CAN RX 到 RTOS -> Linux payload 的转换。
- 验证队列满、Linux offline、RTOS offline、心跳超时等异常路径。
- 确认 offline / fail-safe 状态下不会发送旧业务消息。

## 15. 后续待确认

- 后续统一协议的 payload 格式、长度、版本和校验方式。
- SDK 中 `/dev/cvi-rtos-cmdqu` 单条命令结构和最大 payload。
- Linux 与 RTOS 共享内存的实际物理地址、大小和 cache 属性。
- FreeRTOS BSP 中 SPI2、GPIO14、cache、mailbox API 的具体函数名。
- XL2515 与 MCP2515 的寄存器兼容程度。
- 最终 CAN bitrate 是否固定 500kbps，还是需要 Linux 动态配置。
- CAN RX 回传 payload 中是否需要携带通道号、时间戳、硬件错误状态。

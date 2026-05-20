# FreeRTOS 小核 CAN 转发设计方案

## 1. 设计定位

本方案描述 SG2002 小核 FreeRTOS 侧的实时 CAN 转发设计。系统主线为：

```text
外部协议输入
    ↓
Linux 大核协议解析 / 协议转换 / unified_frame_t 封装
    ↓
/dev/cvi-rtos-cmdqu + 共享内存 ring
    ↓
FreeRTOS 小核接收 / 校验 / 排队
    ↓
XL2515 CAN 控制器 + XL1050 CAN 收发器
    ↓
CAN Bus
```

小核只识别项目内部统一帧 `unified_frame_t`，不解析 4G、WiFi、蓝牙、RS485、以太网、MQTT 等外部协议。外部协议解析、业务命令提取、CAN ID 与 CAN DATA 映射全部由 Linux 大核完成。

## 2. 设计依据

### 2.1 公共协议帧

小核与大核之间统一使用 `common/include/unified_frame.h` 中定义的 `unified_frame_t`。

关键约束如下：

| 项目         | 取值                                 |
| ------------ | ------------------------------------ |
| 帧长度       | 96 字节                              |
| magic        | `0xA55A`                             |
| version      | `0x01`                               |
| CRC          | CRC-16/CCITT-FALSE                   |
| CRC 覆盖范围 | 前 94 字节，不包含 `crc16` 字段      |
| 普通 CAN DLC | `0 ~ 8`                              |
| CAN FD DLC   | `0 ~ 64`，需要 `UNIFIED_CAN_FLAG_FD` |

当前硬件设计优先支持经典 CAN，因此 v1 阶段只接收：

- `UNIFIED_CAN_FLAG_NONE`
- `UNIFIED_CAN_FLAG_EXTENDED_ID`

v1 阶段暂不启用：

- `UNIFIED_CAN_FLAG_FD`
- `UNIFIED_CAN_FLAG_BRS`
- `UNIFIED_CAN_FLAG_RTR`

收到暂不支持的 flag 时，小核丢弃该帧并上报错误统计。

### 2.2 SG2002 小核资源

根据 `docs/手册/sg2002_trm_cn.pdf` 和 `docs/原理图/sg2002-diagram.webp`：

- SG2002 包含一个用于 RTOS 的 RISC-V C906 小核，频率约 700MHz。
- 小核可用于实时控制任务，适合承载 CAN 转发、状态统计和看门狗逻辑。
- SG2002 外设资源包含 SPI、UART、GPIO、Watchdog 等。
- `ap_mailbox` 可作为大小核间轻量通知机制的硬件基础。

### 2.3 CAN 硬件通道

根据 `docs/原理图/duo_iob_v1.11.pdf`：

- CAN 控制器为 `XL2515`，与 MCP2515 类似，通过 SPI 接入主控。
- CAN 收发器为 `XL1050`。
- `XL2515` 使用 16MHz 晶振。
- `XL2515` 与 SG2002 连接关系：

| XL2515 信号 | SG2002 / IOB 信号 |
| ----------- | ----------------- |
| `CS#`       | `SPI2_CSn`        |
| `SCK`       | `SPI2_SCK`        |
| `SI`        | `SPI2_TX`         |
| `SO`        | `SPI2_RX`         |
| `INT#`      | `GPIO14`          |
| `RXCAN`     | `RX_CAN`          |
| `TXCAN`     | `TX_CAN`          |

`XL1050` 将 `TX_CAN/RX_CAN` 转换为物理 CAN 总线的 `CAN_H/CAN_L`。

因此 v1 的 CAN 驱动按外置 SPI CAN 控制器设计，不假设 SG2002 内部集成 CAN IP。

## 3. 推荐目录结构

后续实现小核代码时，建议使用以下目录：

```text
freertos/cvitek/task/comm/
├── include/
│   ├── rtos_gateway_cmd.h       # Linux <-> 小核命令定义
│   ├── rtos_gateway_frame.h     # 统一帧适配与 RTOS 内部 CAN 消息
│   ├── rtos_ipc.h               # cmdqu / mailbox / 共享内存封装
│   ├── rtos_can_forward.h       # 转发核心接口
│   ├── rtos_can_task.h          # FreeRTOS 任务声明
│   ├── rtos_can_driver.h        # XL2515 / CAN 驱动抽象
│   ├── rtos_status.h            # 状态统计
│   ├── rtos_ring.h              # 共享内存 ring buffer
│   └── rtos_config.h            # 队列长度、优先级、开关配置
│
└── src/riscv64/
    ├── comm_main.c              # 小核入口，调用 gateway_forward_init()
    ├── rtos_ipc.c               # 处理 Linux 发来的 cmdqu 命令
    ├── rtos_can_forward.c       # 小核转发主逻辑
    ├── rtos_can_task.c          # CAN_TX_Task / CAN_RX_Task / Status_Task
    ├── rtos_can_driver.c        # XL2515 初始化、发送、接收、错误读取
    ├── rtos_status.c            # 统计 tx/rx/drop/error
    └── rtos_ring.c              # 共享内存 ring 读写
```

`common/include` 中的公共头文件保持为大小核共用接口，不把 FreeRTOS 私有任务、队列、驱动细节放入 `common`。

## 4. 总体架构

```text
                         Linux 大核
        协议解析 / 协议转换 / unified_frame_t 封装
                            |
                            |
                    /dev/cvi-rtos-cmdqu
                            |
                     共享内存 ring
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
|  | Frame Validator  |                            |
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
|  | Linux Return     | -----> cmdqu / ring        |
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
- cmdqu / mailbox 只做命令与通知，不承载大量帧数据。
- 96 字节统一帧通过共享内存 ring 传输。
- 小核内部用 FreeRTOS queue 解耦 IPC 接收、CAN 发送、CAN 接收和状态上报。
- Web 模块不直接访问小核，也不读取共享内存 ring；小核状态统一先回传 Linux 大核，再由 `linux_app` 写入 `/run/put/status/` 供 Web 后端只读展示。

## 5. 大小核通信设计

### 5.1 通信职责

Linux 到 FreeRTOS：

- 通知小核有新的 `unified_frame_t` 可读取。
- 下发配置命令，例如启停 CAN、设置 bitrate、读取状态、清空统计。
- 发送心跳。

FreeRTOS 到 Linux：

- 回传 CAN RX 帧。
- 回传 TX 结果、错误事件、状态统计。
- 上报小核心跳。

Web 监控边界：

- FreeRTOS 不提供 Web API，也不面向浏览器或 Rust Web 后端开放接口。
- Web 需要展示的 CAN、IPC、错误统计都先通过 RTOS -> Linux 通道回传给大核。
- `linux_app` 接收小核状态后，整理为 `can_status.json`、`ipc_status.json`、`events.jsonl` 等快照文件。
- Web 后端只读取这些快照文件，不向小核下发启停 CAN、清空统计、设置 bitrate 等控制命令。

### 5.2 cmdqu 与共享内存分工

推荐采用：

```text
cmdqu / mailbox：doorbell + command id + ring index hint
共享内存 ring：固定 96 字节 unified_frame_t slot
```

原因：

- `unified_frame_t` 固定 96 字节，直接塞进 cmdqu 可能受命令长度限制。
- 共享内存 ring 更适合连续转发和压力测试。
- cmdqu 只负责唤醒小核，降低 ISR 和 IPC 处理复杂度。

### 5.3 共享 ring 设计

Linux -> RTOS 和 RTOS -> Linux 各使用一个 ring：

```text
linux_to_rtos_ring
rtos_to_linux_ring
```

每个 ring 建议：

| 项目           |  v1 默认值 |
| -------------- | ---------: |
| slot 数量      |         64 |
| slot 大小      |    96 字节 |
| write index    | `uint32_t` |
| read index     | `uint32_t` |
| flags / status | `uint32_t` |

跨核共享结构不直接使用 `common/include/ring_buffer.h` 中的 `ring_buffer_t`，原因是该结构包含本地指针 `uint8_t *buffer`，不适合作为跨核共享内存 ABI。`ring_buffer_t` 可继续用于单核内部字节缓存，但共享内存 ring 应使用固定偏移、固定 slot 的结构。

### 5.4 Cache 与内存屏障

共享内存通信必须显式处理 cache 一致性：

- Linux 写 slot 后，先 flush cache，再更新 `write_index`，最后通过 cmdqu 通知小核。
- 小核收到通知后，先 invalidate 对应 slot，再读取 `unified_frame_t`。
- 小核写回传 slot 后，同样 flush，再通知 Linux。
- 更新索引前后使用内存屏障，避免乱序导致对端读到半帧。

具体 cache API 由 SG2002 SDK / FreeRTOS BSP 提供，文档只约束调用时机。

## 6. 统一帧校验设计

`Frame Validator` 对每个从 Linux 收到的 slot 按以下顺序校验：

1. 长度必须为 `UNIFIED_FRAME_LENGTH`，即 96 字节。
2. `magic == UNIFIED_FRAME_MAGIC`。
3. `version == UNIFIED_FRAME_VERSION`。
4. `frame_type` 必须是已知类型。
5. v1 只处理 `UNIFIED_FRAME_TYPE_CAN_DATA`、`UNIFIED_FRAME_TYPE_HEARTBEAT`、`UNIFIED_FRAME_TYPE_STATUS`。
6. `source_protocol` 必须是 `protocol_type_t` 中已定义值。
7. `vehicle_type` 必须通过 `vehicle_msg_type_is_valid()`。
8. `reserved[0..1]` 建议为 0；非 0 不丢弃，但计入 warning。
9. `can_flags` 不得包含 v1 不支持的 `FD/BRS/RTR`。
10. 标准帧 `can_id <= 0x7FF`。
11. 扩展帧 `can_id <= 0x1FFFFFFF`。
12. `can_dlc` 必须通过 `unified_frame_can_dlc_is_valid()`；v1 实际发送仍要求经典 CAN `can_dlc <= 8`。
13. 计算 CRC-16/CCITT-FALSE，并与 `crc16` 比较。

校验失败时：

- 不进入 CAN TX Queue。
- 增加对应错误计数。
- 需要时向 Linux 回传 `UNIFIED_FRAME_TYPE_ERROR` 或状态事件。

## 7. FreeRTOS 任务设计

### 7.1 任务与优先级

推荐任务如下：

| 任务               | 优先级 | 职责                                   |
| ------------------ | -----: | -------------------------------------- |
| `CAN_TX_Task`      |   最高 | 消费 TX 队列，调用 XL2515 发送 CAN     |
| `Gateway_IPC_Task` |     高 | 处理 cmdqu 通知，读取共享 ring，校验帧 |
| `CAN_RX_Task`      |     高 | 响应 GPIO14/XL2515 中断，读取 CAN RX   |
| `Status_Task`      |     中 | 周期上报统计、心跳和队列水位           |
| `Watchdog_Task`    |     中 | 喂狗、检测任务心跳和总线异常           |

### 7.2 Gateway_IPC_Task

职责：

- 初始化 cmdqu / mailbox 接收。
- 等待 Linux doorbell。
- 批量读取 `linux_to_rtos_ring` 中可用 slot。
- 对每个 `unified_frame_t` 执行校验。
- 根据 `frame_type` 分发：
  - `CAN_DATA`：转为 RTOS 内部 CAN 消息，投递到 `CAN TX Queue`。
  - `HEARTBEAT`：更新 Linux 心跳时间。
  - `STATUS`：作为查询或配置命令处理。
  - `ERROR`：v1 只统计，不作为下发主路径。

队列满时：

- 丢弃最新帧。
- 增加 `drop_tx_queue_full`。
- 回传 queue full 状态。

### 7.3 CAN_TX_Task

职责：

- 等待 `CAN TX Queue`。
- 将内部 CAN 消息转换为 XL2515 发送 buffer。
- 调用 `rtos_can_driver_send()`。
- 根据返回结果更新 `tx_ok`、`tx_fail`、`bus_error`、`spi_error` 等统计。
- 发送失败时按错误类型决定是否重试。

推荐重试策略：

| 错误              | 策略                                |
| ----------------- | ----------------------------------- |
| SPI 短暂忙        | 最多重试 2 次                       |
| TX buffer 满      | 等待一个短周期后重试                |
| CAN error passive | 继续发送并上报                      |
| bus-off           | 停止发送，复位 XL2515，恢复后再发送 |

### 7.4 CAN_RX_Task

职责：

- 初始化 GPIO14 为 XL2515 `INT#` 输入中断。
- 中断中只释放 semaphore，不在 ISR 中读 SPI。
- 任务上下文中读取 XL2515 中断标志。
- 读取 RX buffer，转换为 `unified_frame_t` 回传 Linux。

回传帧建议：

- `frame_type = UNIFIED_FRAME_TYPE_CAN_DATA`
- `source_protocol` v1 可暂填 `PROTOCOL_TYPE_UNKNOWN`；后续建议新增 `PROTOCOL_TYPE_CAN_BUS`
- `vehicle_type` 若无法从 CAN ID 反推，可填业务映射表中的默认类型，或由 Linux 侧二次解释
- `source_id` 表示 CAN 通道号
- `destination_id` 表示 Linux 或上层逻辑目的

### 7.5 Status_Task

职责：

- 每 1000ms 上报一次小核状态。
- 统计 CAN TX/RX、丢帧、CRC 错误、DLC 错误、队列满、SPI 错误、bus-off 次数。
- 上报 TX/RX 队列水位。
- 上报 Linux 心跳是否超时。
- 支持 Linux 主动查询。

状态上报只面向 Linux 大核。Web 页面展示的“小核在线、CAN 总线状态、IPC 超时、队列水位、错误计数”等信息，都由 Linux 大核接收后转换为 `/run/put/status/` 下的只读快照文件。

### 7.6 Watchdog_Task

职责：

- 周期喂硬件看门狗。
- 检查关键任务 heartbeat。
- 检查 XL2515 是否长时间无响应。
- 检查 Linux 心跳超时。

异常处理：

- 单个任务 heartbeat 超时：记录错误，尝试软恢复相关模块。
- XL2515 无响应：复位 CAN 控制器并重新初始化。
- Linux 心跳超时：进入降级状态，保留 CAN RX 和状态上报，暂停接收新的 Linux 下发帧。

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
rtos_can_driver_reset()
```

`rtos_can_forward` 只调用这些接口，不直接读写 SPI。

### 8.2 XL2515 默认配置

| 项目             | v1 默认值              |
| ---------------- | ---------------------- |
| 控制器           | XL2515                 |
| 收发器           | XL1050                 |
| 晶振             | 16MHz                  |
| SPI 控制器       | SPI2                   |
| SPI 模式         | Motorola SPI mode 0    |
| SPI 数据宽度     | 8 bit                  |
| SPI 初始频率     | 1MHz                   |
| SPI 稳定运行频率 | 8MHz                   |
| CAN bitrate      | 500kbps                |
| CAN 模式         | Normal                 |
| 调试模式         | 支持 Loopback 编译开关 |

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

| SG2002 引脚 | SPI2 功能   |
| ----------- | ----------- |
| `SD1_CLK`   | `SPI2_SCK`  |
| `SD1_CMD`   | `SPI2_SDO`  |
| `SD1_D0`    | `SPI2_SDI`  |
| `SD1_D3`    | `SPI2_CS_X` |

IOB 原理图已将这些信号引到 XL2515，因此 FreeRTOS BSP 侧需要确保这些 pinmux 被配置为 SPI2，而不是默认 SD1 / PWR_SPINOR1 功能。

## 9. 状态与错误设计

### 9.1 统计项

小核维护以下统计：

| 统计项                    | 含义                       |
| ------------------------- | -------------------------- |
| `rx_from_linux`           | 从 Linux ring 读取到的帧数 |
| `tx_to_can_ok`            | 成功发送到 CAN 的帧数      |
| `tx_to_can_fail`          | CAN 发送失败次数           |
| `rx_from_can`             | CAN 总线收到的帧数         |
| `tx_to_linux`             | 回传 Linux 的帧数          |
| `drop_crc`                | CRC 错误丢帧               |
| `drop_magic`              | magic/version 错误丢帧     |
| `drop_dlc`                | DLC 非法丢帧               |
| `drop_flag`               | CAN flag 不支持丢帧        |
| `drop_queue_full`         | FreeRTOS 队列满丢帧        |
| `drop_ring_full`          | 回传 ring 满丢帧           |
| `spi_error`               | SPI 读写错误               |
| `can_bus_off`             | CAN bus-off 次数           |
| `can_error_passive`       | CAN error passive 次数     |
| `linux_heartbeat_timeout` | Linux 心跳超时次数         |

### 9.2 错误处理策略

| 场景                     | 处理                                             |
| ------------------------ | ------------------------------------------------ |
| magic/version 错误       | 丢弃，计数，上报协议错误                         |
| CRC 错误                 | 丢弃，计数，上报 CRC 错误                        |
| DLC 越界                 | 丢弃，计数，上报 DLC 错误                        |
| v1 不支持 CAN FD/BRS/RTR | 丢弃，计数，上报 unsupported flag                |
| Linux->RTOS ring 空      | 忽略本次通知                                     |
| CAN TX Queue 满          | 丢弃最新帧，计数，上报队列满                     |
| RTOS->Linux ring 满      | 丢弃回传帧，保留统计，下一次状态上报体现         |
| SPI 超时                 | 重试，仍失败则复位 XL2515                        |
| CAN bus-off              | 停止 TX，复位 XL2515，恢复后重新进入 Normal Mode |
| Linux 心跳超时           | 暂停下发接收，继续 CAN RX 与状态维护             |

这些统计和错误事件的权威来源是小核状态上报；Web 只展示 Linux 大核整理后的快照，不参与错误恢复决策。

## 10. 配置项

`rtos_config.h` 建议包含：

| 配置                              |   默认值 |
| --------------------------------- | -------: |
| `RTOS_GATEWAY_L2R_RING_SLOTS`     |       64 |
| `RTOS_GATEWAY_R2L_RING_SLOTS`     |       64 |
| `RTOS_CAN_TX_QUEUE_LEN`           |       32 |
| `RTOS_CAN_RX_QUEUE_LEN`           |       32 |
| `RTOS_STATUS_PERIOD_MS`           |     1000 |
| `RTOS_LINUX_HEARTBEAT_TIMEOUT_MS` |     3000 |
| `RTOS_CAN_BITRATE`                |   500000 |
| `RTOS_XL2515_OSC_HZ`              | 16000000 |
| `RTOS_SPI_INIT_HZ`                |  1000000 |
| `RTOS_SPI_RUN_HZ`                 |  8000000 |
| `RTOS_CAN_TX_RETRY_MAX`           |        2 |
| `RTOS_CAN_LOOPBACK_ENABLE`        |        0 |

这些默认值用于 v1 调通，后续可根据实测吞吐和稳定性调整。

## 11. 初始化流程

小核入口 `comm_main.c` 调用：

```text
gateway_forward_init()
```

推荐初始化顺序：

1. 初始化状态统计模块。
2. 初始化共享内存 ring 元数据。
3. 初始化 cmdqu / mailbox。
4. 初始化 SPI2、GPIO14 和 XL2515。
5. 创建 `CAN TX Queue` 和 `CAN RX Queue`。
6. 创建 `Gateway_IPC_Task`。
7. 创建 `CAN_TX_Task`。
8. 创建 `CAN_RX_Task`。
9. 创建 `Status_Task`。
10. 创建 `Watchdog_Task`。
11. 向 Linux 回传 `RTOS_READY`。

如果 CAN 初始化失败：

- 小核仍启动 IPC 和 Status 任务。
- 状态中标记 `can_ready = false`。
- Linux 可通过命令触发重新初始化。

## 12. Linux 协作约束

Linux 大核侧需要遵守：

- 写入共享 ring 的必须是完整 96 字节 `unified_frame_t`。
- 写 slot 前填好全部字段，包括 reserved 清零和 CRC。
- `sequence` 单调递增，方便小核检测丢帧和乱序。
- `timestamp_ms` 使用 Linux 打包时刻。
- 普通 CAN 不设置 `FD/BRS/RTR`。
- 标准帧不设置 `EXTENDED_ID`，且 `can_id <= 0x7FF`。
- 扩展帧设置 `EXTENDED_ID`，且 `can_id <= 0x1FFFFFFF`。
- 写共享内存后 flush cache，再通过 `/dev/cvi-rtos-cmdqu` 通知小核。

## 13. 测试方案

### 13.1 公共协议测试

- `sizeof(unified_frame_t) == 96`。
- CRC-16/CCITT-FALSE 标准向量测试。
- magic/version 错误检测。
- CRC 错误检测。
- 标准帧 CAN ID 范围检测。
- 扩展帧 CAN ID 范围检测。
- DLC 与 flag 组合检测。
- 保留字段非 0 warning 检测。

### 13.2 共享 ring 测试

- 空 ring 读取。
- 满 ring 写入。
- 读写索引回绕。
- 连续 64 slot 写满再读空。
- Linux 写入后小核读取的 cache 一致性验证。
- 小核写回后 Linux 读取的 cache 一致性验证。

### 13.3 FreeRTOS 任务测试

- cmdqu 通知一次读取一帧。
- cmdqu 通知一次批量读取多帧。
- 非法帧不进入 CAN TX Queue。
- CAN TX Queue 满时丢弃最新帧。
- Status_Task 周期上报统计。
- Watchdog_Task 检测任务 heartbeat。

### 13.4 CAN 硬件测试

- XL2515 SPI 读写 ID / 寄存器测试。
- XL2515 loopback 模式发送接收测试。
- Normal 模式接 CAN 分析仪发送测试。
- CAN 分析仪发送，小核 RX 并回传 Linux。
- bus-off 场景恢复测试。
- 拔掉 CAN_H/CAN_L 后错误统计测试。

### 13.5 集成测试

完整链路：

```text
RS485 / WiFi / MQTT / Ethernet 输入
    ↓
Linux parser
    ↓
unified_frame_t
    ↓
/dev/cvi-rtos-cmdqu + shared ring
    ↓
FreeRTOS Validator
    ↓
CAN_TX_Task
    ↓
XL2515 + XL1050
    ↓
CAN 分析仪 / 车身节点
```

验收条件：

- 合法帧能稳定发送到 CAN。
- 非法帧不会进入 CAN 总线。
- Linux 能收到小核状态和错误统计。
- CAN RX 能回传 Linux。
- Web 相关状态只通过 Linux 大核快照展示，不直接连接或控制 FreeRTOS 小核。
- 连续运行 24 小时无任务死锁、无异常重启、无无法解释的丢帧。

## 14. 分阶段实现路线

### 阶段 1：文档与 ABI 锁定

- 确认 `unified_frame_t` 96 字节 ABI 不再变更。
- 补充 CRC16 公共实现方案。
- 确认 Linux 与 FreeRTOS 的共享内存地址和 cache API。

### 阶段 2：小核最小链路

- 建立 `freertos/cvitek/task/comm` 目录。
- 实现 `comm_main.c` 和 `gateway_forward_init()`。
- 实现 cmdqu doorbell 接收。
- 实现共享 ring 读取。
- 实现 frame validator。
- 暂用 mock CAN driver 打印或计数。

### 阶段 3：XL2515 驱动

- 实现 SPI2 初始化。
- 实现 XL2515 reset / register read-write。
- 实现 bitrate 配置。
- 实现 loopback send / receive。
- 实现 normal mode 发送。

### 阶段 4：CAN RX 与回传

- 实现 GPIO14 中断。
- 实现 CAN_RX_Task。
- 将 CAN RX 封装为 `unified_frame_t` 写入 RTOS->Linux ring。
- 通过 cmdqu 通知 Linux。

### 阶段 5：稳定性与错误恢复

- 完成状态统计。
- 完成 bus-off 恢复。
- 完成 SPI 超时恢复。
- 完成 Linux heartbeat 超时处理。
- 完成长时间压力测试。

## 15. 后续待确认

- SDK 中 `/dev/cvi-rtos-cmdqu` 单条命令结构和最大 payload。
- Linux 与 RTOS 共享内存的实际物理地址、大小和 cache 属性。
- FreeRTOS BSP 中 SPI2、GPIO14、cache、mailbox API 的具体函数名。
- XL2515 与 MCP2515 的寄存器兼容程度。
- 最终 CAN bitrate 是否固定 500kbps，还是需要 Linux 动态配置。
- CAN RX 回传时是否需要新增 `PROTOCOL_TYPE_CAN_BUS`。

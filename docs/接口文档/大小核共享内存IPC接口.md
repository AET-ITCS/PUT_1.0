# 大小核共享内存 IPC v1 ABI 冻结说明

## 1. 文档定位

本文冻结本项目 v1 阶段的大核 Linux 与小核 RTOS 共享内存 IPC 公共 ABI。

正式小核固件目标目录为：

```text
rtos_firmware/
```

当前仓库中的：

```text
freertos/
```

只作为历史参考实现和行为参考，不作为后续主开发目录。后续新增的小核固件工程、任务、BSP、共享内存适配和 CAN 驱动都应进入 `rtos_firmware/`。

公共 ABI 头文件为：

```text
common/include/shared_memory_ipc.h
```

该头文件已从“占位接口”升级为 v1 ABI 定义，冻结共享内存区域、双向 SPSC ring、slot、heartbeat、status、event 和最小发送接口。

---

## 2. 总体边界

v1 主链路固定为：

```text
外部协议输入
    ↓
linux_app 协议解析
    ↓
frame_packer_pack()
    ↓
unified_frame_t
    ↓
ipc_to_rtos_send()
    ↓
Linux -> RTOS shared memory ring
    ↓
rtos_firmware IPC RX
    ↓
统一帧校验 / CAN message 适配
    ↓
CAN TX Queue
    ↓
XL2515 / XL1050
```

职责边界：

| 模块 | 负责 | 不负责 |
| ---- | ---- | ------ |
| `linux_app` 协议层 | 外部协议解析、生成 `unified_frame_t` | 共享内存内部索引和 cache 操作 |
| `ipc_to_rtos_send()` | 调用共享内存发送能力 | CAN 业务解析 |
| 共享内存 IPC 层 | ring 写入/读取、cache 同步、doorbell 通知、错误返回 | 解释 CAN 业务语义 |
| `rtos_firmware` IPC 层 | 从 ring 读取 payload、校验公共 ABI | 解析 4G/WiFi/蓝牙/RS485/以太网 |
| `rtos_firmware` CAN 层 | 经典 CAN v1 转发、状态统计、fail-safe | 共享内存物理地址分配 |
| Web 模块 | 读取 Linux 生成的 `/run/put/status/` 快照 | 直接访问共享内存或小核寄存器 |

---

## 3. 已冻结公共 ABI

### 3.1 基本常量

| 常量 | v1 值 | 说明 |
| ---- | ----: | ---- |
| `PUT_SHM_IPC_VERSION` | `1` | IPC ABI 版本 |
| `PUT_SHM_PAYLOAD_MAX_LEN` | `128` | 单个 slot payload 最大长度 |
| `PUT_SHM_SLOT_SIZE` | `256` | 单个 slot 固定大小 |
| `PUT_SHM_L2R_DEPTH` | `32` | Linux -> RTOS ring 深度 |
| `PUT_SHM_R2L_DEPTH` | `32` | RTOS -> Linux ring 深度 |
| `PUT_SHM_REGION_SIZE` | `64 KiB` | reserved-memory 总大小 |
| `PUT_SHM_CACHE_LINE_SIZE` | `64` | cache line 对齐粒度 |

共享内存物理地址不在 C 业务层硬编码，由 Linux DTS `reserved-memory` 与 `rtos_firmware` BSP/linker 配置共同提供。

### 3.2 ring 模型

v1 使用双向 SPSC ring：

```text
linux_to_rtos：Linux 单生产者，RTOS 单消费者
rtos_to_linux：RTOS 单生产者，Linux 单消费者
```

索引规则：

| 字段 | 写入方 | 说明 |
| ---- | ------ | ---- |
| `write_seq` | 生产者 | 单调递增写入序号 |
| `read_seq` | 消费者 | 单调递增读取序号 |

状态判断：

```text
empty: write_seq == read_seq
full : write_seq - read_seq >= depth
```

队列满时必须丢弃最新消息并递增 drop 计数，不允许覆盖旧 slot。

### 3.3 slot 载荷

slot header 描述 payload 元数据：

```text
magic / version / header_size / sequence / epoch
message_type / payload_length / payload_crc16 / flags
```

payload CRC 使用 `CRC-16/CCITT-FALSE`，只覆盖实际 payload 字节。`UNIFIED_FRAME` 消息中的 `unified_frame_t` 仍保留自身 CRC，两层 CRC 含义不同：

| CRC | 覆盖范围 | 作用 |
| --- | -------- | ---- |
| slot `payload_crc16` | 共享内存 slot payload | 检查跨核搬运过程是否损坏 |
| `unified_frame_t.crc16` | unified frame 前 94 字节 | 检查业务帧字段是否合法 |

---

## 4. 消息类型

| 类型 | 方向 | payload |
| ---- | ---- | ------- |
| `PUT_SHM_MESSAGE_TYPE_UNIFIED_FRAME` | Linux -> RTOS | `unified_frame_t` |
| `PUT_SHM_MESSAGE_TYPE_HEARTBEAT` | Linux -> RTOS | `put_shm_heartbeat_payload_t` |
| `PUT_SHM_MESSAGE_TYPE_HELLO` | Linux -> RTOS | `put_shm_heartbeat_payload_t` |
| `PUT_SHM_MESSAGE_TYPE_READY` | RTOS -> Linux | `put_shm_heartbeat_payload_t` |
| `PUT_SHM_MESSAGE_TYPE_CAN_RX` | RTOS -> Linux | `put_shm_can_rx_payload_t` |
| `PUT_SHM_MESSAGE_TYPE_STATUS` | RTOS -> Linux | `put_shm_status_payload_t` |
| `PUT_SHM_MESSAGE_TYPE_EVENT` | RTOS -> Linux | `put_shm_event_payload_t` |

Linux -> RTOS 的业务消息 v1 固定使用 96 字节 `unified_frame_t`。`unified_frame_t` 当前保持不变：

```text
UNIFIED_FRAME_LENGTH = 96
UNIFIED_FRAME_VERSION = 0x01
UNIFIED_FRAME_CRC_INPUT_LENGTH = 94
```

RTOS -> Linux 的 CAN RX 回传不复用 `PUT_SHM_MESSAGE_TYPE_UNIFIED_FRAME`，而是固定使用 `PUT_SHM_MESSAGE_TYPE_CAN_RX`。该 payload 只描述小核从 CAN 总线收到的经典 CAN 报文：

| 字段 | 说明 |
| ---- | ---- |
| `sequence` | RTOS 侧递增的 CAN RX 回传序号 |
| `timestamp_ms` | RTOS 接收 CAN 报文的时间戳 |
| `can_id` | CAN 标准 ID 或扩展 ID |
| `can_dlc` | CAN 数据字节数，v1 范围为 `0 ~ 8` |
| `can_flags` | 采用 `unified_can_flag_t` 语义，v1 只允许 `UNIFIED_CAN_FLAG_NONE` 或 `UNIFIED_CAN_FLAG_EXTENDED_ID` |
| `can_data` | 经典 CAN 数据区，长度为 8 字节 |

这样区分后，`PUT_SHM_MESSAGE_TYPE_UNIFIED_FRAME` 只表示 Linux -> RTOS 的业务下发帧，`PUT_SHM_MESSAGE_TYPE_CAN_RX` 只表示 RTOS -> Linux 的 CAN 总线接收回传。

---

## 5. cache / barrier / doorbell 顺序

写入方顺序：

```text
写 slot header 和 payload
    ↓
flush slot cache
    ↓
memory barrier
    ↓
更新 write_seq
    ↓
flush producer cache line
    ↓
发送 doorbell / mailbox / cmdqu 通知
```

读取方顺序：

```text
invalidate producer cache line
    ↓
判断 write_seq != read_seq
    ↓
invalidate slot cache
    ↓
校验 magic/version/length/payload_crc16
    ↓
处理 payload
    ↓
更新 read_seq
    ↓
flush consumer cache line
```

doorbell 只表示“对应 ring 可能有新消息”，不承载业务 payload。通知丢失时，接收端必须可通过周期 drain 兜底；共享内存 ring 是唯一数据源。

---

## 6. heartbeat / rehandshake / fail-safe

Linux 启动或重启时：

1. 递增 `linux_epoch`。
2. 初始化共享内存 region 和两个 ring。
3. 发送 `PUT_SHM_MESSAGE_TYPE_HELLO`。
4. 等待 RTOS 回传 `PUT_SHM_MESSAGE_TYPE_READY`。
5. 握手完成后才发送普通业务 `UNIFIED_FRAME`。

RTOS 发现新的 `linux_epoch` 或收到 `HELLO` 后：

1. 丢弃旧 Linux epoch 下的未发送业务帧。
2. 清空 CAN TX Queue。
3. 重新进入握手流程。
4. READY 前拒绝普通业务帧。

Linux heartbeat 超过 `RTOS_LINUX_HEARTBEAT_TIMEOUT_MS` 后，RTOS 必须进入 fail-safe offline：

```text
禁止 CAN TX
清空 CAN TX Queue
abort XL2515 TX buffer
切换 Listen-Only
保留 CAN RX、STATUS、EVENT 回传能力
```

---

## 7. CAN 层边界

v1 冻结为经典 CAN：

| 项目 | v1 规则 |
| ---- | ------- |
| 标准帧 ID | `0x000 ~ 0x7FF` |
| 扩展帧 ID | `0x00000000 ~ 0x1FFFFFFF` |
| DLC | `0 ~ 8` |
| 支持 flag | 标准帧、扩展帧 |
| 暂不支持 | CAN FD、BRS、RTR |

如果 `unified_frame_t` 中带有 CAN FD、BRS 或 RTR 等 v1 不支持 flag，`rtos_firmware` parser/adapter 必须拒绝该帧并上报统计，不得发送到 CAN 总线。

---

## 8. 错误码

IPC 专用错误码已经加入：

| 错误码 | 含义 |
| ------ | ---- |
| `UNIFIED_ERR_IPC_QUEUE_EMPTY` | ring 为空 |
| `UNIFIED_ERR_IPC_QUEUE_FULL` | ring 已满 |
| `UNIFIED_ERR_IPC_NOT_READY` | IPC 未初始化或未就绪 |
| `UNIFIED_ERR_IPC_NOTIFY_FAILED` | doorbell/mailbox/cmdqu 通知失败 |
| `UNIFIED_ERR_IPC_OFFLINE` | 对端离线或 fail-safe 状态下拒绝业务帧 |

现有错误码数值保持不变，IPC 错误码独立放在 `-30` 区间。

---

## 9. 本阶段不实现的内容

第一步只冻结架构与接口，不实现以下内容：

- 不创建完整 `rtos_firmware/` 工程骨架。
- 不替换 `linux_app/ipc/ipc_to_rtos.c` 的 stdout stub。
- 不实现共享内存 ring enqueue/dequeue。
- 不接入 `/dev/cvi-rtos-cmdqu`、mailbox 或硬件 doorbell。
- 不实现 cache flush / invalidate 平台函数。
- 不修改 `freertos/` 目录下的参考实现。

这些内容进入后续步骤完成。

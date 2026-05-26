# Milk-V Duo256M FreeRTOS 小核共享内存路由设计文档

版本：v0.4.1
日期：2026-05-26
适用平台：Milk-V Duo256M  
文档定位：FreeRTOS 小核设计文档  
设计边界：小核只负责共享内存 v2 Descriptor Ring 到 TX Ring 的路由、优先级排序、状态监控、端到网关心跳维护、Frame Pool 回收标记和 Mailbox 通知。小核不直接处理 CAN、RS485 等物理层收发，也不直接释放 Frame Buffer。

---

## 0. TODO 对齐修订说明

> TODO 对齐：覆盖 `docs/TODO/TODO_list.md` 中与 FreeRTOS 小核相关的 P0/P1/P2 架构隐患。

本次修订将 FreeRTOS 小核设计收敛到目标架构：

```text
完整 anyMSG + Frame Pool + v2 Descriptor Ring + Pending Bitmap + reclaim/free ring
```

重点补齐：

1. Frame Pool 分配、消费、丢弃、TX 出队、Linux 发送完成和最终释放的回收闭环。
2. `shared_memory_region_v2` 的小核使用边界，明确 v1 `unified_frame_t` / 128B slot / CAN direct 仅为历史参考。
3. anyMSG 入口可信性、完整性、重放保护和 descriptor CRC 的职责边界。
4. Frame Pool、RX/TX Ring、本地队列和优先级预留的背压与水位线策略。
5. CID 地址首字节到目标 TX Ring 的固定路由规则。
6. 小核 priority 调度与 Linux 出口真实发送之间的实时性边界和统计点。

---

## 1. 设计背景

系统采用 Linux 大核 + FreeRTOS 小核的异构双核架构。

当前共享内存通信模型如下：

```text
完整 anyMSG 存放在 Frame Pool
每个物理层独立 RX Descriptor Ring / TX Descriptor Ring
Pending Bitmap 表示 Ring 事件状态
reclaim/free ring 表示小核消费或丢弃后可回收的 Frame Buffer
Mailbox Doorbell 只负责跨核唤醒
```

本设计文档只描述 FreeRTOS 小核部分。

> TODO 对齐：P0-2 v2 ABI 依赖说明，明确 `priority`、`ttl`、`epoch`、CRC、鉴权结果来自共享内存 descriptor 元数据，不写入 anyMSG 保留字段。

本次修正的核心点是：

```text
小核不再直接处理 CAN / RS485 / Ethernet / WiFi / Bluetooth / 4G 等物理层驱动。
小核只从共享内存中各物理层 RX Descriptor Ring 取 descriptor，
通过 frame_id / frame_offset / frame_length 定位 Frame Pool 中的完整 anyMSG，
按目的通信地址和 priority 排序后，
写入对应物理层 TX Descriptor Ring，
当目标 TX Ring 从 empty 变为 non-empty 时，
再通过 Mailbox 通知 Linux 大核发送。

同时，小核识别 anyMSG `type = 0x00` 的端到网关心跳帧，
以 `source_cid` 维护端设备在线状态。

小核消费但不进入 TX Ring 的帧必须写入 reclaim/free ring，
由 Linux 完成 Frame Buffer 最终释放。
```

---

## 2. 小核核心定位

FreeRTOS 小核定位为：

```text
共享内存多 Ring 实时路由调度核心
```

它不是物理层收发核心。

它不直接访问 CAN 控制器，不控制 RS485 方向引脚，不处理 SPI/UART 中断，不负责真实总线发送。

> TODO 对齐：P0-2 消除 v1/v2 分叉。小核目标实现只处理 v2 descriptor 引用的完整 anyMSG；v1 `unified_frame_t` 固定 payload 和 CAN direct 路径只作为历史参考。

小核只处理共享内存中的 v2 descriptor 和对应 Frame Pool 中的完整 anyMSG。

---

## 3. 系统职责划分

> TODO 对齐：P0-1、P0-3 明确 Linux 负责外部入口鉴权与 Frame Pool 最终释放，小核只做 descriptor 级路由、丢弃标记和统计。

| 模块          | 职责                                                                 |
| ------------- | -------------------------------------------------------------------- |
| Linux 大核    | 负责 CAN、RS485、Ethernet、WiFi、Bluetooth、4G 等物理层实际收发      |
| Linux 大核    | 负责复杂协议解析、分片重组、完整 anyMSG 校验、入口鉴权和重放保护     |
| Linux 大核    | 接收外部数据后写入 Frame Pool，并写入对应物理层 RX Descriptor Ring   |
| Linux 大核    | 消费 TX Descriptor Ring，真实发送完成或失败后释放 Frame Buffer       |
| Linux 大核    | 消费 reclaim/free ring，根据 frame_id 和 drop reason 回收 Frame Pool |
| FreeRTOS 小核 | 从共享内存 RX Descriptor Ring 读取 descriptor                        |
| FreeRTOS 小核 | 校验 descriptor、epoch、TTL、anyMSG 头部和 Linux 写入的可信性状态    |
| FreeRTOS 小核 | 根据 destination_cid 选择目标 TX Ring                                |
| FreeRTOS 小核 | 根据 descriptor priority 对帧进行排序                                |
| FreeRTOS 小核 | 将 descriptor 写入对应物理层 TX Descriptor Ring                      |
| FreeRTOS 小核 | 设置 TX Pending Bitmap                                               |
| FreeRTOS 小核 | 当 TX Ring 从空变非空时通过 Mailbox Doorbell 通知 Linux              |
| FreeRTOS 小核 | 识别 `type = 0x00` 端到网关心跳并维护端在线状态                      |
| FreeRTOS 小核 | 对消费但不转发的帧写入 reclaim/free ring                             |

一句话总结：

```text
Linux 负责“真实物理收发和协议处理”；
小核负责“共享内存内的实时路由和优先级调度”。
```

---

## 4. 小核负责内容

FreeRTOS 小核负责：

1. 接收 Linux 发来的 Mailbox Doorbell。
2. 读取 RX Pending Bitmap。
3. 判断哪些物理层 RX Ring 有待处理数据。
4. 从对应 RX Descriptor Ring 中取出 descriptor。
5. 检查 descriptor magic / version / length / CRC / state。
6. 通过 descriptor 定位 Frame Pool 中的完整 anyMSG，并检查 anyMSG 头部合法性。
7. 检查 epoch。
8. 检查 TTL。
9. 读取 descriptor priority。
10. 根据目的通信地址查询路由表。
11. 将 descriptor 引用放入本地优先级调度队列。
12. 按优先级选择待转发帧。
13. 写入目标物理层 TX Descriptor Ring。
14. 设置 TX Pending Bitmap。
15. 当 TX Ring 从空变非空时，通过 Mailbox Doorbell 通知 Linux。
16. 对端心跳、无路由、TTL 过期、epoch 不匹配、非法帧、鉴权失败、重放失败等不转发帧写 reclaim/free ring。
17. 维护小核心跳。
18. 检测 Linux 心跳。
19. 识别 anyMSG `type = 0x00` 的端到网关心跳。
20. 以 `source_cid` 维护端设备心跳表和在线状态。
21. 处理 Ring 满、Frame Pool 耗尽、TTL 过期、epoch 不匹配、无路由等异常。
22. 执行 Recovery 同步。
23. 维护路由、丢弃、回收和心跳统计信息。

---

## 5. 小核不负责内容

FreeRTOS 小核不负责：

1. 不直接接收 CAN 数据。
2. 不直接发送 CAN 数据。
3. 不直接接收 RS485 数据。
4. 不直接发送 RS485 数据。
5. 不控制 RS485 DE/RE 方向引脚。
6. 不处理 SPI CAN 控制器。
7. 不处理 UART 中断。
8. 不处理 Ethernet / WiFi / Bluetooth / 4G 协议栈。
9. 不解析 Modbus、CANopen、J1939、UDS 等复杂协议语义。
10. 不负责 Linux 用户态发送进程的业务逻辑。
11. 不通过 Mailbox 传输完整数据帧。
12. 不长期缓存大量业务数据。
13. 不处理 CAN BusOff 的真实控制器恢复。
14. 不处理 RS485 总线阻塞的真实硬件恢复。
15. 不清空共享内存 payload，不删除业务内容。
16. 不将 Frame Buffer 归还 Frame Pool，不执行共享内存内容最终回收。
17. 不在丢弃帧时直接释放 Frame Buffer。
18. 不生成 `type = 0x01` 网关到端心跳响应。
19. 不解析端心跳 payload 的业务语义。
20. 不绕过 reclaim/free ring 私自改写 Linux 的 Frame Pool free list。

这些内容由 Linux 大核或对应物理层驱动进程负责。

> TODO 对齐：P0-1 小核“删除”帧时必须产生可回收描述符，不能只更新本地统计后丢失 Frame Buffer 引用。

关于“删除”的职责边界：

```text
小核可以消费 RX Descriptor Ring Slot；
小核可以移除本地优先级队列中的引用；
小核必须记录 drop reason、统计计数，并写入 reclaim/free ring；
但小核不负责清零 payload、不负责删除业务内容、不负责 Frame Buffer 最终释放。
```

被小核判定为无路由、TTL 过期、epoch 不匹配或降级丢弃的帧，
最终由 Linux 大核消费 reclaim/free ring 后完成内容清理、Frame Buffer 回收和 Frame Pool 归还。

---

## 6. RX Ring / TX Ring 语义

系统中每个物理层都有独立的 RX Ring 和 TX Ring。

示例：

```text
CAN0_RX_RING
CAN0_TX_RING

RS485_0_RX_RING
RS485_0_TX_RING

ETH0_RX_RING
ETH0_TX_RING

WIFI_RX_RING
WIFI_TX_RING

BT_RX_RING
BT_TX_RING

LTE_RX_RING
LTE_TX_RING
```

从小核视角理解：

> TODO 对齐：P0-2 v2 ABI 以 descriptor 搬运 Frame Pool 引用，不在 Ring Slot 中内嵌完整 payload。

| Ring / 区域          | 生产者   | 消费者      | 含义                                           |
| -------------------- | -------- | ----------- | ---------------------------------------------- |
| `frame_pool`         | Linux    | Linux/小核读 | 保存完整 anyMSG 字节，小核只读 header 和元数据 |
| `xxx_RX_RING`        | Linux    | 小核        | Linux 收到外部数据后交给小核路由的 descriptor  |
| `xxx_TX_RING`        | 小核     | Linux       | 小核路由完成后交给 Linux 发送的 descriptor     |
| `rx_pending_bitmap`  | Linux 写 | 小核读/清   | 表示哪些 RX Ring 有新 descriptor               |
| `tx_pending_bitmap`  | 小核写   | Linux 读/清 | 表示哪些 TX Ring 有待发送 descriptor           |
| `reclaim_free_ring`  | 小核写   | Linux 读    | 表示小核已消费或丢弃、可由 Linux 回收的帧      |
| `stats/event area`   | 双方写各自区域 | 双方读 | 统计、错误事件、水位线和回收确认状态           |

注意：

```text
RX Ring 不是小核从物理总线接收；
TX Ring 不是小核向物理总线发送。

RX Ring / TX Ring 只是共享内存中的 descriptor 方向。
```

---

### 6.1 `shared_memory_region_v2` 小核侧 ABI 依赖清单

> TODO 对齐：P0-2 / P1 审查修订。本节只列出 FreeRTOS 小核侧依赖的 v2 ABI 语义；正式 ABI 必须在接口文档和 Linux/FreeRTOS 共用公共头文件中冻结，本文不单独作为结构体 ABI 来源。

小核依赖 `shared_memory_region_v2` 提供以下区域：

```text
region header
frame_pool
rx_descriptor_rings[interface_count]
tx_descriptor_rings[interface_count]
rx_pending_bitmap
tx_pending_bitmap
reclaim_free_ring
stats_event_area
control_area
```

普通 descriptor 小核依赖字段：

| 字段 | 建议大小 | 字节序 / 对齐 | CRC 覆盖 | 小核使用方式 |
| ---- | -------: | ------------- | -------- | ------------ |
| `magic` | 32 bit | little-endian / 4B | 覆盖 | descriptor 类型和初始化校验 |
| `version` | 16 bit | little-endian / 2B | 覆盖 | v2 ABI 版本校验 |
| `state` | 16 bit | little-endian / 2B | 覆盖 | owner/state 校验，防止读半写 descriptor |
| `frame_id` | 32 bit | little-endian / 4B | 覆盖 | Frame Pool 中完整 anyMSG 的唯一引用 |
| `frame_offset` | 32 bit | little-endian / 4B | 覆盖 | 完整 anyMSG 在 Frame Pool 中的偏移 |
| `frame_length` | 32 bit | little-endian / 4B | 覆盖 | 完整 anyMSG 字节数，用于头部长度检查 |
| `source_interface` | 16 bit | little-endian / 2B | 覆盖 | 来源物理接口，用于统计和限流 |
| `target_interface` | 16 bit | little-endian / 2B | 覆盖 | 小核路由后写入 TX Ring 时填写 |
| `source_cid` | 32 bit | raw bytes | 覆盖 | 从 anyMSG 头部提取的源地址缓存 |
| `destination_cid` | 32 bit | raw bytes | 覆盖 | 从 anyMSG 头部提取的目的地址缓存 |
| `type` | 8 bit | raw byte | 覆盖 | 从 anyMSG 头部提取的 payload 类型缓存 |
| `priority` | 8 bit | raw byte | 覆盖 | 内部调度优先级，默认普通优先级 |
| `ttl` | 16 bit | little-endian / 2B | 覆盖 | 内部转发 TTL，防止异常循环 |
| `epoch` | 32 bit | little-endian / 4B | 覆盖 | Linux 启动纪元，用于重启恢复 |
| `route_epoch` | 32 bit | little-endian / 4B | 覆盖 | descriptor 入队时看到的路由表 epoch |
| `auth_state` | 16 bit | little-endian / 2B | 覆盖 | Linux 接入层写入的鉴权 / 完整性 / 重放检查结果 |
| `flags` | 32 bit | little-endian / 4B | 覆盖 | 分片重组完成、是否需要回执、是否可路由等内部标志 |
| `crc16` | 16 bit | little-endian / 2B | 不覆盖自身 | descriptor 元数据校验，不替代 anyMSG 业务完整性校验 |

reclaim descriptor 小核依赖字段：

| 字段 | 建议大小 | 字节序 / 对齐 | CRC 覆盖 | 小核写入规则 |
| ---- | -------: | ------------- | -------- | ------------ |
| `magic` | 32 bit | little-endian / 4B | 覆盖 | reclaim descriptor 类型校验 |
| `version` | 16 bit | little-endian / 2B | 覆盖 | v2 ABI 版本校验 |
| `frame_id` | 32 bit | little-endian / 4B | 覆盖 | 必填，Linux 通过它归还 Frame Pool |
| `source_ring_id` | 16 bit | little-endian / 2B | 覆盖 | 原 RX Ring 或本地队列来源 |
| `source_desc_seq` | 32 bit | little-endian / 4B | 覆盖 | 原 descriptor 序号，便于 Linux 对账 |
| `drop_reason` | 16 bit | little-endian / 2B | 覆盖 | 必填，表示消费或丢弃原因 |
| `epoch` | 32 bit | little-endian / 4B | 覆盖 | 必填，防止 Linux 重启后回收旧 epoch 引用 |
| `rtos_timestamp_ms` | 32 bit | little-endian / 4B | 覆盖 | 小核产生 reclaim 的时间 |
| `flags` | 32 bit | little-endian / 4B | 覆盖 | 标记是否已消费、是否丢弃、是否需要事件上报 |
| `crc16` | 16 bit | little-endian / 2B | 不覆盖自身 | reclaim descriptor 元数据校验 |

descriptor 状态字段语义：

| state | 生产者 | 消费者 | 含义 |
| ----- | ------ | ------ | ---- |
| `FREE` | Linux | Linux | Slot 空闲，小核不得读取 |
| `WRITING` | Linux | 小核只读检测 | Linux 正在写，不能消费 |
| `READY` | Linux | 小核 | descriptor 可被小核校验和消费 |
| `IN_RTOS` | 小核 | 小核 | 小核已接管引用，Linux 不得复用 |
| `TX_READY` | 小核 | Linux | descriptor 已进入 TX Ring |
| `RECLAIM_PENDING` | 小核 | Linux | 小核已请求 Linux 回收 |
| `ERROR_QUARANTINED` | 小核/Linux | Linux | descriptor 早期损坏，不能信任 `frame_id`，由 Linux sweep |

descriptor 信任分级：

| 信任级别 | 判定条件 | 小核允许动作 |
| -------- | -------- | ------------ |
| `UNTRUSTED_DESCRIPTOR` | magic/version/length/state/CRC 任一失败 | 不读取 `frame_id`，不写 reclaim；记录 `INVALID_DESCRIPTOR_NO_RECLAIM` 事件并进入 Recovery |
| `FRAME_REF_TRUSTED` | descriptor 元数据通过，`frame_id` 范围合法，owner/state 合法，epoch 可判定 | 可在丢弃或消费时写 reclaim descriptor |
| `FRAME_CONTENT_TRUSTED` | `FRAME_REF_TRUSTED` 且 anyMSG 头部、长度、CID/type 基础校验通过 | 可进入心跳消费或 Router Scheduler 前置判断 |
| `ROUTABLE_TRUSTED` | `FRAME_CONTENT_TRUSTED` 且可信性状态允许路由 | 可进入 Router Scheduler |

状态机要求：

1. Linux 写 RX descriptor 前，必须先完成 Frame Pool 写入和 cache flush。
2. 小核读取 RX descriptor 前，必须先校验 magic/version/length/state/CRC。
3. 小核转发成功时，只把同一 `frame_id` 写入目标 TX descriptor，不复制完整 payload。
4. 小核只有在 descriptor 达到 `FRAME_REF_TRUSTED` 后，才能对消费或丢弃路径写 reclaim descriptor。
5. `UNTRUSTED_DESCRIPTOR` 不得使用 `frame_id` 回收，必须隔离 descriptor 并通知 Linux 执行 ring/epoch sweep。
6. 小核消费或丢弃时，必须优先写 reclaim descriptor；如果 reclaim ring 满，进入 `DEGRADED_RECLAIM_FULL` 并停止继续消费新的 RX descriptor。
7. Linux 消费 reclaim descriptor 后释放 Frame Buffer，并通过 reclaim ring 的 read/ack 状态体现回收完成。

---

### 6.2 drop reason 与入口可信性

> TODO 对齐：P0-1、P0-3 统一丢弃原因、鉴权失败和重放失败的回收路径，保证非法输入不进入小核调度。

小核可写入的 `drop_reason` 建议固定为：

| drop reason | 触发场景 |
| ----------- | -------- |
| `HEARTBEAT_CONSUMED` | `type = 0x00` 端到网关心跳被小核消费 |
| `NO_ROUTE` | `destination_cid` 无目标 TX Ring |
| `TTL_EXPIRED` | descriptor TTL 已过期 |
| `EPOCH_MISMATCH` | descriptor epoch 与当前 `linux_epoch` 不一致 |
| `INVALID_DESCRIPTOR` | magic/version/length/state/CRC 异常 |
| `INVALID_DESCRIPTOR_NO_RECLAIM` | descriptor 早期损坏，`frame_id` 不可信，不能写 reclaim |
| `INVALID_ANYMSG` | anyMSG 长度、保留字段、CID 或 type 基础校验失败 |
| `AUTH_FAILED` | Linux 标记外部入口鉴权失败 |
| `INTEGRITY_FAILED` | Linux 标记 anyMSG 业务完整性失败 |
| `REPLAY_DROPPED` | Linux 标记重放保护失败 |
| `GATEWAY_CID_NOT_READY` | `gateway_cid` 未配置，心跳不能进入端在线表 |
| `TX_RING_FULL` | 目标 TX Ring 满且超过重试策略 |
| `LOCAL_QUEUE_OVERFLOW` | 小核本地优先级队列满 |
| `RECOVERY_DISCARD` | Recovery 清理旧本地引用 |

可信性边界：

1. `verify_string[16]` 当前在 anyMSG 文档中未定义真实算法，小核不得把它当作有效鉴权依据。
2. Ethernet、Wi-Fi、4G、Bluetooth 等外部入口必须由 Linux 接入层完成 token/MAC 或等价鉴权，并使用 timestamp/sequence/nonce/session_id 或等价机制做重放保护。
3. descriptor 可信状态固定为 `AUTH_OK`、`INTERNAL_TRUSTED`、`AUTH_FAILED`、`INTEGRITY_FAILED`、`REPLAY_DROPPED`。
4. priority 0/1 控制帧只接受 `AUTH_OK` 或明确可信的 `INTERNAL_TRUSTED` 来源。
5. Linux 只有在鉴权、完整性和重放检查通过后，才允许把外部入口 descriptor 标记为 `AUTH_OK`。
6. 小核发现 descriptor 未通过可信性状态时，不进入 Router Scheduler；仅当 descriptor 达到 `FRAME_REF_TRUSTED` 时写 reclaim，否则记录 `INVALID_DESCRIPTOR_NO_RECLAIM`。
7. descriptor CRC 只保护共享内存搬运元数据，不替代链路层 CRC 或 anyMSG 业务完整性校验。

---

## 7. 修正后总体架构

```text
┌────────────────────────────────────────────────────────────┐
│                    Linux 大核                              │
│                                                            │
│  CAN RX / RS485 RX / ETH RX / WIFI RX / BT RX / LTE RX     │
│        │                                                   │
│        ▼                                                   │
│  协议解析 / 分片重组 / anyMSG 校验 / 鉴权                   │
│        │                                                   │
│        ▼                                                   │
│  写 Frame Pool + 对应物理层 RX Descriptor Ring              │
│        │                                                   │
│        ▼                                                   │
│  设置 RX Pending Bitmap                                    │
│        │                                                   │
│        ▼                                                   │
│  RX Ring 从空变非空时 Doorbell                             │
└────────┬───────────────────────────────────────────────────┘
         │
         ▼
┌────────────────────────────────────────────────────────────┐
│                    FreeRTOS 小核                           │
│                                                            │
│  Mailbox ISR                                               │
│        │                                                   │
│        ▼                                                   │
│  IPC Event Task                                            │
│        │                                                   │
│        ├── 读取 RX Pending Bitmap                          │
│        ├── Drain 各物理层 RX Descriptor Ring                │
│        ├── 校验 descriptor CRC / auth / epoch / TTL / header│
│        ├── 读取 descriptor priority                        │
│        └── 查询目的地址路由表                              │
│                                                            │
│  Router Scheduler Task                                     │
│        │                                                   │
│        ├── 按 priority 排序                                │
│        ├── 按目标物理层分类                                │
│        └── 选择目标 TX Ring                                │
│                                                            │
│  TX Ring Writer Task                                       │
│        │                                                   │
│        ├── 写目标 TX Descriptor Ring                       │
│        ├── 设置 TX Pending Bitmap                          │
│        └── TX Ring 从空变非空时 Doorbell 通知 Linux        │
│                                                            │
│  Reclaim Path                                              │
│        │                                                   │
│        └── 心跳消费 / 丢弃帧写 reclaim/free ring            │
└────────┬───────────────────────────────────────────────────┘
         │
         ▼
┌────────────────────────────────────────────────────────────┐
│                    Linux 大核                              │
│                                                            │
│  读取 TX Pending Bitmap                                    │
│        │                                                   │
│        ▼                                                   │
│  唤醒对应发送进程                                          │
│        │                                                   │
│        ▼                                                   │
│  读取对应 TX Descriptor Ring                               │
│        │                                                   │
│        ▼                                                   │
│  读取 Frame Pool 并真实发送                                │
│        │                                                   │
│        ▼                                                   │
│  CAN TX / RS485 TX / ETH TX / WIFI TX / BT TX / LTE TX     │
│        │                                                   │
│        ▼                                                   │
│  发送完成 / 失败后释放 Frame Buffer                        │
│  并消费 reclaim/free ring 回收小核丢弃帧                   │
└────────────────────────────────────────────────────────────┘
```

---

## 8. 小核任务划分

建议 FreeRTOS 小核任务如下：

| 任务                  | 作用                                                                           |
| --------------------- | ------------------------------------------------------------------------------ |
| Mailbox ISR           | 接收 Mailbox 中断，只清中断并唤醒 IPC Event Task                               |
| IPC Event Task        | 读取 RX Pending Bitmap，Drain 各物理层 RX Descriptor Ring，识别端到网关心跳   |
| Router Scheduler Task | 根据 descriptor priority 和目的通信地址进行排序与路由决策                     |
| TX Ring Writer Task   | 将排序后的 descriptor 写入目标 TX Descriptor Ring                              |
| Heartbeat Task        | 更新 RTOS 心跳，检测 Linux 心跳，维护端到网关心跳表                            |
| Recovery Task         | 处理共享内存重建、epoch 更新、Ring 重映射                                      |
| Statistics Task       | 维护路由、丢弃、回收、Ring、Doorbell、端心跳等统计                             |
| Error Monitor Task    | 监控 Ring 满、Frame Pool 耗尽、reclaim 积压、TTL 过期、epoch 错误、端心跳超时 |

不再设置以下任务：

```text
CAN TX Task
CAN RX Task
RS485 TX Task
RS485 RX Task
CAN Driver Task
RS485 Driver Task
```

---

## 9. Mailbox 设计

Mailbox 只作为 Doorbell 使用，不传输完整数据帧。

Mailbox Doorbell 是“从无到有”的边沿唤醒信号，不是数据通道，也不是每帧通知。

核心规则：

```text
生产者写 Ring 前判断队列是否为空；
写入后如果队列从 empty 变为 non-empty，则设置 pending bit 并发送一次 Doorbell；
如果队列原本已经 non-empty，继续写入时不重复发送 Doorbell；
队列持续非空状态由 pending bitmap 和 Ring read_idx / write_idx 表示。
```

### 9.1 Linux 通知小核

Linux 接收到外部数据、完成完整 anyMSG 重组和入口可信性检查，并准备写入某个 RX Descriptor Ring 时：

```text
1. Linux 分配 Frame Pool buffer
2. Linux 写入完整 anyMSG 并完成 cache flush
3. Linux 写入 xxx_RX_RING descriptor
4. Linux 更新 write_idx
5. Linux 设置 rx_pending_bitmap 对应 bit
6. 如果本次写入导致 Ring 从 empty 变为 non-empty，发送 Mailbox Doorbell 给小核
7. 如果写入前 Ring 已经 non-empty，不重复发送 Doorbell
```

小核收到 Doorbell 后：

```text
1. Mailbox ISR 清中断
2. Mailbox ISR 唤醒 IPC Event Task
3. IPC Event Task 读取 rx_pending_bitmap
4. IPC Event Task 扫描所有 pending RX Ring
5. IPC Event Task 批量 Drain RX Descriptor Ring
6. 如果 Ring 仍然 non-empty，保留 pending bit 并继续调度
7. 如果 Ring 已经 empty，二次检查后清除 pending bit
```

注意：

```text
一次 Doorbell 不代表只有一帧。
Doorbell 只表示小核需要检查 Bitmap。
Doorbell 不是队列计数，也不要求每写一帧都触发一次。
真正的数据数量由 Ring 中的 write_idx / read_idx 决定。
```

---

### 9.2 小核通知 Linux

小核完成路由并准备写入目标 TX Descriptor Ring 时：

```text
1. 小核写入前判断 xxx_TX_RING 是否为空
2. 小核写 xxx_TX_RING descriptor
3. 小核更新 write_idx
4. 小核设置 tx_pending_bitmap 对应 bit
5. 如果本次写入导致 Ring 从 empty 变为 non-empty，发送 Mailbox Doorbell 给 Linux
6. 如果写入前 Ring 已经 non-empty，不重复发送 Doorbell
```

Linux 收到 Doorbell 后：

```text
1. Linux 读取 tx_pending_bitmap
2. Linux 判断哪些 TX Ring 有待发送帧
3. Linux 唤醒对应物理层发送进程
4. Linux 从对应 TX Ring 取 descriptor
5. Linux 根据 frame_id 读取 Frame Pool 中完整 anyMSG
6. Linux 调用真实物理层驱动发送
7. Linux 发送完成或失败后释放 Frame Buffer
8. 如果 TX Ring 仍然 non-empty，保留 pending bit
9. 如果 TX Ring 已经 empty，二次检查后清除 pending bit
```

---

## 10. 小核核心处理流程

> TODO 对齐：P0-1、P0-2、P0-3 核心流程以 v2 descriptor 为单位，所有消费但不转发的帧统一进入 reclaim/free ring。

小核主流程如下：

```text
Mailbox ISR
  ↓
唤醒 IPC Event Task
  ↓
读取 rx_pending_bitmap
  ↓
扫描所有 pending RX Ring
  ↓
从 RX Descriptor Ring 取 descriptor
  ↓
检查 descriptor magic / version / length / CRC / state
  ↓
descriptor 达到 FRAME_REF_TRUSTED ?
  ├── 否：记录 INVALID_DESCRIPTOR_NO_RECLAIM
  │       不读取 frame_id
  │       不写 reclaim/free ring
  │       进入 Recovery，由 Linux ring/epoch sweep 兜底清理
  └── 是：继续
  ↓
检查 auth_state / replay_state
  ↓
通过 frame_id 定位 Frame Pool 中完整 anyMSG
  ↓
检查 anyMSG header / msg_length / payload_length / CID / type
  ↓
检查 descriptor epoch
  ↓
检查 descriptor TTL
  ↓
读取 type / source_cid / destination_cid
  ↓
type = 0x00 ?
  ├── 是：更新端到网关心跳表
  │       写 reclaim/free ring，drop_reason = HEARTBEAT_CONSUMED
  │       不进入 Router Scheduler
  └── 否：读取 descriptor priority
          读取目的通信地址
          查路由表
          进入本地优先级队列
          Router Scheduler 按优先级出队
          TX Ring Writer 写目标 TX Descriptor Ring
          设置 tx_pending_bitmap
          如果 TX Ring 从 empty 变为 non-empty，则 Mailbox 通知 Linux
```

检查失败处理：

```text
descriptor 未达到 FRAME_REF_TRUSTED：
  记录 INVALID_DESCRIPTOR_NO_RECLAIM
  隔离 descriptor / 设置 error bitmap
  不使用 frame_id，不写 reclaim/free ring
  进入 Recovery，等待 Linux ring/epoch sweep

descriptor 已达到 FRAME_REF_TRUSTED：
  记录明确 drop_reason
  写 reclaim/free ring
  更新统计和必要 event

不进入 Router Scheduler
不写 TX Ring
```

---

## 11. IPC Event Task 设计

IPC Event Task 是小核处理 Linux 通知的入口任务。

### 11.1 主要职责

1. 被 Mailbox ISR 唤醒。
2. 读取 `rx_pending_bitmap`。
3. 记录本次 pending bitmap 快照。
4. 扫描所有 pending RX Ring。
5. 从对应 RX Descriptor Ring 中取 descriptor。
6. 执行 descriptor magic/version/length/state/CRC 检查。
7. 检查 Linux 写入的鉴权、完整性、重放状态。
8. 根据 `frame_id`、`frame_offset`、`frame_length` 读取 Frame Pool 中完整 anyMSG 头部。
9. 检查 anyMSG 长度、保留字段、CID 和 type 基础合法性。
10. 检查 epoch。
11. 检查 TTL。
12. 提取 `type`、`source_cid`、`destination_cid`。
13. 如果 `type = 0x00`，更新端到网关心跳表并消费该帧。
14. 如果不是 `type = 0x00`，提取 descriptor priority。
15. 提取目的通信地址。
16. 将待路由 descriptor 投递给 Router Scheduler Task。

### 11.2 处理原则

IPC Event Task 不直接写 TX Ring。

它只负责：

```text
RX Descriptor Ring → 端心跳维护 → reclaim/free ring
RX Descriptor Ring → 本地路由调度队列
```

这样可以避免 IPC Event Task 被某个目标 TX Ring 的拥塞阻塞。

端到网关心跳帧处理原则：

```text
anyMSG type = 0x00 表示端到网关心跳；
小核校验 frame header / epoch / TTL 通过后，读取 source_cid 作为端设备标识；
小核更新端心跳表后消费该帧；
该帧不进入 Router Scheduler，不写 TX Ring，不触发 TX Doorbell；
小核写 reclaim/free ring，drop_reason = HEARTBEAT_CONSUMED；
最终回收仍由 Linux 消费 reclaim descriptor 后完成。
```

---

## 12. RX Ring Drain 策略

如果某个 RX Descriptor Ring 中数据过多，小核不能一直处理同一个 Ring，否则其他 Ring 会被饿死。

建议采用分批 Drain 策略：

```text
每次调度周期内：
CAN0_RX_RING     最多 Drain 8 帧
RS485_0_RX_RING  最多 Drain 8 帧
ETH0_RX_RING     最多 Drain 8 帧
WIFI_RX_RING     最多 Drain 8 帧
BT_RX_RING       最多 Drain 8 帧
LTE_RX_RING      最多 Drain 8 帧
```

建议参数化：

```text
RX_RING_DRAIN_BUDGET_PER_ROUND = 8
RX_RING_DRAIN_MAX_TOTAL        = 64
```

处理原则：

1. 防止单个高流量 Ring 独占 CPU。
2. 防止低优先级输入压住高优先级输入。
3. 每轮处理后重新检查 pending bitmap。
4. 如果 Ring 仍然非空，保留 pending 状态并在下一轮继续处理。
5. 如果 Ring 已空，先执行 memory barrier，再二次检查 read_idx / write_idx。
6. 二次检查仍为空时才清除 pending bit，避免和生产者新写入产生竞态。
7. 清除 pending bit 必须使用平台 atomic AND，避免覆盖其他接口同时设置的 pending bit。
8. 清除 pending bit 后必须再次读取 write_idx；如果 Ring 已经重新非空，立刻 atomic OR 置回 pending bit。
9. Drain 过程中只做 descriptor、anyMSG 头部和可信性状态检查，不做复杂业务解析。

---

## 13. Router Scheduler Task 设计

Router Scheduler Task 负责真正的优先级排序和路由选择。

### 13.1 输入

输入来自 IPC Event Task：

```text
descriptor 指针 / Frame Buffer ID
来源 RX Ring ID
type
source_cid
destination_cid
priority
TTL
epoch
frame_len
auth_state
route_epoch_seen
```

注意：

```text
type = 0x00 的端到网关心跳帧由 IPC Event Task / Heartbeat Task 消费；
不会作为 Router Scheduler 输入。
```

### 13.2 输出

输出给 TX Ring Writer Task：

```text
目标 TX Ring ID
descriptor 指针 / Frame Buffer ID
priority
路由结果
```

### 13.3 本地优先级队列

建议维护 4 个优先级队列：

```text
prio_0_queue
prio_1_queue
prio_2_queue
prio_3_queue
```

priority 定义：

| priority | 含义                | 策略                       |
| -------: | ------------------- | -------------------------- |
|        0 | 紧急帧              | 最高优先级，优先写 TX Ring |
|        1 | 高优先级控制帧      | 优先于普通业务             |
|        2 | 普通业务帧          | 默认优先级                 |
|        3 | 低优先级日志/状态帧 | 拥塞时优先丢弃             |

注意：

```text
priority 数值越小，优先级越高。
priority 来自共享内存 descriptor 元数据，不写入 anyMSG 保留字段。
```

本地队列项必须保存：

```text
descriptor 指针 / Frame Buffer ID
priority
source_ring_id
enqueue_time
route_epoch_seen
retry_count
```

`route_epoch_seen` 表示 descriptor 入队时看到的 active route table epoch；路由表切换后，队列中尚未写入 TX Ring 的 descriptor 必须在出队时重新校验该 epoch。

---

## 14. 防饥饿调度策略

如果一直处理 priority 0，priority 1/2/3 可能长期得不到处理。

建议采用“严格优先级 + 配额”的方式。

示例：

```text
priority 0 : 每轮最多处理 16 帧
priority 1 : 每轮最多处理 12 帧
priority 2 : 每轮最多处理 8 帧
priority 3 : 每轮最多处理 4 帧
```

处理逻辑：

```text
1. 优先处理 priority 0
2. priority 0 达到本轮配额后，处理 priority 1
3. priority 1 达到本轮配额后，处理 priority 2
4. priority 2 达到本轮配额后，处理 priority 3
5. 下一轮重新从 priority 0 开始
```

这样既能保证高优先级优先，又能避免普通帧完全饿死。

---

### 14.1 背压、容量和水位线策略

> TODO 对齐：P1-4 补充 Frame Pool、RX/TX Ring、本地队列和优先级预留的容量边界，避免高吞吐入口耗尽共享内存。

容量参数必须在 v2 公共配置中集中定义，小核只读取配置并执行策略：

| 参数 | 含义 |
| ---- | ---- |
| `FRAME_POOL_TOTAL_BYTES` | Frame Pool 总容量 |
| `FRAME_MAX_LENGTH` | 单个完整 anyMSG 最大长度 |
| `FRAME_POOL_INTERFACE_QUOTA[]` | 单接口可占用 Frame Pool 上限 |
| `RX_RING_DEPTH[]` | 每个入口 RX Descriptor Ring 深度 |
| `TX_RING_DEPTH[]` | 每个出口 TX Descriptor Ring 深度 |
| `LOCAL_PRIO_QUEUE_DEPTH[]` | 小核本地 priority 队列深度 |
| `REASSEMBLY_CACHE_LIMIT[]` | Linux 接入层重组缓存上限，小核只读取统计 |
| `PRIO_0_RESERVED_FRAMES` | priority 0 预留 Frame Pool / TX Ring 配额 |
| `PRIO_1_RESERVED_FRAMES` | priority 1 预留 Frame Pool / TX Ring 配额 |

水位线策略：

1. Frame Pool 或 TX Ring 达到 high watermark 后，小核优先丢弃 priority 3，再丢弃 priority 2。
2. priority 0/1 使用预留水位，除非进入全局降级，不被普通业务占满。
3. 单一高吞吐入口达到接口 quota 后，Linux 接入层应限流；小核侧继续按 RX drain budget 防止该入口占用全部调度周期。
4. reclaim/free ring 积压达到 high watermark 时，小核降低 RX drain budget，优先让 Linux 回收 Frame Buffer。
5. reclaim/free ring 满时，小核停止消费新的 RX descriptor，进入 DEGRADED，避免产生无法回收的 Frame Buffer 引用。

丢弃顺序：

```text
非法 descriptor / 非法 anyMSG / 鉴权失败 / 重放失败
  ↓
priority 3 低优先级日志/状态帧
  ↓
priority 2 普通业务帧
  ↓
priority 1 高优先级业务帧
  ↓
priority 0 紧急控制帧，仅全局降级或 epoch/recovery 异常时丢弃
```

统计必须按接口、priority、drop reason 分开记录。

---

## 15. 路由表设计

> TODO 对齐：P1-5 固化 CID 路由规则，小核按 `destination_cid` 首字节选择目标 TX Ring，保留地址和未定义广播地址不转发。

小核通过目的通信地址选择目标 TX Ring。`destination_cid` 为 4B，首字节必须与《统一数据帧设计》的 CID 地址段一致。

示例路由表：

| `destination_cid[0]` | 地址段              | 目标 TX Ring      |
| -------------------- | ------------------- | ----------------- |
| `0x20 ~ 0x3F`        | CAN 设备地址段      | `CAN0_TX_RING`    |
| `0x40 ~ 0x5F`        | 以太网设备地址段    | `ETH0_TX_RING`    |
| `0x60 ~ 0x7F`        | Wi-Fi 设备地址段    | `WIFI_TX_RING`    |
| `0x80 ~ 0x9F`        | 蓝牙设备地址段      | `BT_TX_RING`      |
| `0xA0 ~ 0xBF`        | 4G 蜂窝设备地址段   | `LTE_TX_RING`     |
| `0xC0 ~ 0xDF`        | RS485 设备地址段    | `RS485_0_TX_RING` |
| `0x00 ~ 0x1F`        | 保留地址            | 丢弃              |
| `0xE0 ~ 0xFF`        | 保留地址            | 丢弃              |

路由表可以来自：

1. Linux 初始化共享内存 v2 控制区时写入。
2. 固定编译配置作为 Linux 未配置时的只读兜底。
3. Linux 通过控制区动态更新。
4. Recovery 时重新加载。

动态更新规则：

```text
Linux 写 shadow route table
Linux 更新 route_version / route_epoch
Linux 设置 route_update_pending
小核在调度边界暂停取新 descriptor
小核校验 route_version / CRC
小核原子切换 active route table
小核记录新的 active_route_epoch
小核清 route_update_pending 并继续调度
```

如果 route table CRC 错误或版本回退，小核保持旧路由表，更新 `route_table_error_count`，并通知 Linux。

本地队列一致性规则：

1. IPC Event Task 投递到本地队列时，队列项必须记录 `route_epoch_seen = active_route_epoch`。
2. route table 原子切换后，已入队但尚未写入 TX Ring 的 descriptor 在出队时必须重新比较 `route_epoch_seen`。
3. 如果 `route_epoch_seen != active_route_epoch`，Router Scheduler 必须重新查询路由表，并更新队列项路由结果。
4. 已写入 TX Ring 的 descriptor 不回滚；从切换点之后进入 TX Ring 的 descriptor 使用新路由表。
5. 重新查询后无路由的帧按 `NO_ROUTE` 写 reclaim/free ring。

广播 CID、网关 CID 和保留 CID：

1. 当前 anyMSG 文档未正式定义广播 CID，小核不自行扩展广播语义。
2. 未定义广播 CID 一律按 `NO_ROUTE` 丢弃并写 reclaim/free ring。
3. gateway cid 只用于判断发往本网关的心跳和控制帧，具体值由 Linux v2 控制区配置。
4. 保留地址段不进入 Router Scheduler。

---

## 16. 目的地址无路由处理

当小核无法根据目的通信地址找到目标 TX Ring 时：

```text
1. 标记该帧为 route miss 丢弃
2. 写入 reclaim/free ring，drop_reason = NO_ROUTE
3. route_miss_count++
4. 记录最近一次无路由目的地址
5. 必要时设置 error bitmap
6. 必要时通知 Linux
7. 等待 Linux 消费 reclaim descriptor 并回收 Frame Buffer
```

不建议小核将无路由帧写入默认 TX Ring。

原因：

```text
错误路由比丢弃更危险。
```

---

## 17. TX Ring Writer Task 设计

TX Ring Writer Task 负责将 Router Scheduler 输出的 descriptor 写入目标 TX Descriptor Ring。

### 17.1 主要职责

1. 从 Router Scheduler 获取待发送 descriptor。
2. 检查目标 TX Ring 状态。
3. 如果 TX Ring 有空间，写入 TX descriptor。
4. 更新 write index。
5. 设置 `tx_pending_bitmap`。
6. 如果 TX Ring 从 empty 变为 non-empty，触发 Mailbox Doorbell 通知 Linux。
7. 如果 TX Ring 满，执行拥塞策略。
8. 更新统计信息。

### 17.2 写入原则

```text
先写 descriptor，再更新 write_idx。
先更新 TX Ring，再设置 pending bit。
如果 TX Ring 从 empty 变为 non-empty，先设置 pending bit，再发送 Doorbell。
如果 TX Ring 原本已经 non-empty，只保留 pending bit，不重复发送 Doorbell。
```

推荐顺序：

```text
1. cache invalidate / memory barrier
2. 检查 TX Ring 空间，并记录写入前是否为空
3. 写 TX descriptor 元数据
4. 写 Frame Buffer 引用和目标接口
5. cache flush / memory barrier
6. 更新 write_idx
7. 设置 tx_pending_bitmap
8. memory barrier
9. 如果写入前为空且写入后非空，Mailbox Doorbell
```

---

## 18. TX Ring 满处理策略

当目标 TX Ring 满时，根据 priority 处理：

| priority | 处理策略 |
| -------: | -------- |
|        0 | bounded retry，短暂保留并通知 Linux drain，超过阈值后按 `TX_RING_FULL` 回收 |
|        1 | bounded retry，重试窗口短于 priority 0，超过阈值后按 `TX_RING_FULL` 回收 |
|        2 | 可少量重试，超过阈值后按 `TX_RING_FULL` 回收 |
|        3 | 不等待或极短重试，优先按 `TX_RING_FULL` 回收 |

建议策略：

```text
1. 如果目标 TX Ring 满，TX Ring Writer 不允许长时间阻塞。
2. priority 0/1 可进入 bounded retry 队列，并通过 Doorbell / event 请求 Linux 尽快 drain 目标 TX Ring。
3. retry_count 或 retry_deadline 超限后，当前 descriptor 写 reclaim/free ring，drop_reason = TX_RING_FULL。
4. priority 2/3 使用更小的 retry_count / retry_deadline，拥塞时优先回收。
5. 丢弃本地低优先级等待 descriptor 只能释放本地队列和 Frame Pool 压力，不能释放已经被目标 TX Ring 占用的 slot。
6. 所有丢弃都必须写入 drop_reason = TX_RING_FULL 或 LOCAL_QUEUE_OVERFLOW，并更新统计。
```

---

### 18.1 端到端实时性边界

> TODO 对齐：P2-6 明确小核 priority 调度与 Linux 出口真实发送之间的关系，小核不能单独承诺物理发送完成时间。

小核可保证的实时性边界：

1. 在 descriptor 已进入 RX Ring 后，小核按 priority、drain budget 和防饥饿配额决定进入 TX Ring 的顺序。
2. 小核只负责把 descriptor 写入目标 TX Ring 并 Doorbell Linux。
3. Linux 出口线程或 event loop 负责从 TX Ring 取 descriptor、封包、分片和真实物理发送。
4. TX Ring 之后的物理发送延迟取决于 Linux 出口调度、接口驱动、链路状态和目标协议 MTU。

Linux 出口层必须配合：

1. priority 0/1 对应的 TX Ring drain 线程或 event loop 应使用更高调度优先级或更短轮询间隔。
2. 低 MTU 接口分片发送时，必须定义高优先级帧能否插队；默认策略为分片边界允许 priority 0/1 插队。
3. 接口离线、驱动阻塞或发送失败不能阻塞小核，必须由 Linux 更新 TX 失败统计并释放 Frame Buffer。

延迟统计点：

| 统计点 | 写入方 | 含义 |
| ------ | ------ | ---- |
| `rx_ring_enqueue_time` | Linux | descriptor 进入 RX Ring 的时间 |
| `rtos_rx_dequeue_time` | 小核 | 小核从 RX Descriptor Ring 取 descriptor 的时间 |
| `rtos_tx_enqueue_time` | 小核 | 小核写入 TX Ring 的时间 |
| `linux_tx_dequeue_time` | Linux | Linux 从 TX Ring 取 descriptor 的时间 |
| `linux_send_done_time` | Linux | 真实物理发送完成或失败的时间 |

Web/日志展示端到端最大延迟时，必须同时展示 drop reason 和拥塞水位，否则不能判断瓶颈在小核还是 Linux 出口。

小核内部延迟目标建议按 priority 分层配置：

| priority | 小核内部统计目标 | 说明 |
| -------: | ---------------- | ---- |
| 0 / 1 | `rtos_rx_dequeue_time` 到 `rtos_tx_enqueue_time` 记录 max/p95/p99，目标值由 `RTOS_LATENCY_TARGET_PRIO_0_1_US` 配置 | 紧急和高优先级控制帧 |
| 2 | 记录 max/p95/p99，目标值由 `RTOS_LATENCY_TARGET_PRIO_2_US` 配置 | 普通业务帧 |
| 3 | 至少记录 max/count，拥塞时允许被丢弃 | 低优先级日志/状态帧 |

Linux 出口实时性分类：

| 接口 | 出口实时性要求 |
| ---- | -------------- |
| CAN / RS485 | 应配置最大出口阻塞时间和发送线程优先级，压测报告必须给出最大值 |
| Ethernet | 根据部署场景配置目标值，默认尽力而为 |
| Wi-Fi / Bluetooth / 4G | 默认尽力而为，必须展示出口排队和发送失败统计 |

压测验收场景：

1. 高负载 RX Ring drain 下 priority 0/1 的小核内部 max/p95/p99 延迟。
2. 目标 TX Ring 拥塞下 bounded retry、`TX_RING_FULL` 和 reclaim 行为。
3. 低 MTU 分片发送时 priority 0/1 在分片边界插队的实际延迟。
4. Linux 出口线程阻塞时，小核 TX Ring 写入延迟和 Linux send done 延迟分别统计。

---

## 19. TTL 检查设计

TTL 用于防止过期帧被继续转发。

处理原则：

1. TTL 未过期：允许继续路由。
2. TTL 已过期：写 reclaim/free ring，drop_reason = TTL_EXPIRED，等待 Linux 回收资源。
3. TTL 为 0：表示不启用过期检查。
4. Recovery 后必须重新检查 TTL。
5. 过期控制帧绝不能在恢复后继续写入 TX Ring。

TTL 检查位置：

```text
RX Descriptor Ring 取出后检查一次；
进入 TX Descriptor Ring 前再检查一次。
```

这样可以避免帧在本地队列中等待过久后仍然被发送。

---

## 20. Epoch 检查设计

Epoch 用于防止 Linux 重启或共享内存重建后旧帧被错误转发。

处理原则：

1. descriptor epoch 与当前 linux_epoch 一致：允许继续处理。
2. descriptor epoch 与当前 linux_epoch 不一致：认为是旧数据，丢弃。
3. 丢弃后写入 reclaim/free ring，drop_reason = EPOCH_MISMATCH。
4. 更新 `epoch_drop_count`。
5. Recovery 后必须清理本地旧 epoch 引用。
6. 旧 epoch 对应 Frame Buffer 的最终回收由 Linux 消费 reclaim descriptor 后完成。

Epoch 检查位置：

```text
RX Descriptor Ring 取出后立即检查。
Recovery 后重新检查所有本地队列中的帧。
```

---

## 21. Frame Buffer 生命周期

> TODO 对齐：P0-1 补齐 Frame Pool 回收闭环。小核不释放 Frame Buffer，但必须为每条消费或丢弃路径产生 Linux 可识别的回收依据。

共享内存 v2 采用 Descriptor Ring + Frame Pool 分离设计，小核需要遵守以下生命周期：

```text
Linux 分配 Frame Pool buffer
        ↓
Linux 写入完整 anyMSG
        ↓
Linux 写 RX Descriptor Ring Slot
        ↓
小核读取 RX Descriptor Ring Slot
        ↓
小核获得 Frame Buffer 引用
        ↓
小核路由判断
        ├── 转发成功：小核写目标 TX Descriptor Ring Slot
        │              ↓
        │            Linux 读取 TX descriptor
        │              ↓
        │            Linux 真实发送完成或失败后释放 Frame Buffer
        │
        └── 小核消费 / 丢弃：小核写 reclaim/free ring
                       ↓
                     Linux 读取 reclaim descriptor
                       ↓
                     Linux 释放 Frame Buffer
```

小核一般不复制完整 payload，只转移 Frame Buffer 引用。

如果需要跨 Ring 转发，应明确 Frame Buffer 所有权。

建议：

```text
RX Descriptor Ring Slot 被小核消费后，Slot 可标记为已消费；
Frame Buffer 在 TX descriptor 被 Linux 消费并完成真实发送后释放；
如果小核丢弃帧，小核必须写 reclaim descriptor；
Frame Buffer 最终释放和内容清零由 Linux 完成。
```

必须产生 reclaim descriptor 的路径：

1. `type = 0x00` 心跳被消费。
2. 无路由。
3. TTL 过期。
4. epoch 不匹配。
5. descriptor 或 anyMSG 非法。
6. 鉴权、完整性或重放状态失败。
7. TX Ring 满、局部降级或本地队列溢出导致丢弃。
8. Recovery 清理本地队列旧引用。

Frame Pool 泄漏检测统计至少包含：

| 统计项 | 含义 |
| ------ | ---- |
| `frame_pool_allocated` | Linux 已分配 Frame Buffer 数 |
| `frame_pool_released` | Linux 已释放 Frame Buffer 数 |
| `frame_pool_pending_reclaim` | 小核已写 reclaim 但 Linux 尚未确认回收数 |
| `frame_pool_leaked_suspect` | 超过阈值仍未回收的疑似泄漏数 |
| `reclaim_ring_full_count` | reclaim/free ring 满次数 |

---

## 22. Pending Bitmap 设计原则

### 22.1 RX Pending Bitmap

```text
rx_pending_bitmap
```

生产者：Linux  
消费者：小核

用途：

```text
表示哪些 RX Descriptor Ring 有 Linux 写入的新 descriptor。
```

处理原则：

1. Linux 写 RX Ring 前记录 Ring 是否为空。
2. Linux 写 RX descriptor 并更新 write_idx。
3. Linux 设置对应 pending bit。
4. 如果 Ring 从 empty 变为 non-empty，Linux 发送 Doorbell。
5. 如果 Ring 原本已经 non-empty，Linux 不重复发送 Doorbell。
6. 小核收到 Doorbell 后读取 bitmap。
7. 小核 drain 对应 RX Descriptor Ring。
8. 如果 Ring 未空，保留 pending bit 并继续调度。
9. 如果 Ring 已空，小核执行 memory barrier 后二次检查。
10. 二次检查仍为空时，小核清除对应 pending bit。

### 22.2 TX Pending Bitmap

```text
tx_pending_bitmap
```

生产者：小核
消费者：Linux

用途：

```text
表示哪些 TX Descriptor Ring 有小核写入的待发送 descriptor。
```

处理原则：

1. 小核写 TX Ring 前记录 Ring 是否为空。
2. 小核写 TX descriptor 并更新 write_idx。
3. 小核设置对应 pending bit。
4. 如果 Ring 从 empty 变为 non-empty，小核发送 Doorbell。
5. 如果 Ring 原本已经 non-empty，小核不重复发送 Doorbell。
6. Linux 收到 Doorbell 后读取 bitmap。
7. Linux drain 对应 TX Descriptor Ring。
8. 如果 Ring 未空，继续保留 pending 状态。
9. 如果 Ring 已空，Linux 执行 memory barrier 后二次检查。
10. 二次检查仍为空时，Linux 清除对应 pending bit。

通用原则：

```text
Pending Bitmap 表示 Ring 当前是否存在待处理数据；
Mailbox Doorbell 只负责在队列从 empty 变为 non-empty 时唤醒对端；
清 pending bit 前必须二次检查 Ring 为空，并使用平台 atomic AND；
清 pending bit 后必须再次检查 Ring，如果已经重新非空，消费者必须 atomic OR 置回 pending bit。
```

### 22.3 reclaim/free ring

> TODO 对齐：P0-1 明确小核回收标记和 Linux 最终释放之间的 ack 关系。

```text
reclaim_free_ring
```

生产者：小核
消费者：Linux

用途：

```text
表示小核已消费或丢弃、且未进入 TX Ring 的 Frame Buffer 可以由 Linux 回收。
```

处理原则：

1. 小核写 reclaim descriptor 前记录 reclaim ring 是否为空。
2. 小核写入 `frame_id`、`drop_reason`、`epoch`、来源 ring 和 descriptor 序号。
3. 小核更新 reclaim ring write_idx。
4. 小核更新 `frame_pool_pending_reclaim` 和对应 drop reason 统计。
5. 如果 reclaim ring 从 empty 变为 non-empty，发送 Doorbell 或设置事件位通知 Linux。
6. Linux drain reclaim ring 后释放 Frame Buffer。
7. Linux 更新 reclaim ring read_idx、`frame_pool_released` 和 ack 统计。
8. 小核通过 pending_reclaim 水位判断 Linux 是否及时回收。

reclaim ring 满时，小核进入 `DEGRADED_RECLAIM_FULL`：

1. 暂停 RX drain 和所有会产生新 reclaim 的丢弃动作。
2. 保留本地队列引用，不继续扩大 pending reclaim。
3. 只保留 heartbeat、错误统计和 Doorbell/event 通知能力。
4. 等待 Linux drain reclaim ring 后，进入 `RECLAIM_BLOCKED` 恢复流程。
5. Linux 释放出 reclaim ring 空间后，小核按预算分批补写被冻结的 reclaim descriptor。
6. 补写完成后才允许恢复普通 RX drain。

---

## 23. Heartbeat Task 设计

Heartbeat Task 负责三类心跳状态：

```text
1. 小核自身心跳
2. Linux 大核心跳检测
3. 端到网关心跳维护
```

端到网关心跳来自 anyMSG 定义：

|   type | 定义           | 小核处理               |
| -----: | -------------- | ---------------------- |
| `0x00` | 端到网关心跳   | 小核消费并维护端心跳表 |
| `0x01` | 网关到端心跳   | 本设计不由小核生成     |
| `0x02` | 端到网关健康度 | 本设计暂不解析 payload |
| `0x03` | 网关到端健康度 | 本设计暂不由小核生成   |

主要职责：

1. 周期性更新 `rtos_heartbeat_seq`。
2. 周期性读取 `linux_heartbeat_seq`。
3. 判断 Linux 是否超时。
4. Linux 超时时进入降级。
5. Linux 恢复时触发 Recovery。
6. 将心跳状态写入共享内存状态区。
7. 接收 IPC Event Task 投递的 `type = 0x00` 端到网关心跳事件。
8. 以 `source_cid` 为 key 更新端心跳表。
9. 周期性扫描端心跳表，判断端设备 ONLINE / WARN / OFFLINE。
10. 将端心跳摘要写入共享内存状态区，供 Linux 读取。

### 23.1 Linux 心跳状态判断

|           时间 | 状态           |
| -------------: | -------------- |
|  300 ms 未变化 | Linux 心跳警告 |
|  500 ms 未变化 | Linux 疑似异常 |
| 1000 ms 未变化 | 进入全局降级   |

处理原则：

```text
Linux 心跳异常时，小核停止普通路由；
保留错误统计；
等待 Linux 恢复后进入 Recovery；
不要直接继续旧队列。
```

### 23.2 端到网关心跳表

建议小核维护固定大小端心跳表：

```text
endpoint_heartbeat_table[ENDPOINT_HEARTBEAT_MAX]
```

每个 entry 至少包含：

| 字段                    | 含义                                    |
| ----------------------- | --------------------------------------- |
| `source_cid`            | 端设备通信地址，来自 anyMSG `source_cid` |
| `last_rx_ring_id`       | 最近一次心跳来源 RX Ring                |
| `last_rtos_time_ms`     | 小核收到心跳时的本地时间                |
| `last_frame_local_time` | 心跳帧中的 `local_time` 字段            |
| `state`                 | `ONLINE` / `WARN` / `OFFLINE`           |
| `rx_count`              | 收到该端心跳次数                        |
| `timeout_count`         | 该端心跳超时次数                        |

默认参数建议：

| 参数                            |  建议值 | 含义                             |
| ------------------------------- | ------: | -------------------------------- |
| `ENDPOINT_HEARTBEAT_EXPECT_MS`  | 1000 ms | 端心跳期望周期                   |
| `ENDPOINT_HEARTBEAT_WARN_MS`    | 3000 ms | 超过该时间未更新进入 WARN        |
| `ENDPOINT_HEARTBEAT_OFFLINE_MS` | 5000 ms | 超过该时间未更新进入 OFFLINE     |
| `ENDPOINT_HEARTBEAT_MAX`        |      64 | 小核维护的最大端数量，后续可配置 |

处理原则：

```text
source_cid 是端设备身份；
source_cid 首字节必须落在 anyMSG 定义的设备地址段 0x20 ~ 0xDF；
0x00 ~ 0x1F 和 0xE0 ~ 0xFF 为保留地址，不进入端心跳表；
destination_cid 用于判断是否发往本网关，具体 gateway cid 规则由 Linux 配置；
如果 gateway cid 尚未配置，小核不更新端心跳表；
只有 destination_cid 匹配已配置 gateway cid 或 Linux 明确配置的 gateway alias 时，才维护端在线状态；
payload_length 可以为 0；
小核不解析端心跳 payload。
```

### 23.3 端心跳帧消费流程

```text
IPC Event Task 从 RX Descriptor Ring 取 descriptor
  ↓
检查 descriptor / auth_state / header / epoch / TTL
  ↓
读取 type
  ↓
type = 0x00
  ↓
读取 source_cid / destination_cid / local_time
  ↓
校验 source_cid 是否为有效设备地址
  ↓
校验 gateway_cid 已配置且 destination_cid 匹配 gateway cid / alias
  ↓
更新 endpoint_heartbeat_table
  ↓
endpoint_hb_rx_count++
  ↓
写 reclaim/free ring，drop_reason = HEARTBEAT_CONSUMED
  ↓
不进入 Router Scheduler
  ↓
不写 TX Ring
  ↓
不触发 TX Doorbell
```

如果 `source_cid` 非法，或端心跳表已满：

```text
1. 不更新端心跳表
2. 更新 endpoint_hb_invalid_count 或 endpoint_hb_table_full_count
3. 写入 drop reason / error bitmap
4. 写 reclaim/free ring，等待 Linux 回收 Frame Buffer
```

如果 `gateway_cid` 未配置，或 `destination_cid` 不匹配本网关：

```text
1. 不更新端心跳表
2. gateway_cid 未配置时写 drop_reason = GATEWAY_CID_NOT_READY
3. destination_cid 不匹配时写 drop_reason = NO_ROUTE 或 INVALID_ANYMSG
4. 写 reclaim/free ring，等待 Linux 回收 Frame Buffer
```

小核不主动生成 `type = 0x01` 网关到端心跳。

如果 Linux 后续写入 `type = 0x01` 帧到某个 RX Ring，
小核按普通待路由控制帧处理，不在 Heartbeat Task 中本地合成响应。

---

## 24. Error Monitor Task 设计

Error Monitor Task 只监控共享内存和路由层异常。

监控对象：

1. RX Ring 长时间积压。
2. TX Ring 长时间满。
3. Pending Bit 长时间未清除。
4. Frame Pool 耗尽。
5. TTL 过期大量增加。
6. Epoch mismatch 大量增加。
7. 目的地址无路由。
8. Mailbox 发送失败。
9. Linux heartbeat 超时。
10. 端到网关心跳超时。
11. 非法端心跳 source_cid。
12. 端心跳表满。
13. 共享内存 magic/version 异常。
14. Ring descriptor 异常。
15. Cache 同步异常。
16. reclaim/free ring 积压或满。
17. 鉴权失败、完整性失败、重放丢弃大量增加。
18. Frame Pool pending reclaim 长时间不下降。

不监控：

```text
CAN BusOff
RS485 发送超时
RS485 方向控制异常
SPI 控制器错误
UART 接收溢出
```

这些由 Linux 物理层驱动进程处理。

---

## 25. Recovery Task 设计

Recovery Task 负责系统重新同步。

### 25.1 触发条件

1. Linux heartbeat 从异常恢复。
2. Linux ready 标志重新置位。
3. linux_epoch 发生变化。
4. 共享内存 magic/version 重新初始化。
5. Ring descriptor 发生变化。
6. 路由表更新。
7. Mailbox 长时间异常后恢复。
8. reclaim/free ring 从满状态恢复。

### 25.2 Recovery 动作

```text
1. 暂停 IPC Event Task 的新帧投递
2. 暂停 Router Scheduler
3. 暂停 TX Ring Writer
4. 冻结本地优先级队列引用，不在 reclaim ring 满时继续写入 reclaim/free ring
5. 如果 reclaim ring 有空间，按预算分批写 reclaim/free ring，drop_reason = RECOVERY_DISCARD
6. 如果 reclaim ring 满，进入 RECLAIM_BLOCKED，等待 Linux drain 后继续补写
7. 标记旧 epoch 数据为待丢弃
8. 标记 TTL 过期数据为待丢弃
9. 重新检查共享内存 magic/version
10. 重新读取 linux_epoch
11. 重新建立 RX/TX Ring 映射
12. 重新加载路由表
13. 重新加载 gateway cid / 端心跳配置
14. 清理本地 pending bitmap 快照
15. 清理错误状态
16. 将端心跳表标记为待重新确认
17. 恢复任务运行
18. 回到 NORMAL
```

设计原则：

```text
Recovery 不是继续旧状态；
Recovery 必须重新同步状态；
Recovery 清理的是小核本地引用，不直接释放共享内存 payload / Frame Buffer；
Recovery 丢弃本地引用前必须先确认 reclaim ring 可写；
reclaim ring 满时 Recovery 只能冻结本地引用，不能继续写入 reclaim；
Recovery 后端心跳状态必须重新由 type = 0x00 心跳刷新确认。
```

---

## 26. 小核全局状态机

建议状态机：

```text
BOOT
  ↓
INIT_BOARD
  ↓
INIT_MAILBOX
  ↓
WAIT_SHM_READY
  ↓
INIT_RING_MAP
  ↓
INIT_ROUTER_TABLE
  ↓
NORMAL
  ↓
DEGRADED
  ↓
DEGRADED_RECLAIM_FULL
  ↓
RECLAIM_BLOCKED
  ↓
RECOVERY
  ↓
NORMAL
```

状态说明：

| 状态              | 说明                                 |
| ----------------- | ------------------------------------ |
| BOOT              | 小核启动                             |
| INIT_BOARD        | 初始化 FreeRTOS 基础资源             |
| INIT_MAILBOX      | 初始化 Mailbox 中断和 Doorbell       |
| WAIT_SHM_READY    | 等待 Linux 初始化共享内存            |
| INIT_RING_MAP     | 建立 RX/TX Ring 映射                 |
| INIT_ROUTER_TABLE | 加载目的地址到 TX Ring 的映射        |
| NORMAL            | 正常路由调度                         |
| DEGRADED          | Linux 或共享内存异常时降级           |
| DEGRADED_RECLAIM_FULL | reclaim/free ring 满，暂停产生新 reclaim 的消费 |
| RECLAIM_BLOCKED   | 等待 Linux drain 后分批补写被冻结 reclaim |
| RECOVERY          | 重新同步 epoch、Ring、bitmap、路由表 |
| NORMAL            | 恢复正常运行                         |

---

## 27. 降级策略

### 27.1 全局降级

触发条件：

1. Linux heartbeat 超时。
2. 共享内存 magic/version 异常。
3. Mailbox 长时间不可用。
4. Frame Pool 严重异常。
5. Ring descriptor 表异常。
6. tx_pending_bitmap 长时间无人处理。
7. reclaim/free ring 满且超过恢复阈值。

全局降级行为：

```text
1. 停止普通业务路由
2. 冻结普通和低优先级本地队列引用
3. 保留关键错误统计
4. 标记旧 epoch 数据为待丢弃，等待 reclaim ring 可写后分批回收
5. 标记 TTL 过期数据为待丢弃，等待 reclaim ring 可写后分批回收
6. 停止重复发送无意义 Doorbell
7. 等待 Linux 恢复
8. 进入 Recovery
```

### 27.2 局部降级

触发条件：

1. 某个 RX Ring 长时间堆积。
2. 某个 TX Ring 长时间满。
3. 某个目标通道持续不可写。
4. 某类目的地址持续无路由。
5. 某个 priority 队列持续溢出。

局部降级行为：

```text
1. 限制该来源 RX Ring 的 Drain 数量
2. 限制该目标 TX Ring 的写入速率
3. 优先标记丢弃 priority 3 并写 reclaim/free ring
4. 必要时标记丢弃 priority 2 并写 reclaim/free ring
5. 保留 priority 0 / priority 1
6. 更新统计
7. 必要时通知 Linux
```

---

## 28. 统计信息设计

建议小核维护以下统计：

| 统计项                          | 含义                             |
| ------------------------------- | -------------------------------- |
| `doorbell_rx_count`             | 小核收到 Linux Doorbell 次数     |
| `doorbell_tx_count`             | 小核通知 Linux 次数              |
| `rx_ring_drain_count`           | 从 RX Descriptor Ring 取 descriptor 次数 |
| `tx_ring_write_count`           | 写 TX Descriptor Ring 次数       |
| `route_success_count`           | 路由成功次数                     |
| `route_miss_count`              | 目的地址无路由次数               |
| `prio_0_count`                  | priority 0 处理次数              |
| `prio_1_count`                  | priority 1 处理次数              |
| `prio_2_count`                  | priority 2 处理次数              |
| `prio_3_count`                  | priority 3 处理次数              |
| `ttl_drop_count`                | TTL 过期丢弃次数                 |
| `epoch_drop_count`              | epoch 不匹配丢弃次数             |
| `tx_ring_full_count`            | TX Ring 满次数                   |
| `rx_ring_empty_count`           | RX Ring 空读次数                 |
| `frame_pool_error_count`        | Frame Pool 异常次数              |
| `frame_pool_allocated`          | Linux 已分配 Frame Buffer 数     |
| `frame_pool_released`           | Linux 已释放 Frame Buffer 数     |
| `frame_pool_pending_reclaim`    | 等待 Linux 回收的 Frame Buffer 数 |
| `frame_pool_leaked_suspect`     | 疑似 Frame Pool 泄漏数           |
| `reclaim_write_count`           | 小核写 reclaim descriptor 次数   |
| `reclaim_ring_full_count`       | reclaim/free ring 满次数         |
| `reclaim_blocked_count`         | reclaim 满导致暂停 RX drain 次数 |
| `pending_reclaim_retry_count`   | reclaim 满后补写重试次数         |
| `auth_failed_drop_count`        | 鉴权失败丢弃次数                 |
| `integrity_failed_drop_count`   | 完整性失败丢弃次数               |
| `replay_drop_count`             | 重放保护丢弃次数                 |
| `invalid_descriptor_count`      | descriptor 异常次数              |
| `invalid_descriptor_no_reclaim_count` | descriptor 不可信且未写 reclaim 次数 |
| `invalid_anymsg_count`          | anyMSG 基础校验失败次数          |
| `latency_rtos_max_us`           | 小核 RX 出队到 TX 入队最大延迟   |
| `latency_rtos_p95_us`           | 小核 RX 出队到 TX 入队 p95 延迟  |
| `latency_rtos_p99_us`           | 小核 RX 出队到 TX 入队 p99 延迟  |
| `mailbox_fail_count`            | Mailbox 通知失败次数             |
| `linux_heartbeat_timeout_count` | Linux 心跳超时次数               |
| `endpoint_hb_rx_count`          | 收到合法端到网关心跳次数         |
| `endpoint_hb_invalid_count`     | 非法端心跳帧次数                 |
| `endpoint_hb_timeout_count`     | 端心跳超时次数                   |
| `endpoint_hb_recover_count`     | 端设备从 WARN / OFFLINE 恢复次数 |
| `endpoint_hb_table_full_count`  | 端心跳表满次数                   |
| `recovery_count`                | Recovery 次数                    |

统计原则：

1. 高频统计先在小核本地累加。
2. 周期性同步到共享内存统计区。
3. 统计更新不能阻塞路由主流程。
4. Linux 可以读取统计区用于日志和调试。
5. drop、route、latency、ring full 统计必须至少按 `interface`、`priority`、`drop_reason` 维度拆分。
6. Ethernet、Wi-Fi、Bluetooth、4G 等会话型入口应额外记录 `session_id` 或等价会话维度。
7. 延迟统计至少包含 max + count；priority 0/1 和 priority 2 应保留 p95/p99 或等价滑动窗口摘要。

---

## 29. 典型数据流

### 29.1 CAN 输入，RS485 输出

```text
CAN 总线
  ↓
Linux CAN 接收进程
  ↓
Linux 重组 / 校验 / 鉴权后写完整 anyMSG 到 Frame Pool
  ↓
写 CAN0_RX_RING descriptor
  ↓
设置 rx_pending_bitmap[CAN0_RX]
  ↓
如果 CAN0_RX_RING 从空变非空，则 Mailbox 通知小核
  ↓
小核读取 CAN0_RX_RING descriptor
  ↓
小核读取目的通信地址
  ↓
小核判断目标为 RS485_0
  ↓
小核根据 priority 排序
  ↓
写 RS485_0_TX_RING descriptor
  ↓
设置 tx_pending_bitmap[RS485_0_TX]
  ↓
如果 RS485_0_TX_RING 从空变非空，则 Mailbox 通知 Linux
  ↓
Linux RS485 发送进程读取 RS485_0_TX_RING descriptor
  ↓
Linux 读取 Frame Pool，调用 RS485 驱动发送并释放 Frame Buffer
  ↓
RS485 总线
```

### 29.2 RS485 输入，CAN 输出

```text
RS485 总线
  ↓
Linux RS485 接收进程
  ↓
Linux 重组 / 校验 / 鉴权后写完整 anyMSG 到 Frame Pool
  ↓
写 RS485_0_RX_RING descriptor
  ↓
设置 rx_pending_bitmap[RS485_0_RX]
  ↓
如果 RS485_0_RX_RING 从空变非空，则 Mailbox 通知小核
  ↓
小核读取 RS485_0_RX_RING descriptor
  ↓
小核读取目的通信地址
  ↓
小核判断目标为 CAN0
  ↓
小核根据 priority 排序
  ↓
写 CAN0_TX_RING descriptor
  ↓
设置 tx_pending_bitmap[CAN0_TX]
  ↓
如果 CAN0_TX_RING 从空变非空，则 Mailbox 通知 Linux
  ↓
Linux CAN 发送进程读取 CAN0_TX_RING descriptor
  ↓
Linux 读取 Frame Pool，调用 CAN 驱动发送并释放 Frame Buffer
  ↓
CAN 总线
```

### 29.3 Ethernet 输入，CAN 输出

```text
Ethernet
  ↓
Linux 网络接收进程
  ↓
Linux 协议解析 / 重组 / 鉴权后写完整 anyMSG 到 Frame Pool
  ↓
写 ETH0_RX_RING descriptor
  ↓
设置 rx_pending_bitmap[ETH0_RX]
  ↓
如果 ETH0_RX_RING 从空变非空，则 Mailbox 通知小核
  ↓
小核读取 ETH0_RX_RING descriptor
  ↓
小核根据目的通信地址选择 CAN0_TX_RING
  ↓
按 priority 排序
  ↓
写 CAN0_TX_RING descriptor
  ↓
设置 tx_pending_bitmap[CAN0_TX]
  ↓
如果 CAN0_TX_RING 从空变非空，则 Mailbox 通知 Linux
  ↓
Linux CAN 发送进程读取 Frame Pool，真实发送并释放 Frame Buffer
```

### 29.4 端到网关心跳

anyMSG 定义中：

```text
type = 0x00 端到网关心跳
type = 0x01 网关到端心跳
```

本设计只要求小核维护 `type = 0x00`：

```text
外部端设备
  ↓
Linux 物理层接收进程
  ↓
Linux 校验 / 鉴权后写完整 anyMSG 到 Frame Pool
  ↓
type = 0x00
source_cid = 端设备通信地址
destination_cid = 网关通信地址或当前配置允许的网关地址
  ↓
写对应物理层 RX Ring descriptor
  ↓
设置 rx_pending_bitmap
  ↓
如果 RX Ring 从空变非空，则 Mailbox 通知小核
  ↓
小核读取 RX Ring descriptor
  ↓
检查 descriptor / auth_state / header / epoch / TTL
  ↓
识别 type = 0x00
  ↓
更新 endpoint_heartbeat_table[source_cid]
  ↓
写 reclaim/free ring，drop_reason = HEARTBEAT_CONSUMED
  ↓
不进入 Router Scheduler
  ↓
不写 TX Ring
  ↓
不触发 TX Doorbell
```

说明：

```text
小核不生成 type = 0x01 网关到端心跳；
端心跳 payload 暂不解析；
端心跳状态周期性同步到共享内存状态区，供 Linux 读取。
```

---

## 30. 工程目录建议

小核工程建议目录：

```text
freertos_router_core/
├── app/
│   ├── main.c
│   ├── task_init.c
│   └── system_state.c
│
├── ipc/
│   ├── ipc_event_task.c
│   ├── ipc_bitmap_access.c
│   ├── ipc_ring_scan.c
│   └── ipc_notify.c
│
├── mailbox/
│   ├── mailbox_port.c
│   ├── mailbox_isr.c
│   └── mailbox_event.c
│
├── shm/
│   ├── shm_map.c
│   ├── shm_ring.c
│   ├── shm_slot.c
│   ├── shm_cache.c
│   └── shm_frame_pool.c
│
├── router/
│   ├── router_table.c
│   ├── router_scheduler.c
│   ├── router_dispatch.c
│   └── router_policy.c
│
├── queue/
│   ├── priority_queue.c
│   ├── prio_0_queue.c
│   ├── prio_1_queue.c
│   ├── prio_2_queue.c
│   └── prio_3_queue.c
│
├── monitor/
│   ├── heartbeat_task.c
│   ├── endpoint_heartbeat.c
│   ├── error_monitor_task.c
│   ├── recovery_task.c
│   └── statistics_task.c
│
└── config/
    ├── task_config.h
    ├── priority_config.h
    ├── router_config.h
    ├── ring_config.h
    └── safety_config.h
```

# Milk-V Duo256M FreeRTOS 小核共享内存路由设计文档

版本：v0.4.0  
日期：2026-05-25  
适用平台：Milk-V Duo256M  
文档定位：FreeRTOS 小核设计文档  
设计边界：小核只负责共享内存 RX Ring 到 TX Ring 的路由、优先级排序、状态监控、端到网关心跳维护和 Mailbox 通知。小核不直接处理 CAN、RS485 等物理层收发。

---

## 1. 设计背景

系统采用 Linux 大核 + FreeRTOS 小核的异构双核架构。

当前共享内存通信模型如下：

```text
每个物理层独立 RX Ring / TX Ring
Pending Bitmap 表示 Ring 事件状态
Mailbox Doorbell 只负责跨核唤醒
```

本设计文档只描述 FreeRTOS 小核部分。

本次修正的核心点是：

```text
小核不再直接处理 CAN / RS485 / Ethernet / WiFi / Bluetooth / 4G 等物理层驱动。
小核只从共享内存中各物理层 RX Ring 取统一帧，
按目的通信地址和 priority 排序后，
写入对应物理层 TX Ring，
当目标 TX Ring 从 empty 变为 non-empty 时，
再通过 Mailbox 通知 Linux 大核发送。

同时，小核识别统一数据帧 `type = 0x00` 的端到网关心跳帧，
以 `source_cid` 维护端设备在线状态。
```

---

## 2. 小核核心定位

FreeRTOS 小核定位为：

```text
共享内存多 Ring 实时路由调度核心
```

它不是物理层收发核心。

它不直接访问 CAN 控制器，不控制 RS485 方向引脚，不处理 SPI/UART 中断，不负责真实总线发送。

小核只处理共享内存中的统一帧。

---

## 3. 系统职责划分

| 模块 | 职责 |
|---|---|
| Linux 大核 | 负责 CAN、RS485、Ethernet、WiFi、Bluetooth、4G 等物理层实际收发 |
| Linux 大核 | 负责复杂协议解析、封装和解包 |
| Linux 大核 | 接收外部数据后封装为统一帧，并写入对应物理层 RX Ring |
| FreeRTOS 小核 | 从共享内存 RX Ring 读取统一帧 |
| FreeRTOS 小核 | 根据目的通信地址选择目标 TX Ring |
| FreeRTOS 小核 | 根据 priority 对帧进行排序 |
| FreeRTOS 小核 | 将帧写入对应物理层 TX Ring |
| FreeRTOS 小核 | 设置 TX Pending Bitmap |
| FreeRTOS 小核 | 当 TX Ring 从空变非空时通过 Mailbox Doorbell 通知 Linux |
| FreeRTOS 小核 | 识别 `type = 0x00` 端到网关心跳并维护端在线状态 |
| Linux 大核 | 收到通知后读取 TX Ring，并调用真实物理层驱动发送 |

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
4. 从对应 RX Ring 中取出统一帧。
5. 检查统一帧头部合法性。
6. 检查 epoch。
7. 检查 TTL。
8. 从统一帧保留字段第 1 字节读取 priority。
9. 根据目的通信地址查询路由表。
10. 将帧放入本地优先级调度队列。
11. 按优先级选择待转发帧。
12. 写入目标物理层 TX Ring。
13. 设置 TX Pending Bitmap。
14. 当 TX Ring 从空变非空时，通过 Mailbox Doorbell 通知 Linux。
15. 维护小核心跳。
16. 检测 Linux 心跳。
17. 识别统一数据帧 `type = 0x00` 的端到网关心跳。
18. 以 `source_cid` 维护端设备心跳表和在线状态。
19. 处理 Ring 满、Frame Pool 耗尽、TTL 过期、epoch 不匹配、无路由等异常。
20. 执行 Recovery 同步。
21. 维护路由和心跳统计信息。

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

这些内容由 Linux 大核或对应物理层驱动进程负责。

关于“删除”的职责边界：

```text
小核可以消费 RX Ring Slot；
小核可以移除本地优先级队列中的引用；
小核可以记录 drop reason、统计计数和必要的可回收标记；
但小核不负责清零 payload、不负责删除业务内容、不负责 Frame Buffer 最终释放。
```

被小核判定为无路由、TTL 过期、epoch 不匹配或降级丢弃的帧，
最终由 Linux 大核或共享内存管理侧完成内容清理、Frame Buffer 回收和 Frame Pool 归还。

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

| Ring | 生产者 | 消费者 | 含义 |
|---|---|---|---|
| `xxx_RX_RING` | Linux | 小核 | Linux 收到外部数据后交给小核路由 |
| `xxx_TX_RING` | 小核 | Linux | 小核路由完成后交给 Linux 发送 |
| `rx_pending_bitmap` | Linux 写 | 小核读/清 | 表示哪些 RX Ring 有新数据 |
| `tx_pending_bitmap` | 小核写 | Linux 读/清 | 表示哪些 TX Ring 有待发送数据 |

注意：

```text
RX Ring 不是小核从物理总线接收；
TX Ring 不是小核向物理总线发送。

RX Ring / TX Ring 只是共享内存中的数据方向。
```

---

## 7. 修正后总体架构

```text
┌────────────────────────────────────────────────────────────┐
│                    Linux 大核                              │
│                                                            │
│  CAN RX / RS485 RX / ETH RX / WIFI RX / BT RX / LTE RX      │
│        │                                                   │
│        ▼                                                   │
│  协议解析 / 统一帧封装                                      │
│        │                                                   │
│        ▼                                                   │
│  写对应物理层 RX Ring                                       │
│        │                                                   │
│        ▼                                                   │
│  设置 RX Pending Bitmap                                     │
│        │                                                   │
│        ▼                                                   │
│  RX Ring 从空变非空时 Doorbell                              │
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
│        ├── Drain 各物理层 RX Ring                           │
│        ├── 校验 epoch / TTL / header                        │
│        ├── 读取 priority                                   │
│        └── 查询目的地址路由表                               │
│                                                            │
│  Router Scheduler Task                                     │
│        │                                                   │
│        ├── 按 priority 排序                                 │
│        ├── 按目标物理层分类                                 │
│        └── 选择目标 TX Ring                                 │
│                                                            │
│  TX Ring Writer Task                                       │
│        │                                                   │
│        ├── 写目标 TX Ring                                   │
│        ├── 设置 TX Pending Bitmap                           │
│        └── TX Ring 从空变非空时 Doorbell 通知 Linux          │
└────────┬───────────────────────────────────────────────────┘
         │
         ▼
┌────────────────────────────────────────────────────────────┐
│                    Linux 大核                              │
│                                                            │
│  读取 TX Pending Bitmap                                     │
│        │                                                   │
│        ▼                                                   │
│  唤醒对应发送进程                                           │
│        │                                                   │
│        ▼                                                   │
│  读取对应 TX Ring                                           │
│        │                                                   │
│        ▼                                                   │
│  CAN TX / RS485 TX / ETH TX / WIFI TX / BT TX / LTE TX      │
└────────────────────────────────────────────────────────────┘
```

---

## 8. 小核任务划分

建议 FreeRTOS 小核任务如下：

| 任务 | 作用 |
|---|---|
| Mailbox ISR | 接收 Mailbox 中断，只清中断并唤醒 IPC Event Task |
| IPC Event Task | 读取 RX Pending Bitmap，Drain 各物理层 RX Ring，识别端到网关心跳 |
| Router Scheduler Task | 根据 priority 和目的通信地址进行排序与路由决策 |
| TX Ring Writer Task | 将排序后的统一帧写入目标 TX Ring |
| Heartbeat Task | 更新 RTOS 心跳，检测 Linux 心跳，维护端到网关心跳表 |
| Recovery Task | 处理共享内存重建、epoch 更新、Ring 重映射 |
| Statistics Task | 维护路由、丢弃、Ring、Doorbell、端心跳等统计 |
| Error Monitor Task | 监控 Ring 满、Frame Pool 耗尽、TTL 过期、epoch 错误、端心跳超时等异常 |

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

Linux 接收到外部数据并准备写入某个 RX Ring 时：

```text
1. Linux 写入前判断 xxx_RX_RING 是否为空
2. Linux 写 xxx_RX_RING
3. Linux 更新 write_idx
4. Linux 设置 rx_pending_bitmap 对应 bit
5. 如果本次写入导致 Ring 从 empty 变为 non-empty，发送 Mailbox Doorbell 给小核
6. 如果写入前 Ring 已经 non-empty，不重复发送 Doorbell
```

小核收到 Doorbell 后：

```text
1. Mailbox ISR 清中断
2. Mailbox ISR 唤醒 IPC Event Task
3. IPC Event Task 读取 rx_pending_bitmap
4. IPC Event Task 扫描所有 pending RX Ring
5. IPC Event Task 批量 Drain Ring
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

小核完成路由并准备写入目标 TX Ring 时：

```text
1. 小核写入前判断 xxx_TX_RING 是否为空
2. 小核写 xxx_TX_RING
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
4. Linux 从对应 TX Ring 取帧
5. Linux 调用真实物理层驱动发送
6. 如果 TX Ring 仍然 non-empty，保留 pending bit
7. 如果 TX Ring 已经 empty，二次检查后清除 pending bit
```

---

## 10. 小核核心处理流程

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
从 RX Ring 取统一帧
  ↓
检查 frame header
  ↓
检查 epoch
  ↓
检查 TTL
  ↓
读取 type / source_cid / destination_cid
  ↓
如果 type = 0x00，更新端到网关心跳表并消费该帧
  ↓
如果不是 type = 0x00，继续普通路由
  ↓
读取 priority
  ↓
读取目的通信地址
  ↓
查路由表
  ↓
进入本地优先级队列
  ↓
Router Scheduler 按优先级出队
  ↓
TX Ring Writer 写目标 TX Ring
  ↓
设置 tx_pending_bitmap
  ↓
如果 TX Ring 从 empty 变为 non-empty，则 Mailbox 通知 Linux
```

---

## 11. IPC Event Task 设计

IPC Event Task 是小核处理 Linux 通知的入口任务。

### 11.1 主要职责

1. 被 Mailbox ISR 唤醒。
2. 读取 `rx_pending_bitmap`。
3. 记录本次 pending bitmap 快照。
4. 扫描所有 pending RX Ring。
5. 从对应 RX Ring 中取统一帧。
6. 执行基础合法性检查。
7. 检查 epoch。
8. 检查 TTL。
9. 提取 `type`、`source_cid`、`destination_cid`。
10. 如果 `type = 0x00`，更新端到网关心跳表并消费该帧。
11. 如果不是 `type = 0x00`，提取 priority。
12. 提取目的通信地址。
13. 将待路由帧投递给 Router Scheduler Task。

### 11.2 处理原则

IPC Event Task 不直接写 TX Ring。

它只负责：

```text
RX Ring → 端心跳维护
RX Ring → 本地路由调度队列
```

这样可以避免 IPC Event Task 被某个目标 TX Ring 的拥塞阻塞。

端到网关心跳帧处理原则：

```text
统一数据帧 type = 0x00 表示端到网关心跳；
小核校验 frame header / epoch / TTL 通过后，读取 source_cid 作为端设备标识；
小核更新端心跳表后消费该帧；
该帧不进入 Router Scheduler，不写 TX Ring，不触发 TX Doorbell；
小核只标记 RX Slot 已消费和 Frame Buffer 可回收，最终回收仍由 Linux 或共享内存管理侧完成。
```

---

## 12. RX Ring Drain 策略

如果某个 RX Ring 中数据过多，小核不能一直处理同一个 Ring，否则其他 Ring 会被饿死。

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
7. Drain 过程中只做轻量检查，不做复杂业务解析。

---

## 13. Router Scheduler Task 设计

Router Scheduler Task 负责真正的优先级排序和路由选择。

### 13.1 输入

输入来自 IPC Event Task：

```text
统一帧指针 / Frame Buffer ID
来源 RX Ring ID
type
source_cid
目的通信地址
priority
TTL
epoch
frame_len
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
统一帧指针 / Frame Buffer ID
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

| priority | 含义 | 策略 |
|---:|---|---|
| 0 | 紧急帧 | 最高优先级，优先写 TX Ring |
| 1 | 高优先级控制帧 | 优先于普通业务 |
| 2 | 普通业务帧 | 默认优先级 |
| 3 | 低优先级日志/状态帧 | 拥塞时优先丢弃 |

注意：

```text
priority 数值越小，优先级越高。
```

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

## 15. 路由表设计

小核通过目的通信地址选择目标 TX Ring。

示例路由表：

| 目的通信地址范围 | 目标 TX Ring |
|---|---|
| `0x1000 ~ 0x10FF` | `CAN0_TX_RING` |
| `0x2000 ~ 0x20FF` | `RS485_0_TX_RING` |
| `0x3000 ~ 0x30FF` | `ETH0_TX_RING` |
| `0x4000 ~ 0x40FF` | `WIFI_TX_RING` |
| `0x5000 ~ 0x50FF` | `BT_TX_RING` |
| `0x6000 ~ 0x60FF` | `LTE_TX_RING` |

路由表可以来自：

1. 固定编译配置。
2. Linux 初始化共享内存时写入。
3. Linux 通过控制区动态更新。
4. Recovery 时重新加载。

建议初期采用固定配置，后期再支持动态更新。

---

## 16. 目的地址无路由处理

当小核无法根据目的通信地址找到目标 TX Ring 时：

```text
1. 标记该帧为 route miss 丢弃
2. 写入 drop reason / 可回收标记
3. route_miss_count++
4. 记录最近一次无路由目的地址
5. 必要时设置 error bitmap
6. 必要时通知 Linux
7. 等待 Linux 或共享内存管理侧回收 Frame Buffer
```

不建议小核将无路由帧写入默认 TX Ring。

原因：

```text
错误路由比丢弃更危险。
```

---

## 17. TX Ring Writer Task 设计

TX Ring Writer Task 负责将 Router Scheduler 输出的帧写入目标 TX Ring。

### 17.1 主要职责

1. 从 Router Scheduler 获取待发送帧。
2. 检查目标 TX Ring 状态。
3. 如果 TX Ring 有空间，写入 Ring Slot。
4. 更新 write index。
5. 设置 `tx_pending_bitmap`。
6. 如果 TX Ring 从 empty 变为 non-empty，触发 Mailbox Doorbell 通知 Linux。
7. 如果 TX Ring 满，执行拥塞策略。
8. 更新统计信息。

### 17.2 写入原则

```text
先写数据，再更新 write_idx。
先更新 TX Ring，再设置 pending bit。
如果 TX Ring 从 empty 变为 non-empty，先设置 pending bit，再发送 Doorbell。
如果 TX Ring 原本已经 non-empty，只保留 pending bit，不重复发送 Doorbell。
```

推荐顺序：

```text
1. cache invalidate / memory barrier
2. 检查 TX Ring 空间，并记录写入前是否为空
3. 写 Slot 元数据
4. 写 Frame Buffer 引用
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
|---:|---|
| 0 | 尽量保留，可尝试抢占低优先级缓存 |
| 1 | 保留，短暂等待或重试 |
| 2 | 可重试，超过阈值后丢弃 |
| 3 | 直接丢弃或优先丢弃 |

建议策略：

```text
1. 如果目标 TX Ring 满，先检查本地是否有低优先级等待帧。
2. 如果当前帧优先级更高，可以移除低优先级等待帧的本地队列引用，并标记丢弃。
3. 如果当前帧也是低优先级，直接标记丢弃。
4. 不允许 TX Ring Writer 长时间阻塞。
5. 所有丢弃都必须更新统计。
```

---

## 19. TTL 检查设计

TTL 用于防止过期帧被继续转发。

处理原则：

1. TTL 未过期：允许继续路由。
2. TTL 已过期：标记丢弃，记录 drop reason，等待 Linux 回收资源。
3. TTL 为 0：表示不启用过期检查。
4. Recovery 后必须重新检查 TTL。
5. 过期控制帧绝不能在恢复后继续写入 TX Ring。

TTL 检查位置：

```text
RX Ring 取出后检查一次；
进入 TX Ring 前再检查一次。
```

这样可以避免帧在本地队列中等待过久后仍然被发送。

---

## 20. Epoch 检查设计

Epoch 用于防止 Linux 重启或共享内存重建后旧帧被错误转发。

处理原则：

1. Slot epoch 与当前 linux_epoch 一致：允许继续处理。
2. Slot epoch 与当前 linux_epoch 不一致：认为是旧数据，丢弃。
3. 丢弃后写入 drop reason / 可回收标记。
4. 更新 `epoch_drop_count`。
5. Recovery 后必须清理本地旧 epoch 引用。
6. 旧 epoch 对应 Frame Buffer 的最终回收由 Linux 或共享内存管理侧完成。

Epoch 检查位置：

```text
RX Ring 取出后立即检查。
Recovery 后重新检查所有本地队列中的帧。
```

---

## 21. Frame Buffer 生命周期

如果共享内存采用 Slot + Frame Buffer 分离设计，则小核需要遵守以下生命周期：

```text
Linux 写 RX Ring Slot
        ↓
小核读取 RX Ring Slot
        ↓
小核获得 Frame Buffer 引用
        ↓
小核路由判断
        ↓
小核写目标 TX Ring Slot
        ↓
Linux 读取 TX Ring Slot
        ↓
Linux 发送完成后释放 Frame Buffer
```

小核一般不复制完整 payload，只转移 Frame Buffer 引用。

如果需要跨 Ring 转发，应明确 Frame Buffer 所有权。

建议：

```text
RX Ring Slot 被小核消费后，Slot 可标记为已消费或可回收；
Frame Buffer 在 TX Ring 被 Linux 消费后释放；
如果小核丢弃帧，小核只写 drop reason / 可回收标记；
Frame Buffer 最终释放和内容清零由 Linux 或共享内存管理侧完成。
```

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
表示哪些 RX Ring 有 Linux 写入的新数据。
```

处理原则：

1. Linux 写 RX Ring 前记录 Ring 是否为空。
2. Linux 写 RX Ring 并更新 write_idx。
3. Linux 设置对应 pending bit。
4. 如果 Ring 从 empty 变为 non-empty，Linux 发送 Doorbell。
5. 如果 Ring 原本已经 non-empty，Linux 不重复发送 Doorbell。
6. 小核收到 Doorbell 后读取 bitmap。
7. 小核 drain 对应 RX Ring。
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
表示哪些 TX Ring 有小核写入的待发送数据。
```

处理原则：

1. 小核写 TX Ring 前记录 Ring 是否为空。
2. 小核写 TX Ring 并更新 write_idx。
3. 小核设置对应 pending bit。
4. 如果 Ring 从 empty 变为 non-empty，小核发送 Doorbell。
5. 如果 Ring 原本已经 non-empty，小核不重复发送 Doorbell。
6. Linux 收到 Doorbell 后读取 bitmap。
7. Linux drain 对应 TX Ring。
8. 如果 Ring 未空，继续保留 pending 状态。
9. 如果 Ring 已空，Linux 执行 memory barrier 后二次检查。
10. 二次检查仍为空时，Linux 清除对应 pending bit。

通用原则：

```text
Pending Bitmap 表示 Ring 当前是否存在待处理数据；
Mailbox Doorbell 只负责在队列从 empty 变为 non-empty 时唤醒对端；
清 pending bit 前必须二次检查 Ring 为空，避免漏掉并发写入。
```

---

## 23. Heartbeat Task 设计

Heartbeat Task 负责三类心跳状态：

```text
1. 小核自身心跳
2. Linux 大核心跳检测
3. 端到网关心跳维护
```

端到网关心跳来自统一数据帧定义：

| type | 定义 | 小核处理 |
|---:|---|---|
| `0x00` | 端到网关心跳 | 小核消费并维护端心跳表 |
| `0x01` | 网关到端心跳 | 本设计不由小核生成 |
| `0x02` | 端到网关健康度 | 本设计暂不解析 payload |
| `0x03` | 网关到端健康度 | 本设计暂不由小核生成 |

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

| 时间 | 状态 |
|---:|---|
| 300 ms 未变化 | Linux 心跳警告 |
| 500 ms 未变化 | Linux 疑似异常 |
| 1000 ms 未变化 | 进入全局降级 |

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

| 字段 | 含义 |
|---|---|
| `source_cid` | 端设备通信地址，来自统一帧 `source_cid` |
| `last_rx_ring_id` | 最近一次心跳来源 RX Ring |
| `last_rtos_time_ms` | 小核收到心跳时的本地时间 |
| `last_frame_local_time` | 心跳帧中的 `local_time` 字段 |
| `state` | `ONLINE` / `WARN` / `OFFLINE` |
| `rx_count` | 收到该端心跳次数 |
| `timeout_count` | 该端心跳超时次数 |

默认参数建议：

| 参数 | 建议值 | 含义 |
|---|---:|---|
| `ENDPOINT_HEARTBEAT_EXPECT_MS` | 1000 ms | 端心跳期望周期 |
| `ENDPOINT_HEARTBEAT_WARN_MS` | 3000 ms | 超过该时间未更新进入 WARN |
| `ENDPOINT_HEARTBEAT_OFFLINE_MS` | 5000 ms | 超过该时间未更新进入 OFFLINE |
| `ENDPOINT_HEARTBEAT_MAX` | 64 | 小核维护的最大端数量，后续可配置 |

处理原则：

```text
source_cid 是端设备身份；
source_cid 首字节必须落在统一帧定义的设备地址段 0x20 ~ 0xDF；
0x00 ~ 0x1F 和 0xE0 ~ 0xFF 为保留地址，不进入端心跳表；
destination_cid 用于判断是否发往本网关，具体 gateway cid 规则由 Linux 配置；
如果 gateway cid 尚未配置，小核先按 type = 0x00 + source_cid 维护心跳状态；
payload_length 可以为 0；
小核不解析端心跳 payload。
```

### 23.3 端心跳帧消费流程

```text
IPC Event Task 从 RX Ring 取帧
  ↓
检查 header / epoch / TTL
  ↓
读取 type
  ↓
type = 0x00
  ↓
读取 source_cid / destination_cid / local_time
  ↓
校验 source_cid 是否为有效设备地址
  ↓
更新 endpoint_heartbeat_table
  ↓
endpoint_hb_rx_count++
  ↓
标记 RX Slot 已消费、Frame Buffer 可回收
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
4. 标记该帧可由 Linux 或共享内存管理侧回收
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

### 25.2 Recovery 动作

```text
1. 暂停 IPC Event Task 的新帧投递
2. 暂停 Router Scheduler
3. 暂停 TX Ring Writer
4. 清空本地优先级队列引用
5. 标记旧 epoch 数据为丢弃
6. 标记 TTL 过期数据为丢弃
7. 重新检查共享内存 magic/version
8. 重新读取 linux_epoch
9. 重新建立 RX/TX Ring 映射
10. 重新加载路由表
11. 重新加载 gateway cid / 端心跳配置
12. 清理本地 pending bitmap 快照
13. 清理错误状态
14. 将端心跳表标记为待重新确认
15. 恢复任务运行
16. 回到 NORMAL
```

设计原则：

```text
Recovery 不是继续旧状态；
Recovery 必须重新同步状态；
Recovery 清空的是小核本地引用，不释放共享内存 payload / Frame Buffer；
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
RECOVERY
  ↓
NORMAL
```

状态说明：

| 状态 | 说明 |
|---|---|
| BOOT | 小核启动 |
| INIT_BOARD | 初始化 FreeRTOS 基础资源 |
| INIT_MAILBOX | 初始化 Mailbox 中断和 Doorbell |
| WAIT_SHM_READY | 等待 Linux 初始化共享内存 |
| INIT_RING_MAP | 建立 RX/TX Ring 映射 |
| INIT_ROUTER_TABLE | 加载目的地址到 TX Ring 的映射 |
| NORMAL | 正常路由调度 |
| DEGRADED | Linux 或共享内存异常时降级 |
| RECOVERY | 重新同步 epoch、Ring、bitmap、路由表 |
| NORMAL | 恢复正常运行 |

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

全局降级行为：

```text
1. 停止普通业务路由
2. 清空普通和低优先级本地队列引用
3. 保留关键错误统计
4. 标记旧 epoch 数据为丢弃
5. 标记 TTL 过期数据为丢弃
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
3. 优先标记丢弃 priority 3
4. 必要时标记丢弃 priority 2
5. 保留 priority 0 / priority 1
6. 更新统计
7. 必要时通知 Linux
```

---

## 28. 统计信息设计

建议小核维护以下统计：

| 统计项 | 含义 |
|---|---|
| `doorbell_rx_count` | 小核收到 Linux Doorbell 次数 |
| `doorbell_tx_count` | 小核通知 Linux 次数 |
| `rx_ring_drain_count` | 从 RX Ring 取帧次数 |
| `tx_ring_write_count` | 写 TX Ring 次数 |
| `route_success_count` | 路由成功次数 |
| `route_miss_count` | 目的地址无路由次数 |
| `prio_0_count` | priority 0 处理次数 |
| `prio_1_count` | priority 1 处理次数 |
| `prio_2_count` | priority 2 处理次数 |
| `prio_3_count` | priority 3 处理次数 |
| `ttl_drop_count` | TTL 过期丢弃次数 |
| `epoch_drop_count` | epoch 不匹配丢弃次数 |
| `tx_ring_full_count` | TX Ring 满次数 |
| `rx_ring_empty_count` | RX Ring 空读次数 |
| `frame_pool_error_count` | Frame Pool 异常次数 |
| `mailbox_fail_count` | Mailbox 通知失败次数 |
| `linux_heartbeat_timeout_count` | Linux 心跳超时次数 |
| `endpoint_hb_rx_count` | 收到合法端到网关心跳次数 |
| `endpoint_hb_invalid_count` | 非法端心跳帧次数 |
| `endpoint_hb_timeout_count` | 端心跳超时次数 |
| `endpoint_hb_recover_count` | 端设备从 WARN / OFFLINE 恢复次数 |
| `endpoint_hb_table_full_count` | 端心跳表满次数 |
| `recovery_count` | Recovery 次数 |

统计原则：

1. 高频统计先在小核本地累加。
2. 周期性同步到共享内存统计区。
3. 统计更新不能阻塞路由主流程。
4. Linux 可以读取统计区用于日志和调试。

---

## 29. 典型数据流

### 29.1 CAN 输入，RS485 输出

```text
CAN 总线
  ↓
Linux CAN 接收进程
  ↓
Linux 封装统一帧
  ↓
写 CAN0_RX_RING
  ↓
设置 rx_pending_bitmap[CAN0_RX]
  ↓
如果 CAN0_RX_RING 从空变非空，则 Mailbox 通知小核
  ↓
小核读取 CAN0_RX_RING
  ↓
小核读取目的通信地址
  ↓
小核判断目标为 RS485_0
  ↓
小核根据 priority 排序
  ↓
写 RS485_0_TX_RING
  ↓
设置 tx_pending_bitmap[RS485_0_TX]
  ↓
如果 RS485_0_TX_RING 从空变非空，则 Mailbox 通知 Linux
  ↓
Linux RS485 发送进程读取 RS485_0_TX_RING
  ↓
Linux 调用 RS485 驱动发送
  ↓
RS485 总线
```

### 29.2 RS485 输入，CAN 输出

```text
RS485 总线
  ↓
Linux RS485 接收进程
  ↓
Linux 封装统一帧
  ↓
写 RS485_0_RX_RING
  ↓
设置 rx_pending_bitmap[RS485_0_RX]
  ↓
如果 RS485_0_RX_RING 从空变非空，则 Mailbox 通知小核
  ↓
小核读取 RS485_0_RX_RING
  ↓
小核读取目的通信地址
  ↓
小核判断目标为 CAN0
  ↓
小核根据 priority 排序
  ↓
写 CAN0_TX_RING
  ↓
设置 tx_pending_bitmap[CAN0_TX]
  ↓
如果 CAN0_TX_RING 从空变非空，则 Mailbox 通知 Linux
  ↓
Linux CAN 发送进程读取 CAN0_TX_RING
  ↓
Linux 调用 CAN 驱动发送
  ↓
CAN 总线
```

### 29.3 Ethernet 输入，CAN 输出

```text
Ethernet
  ↓
Linux 网络接收进程
  ↓
Linux 协议解析并封装统一帧
  ↓
写 ETH0_RX_RING
  ↓
设置 rx_pending_bitmap[ETH0_RX]
  ↓
如果 ETH0_RX_RING 从空变非空，则 Mailbox 通知小核
  ↓
小核读取 ETH0_RX_RING
  ↓
小核根据目的通信地址选择 CAN0_TX_RING
  ↓
按 priority 排序
  ↓
写 CAN0_TX_RING
  ↓
设置 tx_pending_bitmap[CAN0_TX]
  ↓
如果 CAN0_TX_RING 从空变非空，则 Mailbox 通知 Linux
  ↓
Linux CAN 发送进程发送
```

### 29.4 端到网关心跳

统一数据帧定义中：

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
Linux 封装统一帧
  ↓
type = 0x00
source_cid = 端设备通信地址
destination_cid = 网关通信地址或当前配置允许的网关地址
  ↓
写对应物理层 RX Ring
  ↓
设置 rx_pending_bitmap
  ↓
如果 RX Ring 从空变非空，则 Mailbox 通知小核
  ↓
小核读取 RX Ring
  ↓
检查 header / epoch / TTL
  ↓
识别 type = 0x00
  ↓
更新 endpoint_heartbeat_table[source_cid]
  ↓
标记 RX Slot 已消费、Frame Buffer 可回收
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

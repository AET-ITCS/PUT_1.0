# 大小核共享内存 IPC v2 ABI 冻结说明

## 1. 文档定位

本文冻结本项目目标架构的大核 Linux 与小核 FreeRTOS 共享内存 IPC v2 公共 ABI。

正式小核固件目标目录为：

```text
rtos_firmware/
```

当前仓库中的：

```text
freertos/
```

只作为历史参考实现和行为参考，不作为后续主开发目录。

公共 ABI 头文件为：

```text
common/include/shared_memory_ipc.h
```

v2 主线不再使用 `unified_frame_t + 128B slot + CAN direct`。旧 v1 方案仅作为历史原型参考。

---

## 2. 总体边界

v2 主链路固定为：

```text
Linux 接入层得到完整 anyMSG
    ↓
Linux 分配 Frame Pool block 并写入完整 anyMSG
    ↓
Linux 写对应物理接口 RX Descriptor Ring
    ↓
Linux 设置 rx_pending_bitmap 并按需 Mailbox Doorbell
    ↓
rtos_firmware drain RX ring
    ↓
小核校验 anyMSG header / heartbeat / CID 路由 / priority / TTL
    ↓
小核写目标物理接口 TX Descriptor Ring 或 reclaim ring
    ↓
Linux 出口层读取 TX descriptor 并真实发送
    ↓
Linux 最终释放 Frame Pool block
```

职责边界：

| 模块 | 负责 | 不负责 |
| ---- | ---- | ------ |
| Linux 接入层 | 真实物理接收、解包、分片重组、完整 anyMSG 写入 Frame Pool | 小核路由调度 |
| 共享内存 IPC 层 | Frame Pool、Descriptor Ring、Pending Bitmap、cache 同步、Doorbell | 解释 anyMSG payload 业务语义 |
| `rtos_firmware` IPC 层 | descriptor 搬运、CRC、Frame Pool 边界检查、pending 状态 | 物理接口收发 |
| `rtos_firmware` 路由层 | anyMSG header 校验、心跳消费、CID 路由、priority/TTL 调度 | CAN/RS485/网络/蓝牙/4G 驱动 |
| Linux 出口层 | 读取 TX descriptor、封包、分片、真实发送、释放 Frame Pool | 修改小核路由结果 |
| Web 模块 | 读取 Linux 状态快照和日志 | 直接访问共享内存 |

---

## 3. 已冻结公共 ABI

### 3.1 基本常量

| 常量 | v2 值 | 说明 |
| ---- | ---: | ---- |
| `PUT_SHM_IPC_VERSION` | `2` | IPC ABI 版本 |
| `PUT_SHM_REGION_SIZE` | `64 KiB` | reserved-memory 总大小 |
| `PUT_SHM_CACHE_LINE_SIZE` | `64` | cache line 对齐粒度 |
| `PUT_SHM_INTERFACE_COUNT` | `6` | CAN / Ethernet / Wi-Fi / Bluetooth / 4G / RS485 |
| `PUT_SHM_FRAME_POOL_BLOCK_COUNT` | `64` | Frame Pool block 数量 |
| `PUT_SHM_FRAME_POOL_BLOCK_SIZE` | `512` | 单个完整 anyMSG 最大承载长度 |
| `PUT_SHM_DESCRIPTOR_RING_DEPTH` | `8` | 每个 RX/TX descriptor ring 深度 |
| `PUT_SHM_DESCRIPTOR_SIZE` | `64` | 单个 descriptor 固定长度 |
| `PUT_SHM_RECLAIM_RING_DEPTH` | `8` | reclaim ring 深度 |

共享内存物理地址不在 C 业务层硬编码，由 Linux DTS `reserved-memory` 与 `rtos_firmware` BSP/linker 配置共同提供。

### 3.2 共享内存区域

v2 region 由以下部分组成：

```text
put_shm_region_t
├── header
├── frame_pool[64][512]
├── rx_rings[6]
├── tx_rings[6]
├── rx_pending_bitmap
├── tx_pending_bitmap
├── reclaim_pending
├── reclaim_ring
└── reserved
```

接口顺序固定为：

| ID | 接口 |
| --: | ---- |
| `0` | CAN |
| `1` | Ethernet |
| `2` | Wi-Fi |
| `3` | Bluetooth |
| `4` | 4G |
| `5` | RS485 |

### 3.3 descriptor ring

每个物理接口有独立 RX ring 和 TX ring：

```text
xxx_RX_RING：Linux 单生产者，RTOS 单消费者
xxx_TX_RING：RTOS 单生产者，Linux 单消费者
reclaim_ring：RTOS 单生产者，Linux 单消费者
```

索引规则：

```text
empty: write_seq == read_seq
full : write_seq - read_seq >= depth
```

ring 满时必须丢弃最新 descriptor 并递增 `drop_count`，不得覆盖旧 descriptor。

### 3.4 descriptor 字段

`put_shm_descriptor_t` 固定 64B，描述 Frame Pool 中的完整 anyMSG：

| 字段 | 说明 |
| ---- | ---- |
| `frame_id` | Frame Pool block ID |
| `frame_offset` | 完整 anyMSG 相对 Frame Pool 起点的偏移 |
| `frame_length` | 完整 anyMSG 字节数，包含 40B header |
| `source_interface` | 来源物理接口 |
| `target_interface` | 目标物理接口 |
| `source_cid` | anyMSG `source_cid` raw bytes |
| `destination_cid` | anyMSG `destination_cid` raw bytes |
| `type` | anyMSG payload type |
| `priority` | 内部调度优先级，不写入 anyMSG 保留字段 |
| `ttl` | 内部转发 TTL |
| `epoch` | Linux 启动纪元 |
| `flags` | descriptor 内部标志 |
| `descriptor_crc16` | CRC-16/CCITT-FALSE，覆盖本字段之前的 descriptor 字节 |

Frame Pool 当前采用固定 block 模型：`frame_offset` 必须等于 `frame_id * 512`，`frame_length` 必须在 `40 ~ 512` 范围内。

接口一致性是 descriptor 格式校验的一部分：RX ring 出队时要求 `source_interface == ring.interface_id`，TX ring 出队时要求 `target_interface == ring.interface_id`。校验失败时消费坏 descriptor、递增 `format_error_count`，避免 ring 被坏元素永久卡住。

---

## 4. Pending Bitmap 与 Doorbell

`rx_pending_bitmap`、`tx_pending_bitmap` 和 `reclaim_pending` 分别独占一个 64B cache line。

规则：

1. 生产者写 descriptor 前判断 ring 是否为空。
2. 生产者写 descriptor，flush descriptor。
3. 执行 memory barrier。
4. 更新 `write_seq`，flush producer line。
5. 通过平台原子 OR 设置对应 pending bit。
6. 只有 ring 从 empty 变为 non-empty 时发送 Doorbell。
7. Doorbell 失败发生在 `write_seq` 发布后时，发送函数仍返回成功，只递增 `notify_fail_count`。

pending bitmap 的 `bits` 是多个接口共享的 RMW 字段，设置和清除必须通过平台 atomic bit operation 完成，不能通过整条 cache line 的普通读改写再 flush 发布。`set_count`、`clear_count` 等诊断计数也必须使用平台原子 ADD 更新，避免和 `bits` 处于同一 cache line 时覆盖并发写入。

消费者准备清 pending bit 时必须先确认当前 ring 为空；执行 atomic clear 后还必须重新读取 `producer.write_seq`。如果发现 `read_seq != latest_write_seq`，说明同接口生产者在清除窗口内发布了新 descriptor，消费者必须立刻通过 atomic OR 把对应 pending bit 置回。

Doorbell 只表示“对应 ring 可能有新 descriptor”，不承载业务 payload。Doorbell 丢失时，接收方必须通过周期 drain 兜底；ring 与 pending bitmap 是唯一可信数据状态。

---

## 5. Frame Pool 回收

Linux 负责 Frame Pool 分配和最终释放。RTOS 不清零 payload、不释放 block，只通过 reclaim ring 通知 Linux 某个 block 可回收。

RTOS 必须写 reclaim descriptor 的典型场景：

| 场景 | 回收原因 |
| ---- | -------- |
| 端到网关心跳被小核消费 | `PUT_SHM_RECLAIM_REASON_HEARTBEAT_CONSUMED` |
| 无路由 | `PUT_SHM_RECLAIM_REASON_NO_ROUTE` |
| TTL 过期 | `PUT_SHM_RECLAIM_REASON_TTL_EXPIRED` |
| epoch 不匹配 | `PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH` |
| anyMSG 基础校验失败 | `PUT_SHM_RECLAIM_REASON_INVALID_FRAME` |
| 本地队列或目标 ring 满 | `PUT_SHM_RECLAIM_REASON_QUEUE_FULL` |

---

## 6. RTOS IPC API

`rtos_firmware/ipc/rtos_shm_ipc.h` 提供 v2 descriptor API：

| API | 说明 |
| ---- | ---- |
| `rtos_shm_ipc_format_region()` | 初始化 v2 region、Frame Pool、12 个 RX/TX ring、pending bitmap 和 reclaim ring |
| `rtos_shm_ipc_attach()` | 校验并绑定 v2 region |
| `rtos_shm_ipc_dequeue_rx_descriptor()` | 小核从指定接口 RX ring 取 descriptor |
| `rtos_shm_ipc_enqueue_tx_descriptor()` | 小核向指定接口 TX ring 写 descriptor |
| `rtos_shm_ipc_reclaim_frame()` | 小核写 reclaim descriptor |
| `rtos_shm_ipc_get_frame_const()` | 小核根据 descriptor 获取只读完整 anyMSG 指针 |
| `rtos_shm_descriptor_ring_enqueue()` | descriptor ring 通用入队 helper |
| `rtos_shm_descriptor_ring_dequeue()` | descriptor ring 通用出队 helper |

IPC 层只校验共享内存 ABI、descriptor CRC、Frame Pool 边界和接口 ID，不解析 Modbus、CANopen、UDS、J1939 等 payload 语义。

---

## 7. 错误码

IPC 专用错误码：

| 错误码 | 含义 |
| ------ | ---- |
| `UNIFIED_ERR_IPC_QUEUE_EMPTY` | ring 为空 |
| `UNIFIED_ERR_IPC_QUEUE_FULL` | ring 已满 |
| `UNIFIED_ERR_IPC_NOT_READY` | IPC 未初始化或平台操作未就绪 |
| `UNIFIED_ERR_IPC_NOTIFY_FAILED` | 底层 Doorbell 通知动作失败 |
| `UNIFIED_ERR_IPC_OFFLINE` | 对端离线或 fail-safe 状态下拒绝业务 |

`UNIFIED_ERR_IPC_NOTIFY_FAILED` 不作为 descriptor 已发布后的可重试发送失败返回；发送方记录 `notify_fail_count`，接收方依赖 pending bitmap 和周期 drain 兜底。

---

## 8. v1 历史原型

旧 v1 `unified_frame_t + 128B slot + CAN direct` 路径仅作为历史原型。后续主开发不得继续以 `PUT_SHM_MESSAGE_TYPE_*`、固定 128B payload 或小核 CAN direct 作为主链路设计依据。

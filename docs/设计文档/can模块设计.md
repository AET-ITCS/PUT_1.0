# CAN 模块设计说明书 (v2 anyMSG 架构)

版本号：0.1.0
修改时间：2026-05-28
文档定位：描述 Linux 大核 `can_adapter.c` 的模块边界、SocketCAN classic 第一版实现、anyMSG 分片重组、共享内存 IPC 对接、状态统计、故障恢复和测试方案。

---

## 1. 模块定位与设计目标

CAN 模块是六类物理接口适配器之一，运行在 Milk-V Duo256M 大核 Linux 侧。它通过 Linux SocketCAN raw socket 访问 `can0`，负责真实 CAN 总线收发、CAN 私有分片重组、完整 anyMSG 校验、写入共享内存 CAN RX Ring，以及从 CAN TX Ring 读取完整 anyMSG 后分片发送。

核心边界如下：

```text
外部 CAN 节点
    ↓ Classic CAN frame
Linux can_adapter.c
    ↓ CAN 私有分片重组 / anyMSG 校验
共享内存 Frame Pool + CAN RX Descriptor Ring
    ↓ Mailbox Doorbell
FreeRTOS 小核路由调度
    ↓ CAN TX Descriptor Ring
Linux can_adapter.c
    ↓ CAN 私有分片发送
外部 CAN 节点
```

第一版实现范围：

1. 使用 SocketCAN raw socket，默认接口名为 `can0`。
2. 以 classic CAN 8B data field 为基础承载 anyMSG 分片。
3. 最大完整 anyMSG 长度为共享内存 v2 当前 block 上限 `512B`。
4. CAN ID 只用于物理仲裁、过滤和链路定位，不参与业务路由决策。
5. 业务路由只由 anyMSG `destination_cid` 和小核路由表决定。
6. CAN FD、ISO-TP、UDS、J1939、CANopen 作为扩展路径，不纳入第一版必须实现。

CAN 模块不负责：

1. 不在 FreeRTOS 小核中操作 CAN 控制器。
2. 不把 CAN 分片写入共享内存。
3. 不修改 anyMSG 帧头结构。
4. 不解析 UDS、J1939、CANopen 等复杂 payload 业务语义。
5. 不绕过共享内存 TX Ring 直接根据业务地址发送。

---

## 2. 与总体架构的关系

CAN 入口和出口都服从 v2 主链路：

```text
完整 anyMSG 是唯一进入共享内存的业务实体；
CAN 分片头是 Linux CAN adapter 的私有物理封装；
小核只看到完整 anyMSG descriptor；
Linux 是 Frame Pool 的唯一分配者和最终释放者。
```

对应接口 ID 固定为：

```c
PUT_SHM_INTERFACE_CAN = 0
```

CID 地址段中，CAN 设备地址首字节范围为：

```text
0x20 ~ 0x3F
```

入口校验要求：

1. CAN adapter 从总线重组出完整 anyMSG 后，必须校验 `msg_length`、`payload_length`、保留字段、CID 和实际长度。
2. 从 CAN 总线进入的 anyMSG，其 `source_cid[0]` 必须位于 `0x20 ~ 0x3F`。
3. `destination_cid[0]` 可指向 CAN、Ethernet、Wi-Fi、Bluetooth、4G、RS485 或保留地址段；后续是否可路由由小核判断。
4. 合法完整 anyMSG 才能写入 Frame Pool 和 CAN RX Descriptor Ring。

出口处理要求：

1. Linux 只从 CAN TX Ring 读取小核已经路由到 CAN 的 descriptor。
2. CAN adapter 从 Frame Pool 读取完整 anyMSG，按 classic CAN 分片协议封装成 SocketCAN frame。
3. 发送完成后 Linux 释放对应 Frame Pool block。
4. 发送失败时记录错误并释放或按出口层统一策略处理，不把失败帧退回小核。

---

## 3. RX Path 设计

### 3.1 接收主流程

```text
SocketCAN raw socket 读取 struct can_frame
    ↓
按 CAN 私有分片类型分发 SOF / DATA
    ↓
更新或创建 reassembly session
    ↓
按 session_id + source can_id 聚合分片
    ↓
收齐后校验 total_len 和完整 anyMSG CRC16
    ↓
执行 anyMSG 基础校验
    ↓
source_cid 地址段必须为 CAN
    ↓
linux_shm_frame_alloc(PUT_SHM_INTERFACE_CAN)
    ↓
memcpy 完整 anyMSG 到 Frame Pool block
    ↓
linux_shm_frame_commit_rx(..., source_interface = CAN, target_interface = route_hint)
    ↓
status_collector_record_rx()
```

接收线程采用独立 pthread 或接入统一 event loop 均可。第一版推荐独立 RX 线程，避免 CAN 阻塞、Bus-Off 恢复和重组超时扫描影响 Ethernet、Bluetooth 等其他接口。

### 3.2 解码与校验职责

`decode_rx()` 接收的是已经重组完成的完整 anyMSG 字节，不直接接收单个 CAN fragment。单个 SocketCAN frame 的分片解析由 CAN adapter 内部重组逻辑完成。

`decode_rx()` 必须检查：

| 检查项 | 规则 |
| ------ | ---- |
| 空指针 | `input` 和 `out` 不得为 NULL |
| 长度下限 | `input_len >= ANYMSG_HEADER_SIZE` |
| 长度上限 | `input_len <= PUT_SHM_FRAME_POOL_BLOCK_SIZE` |
| `msg_length` | 小端读取，必须等于实际 `input_len` |
| `payload_length` | 小端读取，必须满足 `msg_length == 40 + payload_length` |
| 保留字段 | `__RESERVED__`、`__SRCHLD__`、`__PADDING__` 当前必须为 0 |
| 源 CID | `source_cid[0]` 必须在 `0x20 ~ 0x3F` |
| 目标 CID | 地址首字节必须是文档定义或保留段 |
| type | 允许文档定义和保留范围，CAN adapter 不解析 payload 语义 |

基础 helper 复用：

```c
anymsg_validate_normalized_lengths()
anymsg_validate_header_static_fields()
anymsg_cid_segment_from_first_byte()
```

### 3.3 写入共享内存

CAN adapter 在校验完整 anyMSG 成功后调用 Linux IPC API：

```c
linux_shm_frame_alloc(ipc,
                      PUT_SHM_INTERFACE_CAN,
                      &frame_id,
                      &frame_buffer,
                      &frame_capacity);

linux_shm_frame_commit_rx(ipc,
                          frame_id,
                          frame_length,
                          PUT_SHM_INTERFACE_CAN,
                          target_interface,
                          source_cid,
                          destination_cid,
                          type,
                          CAN_ADAPTER_DEFAULT_PRIORITY,
                          CAN_ADAPTER_DEFAULT_TTL,
                          linux_epoch,
                          flags);
```

`target_interface` 只作为 route hint，由 `destination_cid[0]` 初步映射：

| CID 段 | route hint |
| ------ | ---------- |
| `0x20 ~ 0x3F` | `PUT_SHM_INTERFACE_CAN` |
| `0x40 ~ 0x5F` | `PUT_SHM_INTERFACE_ETHERNET` |
| `0x60 ~ 0x7F` | `PUT_SHM_INTERFACE_WIFI` |
| `0x80 ~ 0x9F` | `PUT_SHM_INTERFACE_BLUETOOTH` |
| `0xA0 ~ 0xBF` | `PUT_SHM_INTERFACE_4G` |
| `0xC0 ~ 0xDF` | `PUT_SHM_INTERFACE_RS485` |
| 保留段 | 默认保留为 CAN 或交由小核判定无路由 |

第一版推荐对保留段设置 `target_interface = PUT_SHM_INTERFACE_CAN`，并依赖小核 CID 路由校验最终丢弃或处理，避免 Linux adapter 自行定义路由策略。

---

## 4. TX Path 设计

### 4.1 发送主流程

```text
Linux 出口调度收到 TX pending
    ↓
linux_shm_dequeue_tx_descriptor(ipc, PUT_SHM_INTERFACE_CAN, ...)
    ↓
校验 target_interface == CAN
    ↓
从 Frame Pool 获取完整 anyMSG
    ↓
can_adapter.fragment_tx()
    ↓
按 SOF + DATA frames 写 SocketCAN raw socket
    ↓
发送成功：status_collector_record_tx_ok()
    ↓
linux_shm_frame_release(frame_id, ...)
```

CAN adapter 不改变小核的路由结果。只要 descriptor 出现在 CAN TX Ring，就认为目标物理出口是 CAN。

### 4.2 发送错误处理

| 错误 | 处理 |
| ---- | ---- |
| TX Ring descriptor 格式错误 | 消费 descriptor，记录 IPC format error，不发送 |
| Frame Pool 指针无效 | 记录 IPC error，消费 descriptor |
| SocketCAN socket 未打开 | 记录 interface offline / send fail |
| `write()` 返回短写 | 视为发送失败，记录 send fail |
| `ENETDOWN` / `ENODEV` | 标记接口 offline，进入恢复扫描 |
| `ENOBUFS` / `EAGAIN` | 记录拥塞；第一版不在 adapter 内无限重试 |
| Bus-Off | 记录 bus-off/offline，等待恢复策略重新启用接口 |

发送失败后的 Frame Pool 释放由 Linux 出口层统一负责。第一版默认释放失败帧并记录错误，避免 Frame Pool 泄漏。

---

## 5. Classic CAN 私有分片协议

Classic CAN 单帧 data field 只有 8B，无法直接承载最大 512B anyMSG。CAN adapter 使用私有分片头在物理层承载完整 anyMSG。该分片头不属于 anyMSG，不写入共享内存 descriptor。

### 5.1 分片基本参数

| 参数 | 默认值 |
| ---- | ------ |
| 物理模式 | SocketCAN classic CAN |
| 单 CAN frame data 长度 | 8B |
| 完整 anyMSG 最大长度 | 512B |
| DATA 帧有效数据长度 | 5B |
| 最大 DATA 帧数 | `ceil(512 / 5) = 103` |
| 最大并发重组 session | 8 |
| 默认重组超时 | 500ms |
| 完整帧校验 | CRC-16/CCITT-FALSE |

### 5.2 CAN ID 使用

第一版配置使用固定或过滤型 CAN ID：

| 配置项 | 用途 |
| ------ | ---- |
| `tx_can_id` | 本网关向 CAN 总线发送 anyMSG 分片使用的 CAN ID |
| `rx_filter_id` | 接收过滤 ID |
| `rx_filter_mask` | 接收过滤 mask |
| `extended_id` | 是否使用 29-bit 扩展帧 |

CAN ID 只用于物理链路：

1. 仲裁优先级。
2. Linux socket 过滤。
3. 重组 session 的来源区分。
4. 诊断日志定位。

CAN ID 不决定 `target_interface`，也不替代 anyMSG `source_cid` 或 `destination_cid`。

### 5.3 SOF 帧格式

SOF 帧用于声明一个完整 anyMSG 分片会话。

```text
CAN data[8]
byte 0: frame_kind = 0xA0
byte 1: session_id
byte 2: total_len low
byte 3: total_len high
byte 4: full_crc16 low
byte 5: full_crc16 high
byte 6: flags
byte 7: data_frame_count
```

字段说明：

| 字段 | 说明 |
| ---- | ---- |
| `frame_kind` | 固定 `0xA0`，表示 SOF |
| `session_id` | 8-bit 会话号，由发送端递增生成 |
| `total_len` | 完整 anyMSG 长度，小端，范围 `40 ~ 512` |
| `full_crc16` | 完整 anyMSG CRC-16/CCITT-FALSE，小端 |
| `flags` | bit0 表示最后分片不足 5B 时按 `total_len` 截断，其他 bit 保留为 0 |
| `data_frame_count` | DATA 帧总数，必须等于 `ceil(total_len / 5)` |

SOF 处理规则：

1. `total_len < 40` 或 `total_len > 512` 时立即拒绝。
2. `data_frame_count == 0` 或不等于 `ceil(total_len / 5)` 时拒绝。
3. 同一 `source can_id + session_id` 已存在时，若旧 session 未超时，则记录 session conflict 并覆盖旧 session。
4. 创建 session 后清空收片 bitmap 和缓存。

### 5.4 DATA 帧格式

DATA 帧用于携带 anyMSG 字节数据。

```text
CAN data[8]
byte 0: frame_kind = 0xD0
byte 1: session_id
byte 2: seq
byte 3: payload byte 0
byte 4: payload byte 1
byte 5: payload byte 2
byte 6: payload byte 3
byte 7: payload byte 4
```

字段说明：

| 字段 | 说明 |
| ---- | ---- |
| `frame_kind` | 固定 `0xD0`，表示 DATA |
| `session_id` | 与 SOF 一致 |
| `seq` | DATA 帧序号，从 0 开始 |
| `payload` | 最多 5B anyMSG 连续数据 |

DATA 处理规则：

1. 找不到对应 session 时丢弃并记录 orphan data。
2. `seq >= data_frame_count` 时丢弃并记录 fragment format error。
3. 重复 `seq` 忽略并记录 duplicate。
4. 乱序允许，按 `seq * 5` 写入重组 buffer。
5. 最后一片有效长度由 `total_len - seq * 5` 决定，最多 5B。
6. 收齐全部 DATA 后计算完整 buffer CRC16，与 SOF 中 `full_crc16` 比较。
7. CRC 正确后输出完整 anyMSG；CRC 错误则丢弃 session。

### 5.5 重组 session 表

第一版 session 表固定 8 项：

```c
typedef struct {
    bool in_use;
    uint32_t source_can_id;
    uint8_t session_id;
    uint16_t total_len;
    uint16_t full_crc16;
    uint8_t data_frame_count;
    uint8_t received_count;
    uint64_t started_at_ms;
    uint64_t updated_at_ms;
    uint8_t buffer[PUT_SHM_FRAME_POOL_BLOCK_SIZE];
    uint8_t received_bitmap[16];
} can_reassembly_session_t;
```

session 匹配键：

```text
source_can_id + session_id
```

驱逐策略：

1. 优先清理已超时 session。
2. 无超时 session 且表满时，丢弃最旧的 session，记录 no buffer。
3. 新 SOF 与旧 session 冲突时覆盖旧 session，记录 session conflict。

超时扫描：

1. RX 线程每收到一个 CAN frame 后扫描一次。
2. 至少每 100ms 周期扫描一次，避免静默缺片长期占用 session。
3. 超过 `reassembly_timeout_ms` 后释放 session，记录 reassemble timeout。

---

## 6. 统一物理接口适配器设计

CAN 模块对外暴露统一适配器实例：

```c
extern physical_interface_adapter_t can_adapter;
```

接口表：

```c
physical_interface_adapter_t can_adapter = {
    .name = "can",
    .interface_id = PUT_SHM_INTERFACE_CAN,
    .get_mtu = can_get_mtu,
    .decode_rx = can_decode_rx,
    .reassemble = can_reassemble,
    .encapsulate = can_encapsulate,
    .fragment_tx = can_fragment_tx,
    .send = can_send,
    .status = can_status
};
```

各回调职责：

| 回调 | 职责 |
| ---- | ---- |
| `get_mtu` | 返回 CAN 私有分片后单 DATA 帧有效载荷 `5B`，或返回完整 anyMSG 上限 `512B` 时需在注释中说明语义 |
| `decode_rx` | 校验完整 anyMSG 并填充 `adapter_rx_result_t` |
| `reassemble` | 接收 CAN fragment wrapper，更新 session，收齐时输出完整 anyMSG |
| `encapsulate` | 对可单包发送的情况封装；classic CAN 下通常转调 `fragment_tx` |
| `fragment_tx` | 将完整 anyMSG 转为 SOF + DATA frame 列表 |
| `send` | 通过 SocketCAN `write()` 真实发送一个 `struct can_frame` |
| `status` | 输出当前 CAN 模块状态和统计 |

当前 `physical_interface_adapter_t` 的 `adapter_tx_packet_t` 只包含 `data/len`，第一版实现时可将 `data` 指向 `struct can_frame` 数组中的单帧，`len = sizeof(struct can_frame)`。如后续需要一次性返回多帧列表，`adapter_tx_packet_list_t` 的 `packets/count` 表达 SOF + DATA 序列。

---

## 7. 核心数据结构

### 7.1 RX 上下文

```c
typedef struct {
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
    uint8_t default_priority;
    uint8_t default_ttl;
} can_rx_context_t;
```

### 7.2 配置结构

```c
#define CAN_ADAPTER_IFNAME_MAX 32u

typedef struct {
    bool enabled;
    char ifname[CAN_ADAPTER_IFNAME_MAX];
    uint32_t bitrate;
    uint32_t tx_can_id;
    uint32_t rx_filter_id;
    uint32_t rx_filter_mask;
    bool extended_id;
    uint32_t reassembly_timeout_ms;
    linux_shm_ipc_t *ipc;
    status_collector_t *collector;
    uint32_t linux_epoch;
} can_adapter_config_t;
```

默认值：

| 字段 | 默认值 |
| ---- | ------ |
| `enabled` | `false` |
| `ifname` | `"can0"` |
| `bitrate` | `500000` |
| `tx_can_id` | `0x321` |
| `rx_filter_id` | `0x320` |
| `rx_filter_mask` | `0x7FF` |
| `extended_id` | `false` |
| `reassembly_timeout_ms` | `500` |
| `default_priority` | `2` |
| `default_ttl` | `8` |

`bitrate` 用于启动前配置检查或日志提示。第一版用户态不强制调用 `ip link set can0 type can bitrate ...`，避免在应用内执行需要额外权限的系统命令；部署脚本负责配置 CAN bitrate。

### 7.3 状态结构

```c
typedef struct {
    bool enabled;
    bool running;
    bool socket_open;
    bool interface_online;
    bool bus_off;
    char ifname[CAN_ADAPTER_IFNAME_MAX];

    uint64_t rx_frames;
    uint64_t tx_frames;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t error_count;
    uint64_t decode_error_count;
    uint64_t fragment_drop_count;
    uint64_t duplicate_fragment_count;
    uint64_t orphan_fragment_count;
    uint64_t reassemble_timeout_count;
    uint64_t crc_error_count;
    uint64_t session_conflict_count;
    uint64_t session_no_buffer_count;
    uint64_t shm_alloc_fail_count;
    uint64_t ipc_error_count;
    uint64_t send_fail_count;
    uint64_t interface_offline_count;

    uint64_t started_at_ms;
    uint64_t updated_at_ms;
    uint64_t last_rx_ms;
    uint64_t last_tx_ms;
    uint64_t last_error_ms;
    char last_error_stage[128];
    char last_error_message[128];
} can_status_t;
```

状态收集映射到 `status_collector_t`：

| can_status_t | status_collector 字段 |
| ------------ | ---------------------- |
| `rx_frames` / `rx_bytes` | `status_collector_record_rx(STATUS_MODULE_CAN, bytes)` |
| `tx_frames` / `tx_bytes` | `status_collector_record_tx_ok(STATUS_MODULE_CAN, bytes)` |
| 解码、CRC、分片、IPC、发送错误 | `status_collector_record_error(STATUS_MODULE_CAN, stage, err)` |
| 接口启动/停止 | `status_collector_mark_running()` / `status_collector_mark_stopped()` |

---

## 8. 配置文件设计

`linux_app/config/device_config.ini` 新增 `[can]` 段：

```ini
[can]
enabled = false
ifname = "can0"
bitrate = 500000
tx_can_id = 0x321
rx_filter_id = 0x320
rx_filter_mask = 0x7FF
extended_id = false
reassembly_timeout_ms = 500
```

校验规则：

| 字段 | 规则 |
| ---- | ---- |
| `enabled` | bool |
| `ifname` | 非空，长度小于 `CAN_ADAPTER_IFNAME_MAX` |
| `bitrate` | 大于 0 |
| `tx_can_id` | classic 标准帧时 `<= 0x7FF`，扩展帧时 `<= 0x1FFFFFFF` |
| `rx_filter_id` | 同 CAN ID 范围 |
| `rx_filter_mask` | 标准帧默认 `0x7FF`，扩展帧默认 `0x1FFFFFFF` |
| `extended_id` | bool |
| `reassembly_timeout_ms` | 建议范围 `100 ~ 5000` |

启动前部署建议：

```bash
ip link set can0 down
ip link set can0 type can bitrate 500000 restart-ms 100
ip link set can0 up
```

应用内只负责打开和使用 `can0`，不默认修改系统网络接口配置。

---

## 9. SocketCAN 实现要点

### 9.1 打开 socket

第一版使用：

```c
socket(PF_CAN, SOCK_RAW, CAN_RAW)
ioctl(fd, SIOCGIFINDEX, &ifr)
bind(fd, (struct sockaddr *)&addr, sizeof(addr))
setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, ...)
```

推荐 socket 参数：

| 参数 | 建议 |
| ---- | ---- |
| receive timeout | 100ms，用于周期扫描重组超时 |
| non-blocking | 可选；独立线程下阻塞 + timeout 更简单 |
| CAN_RAW_FILTER | 使用 `rx_filter_id` / `rx_filter_mask` |
| CAN_RAW_ERR_FILTER | 订阅 Bus-Off、controller error 等错误帧 |

### 9.2 读取 CAN frame

读取后检查：

1. `read()` 必须返回 `sizeof(struct can_frame)`。
2. `can_dlc` 必须为 8，第一版私有分片协议固定使用 8B。
3. 错误帧 `CAN_ERR_FLAG` 单独处理，不进入重组。
4. 远程帧 RTR 不参与 anyMSG 分片，直接忽略并记录。

### 9.3 发送 CAN frame

发送前构造：

```c
struct can_frame frame;
frame.can_id = tx_can_id 或 tx_can_id | CAN_EFF_FLAG;
frame.can_dlc = 8;
memcpy(frame.data, fragment_data, 8);
```

发送规则：

1. SOF 必须先于 DATA 发送。
2. DATA 按 `seq` 从小到大发送。
3. 每个 CAN frame 都要求 `write()` 返回 `sizeof(struct can_frame)`。
4. 第一版不设计链路层 ACK；外部节点若需要可靠重发，应在 anyMSG payload 业务层或后续 CAN adapter 扩展中实现。

---

## 10. 故障恢复与 Fail-safe

### 10.1 接口离线

当 socket 打开失败、`read()` 返回 `ENETDOWN` / `ENODEV` 或错误帧报告 Bus-Off 时：

1. 标记 `interface_online = false`。
2. 记录 `interface_offline_count`。
3. 关闭 socket。
4. 后台线程按固定退避周期重新尝试打开 `ifname`。
5. 恢复后重新设置 filter 并标记 running。

第一版推荐退避周期：

```text
1s, 2s, 5s, 5s ...
```

### 10.2 Bus-Off

Bus-Off 由 SocketCAN 错误帧或发送错误表现出来。CAN adapter 的职责是记录和恢复 socket；底层控制器恢复策略由系统配置负责，例如 `restart-ms 100`。

处理：

1. 记录 `bus_off = true`。
2. 清空未完成重组 session。
3. 暂停 RX/TX。
4. 等待 SocketCAN 接口恢复或重新打开 socket。
5. 恢复成功后记录 `bus_off = false`。

### 10.3 重组异常

| 异常 | 处理 |
| ---- | ---- |
| 缺片 | 超时后释放 session，记录 reassemble timeout |
| 重复片 | 忽略重复 DATA，记录 duplicate |
| 乱序 | 允许，按 offset 写入 buffer |
| orphan DATA | 丢弃，记录 orphan fragment |
| CRC 错误 | 丢弃完整 session，记录 crc error |
| `total_len` 超上限 | 拒绝 SOF，记录 oversized / decode error |
| session 表耗尽 | 丢弃最旧或超时 session，记录 no buffer |

### 10.4 IPC 异常

| 异常 | 处理 |
| ---- | ---- |
| Frame Pool 满 | 丢弃完整 anyMSG，记录 `shm_alloc_fail_count` |
| CAN RX Ring 满 | 释放刚分配的 Frame Pool block，记录 IPC queue full |
| Doorbell 失败 | 由 IPC 层记录 notify fail；CAN adapter 按 API 返回处理 |
| Linux epoch 不可用 | 拒绝启动 CAN RX 服务 |

---

## 11. 扩展路径

### 11.1 CAN FD

CAN FD 可作为第二阶段扩展：

1. 使用 `struct canfd_frame` 和 `CAN_RAW_FD_FRAMES`。
2. 单帧 payload 可达 64B，可减少或取消私有分片。
3. `type = 0x4A` 表示 CAN_FD payload 语义，但物理出口仍由 `destination_cid` 决定。
4. CAN FD 与 classic CAN 使用独立配置项或运行时能力探测，不混用同一分片格式。

### 11.2 ISO-TP / UDS

ISO-TP 和 UDS 属于 CAN 上层协议：

1. 第一版不在 adapter 内解析 ISO-TP 或 UDS 业务。
2. `type = 0x4B` / `0x4C` 仅表示 anyMSG payload 语义。
3. 后续可新增 ISO-TP 子适配层，负责把 anyMSG payload 和 Linux ISO-TP socket 对接。
4. 小核仍不处理 ISO-TP 分片和 UDS 服务语义。

### 11.3 J1939 / CANopen

J1939 和 CANopen 后续作为协议子模块：

1. 可复用 Linux SocketCAN J1939 或用户态 CANopen 栈。
2. 适配层输出仍必须是完整 anyMSG。
3. 复杂对象字典、PGN、节点管理等逻辑不得放入共享内存层或小核路由层。

---

## 12. 目标文件与接口清单

第一版实现建议新增：

```text
linux_app/adapters/can_adapter.h
linux_app/adapters/can_adapter.c
linux_app/test/can_adapter_test.c
```

`can_adapter.h` 对外接口建议：

```c
extern physical_interface_adapter_t can_adapter;

unified_error_t can_adapter_decode_anymsg(const uint8_t *input,
                                          size_t input_len,
                                          adapter_rx_result_t *out);

unified_error_t can_adapter_submit_to_ipc(linux_shm_ipc_t *ipc,
                                          const uint8_t *frame,
                                          const adapter_rx_result_t *rx,
                                          uint32_t linux_epoch);

int can_adapter_start(const can_adapter_config_t *config);
void can_adapter_stop(void);
```

启动接入 `linux_app/main.c` 时遵循现有 Ethernet 模式：

1. 解析 `[can]` 配置。
2. `status_collector_configure_module(STATUS_MODULE_CAN, implemented = true, enabled = config.can_enabled, ...)`。
3. IPC format 或 attach 成功后启动 CAN RX 线程。
4. 主循环周期写状态快照。
5. 退出时 stop CAN 线程并关闭 socket。

---

## 13. 测试计划

### 13.1 anyMSG 校验测试

- 40B 最小帧，`payload_length = 0`。
- 512B 最大帧。
- `msg_length != actual_length`。
- `msg_length != 40 + payload_length`。
- 保留字段非 0。
- `source_cid[0]` 不在 `0x20 ~ 0x3F`。
- `destination_cid[0]` 指向六类接口地址段。
- `type = 0x49`、`0x4A`、`0x4B`、保留 type 均只做头部合法性处理。

### 13.2 CAN 分片重组测试

- SOF + 顺序 DATA 正常重组。
- DATA 乱序到达。
- DATA 重复到达。
- 缺失 DATA 后超时释放。
- CRC16 错误。
- orphan DATA。
- SOF `total_len < 40`。
- SOF `total_len > 512`。
- `data_frame_count` 与 `total_len` 不匹配。
- 同一 `source_can_id + session_id` 冲突。
- session 表满时驱逐最旧 session。

### 13.3 IPC 对接测试

- 完整 CAN anyMSG 写入 Frame Pool。
- CAN RX descriptor 字段正确：`source_interface = CAN`、`source_cid`、`destination_cid`、`type`、priority、TTL、epoch。
- Frame Pool 满时返回错误并记录。
- CAN RX Ring 满时释放已分配 block。
- pending bitmap 和 doorbell 行为由 `linux_shm_ipc_test` 兜底验证。

### 13.4 TX 出口测试

- 40B anyMSG 分片数量为 8 个 DATA，加 1 个 SOF。
- 512B anyMSG 分片数量为 103 个 DATA，加 1 个 SOF。
- SOF 字段正确。
- DATA 序号和最后一片有效长度正确。
- SocketCAN `write()` 短写返回失败。
- socket 离线返回失败并记录 status。
- 发送成功后释放 Frame Pool。

### 13.5 集成测试

- CAN RX 到 RS485 TX。
- CAN RX 到 Ethernet TX。
- Ethernet RX 到 CAN TX。
- Bluetooth RX 到 CAN TX。
- CAN 心跳 `type = 0x00` 进入小核后被消费并 reclaim。
- Web `/api/modules` 可见 CAN running、rx/tx/error 统计。
- Web `/api/ipc-status` 可见 CAN RX/TX ring 统计变化。

---

## 14. 第一版验收标准

第一版 CAN adapter 完成后应满足：

1. `can0` 在线时 CAN RX 线程可启动并写入 running 状态。
2. 外部 CAN 节点发送 SOF + DATA 后，Linux 能重组出完整 anyMSG。
3. 合法 CAN 源 CID 帧能进入 CAN RX Ring。
4. 非法长度、CRC 错误、非 CAN 源 CID 不进入共享内存。
5. 小核可将其他接口入口帧路由到 CAN TX Ring。
6. Linux 能从 CAN TX Ring 读取完整 anyMSG 并分片发送到 `can0`。
7. Frame Pool 在成功发送、入口失败、RX Ring 满等路径下无泄漏。
8. Web 状态快照中能看到 CAN 模块收发和错误统计。

---

## 15. 关键约束总结

```text
CAN 物理收发只在 Linux can_adapter.c；
共享内存只接收完整 anyMSG；
CAN 分片头不属于 anyMSG；
FreeRTOS 小核不接触 CAN 控制器；
CAN ID 不参与业务路由；
Linux 负责 Frame Pool 分配和最终释放；
Classic CAN 是第一版主线，CAN FD / ISO-TP / J1939 / CANopen 后续扩展。
```

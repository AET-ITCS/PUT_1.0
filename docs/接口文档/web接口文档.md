# Web 模块对外接口文档

适用架构：anyMSG v0.2.1 / 目标 v2 架构

修改时间：2026-05-26

文档定位：定义 Web 模块（`put-webd` + Vue3 前端）需要 `linux_app` 和部署环境提供的只读数据契约。

---

## 1. 文档定位

Web 模块是目标 v2 架构中的旁路监控链路，不参与主通信链路，不改变任何路由、队列或物理接口状态。

主通信链路如下：

```text
外部设备
  ↓ 完整 anyMSG 放入物理协议载荷，必要时由物理适配层分片
Linux 大核接入层
  ↓ 解包 / 重组 / 校验出完整 anyMSG
共享内存 RX Ring + Frame Pool
  ↓ Mailbox Doorbell 唤醒
FreeRTOS 小核路由调度
  ↓ anyMSG 头部校验 / 心跳消费 / CID 路由 / 优先级调度
共享内存 TX Ring + Frame Pool
  ↓ Mailbox Doorbell 通知
Linux 大核出口层
  ↓ 按目标物理接口封包 / 分片 / 发送
目标设备
```

Web 模块只读取三类数据：

| 数据类别 | 提供方                                                     | 说明                                          |
| -------- | ---------------------------------------------------------- | --------------------------------------------- |
| 状态快照 | `linux_app` 写入 `/run/put/status/`                        | 六类物理接口、共享内存 v2、小核路由、异常事件 |
| 应用日志 | `linux_app`、`put-webd` 或系统日志组件写入 `/var/log/put/` | 大核应用、Web、IPC、路由、适配器和系统日志    |
| 系统资源 | 内核 `/proc`、`/sys`、`/dev`                               | Web 后端直接读取，不需要其余模块提供          |

Web 模块必须满足：

- 不解析外部业务协议，不构造或修改 anyMSG。
- 不直接读取或写入共享内存、Descriptor Ring、Frame Pool、Mailbox 寄存器。
- 不向 FreeRTOS 小核发送控制命令。
- 不控制 CAN、Ethernet、Wi-Fi、Bluetooth、4G、RS485 任一物理接口。
- 所有共享内存和小核状态均由 `linux_app` 汇总后以快照形式提供。

---

## 2. 总体数据流

```text
┌──────────────────────────────────────────────────────────────────┐
│                       linux_app 大核程序                         │
│                                                                  │
│  status_collector.c                 log_manager.c                │
│  ├── 汇总六类接口状态             ├── 按 source 分文件           │
│  ├── 汇总共享内存 v2 状态          ├── 对敏感字段脱敏            │
│  ├── 汇总小核路由调度统计          └── 写入 /var/log/put/        │
│  ├── 记录异常事件                                                │
│  └── 原子写入 /run/put/status/ 快照                              │
└───────────────┬───────────────────────────┬──────────────────────┘
                │ 写入                      │ 写入
                v                           v
┌────────────────────────────┐  ┌───────────────────────────────┐
│ /run/put/status/           │  │ /var/log/put/                 │
│ ├── modules.json           │  │ ├── linux_app.log             │
│ ├── ipc_status.json        │  │ ├── web.log                   │
│ ├── route_status.json      │  │ ├── ipc.log                   │
│ └── events.jsonl           │  │ ├── router.log                │
└──────────────┬─────────────┘  │ ├── adapter.log               │
               │ 只读           │ └── system.log                │
               │                └──────────────┬────────────────┘
               v                               v
┌──────────────────────────────────────────────────────────────────┐
│                         Rust Web 后端 put-webd                   │
│                                                                  │
│  status_snapshot.rs        log_reader.rs        system_reader.rs │
│  ├── read_modules()        ├── read_logs()      ├── /proc        │
│  ├── read_ipc_status()     ├── source 白名单     ├── /sys        │
│  ├── read_route_status()   ├── level / keyword  └── /dev         │
│  └── read_events()         └── 分页游标                          │
│                                                                  │
│  REST API，只读 GET                                              │
│  ├── /api/health                                                 │
│  ├── /api/modules                                                │
│  ├── /api/resources                                              │
│  ├── /api/ipc-status                                             │
│  ├── /api/route-status                                           │
│  ├── /api/events                                                 │
│  └── /api/logs                                                   │
└──────────────────────────────────────────────────────────────────┘
```

---

## 3. 快照文件接口（linux_app -> Web 后端）

### 3.1 总体约定

| 项目       | 约定                                                                   |
| ---------- | ---------------------------------------------------------------------- |
| 根目录     | `/run/put/status/`                                                     |
| 固定快照   | `modules.json`、`ipc_status.json`、`route_status.json`、`events.jsonl` |
| 文件格式   | UTF-8 JSON（`.json`）或 JSON Lines（`.jsonl`）                         |
| 写入方式   | 先写临时文件，再 `rename(2)` 原子替换正式文件                          |
| 时间戳字段 | 每个 `.json` 快照必须包含 `updated_at_ms`（毫秒级时间戳）              |
| 时间戳基准 | 推荐使用系统启动后的单调时间，也可使用 UTC 绝对毫秒时间戳              |
| 过期阈值   | 默认 `5000` ms，可通过 `web_config.toml` 的 `snapshot_stale_ms` 配置   |
| 文件缺失   | Web 返回 `"unknown"` 状态，HTTP 200                                    |
| 文件过期   | Web 返回 `"stale"` 状态，HTTP 200，并保留可解析的最后内容              |
| JSON 损坏  | Web 返回 `"unknown"` 状态，HTTP 200，解析错误记入 `web.log`            |
| 字段缺失   | Web 后端使用默认值填充，不因单个字段缺失导致接口失败                   |

状态字段建议统一使用：

| 值          | 含义                         |
| ----------- | ---------------------------- |
| `"ok"`      | 快照有效且无明显异常         |
| `"warn"`    | 存在水位线、延迟、丢弃等警告 |
| `"error"`   | 存在明确错误                 |
| `"stale"`   | 快照超过过期阈值             |
| `"unknown"` | 快照缺失、损坏或数据不足     |

### 3.2 `modules.json` - 六类物理接口状态快照

**文件路径：** `/run/put/status/modules.json`

**用途：** 提供 CAN、Ethernet、Wi-Fi、Bluetooth、4G、RS485 六类物理接口的连通性、收发、分片重组和错误统计。CAN 是六类接口之一，不再单独定义独立状态快照。

**JSON 示例：**

```json
{
  "updated_at_ms": 12345678,
  "state": "ok",
  "modules": [
    {
      "name": "can",
      "status": "online",
      "rx_bytes": 4096,
      "tx_bytes": 8192,
      "rx_frames": 128,
      "tx_frames": 256,
      "decode_error_count": 0,
      "fragment_drop_count": 0,
      "reassemble_timeout_count": 0,
      "crc_error_count": 0,
      "send_fail_count": 0,
      "interface_offline_count": 0,
      "last_rx_ms": 12345000,
      "last_tx_ms": 12345500,
      "last_error": "none",
      "message": "interface active"
    }
  ]
}
```

**字段说明：**

| 字段                                 | 类型     | 必填 | 说明                                                  |
| ------------------------------------ | -------- | ---- | ----------------------------------------------------- |
| `updated_at_ms`                      | `u64`    | 是   | 快照生成时间戳                                        |
| `state`                              | `string` | 否   | 顶层状态汇总                                          |
| `modules`                            | `array`  | 是   | 六类物理接口状态列表，允许某些接口因硬件未启用而缺失  |
| `modules[].name`                     | `string` | 是   | `can`、`ethernet`、`wifi`、`bluetooth`、`4g`、`rs485` |
| `modules[].status`                   | `string` | 是   | `online`、`offline`、`stale`、`error`、`unknown`      |
| `modules[].rx_bytes`                 | `u64`    | 否   | 入口接收字节数                                        |
| `modules[].tx_bytes`                 | `u64`    | 否   | 出口发送字节数                                        |
| `modules[].rx_frames`                | `u64`    | 否   | 已重组并进入接入层处理的完整帧计数                    |
| `modules[].tx_frames`                | `u64`    | 否   | 已交给物理接口发送的完整帧计数                        |
| `modules[].decode_error_count`       | `u64`    | 否   | 物理协议解包错误                                      |
| `modules[].fragment_drop_count`      | `u64`    | 否   | 分片缺失、乱序或缓存淘汰导致的丢弃                    |
| `modules[].reassemble_timeout_count` | `u64`    | 否   | 分片重组超时计数                                      |
| `modules[].crc_error_count`          | `u64`    | 否   | 链路层或适配器层 CRC 错误计数                         |
| `modules[].send_fail_count`          | `u64`    | 否   | 真实物理发送失败计数                                  |
| `modules[].interface_offline_count`  | `u64`    | 否   | 接口离线或不可用计数                                  |
| `modules[].last_rx_ms`               | `u64`    | 否   | 最近一次接收时间戳                                    |
| `modules[].last_tx_ms`               | `u64`    | 否   | 最近一次发送时间戳                                    |
| `modules[].last_error`               | `string` | 否   | 最近一次错误描述，正常时为 `none`                     |
| `modules[].message`                  | `string` | 否   | 人类可读状态说明                                      |

**Web 后端行为：**

- 文件缺失时返回 `state = "unknown"`、`modules = []`。
- 快照过期时返回 `state = "stale"`，仍返回已解析的 `modules`。
- 单个接口缺字段时对应字段填 `0`、`false`、`""` 或 `"unknown"`。

### 3.3 `ipc_status.json` - 共享内存 v2 状态快照

**文件路径：** `/run/put/status/ipc_status.json`

**用途：** 提供大小核通信与共享内存 v2 的只读汇总状态，包括 Frame Pool、Descriptor Ring、Pending Bitmap、Mailbox、水位线和回收闭环统计。

**JSON 示例：**

```json
{
  "updated_at_ms": 12345678,
  "state": "ok",
  "rtos_online": true,
  "heartbeat_ms": 1000,
  "frame_pool": {
    "capacity": 256,
    "used": 12,
    "high_watermark": 40,
    "full_count": 0,
    "allocated": 2048,
    "released": 2036,
    "pending_reclaim": 0,
    "leaked_suspect": 0
  },
  "rx_rings": [
    {
      "interface": "can",
      "capacity": 64,
      "used": 2,
      "high_watermark": 16,
      "full_count": 0
    }
  ],
  "tx_rings": [
    {
      "interface": "rs485",
      "capacity": 64,
      "used": 1,
      "high_watermark": 12,
      "full_count": 0
    }
  ],
  "pending_bitmap": {
    "rx": "0x01",
    "tx": "0x20"
  },
  "mailbox": {
    "rx_doorbell_count": 2048,
    "tx_doorbell_count": 2030,
    "notify_fail_count": 0,
    "periodic_drain_count": 4
  },
  "integrity": {
    "descriptor_crc_error_count": 0,
    "epoch_mismatch_count": 0,
    "cache_sync_error_count": 0
  },
  "reclaim": {
    "heartbeat_consumed": 80,
    "invalid_frame_reclaimed": 2,
    "no_route_reclaimed": 1,
    "ttl_expired_reclaimed": 0,
    "epoch_mismatch_reclaimed": 0,
    "reclaim_ring_used": 0,
    "reclaim_ack_count": 83
  }
}
```

**字段说明：**

| 字段                         | 类型     | 必填 | 说明                                     |
| ---------------------------- | -------- | ---- | ---------------------------------------- |
| `updated_at_ms`              | `u64`    | 是   | 快照生成时间戳                           |
| `state`                      | `string` | 否   | 共享内存总体状态                         |
| `rtos_online`                | `bool`   | 否   | 小核是否在线                             |
| `heartbeat_ms`               | `u64`    | 否   | 最近心跳间隔或周期                       |
| `frame_pool.capacity`        | `u64`    | 否   | Frame Pool 总帧数或容量单位              |
| `frame_pool.used`            | `u64`    | 否   | 当前占用量                               |
| `frame_pool.high_watermark`  | `u64`    | 否   | 启动以来最高占用                         |
| `frame_pool.full_count`      | `u64`    | 否   | Frame Pool 耗尽次数                      |
| `frame_pool.allocated`       | `u64`    | 否   | Linux 分配次数                           |
| `frame_pool.released`        | `u64`    | 否   | Linux 最终释放次数                       |
| `frame_pool.pending_reclaim` | `u64`    | 否   | 小核已消费但等待 Linux 回收的帧数        |
| `frame_pool.leaked_suspect`  | `u64`    | 否   | 疑似泄漏帧数                             |
| `rx_rings[]`                 | `array`  | 否   | 六类入口 RX Descriptor Ring 使用情况     |
| `tx_rings[]`                 | `array`  | 否   | 六类出口 TX Descriptor Ring 使用情况     |
| `pending_bitmap.rx`          | `string` | 否   | RX Pending Bitmap 的可读十六进制值       |
| `pending_bitmap.tx`          | `string` | 否   | TX Pending Bitmap 的可读十六进制值       |
| `mailbox.*`                  | `object` | 否   | Doorbell 通知、失败和周期 drain 统计     |
| `integrity.*`                | `object` | 否   | descriptor CRC、epoch、cache 同步错误    |
| `reclaim.*`                  | `object` | 否   | free/reclaim 闭环和 drop reason 回收统计 |

**Web 后端行为：**

- 文件缺失时返回 `rtos_online = false`、`state = "unknown"`。
- 快照过期时返回 `state = "stale"`。
- `used` 接近 `capacity` 或达到配置水位线时，前端应显示警告。
- `allocated - released - pending_reclaim` 长期增长时，前端应标记为疑似 Frame Pool 泄漏。

### 3.4 `route_status.json` - 小核路由与调度状态快照

**文件路径：** `/run/put/status/route_status.json`

**用途：** 提供 FreeRTOS 小核路由、CID 映射、priority 队列、丢弃原因、鉴权/完整性/重放失败和端到端延迟统计。

**JSON 示例：**

```json
{
  "updated_at_ms": 12345678,
  "state": "ok",
  "route_table": {
    "version": 3,
    "epoch": 12,
    "source": "compiled_config",
    "active_entries": 6
  },
  "priority_queues": [
    {
      "priority": 0,
      "queued": 0,
      "capacity": 16,
      "routed_frames": 102,
      "dropped_frames": 0,
      "max_latency_ms": 8
    }
  ],
  "cid_stats": {
    "routed_frames": 2048,
    "heartbeat_consumed": 80,
    "no_route": 1,
    "invalid_cid": 2,
    "reserved_cid": 0,
    "broadcast_frames": 0
  },
  "drop_reasons": {
    "invalid_length": 1,
    "invalid_type": 0,
    "ttl_expired": 0,
    "frame_pool_full": 0,
    "rx_ring_full": 0,
    "tx_ring_full": 0,
    "target_interface_offline": 0,
    "auth_failed": 0,
    "integrity_failed": 0,
    "replay_dropped": 0
  },
  "latency": {
    "rx_ring_to_tx_ring_max_ms": 12,
    "rx_ring_to_tx_ring_avg_ms": 2,
    "linux_egress_max_ms": 18,
    "end_to_end_max_ms": 30
  }
}
```

**字段说明：**

| 字段                  | 类型     | 必填 | 说明                                                |
| --------------------- | -------- | ---- | --------------------------------------------------- |
| `updated_at_ms`       | `u64`    | 是   | 快照生成时间戳                                      |
| `state`               | `string` | 否   | 路由调度总体状态                                    |
| `route_table.version` | `u64`    | 否   | 当前路由表版本                                      |
| `route_table.epoch`   | `u64`    | 否   | Linux 启动纪元或路由配置纪元                        |
| `route_table.source`  | `string` | 否   | `compiled_config`、`linux_init` 或 `shared_control` |
| `priority_queues[]`   | `array`  | 否   | priority 0~3 队列占用、水位和处理统计               |
| `cid_stats.*`         | `object` | 否   | CID 路由、心跳消费、非法/保留地址统计               |
| `drop_reasons.*`      | `object` | 否   | 所有丢弃必须有明确原因计数                          |
| `latency.*`           | `object` | 否   | 小核调度、Linux 出口和端到端延迟统计                |

**Web 后端行为：**

- 文件缺失时返回 `state = "unknown"`。
- 快照过期时返回 `state = "stale"`。
- `auth_failed`、`integrity_failed`、`replay_dropped` 非零时应在异常事件或总览中突出显示。
- `no_route`、`ttl_expired`、`tx_ring_full`、`frame_pool_full` 非零时应能追踪到具体 drop reason 计数。

### 3.5 `events.jsonl` - 异常事件日志

**文件路径：** `/run/put/status/events.jsonl`

**用途：** 以 JSON Lines 格式记录最近异常事件和关键状态变化，供 Web 展示告警。

**JSON 条目示例：**

```json
{"timestamp_ms":12345670,"level":"info","source":"web","message":"monitor started","detail":"readonly API serving"}
{"timestamp_ms":12345678,"level":"warn","source":"ipc","message":"frame pool high watermark","detail":"used=220 capacity=256"}
{"timestamp_ms":12345999,"level":"error","source":"router","message":"route drop","detail":"reason=no_route destination_cid=0x61000001"}
{"timestamp_ms":12346010,"level":"error","source":"adapter","message":"integrity failed","detail":"source=wifi reason=auth_failed"}
```

**字段说明：**

| 字段           | 类型     | 必填 | 说明                                                     |
| -------------- | -------- | ---- | -------------------------------------------------------- |
| `timestamp_ms` | `u64`    | 是   | 事件发生时间戳                                           |
| `level`        | `string` | 是   | `info`、`warn`、`error`                                  |
| `source`       | `string` | 是   | `web`、`linux_app`、`ipc`、`router`、`adapter`、`system` |
| `message`      | `string` | 是   | 简短事件描述                                             |
| `detail`       | `string` | 否   | 详细上下文，写入前必须避免泄露 token、密钥、完整网络配置 |

**事件来源建议：**

- `adapter`：接口上线/离线、分片重组失败、链路 CRC 错误、发送失败。
- `ipc`：Frame Pool 满、Ring 满、descriptor CRC 错误、Mailbox 通知失败、回收异常。
- `router`：无路由、TTL 过期、非法 CID、非法 type、priority 队列拥塞。
- `linux_app`：快照写入异常、配置加载异常、状态采集异常。
- `web`：快照解析失败、日志读取失败、静态目录缺失。

**Web 后端行为：**

- 文件缺失时返回 `events = []`、`parse_error_count = 0`。
- 损坏行会被跳过并计入 `parse_error_count`。
- `limit` 默认 50，范围 `1~500`。

---

## 4. 日志文件接口

### 4.1 总体约定

| 项目       | 约定                                                                  |
| ---------- | --------------------------------------------------------------------- |
| 根目录     | `/var/log/put/`                                                       |
| 文件命名   | `{source}.log`                                                        |
| 默认日志源 | `linux_app`、`web`、`system`、`ipc`、`router`、`adapter`              |
| 文件编码   | UTF-8                                                                 |
| 行格式     | 自由文本，建议包含时间、等级、来源和简短消息                          |
| 安全约束   | `source` 仅允许白名单值，禁止路径遍历                                 |
| 脱敏要求   | 日志写入前不得暴露 token、密钥、完整 CID 凭据、完整网络配置和认证材料 |

### 4.2 日志源与文件名映射

| source 参数 | 文件名          | 说明                                         |
| ----------- | --------------- | -------------------------------------------- |
| `linux_app` | `linux_app.log` | 大核接入、出口、状态采集和快照写入日志       |
| `web`       | `web.log`       | `put-webd` 启动、API、快照读取、前端托管日志 |
| `system`    | `system.log`    | 系统启动、关机、资源异常日志                 |
| `ipc`       | `ipc.log`       | Frame Pool、Ring、Mailbox、回收闭环日志      |
| `router`    | `router.log`    | CID 路由、priority 调度、drop reason 日志    |
| `adapter`   | `adapter.log`   | 六类物理接口适配器日志                       |

### 4.3 查询参数

| 参数      | 默认        | 说明                               |
| --------- | ----------- | ---------------------------------- |
| `source`  | `linux_app` | 日志源白名单值                     |
| `level`   | 空          | 按等级过滤，不区分大小写子串匹配   |
| `keyword` | 空          | 按关键字搜索，不区分大小写子串匹配 |
| `cursor`  | 空          | 分页游标，从最新行开始反向分页     |
| `limit`   | `200`       | 返回行数，范围 `1~500`             |

**Web 后端行为：**

- 日志文件缺失时返回空列表，HTTP 200。
- 非法 `source` 返回 HTTP 400。
- 返回体包含 `next_cursor` 和 `has_more`。

---

## 5. Web REST API 契约

所有接口均为只读 `GET`。除参数错误外，快照缺失、过期或损坏不应导致 HTTP 5xx。

| API                     | 数据来源                | 说明                                                               |
| ----------------------- | ----------------------- | ------------------------------------------------------------------ |
| `GET /api/health`       | Web 后端自身            | 服务健康、版本和只读标志                                           |
| `GET /api/modules`      | `modules.json`          | 六类物理接口连通性、收发、分片重组和错误统计                       |
| `GET /api/resources`    | `/proc`、`/sys`、`/dev` | CPU、内存、磁盘、网络、运行时间和设备存在性                        |
| `GET /api/ipc-status`   | `ipc_status.json`       | Frame Pool、Descriptor Ring、Pending Bitmap、Mailbox、回收和水位线 |
| `GET /api/route-status` | `route_status.json`     | CID 路由、priority 队列、drop reason、鉴权/完整性/重放失败和延迟   |
| `GET /api/events`       | `events.jsonl`          | 最近异常事件                                                       |
| `GET /api/logs`         | `/var/log/put/*.log`    | 日志查询                                                           |

### 5.1 `GET /api/health`

返回示例：

```json
{
  "service": "put-webd",
  "status": "ok",
  "readonly": true,
  "version": "0.2.1",
  "architecture": "anymsg-v2"
}
```

### 5.2 `GET /api/modules`

快照缺失时返回示例：

```json
{
  "updated_at_ms": 0,
  "state": "unknown",
  "modules": []
}
```

### 5.3 `GET /api/ipc-status`

快照缺失时返回示例：

```json
{
  "updated_at_ms": 0,
  "state": "unknown",
  "rtos_online": false,
  "frame_pool": {
    "capacity": 0,
    "used": 0,
    "pending_reclaim": 0,
    "leaked_suspect": 0
  },
  "rx_rings": [],
  "tx_rings": []
}
```

### 5.4 `GET /api/route-status`

快照缺失时返回示例：

```json
{
  "updated_at_ms": 0,
  "state": "unknown",
  "route_table": {
    "version": 0,
    "epoch": 0,
    "source": "unknown",
    "active_entries": 0
  },
  "priority_queues": [],
  "drop_reasons": {}
}
```

### 5.5 `GET /api/events?limit=50`

参数：

| 参数    | 默认 | 说明                           |
| ------- | ---: | ------------------------------ |
| `limit` |   50 | 返回最近事件数量，范围 `1~500` |

### 5.6 `GET /api/logs`

示例：

```text
GET /api/logs?source=router&level=warn&keyword=no_route&cursor=&limit=200
```

---

## 6. 写入实现建议（对 linux_app 的要求）

### 6.1 原子写入快照

`linux_app` 写入 `/run/put/status/` 下 JSON 快照时，必须避免 Web 读到半截文件。推荐流程：

```c
int write_snapshot(const char *dir, const char *filename, const char *json) {
    char tmp_path[256];
    char final_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.%s.tmp", dir, filename);
    snprintf(final_path, sizeof(final_path), "%s/%s", dir, filename);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, json, strlen(json));
    fsync(fd);
    close(fd);

    rename(tmp_path, final_path);
    return 0;
}
```

### 6.2 写入周期建议

| 快照                | 建议写入周期 | 说明                                    |
| ------------------- | ------------ | --------------------------------------- |
| `modules.json`      | 1 秒         | 物理接口在线、收发、错误统计            |
| `ipc_status.json`   | 1 秒         | Frame Pool、Ring、Mailbox、回收闭环状态 |
| `route_status.json` | 1 秒         | 小核路由、队列、丢弃和延迟状态          |
| `events.jsonl`      | 事件驱动追加 | 有事件发生时立即追加                    |

### 6.3 时间戳选择

建议 `linux_app` 统一使用系统启动后的单调时间，避免系统时间跳变导致误判过期。Web 后端可按如下规则自适应：

- `updated_at_ms < 1000000000000`：按单调时间处理，与 `/proc/uptime` 比较。
- `updated_at_ms >= 1000000000000`：按 UTC 绝对毫秒时间处理。

### 6.4 脱敏要求

写入事件和日志前必须处理敏感字段：

- token、密钥、会话凭据只能输出摘要或固定占位符。
- CID 可按调试需要输出，但不应与鉴权材料、网络凭据同时完整出现。
- IP、APN、账号、密码、SIM 标识等网络配置应按生产策略脱敏。
- 鉴权失败、完整性失败、重放丢弃必须保留统计和原因，但不得输出可复用凭据。

---

## 7. 错误处理契约

| 场景                       | HTTP 状态码       | 响应内容                                         |
| -------------------------- | ----------------- | ------------------------------------------------ |
| 快照文件不存在             | 200               | 对应状态返回 `"unknown"` 或空列表                |
| 快照过期                   | 200               | 顶层 `state` 返回 `"stale"`，其余数据尽量保留    |
| 快照 JSON 损坏             | 200               | 返回 `"unknown"`，损坏记入 `web.log`             |
| 日志文件不存在             | 200               | `lines = []`、`has_more = false`                 |
| 非法日志 `source`          | 400               | `{"error":"invalid log source","readonly":true}` |
| `/proc` 或 `/sys` 读取失败 | 200               | 对应字段标记 `"unknown"`                         |
| 前端静态目录不存在         | 200（API 仍可用） | 根路径访问返回 404                               |

关键原则：快照异常不影响 Web 服务可用性；数据质量通过 `state`、计数和事件体现。

---

## 8. 配置接口（Web 后端 <- 部署配置）

推荐配置文件：`/etc/put/web_config.toml`

```toml
bind_addr = "0.0.0.0:8080"
static_dir = "/opt/put/web/dist"
status_dir = "/run/put/status"
log_dir = "/var/log/put"
readonly = true
snapshot_stale_ms = 5000
log_sources = ["linux_app", "web", "system", "ipc", "router", "adapter"]
```

| 配置项              | 默认值                | 说明                                                   |
| ------------------- | --------------------- | ------------------------------------------------------ |
| `bind_addr`         | `"0.0.0.0:8080"`      | Web 服务监听地址；生产部署必须限制在可信网络或防火墙后 |
| `static_dir`        | `"/opt/put/web/dist"` | Vue3 前端静态文件目录                                  |
| `status_dir`        | `"/run/put/status"`   | 快照文件目录                                           |
| `log_dir`           | `"/var/log/put"`      | 日志文件目录                                           |
| `readonly`          | `true`                | 只读标志，目标 v2 固定为 `true`                        |
| `snapshot_stale_ms` | `5000`                | 快照过期阈值                                           |
| `log_sources`       | 见示例                | 日志源白名单                                           |

---

## 9. 安全与部署边界

Web 只读不等于公网安全。目标 v2 阶段默认用于现场调试、比赛演示和可信局域网运维查看。

部署要求：

- 默认不提供公网访问，不建议直接暴露到互联网。
- `bind_addr = "0.0.0.0:8080"` 只能用于受控局域网、iptables、防火墙或上级路由限制后的环境。
- 生产环境建议绑定到设备管理网地址，或仅监听 `127.0.0.1` 后由受控代理暴露。
- `/run/put/status/` 建议由 `linux_app` 写、Web 只读，目录权限不应允许普通用户篡改快照。
- `/var/log/put/` 日志必须按第 6.4 节脱敏，不得无意暴露鉴权材料。
- Web 后端不得提供 POST、PUT、DELETE 等写接口。

---

## 10. 职责边界重申

| 模块          | 负责                                                                                     | 不负责                                             |
| ------------- | ---------------------------------------------------------------------------------------- | -------------------------------------------------- |
| `linux_app`   | 真实物理收发、适配器解包/封包、分片重组、共享内存读写、状态快照和日志写入                | 提供 Web HTTP API、托管前端页面                    |
| Web 后端      | 读取快照和日志、读取系统资源、提供只读 REST API、托管前端静态文件                        | 解析外部协议、读写共享内存、控制小核、控制物理接口 |
| FreeRTOS 小核 | 共享内存 RX Ring drain、anyMSG 头部校验、心跳消费、CID 路由、priority 调度、TX Ring 写入 | 直接向 Web 提供数据、真实物理接口收发              |
| 共享内存 IPC  | Frame Pool、Descriptor Ring、Pending Bitmap、Mailbox Doorbell、回收闭环                  | 解释业务 payload、向 Web 暴露可写控制面            |

Web 获取小核和共享内存状态的唯一路径：

```text
FreeRTOS 小核 / 共享内存统计
  ↓ linux_app 汇总
/run/put/status/*.json
  ↓ put-webd 只读
浏览器展示
```

---

## 11. 测试验收

### 11.1 快照正常场景

1. `linux_app` 写入 `modules.json`、`ipc_status.json`、`route_status.json` 和 `events.jsonl`。
2. `/api/modules` 返回六类物理接口状态，CAN 作为 `modules[].name = "can"` 展示。
3. `/api/ipc-status` 返回 Frame Pool、RX/TX Ring、Mailbox、reclaim 统计。
4. `/api/route-status` 返回路由表版本、priority 队列、drop reason 和延迟统计。
5. `/api/events` 返回最近事件列表。
6. `/api/logs?source=router` 返回路由日志行。

### 11.2 快照缺失和过期场景

1. 停止 `linux_app` 或删除 `/run/put/status/`。
2. Web API 返回 HTTP 200，状态为 `"unknown"` 或 `"stale"`。
3. 前端不白屏，不显示假在线。

### 11.3 共享内存和路由异常场景

1. Frame Pool 满、RX Ring 满、TX Ring 满时，对应水位和 full 计数可见。
2. 心跳帧、小核消费帧、无路由帧、TTL 过期帧、epoch 不匹配帧的回收统计可见。
3. 无路由、非法 CID、非法 type、长度非法、目标接口离线均有明确 drop reason。
4. 鉴权失败、完整性失败、重放丢弃可在 `/api/route-status` 或事件中看到统计。
5. 延迟统计能区分小核调度延迟、Linux 出口延迟和端到端最大延迟。

### 11.4 日志和安全场景

1. 非法日志 `source` 返回 HTTP 400。
2. 日志文件缺失返回空列表。
3. token、密钥、认证材料和完整网络凭据不会出现在 Web 日志接口中。
4. 可信局域网、iptables、防火墙或绑定地址限制在部署文档中明确可查。

---

## 12. 相关文档

| 文档                                         | 说明                                            |
| -------------------------------------------- | ----------------------------------------------- |
| `docs/设计文档/web模块设计.md`               | Web 模块详细设计方案                            |
| `docs/设计文档/整体架构设计.md`              | 项目整体架构、主链路和 Web 定位                 |
| `docs/设计文档/统一数据帧设计.md`            | anyMSG 帧结构、CID 地址段和 type 定义           |
| `docs/设计文档/共享内存 IPC 架构设计方案.md` | 共享内存 v2、Frame Pool 和 Descriptor Ring 设计 |
| `docs/接口文档/大小核共享内存IPC接口.md`     | 大小核共享内存接口文档                          |
| `web/README.md`                              | Web 模块开发与构建说明                          |

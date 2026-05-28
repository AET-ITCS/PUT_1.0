# Web 模块设计方案

适用架构：anyMSG v0.2.1 / 目标 v2 架构

修改时间：2026-05-26

文档定位：描述 Web 只读监控模块在目标 v2 架构中的职责、数据来源、API、页面、部署和测试方案。

---

## 1. 设计定位

Web 模块用于多协议 anyMSG 网关的现场调试、比赛演示和运维查看。它运行在 Milk-V Duo 256M 的大核 Linux 侧，只做只读状态展示、日志查看和异常事件展示。

目标 v2 主链路如下：

```text
外部设备
    ↓ 完整 anyMSG 放入物理协议载荷，必要时分片
Linux 接入适配器
    ↓ 解包 / 重组 / 校验完整 anyMSG
共享内存 RX Ring + Frame Pool
    ↓ Mailbox Doorbell
FreeRTOS 小核
    ↓ anyMSG 头部校验 / 心跳消费 / CID 路由 / priority 调度
共享内存 TX Ring + Frame Pool
    ↓ Mailbox Doorbell
Linux 出口适配器
    ↓ 封包 / 分片 / 真实发送
目标设备
```

Web 模块是旁路监控链路：

```text
linux_app 状态快照
系统资源 / 设备存在性 / 日志文件
    ↓
Rust Web 后端 put-webd
    ↓
Vue3 监控页面
```

核心原则：

- Web 只读展示，不提供控制小核、控制物理接口、修改路由或修改配置的接口。
- Web 不解析业务 payload，不构造、不校验、不修改 anyMSG。
- Web 不直接读取共享内存、Descriptor Ring、Frame Pool 或 Mailbox 寄存器。
- `linux_app` 负责真实通信、共享内存交互和状态产生；Web 后端只负责读取、聚合和展示。
- 第一版面向可信局域网使用，不设计登录、用户、权限和远程控制。

---

## 2. 技术路线

后端：

| 项目         | 方案                              |
| ------------ | --------------------------------- |
| 语言         | Rust                              |
| Web 框架     | Axum                              |
| 异步运行时   | Tokio                             |
| JSON 序列化  | Serde                             |
| HTTP 中间件  | tower-http                        |
| 日志         | tracing                           |
| 目标平台     | Milk-V Duo 256M 大核 Linux        |
| 交叉编译目标 | `riscv64gc-unknown-linux-musl`    |
| 发布形式     | `put-webd` 单个静态后端可执行文件 |

前端：

| 项目     | 方案                                              |
| -------- | ------------------------------------------------- |
| 框架     | Vue3                                              |
| 构建工具 | Vite                                              |
| 语言     | TypeScript 优先，普通 JavaScript 可作为第一版简化 |
| 发布形式 | 外置 `dist/` 静态文件                             |
| 部署方式 | Rust 后端托管 `dist/`                             |

完整 Web 部署包含：

```text
put-webd
web_config.toml
frontend dist/
```

---

## 3. 职责边界

### 3.1 linux_app 职责

`linux_app` 负责真实业务链路：

- 接入 CAN、Ethernet、Wi-Fi、Bluetooth、4G、RS485 六类物理接口。
- 在入口适配层解包、分片重组并校验完整 anyMSG。
- 将完整 anyMSG 写入 Frame Pool 和对应 RX Descriptor Ring。
- 读取小核写入的 TX Descriptor Ring，从 Frame Pool 取出完整 anyMSG。
- 在出口适配层封包、必要时分片并真实发送。
- 汇总接口、共享内存、小核路由、丢弃原因、延迟和安全统计。
- 周期写入 `/run/put/status/` 下的 JSON 快照。
- 写入 `/var/log/put/` 下的脱敏日志。

`linux_app` 不负责：

- 提供 HTTP API。
- 托管 Vue 页面。
- 处理浏览器请求。
- 绕过 Web 配置暴露未脱敏日志。

### 3.2 Web 后端职责

Rust 后端 `put-webd` 负责：

- 提供只读 REST API。
- 托管 Vue3 构建后的外置 `dist/`。
- 读取 `/run/put/status/` 下的业务状态快照。
- 读取 `/var/log/put/` 下的白名单日志文件。
- 直接读取 `/proc`、`/sys`、`/dev` 获取系统资源和设备存在性。
- 对缺失、过期、格式错误的快照返回 `unknown` 或 `stale` 状态。

Web 后端不负责：

- 重新解析外部协议或业务 payload。
- 读写共享内存、Ring、Frame Pool 或 Mailbox。
- 向小核或 `linux_app` 发送控制命令。
- 修改 `linux_app`、路由表或物理接口配置。
- 实现生产级认证授权系统。

### 3.3 Vue3 前端职责

Vue3 前端负责展示：

- 系统总览。
- 六类物理接口连通性、收发、错误和分片重组状态。
- CPU、内存、磁盘、网络、运行时间。
- 共享内存 v2、Frame Pool、Descriptor Ring、Mailbox 和回收闭环。
- 小核 CID 路由、priority 队列、drop reason 和延迟统计。
- 系统日志和异常事件。
- 快照缺失、过期、服务异常时的 `unknown`、`stale`、`no data` 状态。

---

## 4. 总体架构

```text
                         ┌──────────────────────────────┐
                         │          浏览器 / PC         │
                         │       Vue3 Web UI            │
                         └───────────────┬──────────────┘
                                         │ HTTP GET
                                         v
┌────────────────────────────────────────────────────────────────┐
│                     Rust Web 后端 put-webd                     │
│                                                                │
│  Axum Router                                                   │
│  ├── /api/health                                               │
│  ├── /api/modules                                              │
│  ├── /api/resources                                            │
│  ├── /api/ipc-status                                           │
│  ├── /api/route-status                                         │
│  ├── /api/events                                               │
│  ├── /api/logs                                                 │
│  └── Vue3 dist 静态文件                                        │
│                                                                │
│  数据读取                                                      │
│  ├── status_snapshot.rs  -> /run/put/status/*.json             │
│  ├── system_reader.rs    -> /proc /sys /dev                    │
│  └── log_reader.rs       -> /var/log/put/*.log                 │
└────────────────────────────────────────────────────────────────┘
                         ^
                         │ 只读 JSON 快照和日志
                         │
┌────────────────────────┴────────────────────────┐
│                 linux_app 大核程序              │
│  接入适配 / 出口适配 / 共享内存 / 状态 / 日志   │
│  └── 写入 /run/put/status/ 和 /var/log/put/     │
└────────────────────────┬────────────────────────┘
                         │
                         v
┌────────────────────────────────────────────────┐
│        共享内存 v2 + FreeRTOS 小核路由调度     │
│ Frame Pool / Descriptor Ring / Pending Bitmap  │
└────────────────────────────────────────────────┘
```

---

## 5. 推荐目录结构

```text
web/
├── README.md
├── backend/
│   ├── Cargo.toml
│   └── src/
│       ├── main.rs              # put-webd 入口、配置加载、Axum Router
│       ├── api.rs               # REST API 路由与响应结构
│       ├── config.rs            # web_config.toml 解析
│       ├── status_snapshot.rs   # /run/put/status 快照读取
│       ├── system_reader.rs     # /proc /sys /dev 读取
│       ├── log_reader.rs        # /var/log/put 日志分页读取
│       └── error.rs             # 错误类型和 HTTP 映射
│
├── frontend/
│   ├── package.json
│   ├── index.html
│   ├── src/
│   │   ├── main.ts
│   │   ├── App.vue
│   │   ├── api/
│   │   ├── views/
│   │   └── components/
│   └── dist/
│
└── config/
    └── web_config.toml
```

---

## 6. 数据来源设计

### 6.1 Web 后端直接读取的数据

| 数据                      | 来源                              |
| ------------------------- | --------------------------------- |
| CPU 使用率                | `/proc/stat`                      |
| 内存占用                  | `/proc/meminfo`                   |
| 系统运行时间              | `/proc/uptime`                    |
| 网络接口和流量            | `/proc/net/dev`、`/sys/class/net` |
| 磁盘空间                  | `statvfs` 或 `/proc/mounts`       |
| USB、串口、网络设备存在性 | `/dev`、`/sys`                    |
| Web 后端日志              | `/var/log/put/web.log`            |

### 6.2 linux_app 写入的业务快照

固定快照文件：

```text
/run/put/status/modules.json
/run/put/status/ipc_status.json
/run/put/status/route_status.json
/run/put/status/events.jsonl
```

写入策略：

- `linux_app` 周期写临时文件，再原子 rename 成正式文件。
- 每个 `.json` 快照包含 `updated_at_ms`。
- 快照缺失时 Web 返回 `unknown`。
- 快照超过阈值未更新时 Web 返回 `stale`。
- 所有小核和共享内存状态都必须先由 `linux_app` 汇总成快照，Web 不直接访问底层共享内存。

### 6.3 接口模块快照示例

```json
{
  "updated_at_ms": 12345678,
  "state": "ok",
  "modules": [
    {
      "name": "ethernet",
      "status": "online",
      "rx_bytes": 1048576,
      "tx_bytes": 524288,
      "rx_frames": 4096,
      "tx_frames": 2048,
      "decode_error_count": 0,
      "fragment_drop_count": 1,
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

模块名称固定为：

| 名称        | 含义        |
| ----------- | ----------- |
| `can`       | CAN 接口    |
| `ethernet`  | 以太网接口  |
| `wifi`      | Wi-Fi 接口  |
| `bluetooth` | 蓝牙接口    |
| `4g`        | 4G 蜂窝接口 |
| `rs485`     | RS485 接口  |

### 6.4 IPC 状态快照示例

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
  "rx_rings": [],
  "tx_rings": [],
  "pending_bitmap": {
    "rx": "0x00",
    "tx": "0x00"
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

### 6.5 路由状态快照示例

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

### 6.6 事件日志快照示例

`events.jsonl` 使用 JSON Lines，每行一个事件：

```json
{"timestamp_ms":12345678,"level":"warn","source":"ipc","message":"frame pool high watermark","detail":"used=220 capacity=256"}
{"timestamp_ms":12345999,"level":"error","source":"router","message":"route drop","detail":"reason=no_route destination_cid=0x61000001"}
```

---

## 7. 后端 API 设计

所有接口均为只读 `GET`。

### 7.1 `GET /api/health`

用途：检查 Web 后端自身是否运行。

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

### 7.2 `GET /api/modules`

用途：返回六类物理接口状态、收发、分片重组和错误统计。

数据来源：

```text
/run/put/status/modules.json
```

快照缺失时：

```json
{
  "updated_at_ms": 0,
  "state": "unknown",
  "modules": []
}
```

### 7.3 `GET /api/resources`

用途：返回 CPU、内存、磁盘、网络和运行时间。

数据来源：

```text
/proc/stat
/proc/meminfo
/proc/uptime
/proc/net/dev
/sys/class/net
```

该接口不依赖 `linux_app`。即使 `linux_app` 未启动，也应能正常返回系统资源。

### 7.4 `GET /api/ipc-status`

用途：返回大小核通信和共享内存 v2 状态。

数据来源：

```text
/run/put/status/ipc_status.json
```

重点展示：

- 小核在线和心跳状态。
- Frame Pool 容量、当前占用、水位线、耗尽计数。
- RX/TX Descriptor Ring 容量、占用和满计数。
- Pending Bitmap 和 Mailbox Doorbell 统计。
- descriptor CRC、epoch、cache 同步错误。
- 回收闭环、pending reclaim、疑似泄漏和 drop reason 回收统计。

### 7.5 `GET /api/route-status`

用途：返回小核路由、CID、priority 队列、丢弃和延迟统计。

数据来源：

```text
/run/put/status/route_status.json
```

重点展示：

- 路由表版本、epoch、来源和有效条目数。
- priority 0~3 队列占用、容量、已路由帧数、丢弃帧数和最大延迟。
- CID 路由、心跳消费、无路由、非法 CID、保留 CID 统计。
- 非法长度、非法 type、TTL 过期、Frame Pool 满、Ring 满、目标接口离线。
- 鉴权失败、完整性失败、重放丢弃。
- 小核调度延迟、Linux 出口延迟和端到端最大延迟。

### 7.6 `GET /api/events?limit=50`

用途：返回最近异常事件。

参数：

| 参数    | 默认 | 说明                           |
| ------- | ---: | ------------------------------ |
| `limit` |   50 | 返回最近事件数量，范围 `1~500` |

数据来源：

```text
/run/put/status/events.jsonl
```

### 7.7 `GET /api/logs`

用途：查询系统日志。

参数：

| 参数      | 默认        | 说明                                                     |
| --------- | ----------- | -------------------------------------------------------- |
| `source`  | `linux_app` | `linux_app`、`web`、`system`、`ipc`、`router`、`adapter` |
| `level`   | 空          | 日志等级过滤                                             |
| `keyword` | 空          | 关键字过滤                                               |
| `cursor`  | 空          | 分页游标                                                 |
| `limit`   | `200`       | 返回行数上限，范围 `1~500`                               |

示例：

```text
GET /api/logs?source=router&level=warn&keyword=no_route&cursor=&limit=200
```

日志文件不存在时返回空列表，不视为后端错误。

---

## 8. 后端实现建议

### 8.1 Axum 路由

```text
/api/health        -> health_handler
/api/modules       -> modules_handler
/api/resources     -> resources_handler
/api/ipc-status    -> ipc_status_handler
/api/route-status  -> route_status_handler
/api/events        -> events_handler
/api/logs          -> logs_handler
/*path              -> Vue3 dist 静态资源
```

### 8.2 配置文件

推荐路径：

```text
/etc/put/web_config.toml
```

示例：

```toml
bind_addr = "0.0.0.0:8080"
static_dir = "/opt/put/web/dist"
status_dir = "/run/put/status"
log_dir = "/var/log/put"
readonly = true
snapshot_stale_ms = 5000
log_sources = ["linux_app", "web", "system", "ipc", "router", "adapter"]
```

默认值：

| 配置                | 默认值                                         |
| ------------------- | ---------------------------------------------- |
| `bind_addr`         | `0.0.0.0:8080`                                 |
| `static_dir`        | `/opt/put/web/dist`                            |
| `status_dir`        | `/run/put/status`                              |
| `log_dir`           | `/var/log/put`                                 |
| `readonly`          | `true`                                         |
| `snapshot_stale_ms` | `5000`                                         |
| `log_sources`       | `linux_app, web, system, ipc, router, adapter` |

`bind_addr = "0.0.0.0:8080"` 仅适用于可信局域网或已有防火墙限制的环境。

### 8.3 错误处理

| 场景               | API 行为                           |
| ------------------ | ---------------------------------- |
| 状态快照不存在     | 返回 `unknown`，HTTP 200           |
| 状态快照过期       | 返回 `stale`，HTTP 200             |
| 状态快照 JSON 损坏 | 返回 `unknown`，事件中记录解析错误 |
| 日志文件不存在     | 返回空日志列表，HTTP 200           |
| 非法日志源         | HTTP 400                           |
| `/proc` 读取失败   | 对应资源字段标记 `unknown`         |
| 配置文件不存在     | 使用默认配置启动                   |
| `dist/` 不存在     | API 仍可用，访问页面返回 404       |

### 8.4 日志

`put-webd` 使用 `tracing` 输出日志，建议同时支持：

- 标准输出，便于前台调试。
- `/var/log/put/web.log`，便于 Web 页面展示。

日志内容包括：

- 启动配置。
- API 参数错误。
- 快照读取错误。
- 静态目录缺失。
- JSON 解析错误。

日志不得输出 token、密钥、认证材料和完整网络凭据。

---

## 9. 前端页面设计

### 9.1 页面结构

建议页面：

```text
Vue3 App
├── 总览 Dashboard
├── 接口模块 Interfaces
├── 系统资源 Resources
├── IPC / 路由状态 IpcRouteStatus
├── 日志 Logs
└── 异常事件 Events
```

### 9.2 总览页面

展示：

- 系统总体状态。
- CAN、Ethernet、Wi-Fi、Bluetooth、4G、RS485 的在线状态。
- 小核在线状态、Frame Pool 使用率、Ring 水位线。
- 路由 drop reason 汇总和最近一次严重异常。
- CPU、内存、磁盘、网络简要指标。
- 最近 5 条异常事件。

### 9.3 接口模块页面

展示：

- 接口名称和在线状态。
- 接收/发送字节数。
- 接收/发送完整帧数。
- 解包错误、分片丢弃、重组超时、CRC 错误。
- 发送失败、接口离线次数。
- 最近接收、最近发送和最近错误。

### 9.4 系统资源页面

展示：

- CPU 使用率。
- 内存使用率。
- 磁盘空间。
- 网络接口状态和收发字节数。
- 系统运行时间。
- 关键设备节点是否存在。

### 9.5 IPC / 路由状态页面

展示 IPC：

- 小核心跳和在线状态。
- Frame Pool 容量、占用、水位线、full 计数。
- RX/TX Descriptor Ring 容量、占用、水位线、full 计数。
- Pending Bitmap、Mailbox Doorbell、周期 drain。
- descriptor CRC、epoch、cache 同步错误。
- allocated、released、pending reclaim、疑似泄漏和回收 ACK。

展示路由：

- 路由表版本、epoch、来源。
- priority 0~3 队列占用、容量、路由帧数、丢弃帧数。
- CID 路由统计。
- drop reason 计数。
- 鉴权失败、完整性失败、重放丢弃。
- 小核调度、Linux 出口和端到端延迟。

### 9.6 日志页面

功能：

- 按来源选择日志：`linux_app`、`web`、`system`、`ipc`、`router`、`adapter`。
- 按等级过滤。
- 关键字搜索。
- 分页加载。
- 日志文件缺失时显示空状态。

### 9.7 异常事件页面

展示：

- 最近异常事件列表。
- 事件等级。
- 来源模块。
- 时间戳。
- 简短描述和详情。
- 对安全相关事件进行醒目标记：鉴权失败、完整性失败、重放丢弃。

---

## 10. 刷新策略

第一版采用 HTTP 轮询，不使用 WebSocket 或 SSE。

推荐刷新周期：

| 数据                |              周期 |
| ------------------- | ----------------: |
| `/api/health`       |                5s |
| `/api/modules`      |                1s |
| `/api/resources`    |                2s |
| `/api/ipc-status`   |                1s |
| `/api/route-status` |                1s |
| `/api/events`       |                3s |
| `/api/logs`         | 用户主动刷新或 5s |

---

## 11. 构建与部署

### 11.1 后端构建

目标命令：

```bash
cargo build --release --target riscv64gc-unknown-linux-musl
```

输出：

```text
web/backend/target/riscv64gc-unknown-linux-musl/release/put-webd
```

### 11.2 前端构建

目标命令：

```bash
npm install
npm run build
```

输出：

```text
web/frontend/dist/
```

### 11.3 目标板部署

推荐部署路径：

```text
/usr/local/bin/put-webd
/etc/put/web_config.toml
/opt/put/web/dist/
/run/put/status/
/var/log/put/
```

启动方式：

```bash
/usr/local/bin/put-webd --config /etc/put/web_config.toml
```

访问方式：

```text
http://<Milk-V-Duo-IP>:8080/
```

---

## 12. 安全边界

目标 v2 阶段只支持可信局域网只读访问。只读接口不等于公网安全。

要求：

- 不设计登录页面。
- 不设计用户、角色、权限。
- 不提供 POST、PUT、DELETE 等写接口。
- 不提供启停协议模块、重启小核、修改路由表、清空统计等控制功能。
- 生产部署必须通过绑定管理网地址、iptables、防火墙或上级路由限制访问范围。
- `/run/put/status/` 建议由 `linux_app` 写、Web 只读。
- `/var/log/put/` 必须避免暴露 token、密钥、认证材料和完整网络配置。

---

## 13. 测试计划

### 13.1 后端测试

- 配置文件不存在时使用默认配置启动。
- `/api/health` 能返回 `ok`、`readonly = true` 和 `architecture = "anymsg-v2"`。
- `linux_app` 未启动、快照目录不存在时，模块、IPC、路由状态返回 `unknown`。
- 快照文件过期时，状态返回 `stale`。
- 快照 JSON 损坏时，API 不崩溃。
- `/api/resources` 在没有 `linux_app` 的情况下仍能返回系统资源。
- 日志文件不存在时，`/api/logs` 返回空列表。
- 非法日志源返回 HTTP 400。

### 13.2 前端测试

- 首屏能打开总览页面。
- 接口模块缺失时显示空状态，不显示假在线。
- 资源页面能显示 CPU、内存、磁盘、网络。
- IPC / 路由页面能展示 Frame Pool、Ring、Mailbox、回收闭环、CID 和 drop reason。
- 日志页面支持来源、等级、关键字和分页。
- 后端 API 返回 `unknown` 或 `stale` 时页面有明确提示。

### 13.3 集成测试

- `linux_app` 写入 `modules.json` 后，页面能在刷新周期内更新六类接口状态。
- Frame Pool 满、Ring 满、descriptor CRC 错误经 `ipc_status.json` 暴露后，页面能展示对应警告。
- 无路由、TTL 过期、非法 CID、鉴权失败、完整性失败、重放丢弃经 `route_status.json` 暴露后，页面能展示对应 drop reason。
- Web 轮询不会影响六类物理接口到共享内存再到小核路由的主链路。
- Web 不读取共享内存，不向小核发送控制命令。

### 13.4 构建验收

Rust 后端最终可通过以下命令产出单个静态后端可执行文件：

```bash
cargo build --release --target riscv64gc-unknown-linux-musl
```

Vue3 前端最终可通过以下命令产出外置 `dist/`：

```bash
npm run build
```

---

## 14. 后续扩展

后续可以考虑：

- 增加登录和 token 访问控制。
- 增加 SSE 或 WebSocket，降低轮询开销。
- 增加日志下载。
- 增加演示模式数据源。
- 增加只读配置查看页面。
- 增加系统自检报告导出。

这些扩展不改变 v2 的基本原则：Web 只读展示，控制链路仍由 `linux_app` 和 FreeRTOS 小核负责。

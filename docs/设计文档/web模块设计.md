# Web 模块设计方案

## 1. 设计定位

Web 模块用于多协议统一终端的现场调试、比赛演示和后续运维查看。它运行在 Milk-V Duo 256M 的大核 Linux 侧，只做状态展示和日志查看，不参与外部协议解析、统一帧封装、CAN 实时转发或小核控制。

系统主线仍然保持：

```text
外部协议输入
    ↓
linux_app 协议解析 / unified_frame_t 封装
    ↓
大小核通信
    ↓
FreeRTOS 小核实时 CAN 转发
```

Web 模块是旁路监控链路：

```text
linux_app 业务状态快照
系统资源 / 设备存在性 / 日志文件
    ↓
Rust Web 后端 put-webd
    ↓
Vue3 监控页面
```

核心原则：

- Web 只读展示，不提供控制 CAN、控制小核、修改协议配置的接口。
- Web 不解析 4G、WiFi、蓝牙、以太网、RS485 的业务协议。
- Web 不读取统一帧共享内存 ring，也不直接访问 FreeRTOS 小核。
- `linux_app` 负责真实通信和业务状态产生，Web 后端负责读取、聚合和展示。
- 第一版面向可信局域网使用，不设计登录、用户、权限和远程控制。

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

| 项目     | 方案                                                |
| -------- | --------------------------------------------------- |
| 框架     | Vue3                                                |
| 构建工具 | Vite                                                |
| 语言     | TypeScript 优先，普通 JavaScript 也可作为第一版简化 |
| 发布形式 | 外置 `dist/` 静态文件                               |
| 部署方式 | Rust 后端托管 `dist/`                               |

注意：“单个可执行程序”指 Rust 后端 `put-webd` 是单个静态二进制文件。完整 Web 部署仍包含：

```text
put-webd
web_config.toml
frontend dist/
```

## 3. 职责边界

### 3.1 linux_app 职责

`linux_app` 负责真实业务链路：

- 接入 4G、WiFi、蓝牙、以太网、RS485。
- 解析外部协议并封装 `unified_frame_t`。
- 通过大小核通信发送数据给 FreeRTOS。
- 接收小核回传的 CAN、IPC、错误统计。
- 维护协议业务状态、收发计数、错误计数、最近通信时间。
- 周期写入 `/run/put/status/` 下的 JSON 快照。
- 写入 `/var/log/put/` 下的应用日志。

`linux_app` 不负责：

- 提供 HTTP API。
- 托管 Vue 页面。
- 采集 CPU、内存、磁盘等系统资源。
- 处理浏览器请求。

### 3.2 Web 后端职责

Rust 后端 `put-webd` 负责：

- 提供只读 REST API。
- 托管 Vue3 构建后的外置 `dist/`。
- 读取 `/run/put/status/` 下的业务状态快照。
- 直接读取 `/proc`、`/sys`、`/dev` 获取系统资源和设备存在性。
- 读取 `/var/log/put/` 下的日志文件。
- 对缺失、过期、格式错误的快照返回 `unknown` 或 `offline` 状态。

Web 后端不负责：

- 重新解析外部协议。
- 读取或写入大小核共享 ring。
- 向小核发送命令。
- 修改 `linux_app` 配置。
- 实现用户登录和权限系统。

### 3.3 Vue3 前端职责

Vue3 前端负责：

- 展示系统总览。
- 展示各协议模块连通性和收发统计。
- 展示 CPU、内存、磁盘、网络、运行时间。
- 展示 CAN 和大小核通信状态。
- 展示系统日志和异常事件。
- 在快照缺失或服务异常时显示明确的 `unknown`、`offline`、`no data` 状态。

## 4. 总体架构

```text
                         ┌──────────────────────────────┐
                         │          浏览器 / PC         │
                         │       Vue3 Web UI            │
                         └───────────────┬──────────────┘
                                         │ HTTP
                                         v
┌────────────────────────────────────────────────────────────────┐
│                     Rust Web 后端 put-webd                     │
│                                                                │
│  Axum Router                                                   │
│  ├── /api/health                                               │
│  ├── /api/modules                                              │
│  ├── /api/resources                                            │
│  ├── /api/can-status                                           │
│  ├── /api/ipc-status                                           │
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
                         │ 只读 JSON 快照
                         │
┌────────────────────────┴────────────────────────┐
│                 linux_app 大核程序              │
│  协议接入 / 解析 / 统一帧封装 / IPC / 日志      │
│  └── 写入 /run/put/status/ 和 /var/log/put/     │
└────────────────────────┬────────────────────────┘
                         │
                         v
                  FreeRTOS 小核 CAN 转发
```

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
│   └── dist/                    # npm run build 输出，部署时外置
│
└── config/
    └── web_config.toml
```

第一版实现时可以把后端模块数量简化，但文档层面建议保持上述职责划分，避免后续把系统资源读取、日志读取、状态快照读取混在一个文件里。

## 6. 数据来源设计

### 6.1 Web 后端直接读取的数据

这类数据属于系统事实，不需要 `linux_app` 参与：

| 数据                    | 来源                              |
| ----------------------- | --------------------------------- |
| CPU 使用率              | `/proc/stat`                      |
| 内存占用                | `/proc/meminfo`                   |
| 系统运行时间            | `/proc/uptime`                    |
| 网络接口和流量          | `/proc/net/dev`、`/sys/class/net` |
| 磁盘空间                | `statvfs` 或 `/proc/mounts`       |
| USB/串口/网络设备存在性 | `/dev`、`/sys`                    |
| Web 后端日志            | `/var/log/put/web.log`            |
| 大核应用日志            | `/var/log/put/linux_app.log`      |

### 6.2 linux_app 写入的业务快照

这类数据只有 `linux_app` 真正知道，Web 不应该重新推断：

```text
/run/put/status/modules.json
/run/put/status/can_status.json
/run/put/status/ipc_status.json
/run/put/status/events.jsonl
```

推荐写入策略：

- `linux_app` 周期写临时文件，再原子 rename 成正式文件，避免 Web 读到半截 JSON。
- 每个快照包含 `updated_at_ms`，Web 根据时间判断是否过期。
- 快照缺失时 Web 返回 `unknown`。
- 快照超过阈值未更新时 Web 返回 `offline` 或 `stale`。

### 6.3 模块状态快照示例

```json
{
  "updated_at_ms": 12345678,
  "modules": [
    {
      "name": "rs485",
      "status": "online",
      "rx_count": 1024,
      "tx_count": 1000,
      "error_count": 2,
      "last_seen_ms": 12345000,
      "message": "CAN direct active"
    }
  ]
}
```

模块 `status` 建议取值：

| 状态      | 含义                   |
| --------- | ---------------------- |
| `online`  | 模块存在且最近通信正常 |
| `offline` | 模块不存在或连接断开   |
| `stale`   | 长时间没有新数据       |
| `error`   | 有明确错误             |
| `unknown` | 没有足够数据判断       |

### 6.4 CAN 状态快照示例

```json
{
  "updated_at_ms": 12345678,
  "bus_state": "normal",
  "tx_count": 2048,
  "rx_count": 512,
  "error_count": 3,
  "drop_count": 1,
  "last_error": "none"
}
```

### 6.5 IPC 状态快照示例

```json
{
  "updated_at_ms": 12345678,
  "online": true,
  "heartbeat_ms": 1000,
  "tx_ring_used": 4,
  "rx_ring_used": 1,
  "timeout_count": 0
}
```

### 6.6 事件日志快照示例

`events.jsonl` 使用 JSON Lines，每行一个事件：

```json
{"timestamp_ms":12345678,"level":"warn","source":"ipc","message":"rtos heartbeat delayed","detail":"last heartbeat 2500ms ago"}
{"timestamp_ms":12345999,"level":"error","source":"can","message":"can bus off","detail":"xl2515 reported bus-off"}
```

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
  "version": "0.1.0"
}
```

### 7.2 `GET /api/modules`

用途：返回 4G、WiFi、蓝牙、以太网、RS485 等协议模块状态。

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

### 7.4 `GET /api/can-status`

用途：返回 CAN 总线和小核回传统计。

数据来源：

```text
/run/put/status/can_status.json
```

### 7.5 `GET /api/ipc-status`

用途：返回大小核通信状态。

数据来源：

```text
/run/put/status/ipc_status.json
```

### 7.6 `GET /api/events?limit=50`

用途：返回最近异常事件。

参数：

| 参数    | 默认 | 说明             |
| ------- | ---: | ---------------- |
| `limit` |   50 | 返回最近事件数量 |

数据来源：

```text
/run/put/status/events.jsonl
```

### 7.7 `GET /api/logs`

用途：查询系统日志。

参数：

| 参数      | 默认        | 说明                              |
| --------- | ----------- | --------------------------------- |
| `source`  | `linux_app` | 日志来源，例如 `linux_app`、`web` |
| `level`   | 空          | 日志等级过滤                      |
| `keyword` | 空          | 关键字过滤                        |
| `cursor`  | 空          | 分页游标                          |
| `limit`   | `200`       | 返回行数上限                      |

示例：

```text
GET /api/logs?source=linux_app&level=info&keyword=rs485&cursor=&limit=200
```

日志文件不存在时返回空列表，不视为后端错误。

## 8. 后端实现建议

### 8.1 Axum 路由

```text
/api/health       -> health_handler
/api/modules      -> modules_handler
/api/resources    -> resources_handler
/api/can-status   -> can_status_handler
/api/ipc-status   -> ipc_status_handler
/api/events       -> events_handler
/api/logs         -> logs_handler
/*path             -> Vue3 dist 静态资源
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
```

默认值：

| 配置                | 默认值              |
| ------------------- | ------------------- |
| `bind_addr`         | `0.0.0.0:8080`      |
| `static_dir`        | `/opt/put/web/dist` |
| `status_dir`        | `/run/put/status`   |
| `log_dir`           | `/var/log/put`      |
| `readonly`          | `true`              |
| `snapshot_stale_ms` | `5000`              |

### 8.3 错误处理

| 场景               | API 行为                            |
| ------------------ | ----------------------------------- |
| 状态快照不存在     | 返回 `unknown`，HTTP 200            |
| 状态快照过期       | 返回 `stale` 或 `offline`，HTTP 200 |
| 状态快照 JSON 损坏 | 返回 `unknown`，事件中记录解析错误  |
| 日志文件不存在     | 返回空日志列表，HTTP 200            |
| `/proc` 读取失败   | 对应资源字段标记 `unknown`          |
| 配置文件不存在     | 使用默认配置启动                    |
| `dist/` 不存在     | API 仍可用，访问页面返回 404        |

### 8.4 日志

`put-webd` 使用 `tracing` 输出日志，建议同时支持：

- 标准输出，便于前台调试。
- `/var/log/put/web.log`，便于 Web 页面展示。

日志内容包括：

- 启动配置。
- API 访问错误。
- 快照读取错误。
- 静态目录缺失。
- JSON 解析错误。

## 9. 前端页面设计

### 9.1 页面结构

建议页面：

```text
Vue3 App
├── 总览 Dashboard
├── 模块状态 Modules
├── 系统资源 Resources
├── CAN / IPC 状态 BusStatus
├── 日志 Logs
└── 异常事件 Events
```

### 9.2 总览页面

展示：

- 系统总体状态。
- 4G、WiFi、蓝牙、以太网、RS485、CAN、IPC 的在线状态。
- CPU、内存、磁盘、网络简要指标。
- 最近 5 条异常事件。
- 最近一次日志错误。

### 9.3 模块状态页面

展示：

- 模块名称。
- 在线状态。
- 收发计数。
- 错误计数。
- 最近通信时间。
- 最近错误描述。

### 9.4 系统资源页面

展示：

- CPU 使用率。
- 内存使用率。
- 磁盘空间。
- 网络接口状态和收发字节数。
- 系统运行时间。

### 9.5 CAN / IPC 状态页面

展示：

- CAN bus 状态。
- CAN TX/RX 计数。
- drop/error 计数。
- 大小核心跳。
- ring 使用情况。
- IPC 超时次数。

### 9.6 日志页面

功能：

- 按来源选择日志。
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

## 10. 刷新策略

第一版采用 HTTP 轮询，不使用 WebSocket 或 SSE。

推荐刷新周期：

| 数据              |              周期 |
| ----------------- | ----------------: |
| `/api/health`     |                5s |
| `/api/modules`    |                1s |
| `/api/resources`  |                2s |
| `/api/can-status` |                1s |
| `/api/ipc-status` |                1s |
| `/api/events`     |                3s |
| `/api/logs`       | 用户主动刷新或 5s |

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

当前仓库已有 Rust 交叉编译环境说明，但 Musl 静态目标需要后续补齐工具链支持。文档设计以 `riscv64gc-unknown-linux-musl` 作为最终目标。

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

## 12. 安全边界

v1 阶段只支持可信局域网只读访问：

- 不设计登录页面。
- 不设计用户、角色、权限。
- 不提供 POST、PUT、DELETE 等写接口。
- 不提供启停协议模块、重启小核、修改 CAN bitrate、清空统计等控制功能。
- 如需限制访问，优先通过绑定内网地址、iptables、防火墙或上级路由器完成。

## 13. 测试计划

### 13.1 后端测试

- 配置文件不存在时使用默认配置启动。
- `/api/health` 能返回 `ok`。
- `linux_app` 未启动、快照目录不存在时，模块状态返回 `unknown`。
- 快照文件过期时，状态返回 `stale` 或 `offline`。
- 快照 JSON 损坏时，API 不崩溃。
- `/api/resources` 在没有 `linux_app` 的情况下仍能返回系统资源。
- 日志文件不存在时，`/api/logs` 返回空列表。

### 13.2 前端测试

- 首屏能打开总览页面。
- 模块状态缺失时显示空状态，不显示假在线。
- 资源页面能显示 CPU、内存、磁盘、网络。
- 日志页面支持来源、等级、关键字和分页。
- 后端 API 返回错误时页面有明确提示。

### 13.3 集成测试

- `linux_app` 写入 `/run/put/status/modules.json` 后，页面能在刷新周期内更新状态。
- 小核 CAN 错误经 `linux_app` 写入 `can_status.json` 后，页面能展示错误状态。
- Web 轮询不会影响 4G/WiFi/蓝牙/以太网/RS485 到 CAN 的主链路。
- Web 不读取共享 ring，不向小核发送控制命令。

### 13.4 构建验收

- Rust 后端最终可通过以下命令产出单个静态后端可执行文件：

```bash
cargo build --release --target riscv64gc-unknown-linux-musl
```

- Vue3 前端最终可通过以下命令产出外置 `dist/`：

```bash
npm run build
```

## 14. 后续扩展

后续可以考虑：

- 增加登录和 token 访问控制。
- 增加 SSE 或 WebSocket，降低轮询开销。
- 增加日志下载。
- 增加演示模式数据源。
- 增加只读配置查看页面。
- 增加系统自检报告导出。

这些扩展不影响 v1 的基本原则：Web 只读展示，控制链路仍由 `linux_app` 和 FreeRTOS 小核负责。

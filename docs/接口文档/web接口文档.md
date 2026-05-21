# Web 模块对外接口文档

> 本文档定义 Web 模块（`put-webd` + Vue3 前端）需要其余模块（主要是 `linux_app`）提供的接口与数据契约。

## 1. 文档定位

Web 模块是旁路监控链路，不参与主通信链路：

```text
外部协议输入 → linux_app 协议解析 / unified_frame_t 封装 → 大小核通信 → FreeRTOS 小核实时 CAN 转发
                                                                 ↓
                                                          Web 只读监控展示
```

Web 模块本身**不解析外部协议、不读取共享内存 ring、不直接访问 FreeRTOS 小核**。它依赖两类数据来源：

| 数据类别     | 提供方                              | 说明                                             |
| ------------ | ----------------------------------- | ------------------------------------------------ |
| 业务状态快照 | `linux_app` 写入 `/run/put/status/` | 协议模块连通性、收发统计、CAN/IPC 状态、异常事件 |
| 应用日志     | `linux_app` 写入 `/var/log/put/`    | 大核应用日志、协议日志等                         |
| 系统资源     | 内核 `/proc` `/sys` `/dev`          | Web 后端直接读取，**不需要其余模块提供**         |

本文档只定义前两类数据（即需要其余模块配合提供的接口），系统资源读取属于 Web 模块内部实现，不在本文档范围。

---

## 2. 总体数据流

```text
┌──────────────────────────────────────────────────────────────┐
│                    linux_app 大核程序                        │
│                                                              │
│  status_collector.c          log_manager.c                   │
│  ├── 采集各协议模块状态       ├── 统一日志写入               │
│  ├── 采集 CAN 总线状态        ├── 按 source 分文件           │
│  ├── 采集 IPC 通信状态        └── 包含时间戳和等级           │
│  ├── 记录异常事件                                            │
│  └── 原子写入快照文件                                        │
└────────────┬────────────────────┬────────────────────────────┘
             │ 写入               │ 写入
             v                    v
┌────────────────────────┐  ┌────────────────────┐
│ /run/put/status/       │  │ /var/log/put/       │
│ ├── modules.json       │  │ ├── linux_app.log   │
│ ├── can_status.json    │  │ ├── web.log         │
│ ├── ipc_status.json    │  │ ├── system.log      │
│ └── events.jsonl       │  │ └── can.log         │
└───────────┬────────────┘  └──────────┬─────────┘
            │ 只读                      │ 只读
            v                           v
┌─────────────────────────────────────────────────────────────┐
│                     Rust Web 后端 put-webd                  │
│                                                             │
│  status_snapshot.rs         log_reader.rs                   │
│  ├── read_modules()         ├── read_logs()                 │
│  ├── read_can_status()      ├── 按 source 路由              │
│  ├── read_ipc_status()      ├── 按 level 过滤               │
│  └── read_events()          ├── 按 keyword 搜索             │
│                              └── 分页游标                   │
│                                                             │
│  REST API (只读 GET)                                        │
│  ├── /api/health                                            │
│  ├── /api/modules                                           │
│  ├── /api/resources                                         │
│  ├── /api/can-status                                        │
│  ├── /api/ipc-status                                        │
│  ├── /api/events                                            │
│  └── /api/logs                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 快照文件接口（linux_app → Web 后端）

### 3.1 总体约定

| 项目       | 约定                                                                                                                           |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------ |
| 根目录     | `/run/put/status/`                                                                                                             |
| 文件格式   | UTF-8 JSON（`.json`）或 JSON Lines（`.jsonl`）                                                                                 |
| 写入方式   | 先写临时文件，再 `rename(2)` 原子替换正式文件                                                                                  |
| 时间戳字段 | 每个快照文件必须包含 `updated_at_ms`（毫秒级时间戳）                                                                           |
| 时间戳基准 | 建议使用系统启动后的单调时间（`/proc/uptime` 换算），也可使用 UTC 绝对毫秒时间戳（值 ≥ `1000000000000`），Web 后端会自适应检测 |
| 过期阈值   | 默认 `5000` ms，可通过 `web_config.toml` 中的 `snapshot_stale_ms` 配置                                                         |
| 文件缺失   | Web 返回 `"unknown"` 状态，HTTP 200                                                                                            |
| 文件过期   | Web 返回 `"stale"` 状态，HTTP 200                                                                                              |
| JSON 损坏  | Web 返回 `"unknown"` 状态，HTTP 200，解析错误记入后端日志                                                                      |
| 字段缺失   | 后端使用 `serde(default)` 填充零值或 `"unknown"`                                                                               |

### 3.2 `modules.json` — 协议模块状态快照

**文件路径：** `/run/put/status/modules.json`

**用途：** 提供 4G、WiFi、蓝牙、以太网、RS485 等协议模块的连通性和收发统计。

**JSON Schema：**

```json
{
  "updated_at_ms": 12345678,
  "state": "ok",
  "modules": [
    {
      "name": "4g",
      "status": "online",
      "rx_count": 1842,
      "tx_count": 1810,
      "error_count": 1,
      "last_seen_ms": 12345000,
      "message": "TCP uplink active"
    }
  ]
}
```

**字段说明：**

| 字段                     | 类型     | 必填   | 说明                                                                     |
| ------------------------ | -------- | ------ | ------------------------------------------------------------------------ |
| `updated_at_ms`          | `u64`    | **是** | 快照生成时间戳（ms），用于 Web 判断快照是否过期                          |
| `state`                  | `string` | 否     | 顶层状态汇总，缺失时 Web 根据 `updated_at_ms` 自动推断                   |
| `modules`                | `array`  | **是** | 协议模块列表，`linux_app` 未启动或无法采集时可为空数组                   |
| `modules[].name`         | `string` | **是** | 模块名称，建议取值：`"4g"` `"wifi"` `"bluetooth"` `"ethernet"` `"rs485"` |
| `modules[].status`       | `string` | **是** | 模块状态，取值见下方状态表                                               |
| `modules[].rx_count`     | `u64`    | 否     | 接收帧/包计数                                                            |
| `modules[].tx_count`     | `u64`    | 否     | 发送帧/包计数                                                            |
| `modules[].error_count`  | `u64`    | 否     | 错误计数                                                                 |
| `modules[].last_seen_ms` | `u64`    | 否     | 最近一次收到该模块数据的时间戳（ms）                                     |
| `modules[].message`      | `string` | 否     | 人类可读的状态描述                                                       |

**`status` 取值约定：**

| 值          | 含义                                  | 前端展示     |
| ----------- | ------------------------------------- | ------------ |
| `"online"`  | 模块存在且最近通信正常                | 绿色 / good  |
| `"offline"` | 模块不存在或连接断开                  | 红色 / bad   |
| `"stale"`   | 长时间没有新数据                      | 黄色 / warn  |
| `"error"`   | 有明确错误（如 CRC 不匹配、协议异常） | 红色 / bad   |
| `"unknown"` | 没有足够数据判断                      | 灰色 / muted |

**Web 后端行为：**

- 文件缺失 → `state = "unknown"`, `modules = []`
- `updated_at_ms` 过期 → `state = "stale"`，但仍返回已有 `modules` 数据
- 单模块内字段缺失 → 对应字段填充 `0` 或 `""`

---

### 3.3 `can_status.json` — CAN 总线状态快照

**文件路径：** `/run/put/status/can_status.json`

**用途：** 提供 CAN 总线状态和小核回传的收发统计。

**JSON Schema：**

```json
{
  "updated_at_ms": 12345678,
  "bus_state": "normal",
  "tx_count": 2048,
  "rx_count": 512,
  "error_count": 3,
  "drop_count": 1,
  "last_error": "last arbitration lost recovered"
}
```

**字段说明：**

| 字段            | 类型     | 必填   | 说明                                               |
| --------------- | -------- | ------ | -------------------------------------------------- |
| `updated_at_ms` | `u64`    | **是** | 快照生成时间戳（ms）                               |
| `bus_state`     | `string` | 否     | CAN 总线状态，建议取值见下表，缺失返回 `"unknown"` |
| `tx_count`      | `u64`    | 否     | CAN 发送帧计数                                     |
| `rx_count`      | `u64`    | 否     | CAN 接收帧计数                                     |
| `error_count`   | `u64`    | 否     | CAN 错误计数（含仲裁丢失、ACK 错误、位错误等）     |
| `drop_count`    | `u64`    | 否     | 发送队列满导致的丢帧计数                           |
| `last_error`    | `string` | 否     | 最近一次错误的可读描述，正常时填 `"none"`          |

**`bus_state` 建议取值：**

| 值                | 含义                              |
| ----------------- | --------------------------------- |
| `"normal"`        | CAN 总线正常工作                  |
| `"bus-off"`       | CAN 控制器进入 bus-off 状态       |
| `"error-passive"` | CAN 控制器进入 error-passive 状态 |
| `"error-warning"` | CAN 控制器进入 error-warning 状态 |
| `"unknown"`       | 无法获取 CAN 状态                 |

**Web 后端行为：**

- 文件缺失 → 全部字段返回 `"unknown"` 或 `0`
- `updated_at_ms` 过期 → 顶层 `state` 返回 `"stale"`，但保留已上报数据

---

### 3.4 `ipc_status.json` — 大小核 IPC 通信状态快照

**文件路径：** `/run/put/status/ipc_status.json`

**用途：** 提供大核 Linux 与小核 RTOS 之间的共享内存通信状态。

**JSON Schema：**

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

**字段说明：**

| 字段            | 类型   | 必填   | 说明                                                          |
| --------------- | ------ | ------ | ------------------------------------------------------------- |
| `updated_at_ms` | `u64`  | **是** | 快照生成时间戳（ms）                                          |
| `online`        | `bool` | 否     | 小核是否在线（能否收到心跳）                                  |
| `heartbeat_ms`  | `u64`  | 否     | 心跳间隔（ms），或最近心跳耗时                                |
| `tx_ring_used`  | `u64`  | 否     | 大核→小核发送环形队列当前使用槽位数                           |
| `rx_ring_used`  | `u64`  | 否     | 小核→大核回传环形队列当前使用槽位数（如未实现回传通道则为 0） |
| `timeout_count` | `u64`  | 否     | 心跳超时累计次数                                              |

**Web 后端行为：**

- 文件缺失 → `online = false`, `state = "unknown"`, 其余字段填 `0`
- `updated_at_ms` 过期 → `state = "stale"`

---

### 3.5 `events.jsonl` — 异常事件日志

**文件路径：** `/run/put/status/events.jsonl`

**用途：** 以 JSON Lines 格式记录系统异常事件，供 Web 展示最近告警。

**格式：** 每行一条完整 JSON，行尾 `\n`。Web 从文件末尾反向读取，按 `limit` 参数截断。

**JSON 条目 Schema：**

```json
{"timestamp_ms":12345670,"level":"info","source":"web","message":"monitor started","detail":"put-webd serving Vue dist and read-only API"}
{"timestamp_ms":12345678,"level":"warn","source":"ipc","message":"rtos heartbeat delayed","detail":"last heartbeat arrived after 1500ms"}
{"timestamp_ms":12345999,"level":"error","source":"rs485","message":"can direct crc mismatch","detail":"frame dropped before unified_frame packing"}
```

**字段说明：**

| 字段           | 类型     | 必填   | 说明                                                               |
| -------------- | -------- | ------ | ------------------------------------------------------------------ |
| `timestamp_ms` | `u64`    | **是** | 事件发生时间戳（ms）                                               |
| `level`        | `string` | **是** | 事件等级，建议取值：`"info"` `"warn"` `"error"`                    |
| `source`       | `string` | **是** | 事件来源模块，例如 `"ipc"` `"can"` `"rs485"` `"web"` `"linux_app"` |
| `message`      | `string` | **是** | 简短事件描述（一行）                                               |
| `detail`       | `string` | 否     | 详细描述或上下文信息                                               |

**`level` 取值约定：**

| 值        | 含义                                        | 前端展示 |
| --------- | ------------------------------------------- | -------- |
| `"info"`  | 一般信息事件（如服务启动、模块上线）        | 默认     |
| `"warn"`  | 警告事件（如心跳延迟、队列接近满）          | 黄色     |
| `"error"` | 错误事件（如 CRC 错误、模块断连、总线关闭） | 红色     |

**写入建议：**

- 使用追加模式（`O_APPEND`）写入，避免覆盖已有事件。
- 写入每条事件后 `fsync` 或依赖行缓冲。
- 文件大小建议设置上限（如 1 MB），超过后轮转或截断旧内容。
- 空行会被 Web 忽略，损坏行会增加 `parse_error_count` 但不影响其余事件解析。

**Web 后端行为：**

- 文件缺失 → `events = []`, `parse_error_count = 0`
- 损坏行 → 跳过并在 `parse_error_count` 中计数，输出后端日志
- `limit` 参数 → 默认 50，范围 `1~500`

---

## 4. 日志文件接口（linux_app / 系统 → Web 后端）

### 4.1 总体约定

| 项目     | 约定                                                                     |
| -------- | ------------------------------------------------------------------------ |
| 根目录   | `/var/log/put/`                                                          |
| 文件命名 | `{source}.log`，例如 `linux_app.log`、`web.log`、`system.log`、`can.log` |
| 文件编码 | UTF-8                                                                    |
| 行格式   | 自由文本，建议 `[level] message` 或结构化格式                            |
| 安全约束 | `source` 参数仅允许字母数字、下划线、连字符，禁止路径遍历                |

### 4.2 日志源与文件名映射

| source 参数   | 文件名          | 说明                                                  |
| ------------- | --------------- | ----------------------------------------------------- |
| `"linux_app"` | `linux_app.log` | 大核协议转换层日志（协议收发、打包、IPC 发送等）      |
| `"web"`       | `web.log`       | `put-webd` 自身日志（启动、API 访问、快照读取错误等） |
| `"system"`    | `system.log`    | 系统级日志（启动、关机、资源异常等）                  |
| `"can"`       | `can.log`       | CAN 相关日志（总线状态变化、错误计数变化等）          |

### 4.3 推荐的日志行格式

不强制但建议的格式：

```text
[2025-01-15T10:30:00.123Z] [info] [linux_app] rs485 can_direct frame received, length=76
[2025-01-15T10:30:01.456Z] [warn] [can] tx ring usage at 75%
[2025-01-15T10:30:02.789Z] [error] [ipc] rtos heartbeat timeout, missed 3 heartbeats
```

`linux_app/log_manager.c` 负责统一写入。Web 后端的日志查询支持：

| 参数      | 说明                                   |
| --------- | -------------------------------------- |
| `source`  | 按日志源选择文件（默认 `linux_app`）   |
| `level`   | 按等级过滤（不区分大小写子串匹配）     |
| `keyword` | 按关键字搜索（不区分大小写子串匹配）   |
| `cursor`  | 分页游标（整数），从最新行开始反向分页 |
| `limit`   | 返回行数（默认 200，范围 `1~500`）     |

**Web 后端行为：**

- 日志文件缺失 → 返回空列表，HTTP 200，不是错误
- 非法的 `source` → HTTP 400
- 返回体包含 `next_cursor` 和 `has_more` 用于前端分页

---

## 5. 写入实现建议（对 linux_app 的要求）

### 5.1 原子写入快照

`linux_app` 写入 `/run/put/status/` 下 JSON 快照时，必须避免 Web 读到写到一半的文件。推荐做法：

```c
// 伪代码
int write_snapshot(const char *dir, const char *filename, const char *json) {
    char tmp_path[256];
    char final_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.%s.tmp", dir, filename);
    snprintf(final_path, sizeof(final_path), "%s/%s", dir, filename);

    // 1. 写临时文件
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, json, strlen(json));
    fsync(fd);
    close(fd);

    // 2. 原子替换
    rename(tmp_path, final_path);
    return 0;
}
```

### 5.2 写入周期建议

| 快照              | 建议写入周期 | 说明                             |
| ----------------- | ------------ | -------------------------------- |
| `modules.json`    | 1 秒         | 与前端轮询周期一致               |
| `can_status.json` | 1 秒         | CAN 状态变化较频繁               |
| `ipc_status.json` | 1 秒         | 心跳状态实时性要求高             |
| `events.jsonl`    | 事件驱动追加 | 有事件发生时立即追加，不轮询写入 |

### 5.3 时间戳选择

快照中的 `updated_at_ms` 支持两种基准，Web 后端自适应检测：

- **单调时间**（推荐）：从 `/proc/uptime` 获取系统运行秒数，转换为毫秒。值范围通常为 0 ~ 数亿。Web 后端通过 `(updated_at_ms < 1000000000000)` 识别此模式，并与 `/proc/uptime` 比较判断过期。
- **绝对时间**：UTC 毫秒时间戳，值 ≥ `1000000000000`。Web 后端通过 `SystemTime::now()` 获取当前时间进行比较。

两种模式可混用，Web 后端按阈值自动切换。建议 `linux_app` 统一使用单调时间，避免系统时间跳变导致误判过期。

### 5.4 异常事件写入

`events.jsonl` 建议由 `linux_app` 的 `status_collector.c` 或专门的事件模块负责，在以下场景触发写入：

- 协议模块上线/离线
- 收发错误（CRC 不匹配、帧格式错误）
- CAN 总线状态变化
- IPC 心跳超时
- 发送队列即将满或已满
- 小核回传错误

写入时注意：

- 每条事件为单行 JSON，以 `\n` 结尾。
- 不同事件源可写入同一文件（通过 `source` 字段区分）。
- 文件大小建议监控，超过上限（如 1 MB）后截断或轮转。

---

## 6. Web 后端的错误处理契约

为确保 Web 前端在快照异常时仍能给出明确反馈，Web 后端遵循以下错误处理约定：

| 场景                                        | HTTP 状态码       | 响应内容                                            |
| ------------------------------------------- | ----------------- | --------------------------------------------------- |
| 快照文件不存在                              | 200               | 对应字段返回 `"unknown"` 或空列表                   |
| 快照过期（超过 `snapshot_stale_ms` 未更新） | 200               | 顶层 `state` 返回 `"stale"`，其余数据保留           |
| 快照 JSON 损坏                              | 200               | 返回 `"unknown"`，损坏记入后端日志                  |
| 日志文件不存在                              | 200               | `lines = []`, `has_more = false`                    |
| 非法的日志 `source` 参数                    | 400               | `{"error": "invalid log source", "readonly": true}` |
| `/proc` 读取失败                            | 200               | 对应字段标记 `"unknown"`                            |
| `dist/` 静态目录不存在                      | 200（API 仍可用） | 前端访问根路径返回 404                              |

关键原则：**快照异常不影响 Web 服务可用性，Web 始终返回 HTTP 200（除参数错误外），通过状态字段区分数据质量。**

---

## 7. 配置接口（Web 后端 ← 部署配置）

此部分不属于"其余模块接口"，但为完整性列在这里。Web 后端通过 `/etc/put/web_config.toml` 配置：

```toml
bind_addr = "0.0.0.0:8080"
static_dir = "/opt/put/web/dist"
status_dir = "/run/put/status"
log_dir = "/var/log/put"
readonly = true
snapshot_stale_ms = 5000
log_sources = ["linux_app", "web", "system", "can"]
```

| 配置项              | 默认值                                  | 说明                                                                    |
| ------------------- | --------------------------------------- | ----------------------------------------------------------------------- |
| `bind_addr`         | `"0.0.0.0:8080"`                        | Web 服务监听地址                                                        |
| `static_dir`        | `"/opt/put/web/dist"`                   | Vue3 前端静态文件目录                                                   |
| `status_dir`        | `"/run/put/status"`                     | 快照文件目录，即本文档第 3 节定义的所有 `.json` / `.jsonl` 文件所在目录 |
| `log_dir`           | `"/var/log/put"`                        | 日志文件目录，即本文档第 4 节定义的所有 `.log` 文件所在目录             |
| `readonly`          | `true`                                  | 只读标志（v1 固定为 `true`）                                            |
| `snapshot_stale_ms` | `5000`                                  | 快照过期阈值（ms）                                                      |
| `log_sources`       | `["linux_app", "web", "system", "can"]` | 允许的日志源白名单                                                      |

如果 `linux_app` 使用不同的目录路径，需要同步修改此配置。

---

## 8. 职责边界重申

| 模块          | 负责                                                                                | 不负责                                          |
| ------------- | ----------------------------------------------------------------------------------- | ----------------------------------------------- |
| `linux_app`   | 写入 `/run/put/status/*.json`、`/run/put/status/events.jsonl`、`/var/log/put/*.log` | 提供 HTTP API、托管 Vue 页面、采集系统资源      |
| Web 后端      | 读取快照和日志、提供只读 REST API、托管前端静态文件、直接读取 `/proc` `/sys`        | 解析外部协议、读取共享内存 ring、向小核发送命令 |
| FreeRTOS 小核 | CAN 实时转发、回传 CAN 状态和错误计数（通过共享内存→`linux_app`→快照的间接路径）    | 直接向 Web 提供数据                             |
| 共享内存模块  | 大核↔小核帧传输                                                                     | 向 Web 暴露共享内存或 ring 元数据               |

Web 模块获取小核数据的唯一路径是：

```text
小核 RTOS → 共享内存回传 → linux_app 汇总 → /run/put/status/*.json → Web 后端只读 → 浏览器
```

Web 模块**绝不**直接访问共享内存、ring queue 或小核寄存器。

---

## 9. 测试验收

### 9.1 快照正常场景

1. 启动 `linux_app`，确认 `/run/put/status/modules.json` 存在且 JSON 合法。
2. Web `/api/modules` 返回 `state = "ok"` 且模块列表非空。
3. Web `/api/can-status` 返回 `bus_state` 非 `"unknown"`。
4. Web `/api/ipc-status` 返回 `online = true`。
5. Web `/api/events` 返回最近事件列表。
6. Web `/api/logs?source=linux_app` 返回日志行。

### 9.2 快照缺失场景

1. 停止 `linux_app` 或删除 `/run/put/status/`。
2. Web `/api/modules` 返回 `state = "unknown"`, `modules = []`，HTTP 200。
3. Web `/api/can-status` 返回 `bus_state = "unknown"`，HTTP 200。
4. Web `/api/ipc-status` 返回 `online = false`，HTTP 200。
5. Web 不崩溃，前端不白屏。

### 9.3 快照过期场景

1. 停止 `linux_app` 写入超过 `snapshot_stale_ms`（默认 5 秒）。
2. Web `/api/modules` 返回 `state = "stale"`，但模块列表仍保留最后已知数据。

### 9.4 原子写入验证

1. Web 高频轮询时 `linux_app` 持续写入快照。
2. Web 不应读到截断的 JSON（不会出现解析错误激增）。

### 9.5 日志查询

1. `linux_app/log_manager.c` 向 `/var/log/put/linux_app.log` 写入带 `[error]` `[warn]` `[info]` 标记的行。
2. Web `/api/logs?source=linux_app&level=error` 仅返回包含 `error` 的行。
3. Web `/api/logs?source=linux_app&keyword=rs485` 仅返回包含 `rs485` 的行。
4. 分页游标正常工作。

---

## 10. 相关文档

| 文档                                     | 说明                               |
| ---------------------------------------- | ---------------------------------- |
| `docs/设计文档/web模块设计.md`           | Web 模块详细设计方案               |
| `docs/设计文档/整体架构设计.md`          | 项目整体架构（含 Web 定位）        |
| `docs/接口文档/统一协议帧格式.md`        | `unified_frame_t` 格式定义         |
| `docs/接口文档/大小核共享内存IPC接口.md` | 共享内存 IPC 接口预留              |
| `docs/接口文档/协议中间消息设计.md`      | `protocol_parsed_msg_t` 与解析流程 |
| `web/README.md`                          | Web 模块开发与构建说明             |

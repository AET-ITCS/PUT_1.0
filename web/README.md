# PUT Web 监控

`web/` 目录包含了用于多协议统一终端（PUT）的只读监控界面。

其后端服务是 `put-webd`，这是一个基于 Rust Axum/Tokio 框架构建的服务。它对外暴露只读的 REST API，负责从 `status_dir` 读取业务状态快照，从 `log_dir` 读取日志，从 `/proc`、`/sys` 和 `/dev` 读取系统资源与关键设备节点信息，并从外部的 `dist/` 目录提供 Vue3 前端静态资源服务。

前端界面是一个基于 Vue3/Vite/TypeScript 构建的单页应用（SPA），用于监控六类物理接口、系统资源、IPC/路由状态、各类事件以及系统日志。

## 本地开发

请在代码仓库的根目录下执行以下命令：

```bash
nix develop
npm --prefix web/frontend ci
npm --prefix web/frontend run build
cargo test --manifest-path web/backend/Cargo.toml
cargo run --manifest-path web/backend/Cargo.toml -- --config web/config/web_config.dev.toml
```

随后打开链接：

```text
http://127.0.0.1:8080/
```

开发环境的配置文件（`web_config.dev.toml`）指向的是 `web/mock_status` 和 `web/mock_logs` 目录（即模拟数据源）。而生产环境的配置文件则保留了实际的目标路径：

```text
/run/put/status
/var/log/put
/opt/put/web/dist
```

演示模式可通过额外启动参数进入：

```bash
cargo run --manifest-path web/backend/Cargo.toml -- --config web/config/web_config.dev.toml --demo-mode
```

`--demo-mode` 会启用固定的 `ethernet_to_can` 只读演示覆盖：REST API 仍保持现有路径和响应结构，但会强制展示 CAN、Ethernet、Wi-Fi、Bluetooth、RS485 成功接入，4G 未接入，并在事件中展示 Ethernet 接收、解包成 anyMSG、IPC 入队、路由到 CAN、CAN 封包发送的链路。物理接口的最近收发时间使用真实状态快照值，日志接口读取配置 `log_dir` 中的真实日志内容。该模式不会控制真实接口，也不会向小核或物理设备发送命令。

## API 接口

- `GET /api/health`
- `GET /api/modules`
- `GET /api/resources`
- `GET /api/ipc-status`
- `GET /api/route-status`
- `GET /api/events?limit=50`
- `GET /api/logs?source=router&level=&keyword=&cursor=&limit=200`

所有的 API 均为只读接口。如果对应的状态快照或日志文件缺失，接口将返回稳定的 `unknown`（未知）、`stale`（过期）或空响应，而不会执行任何对设备的控制操作。

`/api/health` 会返回 `mode` 和 `demo_scenario` 字段，用于区分普通模式与演示模式。`/api/modules` 使用目标 v2 字段模型展示 `can`、`ethernet`、`wifi`、`bluetooth`、`4g`、`rs485` 六类接口；CAN 不再提供独立状态快照。`/api/resources` 会返回 CPU、内存、磁盘、网络、运行时间以及 CAN、Ethernet、Wi-Fi、Bluetooth、4G、RS485、USB 等关键设备节点存在性。日志来源白名单为 `linux_app`、`web`、`system`、`ipc`、`router`、`adapter`。

## 目标构建

```bash
npm --prefix web/frontend run build
cargo build --manifest-path web/backend/Cargo.toml --release --target riscv64gc-unknown-linux-musl
```

部署：

```text
/usr/local/bin/put-webd
/etc/put/web_config.toml
/opt/put/web/dist/
```

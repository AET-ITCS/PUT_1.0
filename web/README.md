# PUT Web 监控

`web/` 目录包含了用于多协议统一终端（PUT）的只读监控界面。

其后端服务是 `put-webd`，这是一个基于 Rust Axum/Tokio 框架构建的服务。它对外暴露只读的 REST API，负责从 `status_dir` 读取业务状态快照，从 `log_dir` 读取日志，从 `/proc` 和 `/sys` 读取系统资源信息，并从外部的 `dist/` 目录提供 Vue3 前端静态资源服务。

前端界面是一个基于 Vue3/Vite/TypeScript 构建的单页应用（SPA），用于监控模块状态、系统资源、CAN/IPC 状态、各类事件以及系统日志。

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

## API 接口

- `GET /api/health`
- `GET /api/modules`
- `GET /api/resources`
- `GET /api/can-status`
- `GET /api/ipc-status`
- `GET /api/events?limit=50`
- `GET /api/logs?source=linux_app&level=&keyword=&cursor=&limit=200`

所有的 API 均为只读接口。如果对应的状态快照或日志文件缺失，接口将返回稳定的 `unknown`（未知）、`stale`（过期）或空响应，而不会执行任何对设备的控制操作。

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
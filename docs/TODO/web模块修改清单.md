# Web 模块修改清单

来源：
- `docs/接口文档/web接口文档.md`
- `docs/设计文档/web模块设计.md`
- 当前 `web/` 模块代码审查

日期：2026-05-26

审查结论：当前 Web 模块已基本完成目标 v2 改造，但尚未完全满足两份文档要求，因此本清单继续保留。剩余问题集中在资源接口设备节点展示、接口模块页面字段展示、以及 Web 日志落盘。

---

## P0 必须修改

### 1. 补齐 `/api/route-status`

- [x] 后端新增 `GET /api/route-status`。
- [x] 数据来源固定为 `status_dir/route_status.json`。
- [x] 缺失文件返回 `state = "unknown"`，HTTP 200。
- [x] 过期快照返回 `state = "stale"`，HTTP 200。
- [ ] JSON 损坏返回 `state = "unknown"`，并将解析错误记录到 `/var/log/put/web.log`。
- [x] 返回字段覆盖 `route_table`、`priority_queues`、`cid_stats`、`drop_reasons`、`latency`。
- [x] 前端新增路由状态读取和展示。
- [x] 轮询周期按设计文档设置为 1 秒。

说明：当前解析错误会通过 `tracing::warn!` 输出，但 `put-webd` 尚未配置写入 `/var/log/put/web.log`。

### 2. 移除或兼容废弃 `/api/can-status`

- [x] CAN 不再作为独立状态快照接口。
- [x] CAN 状态改为从 `/api/modules` 的 `modules[].name = "can"` 展示。
- [x] 前端移除对 `/api/can-status` 的强依赖。
- [x] `web/mock_status/can_status.json` 已删除。
- [x] `web/README.md` 删除 `/api/can-status`，补充 `/api/route-status`。

### 3. 更新 `modules.json` 字段模型

- [x] 后端 `ModuleStatus` 改为文档字段：`rx_bytes`、`tx_bytes`、`rx_frames`、`tx_frames`、`decode_error_count`、`fragment_drop_count`、`reassemble_timeout_count`、`crc_error_count`、`send_fail_count`、`interface_offline_count`、`last_rx_ms`、`last_tx_ms`、`last_error`、`message`。
- [x] 保留字段缺失默认值，不因单字段缺失导致接口失败。
- [ ] 前端模块页面展示解包错误 `decode_error_count`。
- [ ] 前端模块页面展示接口离线次数 `interface_offline_count`。
- [x] 前端模块页面展示收发字节、完整帧数、分片重组、CRC、发送失败、最近收发和最近错误。
- [x] 更新 `web/mock_status/modules.json` 为文档格式，并包含 `can`、`ethernet`、`wifi`、`bluetooth`、`4g`、`rs485` 示例。

### 4. 更新 `ipc_status.json` 字段模型

- [x] 后端字段改为 `rtos_online`，不再使用旧字段 `online`。
- [x] 补齐 `frame_pool`、`rx_rings`、`tx_rings`、`pending_bitmap`、`mailbox`、`integrity`、`reclaim`。
- [x] 缺失文件时返回 `rtos_online = false`、`state = "unknown"`。
- [x] 前端 IPC 页面展示 Frame Pool、Descriptor Ring、Mailbox、完整性错误和回收闭环统计。
- [x] 更新 `web/mock_status/ipc_status.json` 为文档格式。

---

## P1 应同步修改

### 5. 补齐 `/api/resources` 设备节点信息

- [x] 后端直接读取 `/proc/stat`、`/proc/meminfo`、`/proc/uptime`、`/proc/net/dev`、`/sys/class/net`、`statvfs`。
- [ ] 后端补充关键设备节点存在性，例如 `/dev` 或 `/sys` 中与 CAN、串口、USB、网络模块相关的只读探测结果。
- [ ] `/api/resources` 返回体增加设备节点状态字段，并在读取失败时标记 `unknown`。
- [ ] 前端资源页面展示关键设备节点是否存在。

说明：设计文档要求资源页展示“关键设备节点是否存在”，当前实现只覆盖 CPU、内存、运行时间、磁盘和网络。

### 6. 更新日志源白名单

- [x] 默认 `log_sources` 改为 `linux_app`、`web`、`system`、`ipc`、`router`、`adapter`。
- [x] 更新 `web/config/web_config.toml`。
- [x] 更新 `web/config/web_config.dev.toml`。
- [x] 前端日志来源选择项移除 `can`，新增 `ipc`、`router`、`adapter`。
- [x] 确认 `/api/logs?source=router` 返回路由日志而不是 HTTP 400。

### 7. 更新前端页面结构

- [x] 将当前 `CAN / IPC` 页面调整为 `IPC / 路由状态`。
- [x] 总览页展示六类接口状态，而不是单独突出 CAN。
- [x] 总览页展示小核在线、Frame Pool 使用率、Ring 水位和最近严重异常。
- [x] 路由页展示 route table、priority queues、CID stats、drop reasons、latency。
- [x] 安全相关事件 `auth_failed`、`integrity_failed`、`replay_dropped` 醒目标记。

### 8. 更新 `/api/health`

- [x] 返回体增加 `architecture = "anymsg-v2"`。
- [x] 保持 `service = "put-webd"`、`readonly = true`、`status = "ok"`。

---

## P2 文档、样例与测试

### 9. 更新 README 和 mock 数据

- [x] `web/README.md` API 列表与目标 v2 文档一致。
- [x] mock 日志补充 `ipc.log`、`router.log`、`adapter.log`。
- [x] mock events 的 `source` 使用文档白名单：`web`、`linux_app`、`ipc`、`router`、`adapter`、`system`。
- [x] 删除旧字段 `rx_count`、`tx_count`、`error_count`、`last_seen_ms`。

### 10. 补齐测试

- [x] 后端测试覆盖文档格式 `modules.json`。
- [x] 后端测试覆盖文档格式 `ipc_status.json`。
- [x] 后端测试覆盖 `route_status.json` 缺失、正常、过期、损坏场景。
- [x] 后端测试覆盖非法日志源返回 HTTP 400。
- [x] 前端构建脚本内的类型检查通过。
- [x] 前端构建通过。
- [ ] 增加 `/api/resources` 设备节点字段测试。
- [ ] 增加 Web 日志落盘或日志写入路径测试。

---

## 验证结果

本次审查已执行：

```bash
nix develop -c cargo test --manifest-path web/backend/Cargo.toml
nix develop -c npm --prefix web/frontend run build
```

结果：
- 后端测试通过：21 passed。
- 前端构建通过，构建脚本中的 `vue-tsc --noEmit` 已通过。

注意：以下旧命令在当前 npm/nix 组合下会从仓库根目录执行 `vue-tsc`，未读取 `web/frontend/tsconfig.json`，返回 TypeScript 帮助页和退出码 1：

```bash
nix develop -c npm --prefix web/frontend exec vue-tsc -- --noEmit
```

如需单独执行前端类型检查，建议使用构建脚本，或在 `web/frontend` 目录内执行：

```bash
nix develop -c npm --prefix web/frontend run build
```

如需运行后端开发服务：

```bash
nix develop -c cargo run --manifest-path web/backend/Cargo.toml -- --config web/config/web_config.dev.toml
```

---

## 验收标准

- [ ] `/api/health`、`/api/modules`、`/api/resources`、`/api/ipc-status`、`/api/route-status`、`/api/events`、`/api/logs` 全部符合目标 v2 文档。
- [x] Web 不提供 POST、PUT、DELETE 等写接口。
- [x] Web 不直接读取共享内存、Descriptor Ring、Frame Pool 或 Mailbox 寄存器。
- [x] CAN 作为六类物理接口之一展示，不再依赖独立 CAN 快照。
- [x] `router`、`ipc`、`adapter` 日志源可用。
- [x] 快照缺失、过期、损坏时 API 返回 HTTP 200，并用 `unknown` 或 `stale` 表示数据质量。
- [ ] 快照解析错误明确落盘到 `/var/log/put/web.log`。
- [ ] 资源接口和资源页面展示关键设备节点存在性。
- [ ] 接口模块页面完整展示文档要求的错误字段，包括解包错误和接口离线次数。

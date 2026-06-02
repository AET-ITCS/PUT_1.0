# FreeRTOS 小核实施计划

来源：

- `docs/设计文档/freeRTOS核设计.md`
- `docs/接口文档/大小核共享内存IPC接口.md`
- `docs/设计文档/共享内存 IPC 架构设计方案.md`
- `docs/设计文档/整体架构设计.md`
- `common/include/shared_memory_ipc.h`
- `rtos_firmware/ipc/`
- `linux_app/ipc/`

日期：2026-05-28  
正式目标目录：`rtos_firmware/`  
阶段策略：P0 固化已有 IPC 基线；P1 将路由核心落入 `rtos_firmware`；P2 接入真实 IPC Event / Router Scheduler / TX Writer 闭环；P3 补齐 Heartbeat、Error Monitor、Recovery、Statistics；P4 完成 Linux/RTOS host 闭环和板端联调。

---

## 0. 实施边界与现状

本计划用于指导 FreeRTOS 小核在 `rtos_firmware/` 中继续实现。当前目标不是重新设计共享内存 ABI，而是在已冻结的 IPC v2 ABI 和已有 Linux/RTOS IPC 底座之上，补齐小核路由、调度、心跳、Recovery、统计和联调闭环。

现状快照：

- 公共 ABI 已冻结在 `common/include/shared_memory_ipc.h`。
- RTOS 侧 IPC v2 底座已在 `rtos_firmware/ipc/rtos_shm_ipc.*` 实现，并已有 host 单测。
- Linux 侧 Frame Pool、RX/TX descriptor、reclaim 状态机已在 `linux_app/ipc/linux_shm_ipc.*` 实现，并已有 host 单测。
- `rtos_firmware/` 已有 BSP、CAN、watchdog 骨架和 `rtos_firmware_main()` 初始化入口。
- `freertos/router_core/` 只作为历史参考和迁移来源，不能作为正式小核实现目录或完成项。
- 待实现重点是 `rtos_firmware` 内的小核路由核心、IPC Event Task、Router Scheduler、TX Writer、Heartbeat、Error Monitor、Recovery、Statistics 和 Linux/RTOS 闭环测试。

固定 ABI 边界：

- `PUT_SHM_IPC_VERSION = 2`。
- region 固定 `64 KiB`。
- 物理接口固定 6 类：CAN、Ethernet、Wi-Fi、Bluetooth、4G、RS485。
- Frame Pool 固定 64 个 block，每个 block 512B。
- 每接口 RX/TX descriptor ring 深度固定 8。
- reclaim ring 深度固定 8。
- descriptor 和 reclaim descriptor 均固定 64B。
- `rx_pending_bitmap`、`tx_pending_bitmap`、`reclaim_pending` 分别独占一个 64B cache line。
- 设计文档中的 `reclaim/free ring` 在本实施计划中统一对应 ABI 的 `reclaim_ring` / `put_shm_reclaim_descriptor_t`，不得新建第二套 free ring。

职责边界：

- Linux 是 Frame Pool 唯一分配者和最终释放者。
- RTOS 不释放 Frame Pool block，不清零 payload，不修改 Linux free list。
- RTOS 只读 Frame Pool 中的完整 anyMSG，写目标 TX descriptor 或 reclaim descriptor。
- RTOS 路由层不得重实现 pending bitmap 原子操作、descriptor CRC、cache flush/invalidate、Doorbell 发布细节，这些继续由 `rtos_firmware/ipc` 层负责。
- IPC 层负责 descriptor ABI、CRC、Frame Pool 边界和接口一致性校验；路由层只处理已经达到可信 frame reference 的 route input。
- 坏 descriptor 如果未达到可信 frame reference，不由路由层读取 `frame_id` 或写 reclaim，交给 IPC 错误统计和 recovery/sweep 路径。

---

## 1. rtos_firmware 工程落位

目标：把正式小核实现集中到 `rtos_firmware/`，让后续代码、测试和 CMake 都围绕该目录演进。

建议新增或整理的模块边界：

- [x] `rtos_firmware/router/`：CID 路由、route table、route epoch、路由策略、drop reason 映射。
- [x] `rtos_firmware/queue/`：四级 priority 本地队列、队列水位线、驱逐策略。
- [x] `rtos_firmware/tasks/`：IPC Event、Router Scheduler、TX Writer、Heartbeat、Recovery、Statistics、Error Monitor 的 task 入口或 host mock 调度入口。
- [x] `rtos_firmware/mailbox/`：Mailbox ISR、Doorbell port、event wakeup 抽象。
- [x] `rtos_firmware/monitor/` 或并入 `tasks/`：端心跳表、Linux heartbeat、错误监控、统计快照。
- [x] 继续复用 `rtos_firmware/ipc/`，不得在路由层重写 descriptor ring、pending bitmap、CRC、cache 同步和 notify 发布。
- [x] 将新增正式模块接入 `rtos_firmware/CMakeLists.txt`，新增 host 单测统一放入 `rtos_firmware/test/`。

建议文件架构：

```text
rtos_firmware/
├── CMakeLists.txt
├── main.c                         # host smoke 入口；板端可替换为 RTOS 启动入口
├── include/
│   ├── rtos_firmware.h            # 固件顶层入口
│   └── rtos_firmware_config.h     # 小核公共配置开关和默认参数
├── src/
│   └── rtos_entry.c               # rtos_firmware_main() 初始化编排
├── bsp/
│   ├── rtos_bsp.h
│   ├── board.c                    # board 占位/真实初始化
│   ├── clock.c                    # 时钟初始化
│   ├── pinmux.c                   # 引脚复用初始化
│   └── interrupt.c                # 中断控制器和基础 ISR 初始化
├── ipc/
│   ├── rtos_shm_ipc.h
│   ├── rtos_shm_ipc.c             # 已有共享内存 IPC v2 descriptor API
│   ├── rtos_shm_platform.h
│   └── rtos_shm_platform.c        # cache / barrier / notify / atomic 平台抽象
├── mailbox/
│   ├── rtos_mailbox.h             # Mailbox / Doorbell 抽象接口
│   ├── rtos_mailbox_port.c        # 平台 doorbell 适配
│   ├── rtos_mailbox_isr.c         # ISR 只清中断并唤醒 IPC Event Task
│   └── rtos_mailbox_event.c       # host mock 或 RTOS event 封装
├── router/
│   ├── rtos_router.h              # 路由核心公共内部接口
│   ├── rtos_router_core.c         # route input 校验、drop 映射、状态机
│   ├── rtos_router_table.c        # CID 路由表、active_route_epoch
│   ├── rtos_router_policy.c       # TTL / epoch / trust / 背压策略
│   └── rtos_router_adapter.c      # IPC descriptor <-> route input/output 适配
├── queue/
│   ├── rtos_priority_queue.h
│   └── rtos_priority_queue.c      # priority 0..3 队列、配额和驱逐策略
├── tasks/
│   ├── rtos_ipc_event_task.c      # RX pending bitmap 扫描和 RX drain
│   ├── rtos_router_scheduler_task.c
│   ├── rtos_tx_writer_task.c
│   ├── rtos_heartbeat_task.c
│   ├── rtos_error_monitor_task.c
│   ├── rtos_recovery_task.c
│   └── rtos_statistics_task.c
├── monitor/
│   ├── rtos_endpoint_heartbeat.c  # 端到网关心跳表
│   ├── rtos_linux_heartbeat.c     # Linux heartbeat 状态判断
│   ├── rtos_error_state.c         # 错误状态和降级原因
│   └── rtos_statistics.c          # 本地统计快照和导出
├── can/
│   ├── rtos_can.h
│   └── rtos_can.c                 # 当前占位；真实物理收发仍以 Linux 为主
├── watchdog/
│   ├── rtos_watchdog.h
│   └── rtos_watchdog.c
└── test/
    ├── rtos_shm_ipc_test.c        # 已有 IPC v2 host 单测
    ├── rtos_router_core_test.c    # P1 路由核心 host 单测
    ├── rtos_ipc_adapter_test.c    # P2 descriptor 到 route input 适配测试
    ├── rtos_recovery_test.c       # P3 Recovery / reclaim full 测试
    └── rtos_host_loop_test.c      # P4 Linux/RTOS host 闭环测试
```

架构约束：

- [x] 已有文件优先原地演进；新增文件按上面的目录归属落位。
- [x] `ipc/` 只提供共享内存 descriptor 搬运能力，不放业务路由策略。
- [x] `router/` 和 `queue/` 不直接调用平台 cache、atomic 或 mailbox API。
- [x] `tasks/` 负责调度编排，可以连接 `ipc/`、`router/`、`queue/`、`monitor/` 和 `mailbox/`。
- [x] `monitor/` 不阻塞路由主流程，高频计数先本地累加。
- [x] `test/` 中新增 host 单测与阶段任务同名对应，避免继续把正式验收放到 `freertos/`。

内部接口规划：

- [x] 定义 `rtos_route_input_t` 或等价内部类型，表示经过 IPC 适配后的可信路由输入，不携带共享内存指针，不执行 Frame Pool 所有权操作。
- [x] 定义 `rtos_route_output_t` 或等价内部类型，表示 TX / reclaim 输出、目标接口、priority、drop reason、latency。
- [x] 定义 TX sink adapter：mock 阶段写测试队列，真实阶段调用 `rtos_shm_ipc_enqueue_tx_descriptor()`。
- [x] 定义 reclaim sink adapter：mock 阶段写测试队列，真实阶段调用 `rtos_shm_ipc_reclaim_frame()`。
- [x] 定义 route table snapshot，至少包含 `route_version`、`active_route_epoch`、CID 段到目标接口映射和 CRC/有效性状态。
- [x] 定义 heartbeat/error/recovery/statistics snapshot，用于 host 测试和后续共享状态区同步。

验收标准：

- `rtos_firmware` 成为正式小核实现和测试入口。
- `freertos/router_core` 不再作为新增验收目标，只允许作为迁移参考。
- 路由层和 IPC 层边界清晰，业务逻辑不直接普通读改写 shared memory pending line。

---

## 2. P0 已有基线

目标：确认当前可复用底座，不在后续阶段重复实现。

RTOS IPC 已有基线：

- [x] `rtos_firmware/ipc/rtos_shm_ipc.*` 支持 format / attach。
- [x] 支持 RX descriptor ring dequeue。
- [x] 支持 TX descriptor ring enqueue。
- [x] 支持 reclaim descriptor 写入。
- [x] 支持 descriptor CRC-16 校验。
- [x] 支持 Frame Pool 边界校验。
- [x] 支持 RX/TX ring 接口一致性校验。
- [x] 支持 pending bit 原子 OR / AND 和诊断计数原子 ADD。
- [x] 支持 cache flush / invalidate / memory barrier / notify 平台抽象。
- [x] notify 失败时 descriptor 保持已发布，通过 pending bitmap 兜底。
- [x] pending bit 清除和并发入队竞态已有 host 单测覆盖。

Linux IPC 已有基线：

- [x] `linux_app/ipc/linux_shm_ipc.*` 支持 host map/unmap、format/attach。
- [x] 支持 Linux Frame Pool alloc、RX commit、TX dequeue、reclaim dequeue 和 reclaim ack。
- [x] `RX_QUEUED` frame 不允许被 Linux 公开 release API 直接释放。
- [x] reclaim ack 后 Linux 最终释放 Frame Pool block。
- [x] TX/reclaim 元数据错配拒绝已有 host 单测覆盖。

P0 保持原则：

- [ ] 后续不修改 `shared_memory_ipc.h` 的 ABI 尺寸、ring 深度、pending bitmap 语义。
- [ ] 后续如果 `rtos_shm_ipc_*` API 返回码或平台 ops 细节变化，只调整 IPC 适配层和对应 TODO。
- [ ] 后续新增统计区或控制区必须另行设计，不混入已冻结 descriptor ABI。

验收标准：

- 当前 `rtos_shm_ipc_test` 和 `linux_shm_ipc_test` 继续通过。
- 后续路由层开发不复制 IPC 底层职责。

---

## 3. P1 路由核心迁入 rtos_firmware

目标：在 `rtos_firmware/` 内建立可 host 单测的路由核心，先使用 mock source/sink，不依赖真实 shared memory region。

迁移与模块化：

- [x] 评估 `freertos/router_core` 已有逻辑，按 `rtos_firmware` 风格迁入或重建到正式模块中。
- [x] 将正式路由核心接入 `rtos_firmware/CMakeLists.txt`，新增 `rtos_firmware/test/rtos_router_*_test.c`。
- [x] 保持 route input 与 `put_shm_descriptor_t` 解耦；第一阶段不访问 Frame Pool 指针。
- [x] 保持 TX / reclaim 输出可替换为 mock sink。
- [x] 使用统一时间源抽象，供 TTL、heartbeat、latency、Recovery 测试使用。

CID 路由规则：

- [x] 使用 `anymsg_frame.h` 中定义的 CID 地址段作为路由依据。
- [x] `destination_cid[0]` 在 `0x20 ~ 0x3F` 时路由到 CAN。
- [x] `destination_cid[0]` 在 `0x40 ~ 0x5F` 时路由到 Ethernet。
- [x] `destination_cid[0]` 在 `0x60 ~ 0x7F` 时路由到 Wi-Fi。
- [x] `destination_cid[0]` 在 `0x80 ~ 0x9F` 时路由到 Bluetooth。
- [x] `destination_cid[0]` 在 `0xA0 ~ 0xBF` 时路由到 4G。
- [x] `destination_cid[0]` 在 `0xC0 ~ 0xDF` 时路由到 RS485。
- [x] `0x00 ~ 0x1F` 和 `0xE0 ~ 0xFF` 作为保留地址，不进入 Router Scheduler。
- [x] 当前 anyMSG 未定义广播 CID，小核不自行扩展广播语义，统一按 `NO_ROUTE` 处理。

Priority 队列与调度：

- [x] 实现 priority 0/1/2/3 四级本地队列，数值越小优先级越高。
- [x] priority 超出 0..3 时按非法输入处理。
- [x] 实现严格优先级 + 配额防饥饿调度。
- [x] 默认配额：priority 0 每轮 16 帧，priority 1 每轮 12 帧，priority 2 每轮 8 帧，priority 3 每轮 4 帧。
- [x] 本地队列项保存 `frame_id`、source ring/interface、priority、enqueue time、retry count、`route_epoch_seen`。
- [x] 本地队列满时优先驱逐 priority 3，再 priority 2，再 priority 1；priority 0 仅在全局降级或 epoch/recovery 异常时丢弃。
- [x] priority 0/1 预留策略作为配置项预留，第一版可先通过队列驱逐和测试断言体现。

TTL、epoch、trust 和 anyMSG 基础状态：

- [x] route input 入队前检查 epoch。
- [x] route input 入队前检查 TTL。
- [x] 写 TX sink 前再次检查 TTL。
- [x] `ttl == 0` 表示不启用过期检查。
- [x] 鉴权失败、完整性失败、重放失败的输入不得进入调度队列。
- [x] 非法 anyMSG header、非法 CID、非法 type 统一走丢弃统计和 mock reclaim。
- [x] descriptor 级 CRC、Frame Pool 边界和 ring 接口一致性校验不在 P1 重做，由 P2 IPC 适配层和 `rtos_shm_ipc_*` API 承担。

Drop/reclaim 映射：

| 小核语义 | IPC reclaim reason |
| ---- | ---- |
| 端到网关心跳被消费 | `PUT_SHM_RECLAIM_REASON_HEARTBEAT_CONSUMED` |
| 无路由 / 保留 CID / 未定义广播 CID / gateway CID 未就绪 | `PUT_SHM_RECLAIM_REASON_NO_ROUTE` |
| TTL 过期 | `PUT_SHM_RECLAIM_REASON_TTL_EXPIRED` |
| epoch 不匹配 | `PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH` |
| 非法 anyMSG / 鉴权失败 / 完整性失败 / 重放失败 | `PUT_SHM_RECLAIM_REASON_INVALID_FRAME` |
| 本地队列满 / 目标 TX ring 满 / Recovery 丢弃 | `PUT_SHM_RECLAIM_REASON_QUEUE_FULL` |

P1 验收标准：

- CID 地址段能稳定路由到 6 类目标接口。
- 保留 CID、未定义广播 CID、无路由不会进入 TX mock。
- priority 0/1 优先，priority 2/3 在持续高优先级流量下不会永久饿死。
- TTL、epoch、trust、invalid anyMSG 失败路径全部写 mock reclaim。
- P1 所有新增单测位于 `rtos_firmware/test/`。

---

## 4. P2 IPC Event / Router Scheduler / TX Writer 闭环

目标：将 P1 路由核心接入 `rtos_firmware/ipc/rtos_shm_ipc.*`，形成 RTOS 侧真实 RX drain、TX enqueue 和 reclaim 闭环。

Mailbox 与 IPC Event Task：

- [x] 新增 Mailbox ISR / Doorbell port 抽象；ISR 只清中断并唤醒 IPC Event Task。
- [x] Doorbell 只作为 empty -> non-empty 的唤醒信号，不作为队列计数。
- [x] IPC Event Task 被 Doorbell 或周期兜底调度唤醒后读取 RX pending bitmap 快照。
- [x] IPC Event Task 对 6 个接口执行 RX drain。
- [x] 默认每接口 RX drain budget 为 8，默认每轮总 budget 为 64。
- [x] Doorbell 丢失或 notify 失败时，周期 drain 仍能依靠 pending bitmap 和 ring read/write seq 兜底。
- [x] 路由层不得直接普通 RMW pending bitmap；pending bit 清除、二次检查和竞态处理继续通过 `rtos_shm_ipc_*` 或 IPC 内部 helper 完成。

RX descriptor 到 route input：

- [x] IPC Event Task 使用 `rtos_shm_ipc_dequeue_rx_descriptor()` 从指定接口 RX ring 获取 descriptor。
- [x] 使用 `rtos_shm_ipc_get_frame_const()` 只读访问 Frame Pool 中的完整 anyMSG。
- [x] 校验 anyMSG normalized length、保留字段、CID、type。
- [x] descriptor 达到 `FRAME_REF_TRUSTED` 前，不读取不可信 `frame_id` 给路由层，不写 reclaim。
- [x] IPC API 返回 CRC、Frame Pool 边界、接口一致性等错误时，不把 descriptor 交给路由核心。
- [x] 将真实 descriptor、anyMSG header、可信状态转换为 P1 route input。
- [x] route input 入队时记录 `route_epoch_seen = active_route_epoch`。

Heartbeat 快速消费路径：

- [x] `type = 0x00` 的端到网关心跳帧在 IPC Event / Heartbeat 路径消费。
- [x] 合法心跳更新 endpoint heartbeat table。
- [x] 心跳消费后写 reclaim，reason 为 `PUT_SHM_RECLAIM_REASON_HEARTBEAT_CONSUMED`。
- [x] 心跳帧不进入 Router Scheduler，不写 TX Ring，不触发 TX Doorbell。
- [x] 小核不主动生成 `type = 0x01` 网关到端心跳。

Router Scheduler：

- [x] 普通帧进入 Router Scheduler 本地 priority 队列。
- [x] Router Scheduler 出队前检查 TTL。
- [x] 出队前比较 `route_epoch_seen` 与当前 `active_route_epoch`。
- [x] route epoch 变化时重新查询 active route table，并更新目标接口。
- [x] 重新查询后无路由的帧写 reclaim，reason 为 `PUT_SHM_RECLAIM_REASON_NO_ROUTE`。
- [x] 已写入 TX Ring 的 descriptor 不回滚。

TX Writer 与 reclaim adapter：

- [x] TX Writer 从 Router Scheduler 获取待发送输出。
- [x] TX sink 真实实现调用 `rtos_shm_ipc_enqueue_tx_descriptor()`。
- [x] reclaim sink 真实实现调用 `rtos_shm_ipc_reclaim_frame()`。
- [x] TX descriptor 和 reclaim descriptor 只能引用 Linux 已发布给 RTOS 的 RX frame。
- [x] TX Ring 满时执行 bounded retry；重试耗尽后写 reclaim，公共 reason 为 `PUT_SHM_RECLAIM_REASON_QUEUE_FULL`。
- [x] reclaim ring 满时暂停继续消费 RX descriptor，进入 `DEGRADED_RECLAIM_FULL` 或等价状态。
- [x] reclaim ring 满期间冻结本地引用，等待 Linux drain 后进入补写流程。

P2 验收标准：

- Linux 写 RX descriptor 后，RTOS 能 drain 并交给路由核心。
- 成功路由帧出现在目标 TX Ring。
- 心跳、无路由、TTL 过期、epoch mismatch、invalid anyMSG 等路径出现在 reclaim ring。
- IPC 错误 descriptor 不进入路由核心，不产生不可信 reclaim。
- Doorbell 丢失时仍可通过 pending bitmap 和周期 drain 消费。
- reclaim ring 满不会导致新的不可回收 Frame Buffer 引用继续产生。

---

## 5. P3 Heartbeat / Error Monitor / Recovery / Statistics

目标：补齐小核运行期状态维护、异常降级、恢复同步和可观测性。

Heartbeat：

- [x] 周期性更新 RTOS heartbeat seq。
- [x] 周期性读取 Linux heartbeat seq。
- [x] Linux heartbeat 300 ms 未变化进入 warning。
- [x] Linux heartbeat 500 ms 未变化进入 suspected abnormal。
- [x] Linux heartbeat 1000 ms 未变化进入全局降级。
- [x] Linux heartbeat 从异常恢复时触发 Recovery。
- [x] 端到网关心跳表最大记录数默认 64。
- [x] endpoint heartbeat warn 默认 3000 ms，offline 默认 5000 ms。
- [x] endpoint entry 至少保存 `source_cid`、last RX interface/ring、last RTOS time、last frame local_time、state、rx_count、timeout_count。
- [x] gateway CID 和 gateway alias 由配置或控制区加载；未配置时不更新端心跳表。

Error Monitor：

- [x] 监控 RX Ring 长时间积压。
- [x] 监控 TX Ring 长时间满。
- [x] 监控 pending bit 长时间未清除。
- [x] 监控 Frame Pool 异常或疑似泄漏。
- [x] 监控 TTL 过期、epoch mismatch、route miss 大量增加。
- [x] 监控 Mailbox notify 失败。
- [x] 监控 Linux heartbeat 超时。
- [x] 监控端心跳超时、非法端心跳 source CID、端心跳表满。
- [x] 监控共享内存 magic/version、ring descriptor、cache sync 异常。
- [x] 监控 reclaim ring 积压或满、pending reclaim 长时间不下降。
- [x] 不监控 CAN BusOff、RS485 方向控制、SPI/UART 物理驱动错误，这些由 Linux 物理层负责。

Recovery：

- [x] Recovery 触发条件包括 Linux heartbeat 恢复、Linux ready 重新置位、linux_epoch 变化、共享内存 magic/version 重建、ring descriptor 变化、route table 更新、Mailbox 恢复、reclaim ring 从满状态恢复。
- [x] Recovery 时暂停 IPC Event Task 新帧投递。
- [x] Recovery 时暂停 Router Scheduler 和 TX Writer。
- [x] 冻结本地 priority 队列引用。
- [x] reclaim ring 可写时，按预算分批写 reclaim，reason 映射为 `PUT_SHM_RECLAIM_REASON_QUEUE_FULL`。
- [x] reclaim ring 满时进入 `RECLAIM_BLOCKED`，等待 Linux drain 后补写。
- [x] 标记旧 epoch 和 TTL 过期数据为待丢弃，不继续写 TX Ring。
- [x] 重新检查共享内存 magic/version。
- [x] 重新读取 linux_epoch。
- [x] 重新 attach 或刷新 RX/TX Ring 映射。
- [x] 重新加载 route table、gateway CID 和端心跳配置。
- [x] 清理本地 pending bitmap 快照和错误状态。
- [x] 端心跳表恢复后必须由新的 `type = 0x00` 心跳重新确认。
- [x] Recovery 完成后回到 `NORMAL`。

状态机：

- [x] 扩展小核全局状态机为：

```text
BOOT
INIT_BOARD
INIT_MAILBOX
WAIT_SHM_READY
INIT_RING_MAP
INIT_ROUTER_TABLE
NORMAL
DEGRADED
DEGRADED_RECLAIM_FULL
RECLAIM_BLOCKED
RECOVERY
```

- [x] `DEGRADED` 停止普通业务路由，保留错误统计和必要 Doorbell/event。
- [x] `DEGRADED_RECLAIM_FULL` 暂停所有会产生新 reclaim 的 RX drain。
- [x] `RECLAIM_BLOCKED` 只等待 Linux drain 后补写被冻结 reclaim descriptor。
- [x] `RECOVERY` 必须重新同步 epoch、Ring、bitmap、route table 和 gateway 配置，不继续旧状态。

Statistics：

- [x] 维护 Doorbell RX/TX 次数、RX ring drain 次数、TX ring write 次数。
- [x] 维护 route success、route miss、drop reason、priority、interface 维度统计。
- [x] 维护 TTL drop、epoch drop、TX ring full、reclaim ring full、reclaim blocked、pending reclaim retry。
- [x] 维护 auth failed、integrity failed、replay drop、invalid descriptor、invalid descriptor no reclaim、invalid anyMSG。
- [x] 维护 Linux heartbeat timeout、endpoint heartbeat rx/invalid/timeout/recover/table full。
- [x] 维护小核内部 latency：RX dequeue 到 TX enqueue 的 max/count，priority 0/1 和 priority 2 预留 p95/p99 或滑动窗口摘要。
- [x] 高频统计先在小核本地累加，后续按周期同步到共享状态区。

P3 验收标准：

- Linux heartbeat 超时后停止普通路由，恢复后进入 Recovery。
- endpoint heartbeat 能从 ONLINE 转 WARN / OFFLINE，并在新心跳到达后恢复。
- reclaim ring full 能阻塞 RX drain，Linux drain 后能分批补写并恢复。
- route epoch 切换后，未写 TX Ring 的本地队列项会重新查路由。
- Recovery 后没有旧 epoch 或 TTL 过期帧继续写 TX Ring。

---

## 6. P4 Linux/RTOS host 闭环与板端联调

目标：联调 Linux IPC library、RTOS IPC library、小核路由层和 Linux 出口层，验证端到端行为。先完成 host 闭环，再推进板端 reserved-memory、cache maintenance 和 Mailbox/CMDQU。

Host 闭环：

- [x] Linux 使用 `linux_shm_frame_alloc()` 分配 Frame Pool block。
- [x] Linux 写入完整 anyMSG 后调用 `linux_shm_frame_commit_rx()` 发布 RX descriptor。
- [x] RTOS 使用 `rtos_shm_ipc_dequeue_rx_descriptor()` drain RX ring。
- [x] RTOS 路由成功时调用 `rtos_shm_ipc_enqueue_tx_descriptor()`。
- [x] Linux 使用 `linux_shm_dequeue_tx_descriptor()` 读取目标 TX descriptor 和只读 frame。
- [x] RTOS 消费或丢弃时调用 `rtos_shm_ipc_reclaim_frame()`。
- [x] Linux 使用 `linux_shm_dequeue_reclaim_descriptor()` ack reclaim 并最终释放 Frame Pool block。
- [x] 验证 RTOS 回写 TX/reclaim 只能引用 Linux 已发布给 RTOS 的 frame。
- [x] 长时间闭环后 Linux Frame Pool used 回落稳定，无持续增长疑似泄漏。

板端联调：

- [ ] 确认 Linux DTS `reserved-memory` 与 RTOS BSP/linker 使用同一物理共享内存区域。
- [ ] 替换 host no-op cache ops 为板端真实 cache flush/invalidate。
- [ ] 替换 host no-op notify 为 Mailbox/CMDQU 或内核 ioctl doorbell。
- [ ] 验证 Doorbell 只作为唤醒信号，pending bitmap 和 ring 状态仍是唯一可信数据状态。
- [ ] 验证 Doorbell 丢失、notify 失败、Linux 出口暂时不 drain 时周期 drain 能兜底。
- [ ] 验证 Linux 重启或共享内存重建触发 RTOS Recovery。
- [ ] 验证 cache maintenance 和 Mailbox 平台 ops 的错误能被统计和降级处理。

压测：

- [ ] 高负载 RX ring drain 下 priority 0/1 小核内部 max/p95/p99 延迟。
- [ ] 单一高吞吐入口打满时，其他入口和高优先级帧仍可处理。
- [ ] 目标 TX ring 拥塞下 bounded retry、`QUEUE_FULL` reclaim 和统计行为。
- [ ] reclaim ring 积压和满载恢复。
- [ ] Linux 出口阻塞时，小核 TX ring 写入延迟与 Linux send done 延迟分开统计。
- [ ] 低 MTU 接口分片发送时，验证 priority 0/1 是否能按 Linux 出口策略在分片边界插队。
- [ ] 压测报告包含最大延迟、p95/p99、drop reason、水位线和 Frame Pool 使用曲线。

P4 验收标准：

- CAN RX 能路由到 RS485 TX。
- RS485 RX 能路由到 CAN TX。
- Ethernet RX 能按 CID 路由到目标 TX ring。
- `type = 0x00` 心跳被 RTOS 消费，Linux 能从 reclaim ring 回收。
- 无路由、TTL 过期、epoch mismatch、invalid frame 进入 reclaim。
- TX ring full 后 bounded retry 失败，最终 reclaim `PUT_SHM_RECLAIM_REASON_QUEUE_FULL`。
- reclaim ring full 时 RTOS 暂停继续消费 RX。
- 板端真实 TX Ring 和 Linux 出口层能收到正确目标帧。

---

## 7. 测试与验收清单

新增测试落位：

- [x] 新增正式路由核心测试放入 `rtos_firmware/test/`。
- [x] 新增 IPC 适配和闭环测试放入 `rtos_firmware/test/` 或跨库 host test 目录。
- [x] 不继续扩展 `freertos/router_core/test` 作为正式验收入口。
- [x] 保留并持续运行 `rtos_firmware/test/rtos_shm_ipc_test.c`。
- [x] 保留并持续运行 `linux_app/test/linux_shm_ipc_test.c`。

P1 测试：

- [x] CID 首字节路由到 CAN / Ethernet / Wi-Fi / Bluetooth / 4G / RS485。
- [x] 保留 CID、未定义广播 CID、无路由进入 drop / mock reclaim。
- [x] priority 0/1 优先，priority 2/3 不长期饿死。
- [x] 非法 priority 不进入调度队列。
- [x] TTL 过期不进入 TX mock。
- [x] `ttl == 0` 不触发 TTL 过期。
- [x] epoch mismatch 不进入 TX mock。
- [x] auth failed、integrity failed、replay dropped 不进入 TX mock。
- [x] invalid anyMSG 不进入 TX mock。
- [x] `type = 0x00` 心跳更新端在线表，不进入 TX mock。
- [x] gateway CID 未配置时心跳不更新端在线表。
- [x] 本地队列满时按优先级丢弃并写 mock reclaim。
- [x] TX mock 拥塞时按 priority 策略重试或丢弃。
- [x] Recovery 清理本地队列引用并写 mock reclaim。
- [x] 统计计数与实际处理路径一致。

P2 测试：

- [x] RX Descriptor Ring drain 到正式路由核心。
- [x] Frame Pool 只读访问完整 anyMSG header。
- [x] TX Ring 写入目标接口 descriptor。
- [x] reclaim ring 写入公共 reclaim reason。
- [x] IPC API 返回 CRC / bounds / interface 错误时不进入路由核心。
- [x] Doorbell 失败后 descriptor 仍可通过 pending bitmap 和周期 drain 消费。
- [x] reclaim ring 满时暂停继续消费 RX descriptor。
- [x] Frame Pool 引用不在小核释放或清零。
- [x] route input 记录 `route_epoch_seen`。
- [x] route epoch 变化后出队前重新查路由。

P3 测试：

- [x] Linux heartbeat warning / suspected abnormal / global degraded 转换。
- [x] Linux heartbeat 恢复触发 Recovery。
- [x] endpoint heartbeat ONLINE / WARN / OFFLINE 转换。
- [x] reclaim full 进入 `DEGRADED_RECLAIM_FULL`。
- [x] Linux drain reclaim 后进入 `RECLAIM_BLOCKED` 补写并恢复。
- [x] linux_epoch 变化触发 Recovery。
- [x] magic/version 变化触发 Recovery。
- [x] route table CRC 错误保持旧表并计数。
- [x] Recovery 后旧 epoch 本地引用不会写 TX Ring。

P4 测试：

- [x] Linux `alloc + commit_rx`，RTOS drain/route，Linux `dequeue_tx`。
- [x] CAN RX 到 RS485 TX。
- [x] RS485 RX 到 CAN TX。
- [x] `type = 0x00` 心跳被 RTOS 消费，Linux 从 reclaim ring 回收。
- [x] 无路由、TTL 过期、epoch mismatch、invalid frame 进入 reclaim。
- [x] TX ring full 后 bounded retry 失败，最终 reclaim `PUT_SHM_RECLAIM_REASON_QUEUE_FULL`。
- [x] reclaim ring full 时 RTOS 暂停继续消费 RX。
- [x] 长时间闭环后 Linux Frame Pool `used` 回落稳定，无持续增长疑似泄漏。

---

## 8. 关键假设

- 本计划只指导 `rtos_firmware/` 小核后续实现，不修改公共 ABI。
- `common/include/shared_memory_ipc.h`、接口文档和共享内存架构设计是第二阶段及以后真实适配的 ABI 来源。
- `rtos_firmware/ipc` 当前实现视为可复用底座。
- `linux_app/ipc` 当前实现视为 host 闭环的 Linux 侧基线。
- `freertos/router_core` 的已有实现可被借鉴或迁移，但不能在实施计划中标为 `rtos_firmware` 已完成。
- 真实 Mailbox/CMDQU、cache maintenance、DTS reserved-memory 细节留到板端联调阶段，不在 TODO 文档中虚构具体驱动接口。
- 后续若新增共享状态区、控制区、route table wire format 或统计 ABI，需要单独设计并评审，不能隐式塞入现有 descriptor。

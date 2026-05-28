# FreeRTOS 小核实施计划

来源：

- `docs/设计文档/freeRTOS核设计.md`
- `docs/接口文档/大小核共享内存IPC接口.md`
- `docs/设计文档/共享内存 IPC 架构设计方案.md`
- `common/include/shared_memory_ipc.h`
- `rtos_firmware/ipc/`
- `linux_app/ipc/`

日期：2026-05-28  
目标目录：`rtos_firmware/`  
阶段策略：第一阶段完成小核路由核心和 mock 验证；第二阶段基于现有 `rtos_shm_ipc_*` API 接入共享内存 IPC v2；第三阶段完成 Linux/RTOS host 闭环、板端联调和压测验收。

---

## 0. 实施边界

本计划用于指导 FreeRTOS 小核后续开发。当前目标不是重新设计共享内存 ABI，而是在已冻结的 IPC v2 ABI 和已有 Linux/RTOS IPC 底座之上补齐小核路由、调度、心跳、Recovery 和集成闭环。

现状快照：

- 公共 ABI 已冻结在 `common/include/shared_memory_ipc.h`。
- RTOS 侧 IPC v2 底座已在 `rtos_firmware/ipc/rtos_shm_ipc.*` 实现，并已有 host 单测。
- Linux 侧 Frame Pool、RX/TX descriptor、reclaim 状态机已在 `linux_app/ipc/linux_shm_ipc.*` 实现，并已有 host 单测。
- 待实现重点是小核路由核心、IPC Event Task、Router Scheduler、Heartbeat、Recovery、统计以及 Linux/RTOS 闭环集成测试。
- `freertos/` 目录只作为历史参考；正式小核开发以 `rtos_firmware/` 为准。

固定 ABI 边界：

- `PUT_SHM_IPC_VERSION = 2`。
- region 固定 `64 KiB`。
- 物理接口固定 6 类：CAN、Ethernet、Wi-Fi、Bluetooth、4G、RS485。
- Frame Pool 固定 64 个 block，每个 block 512B。
- 每接口 RX/TX descriptor ring 深度固定 8。
- reclaim ring 深度固定 8。
- descriptor 和 reclaim descriptor 均固定 64B。
- `rx_pending_bitmap`、`tx_pending_bitmap`、`reclaim_pending` 分别独占一个 64B cache line。

职责边界：

- Linux 是 Frame Pool 唯一分配者和最终释放者。
- RTOS 不释放 Frame Pool block，不清零 payload，不修改 Linux free list。
- RTOS 只读 Frame Pool 中的完整 anyMSG，写目标 TX descriptor 或 reclaim descriptor。
- RTOS 路由层不得重实现 pending bitmap 原子操作、descriptor CRC、cache flush/invalidate、Doorbell 发布细节，这些由 `rtos_firmware/ipc` 层负责。
- IPC 层负责 descriptor ABI、CRC、Frame Pool 边界和接口一致性校验；路由层只处理可信 route input。
- 坏 descriptor 如果未达到可信 frame reference，不由路由层写 reclaim，交给 IPC 错误统计和 recovery/sweep 路径。
- 第一阶段内部 mock 类型不是公共 ABI；第二阶段适配以 `shared_memory_ipc.h` 和 `rtos_shm_ipc.h` 为准。

目标拆分：

```text
第一阶段：小核路由核心和 mock 验证
第二阶段：基于现有 RTOS IPC v2 API 的共享内存适配
第三阶段：Linux/RTOS host 闭环、板端联调和压测验收
```

---

## 1. 第一阶段：路由核心和 mock 验证

目标：在不依赖真实共享内存 region 的条件下，完成小核路由、调度、心跳、统计和状态机核心能力。第一阶段所有输入输出都通过 mock source/sink 驱动，确保核心逻辑可在 host 单元测试中独立验证。

### 1.1 任务骨架与模块边界

- [ ] 在 `rtos_firmware/` 规划小核核心模块边界：抽象入口、Router Scheduler、TX Writer 抽象出口、Heartbeat、Error Monitor、Recovery、Statistics。
- [ ] 明确第一阶段所有输入来自 mock descriptor / mock anyMSG header，不直接访问 Frame Pool。
- [ ] 明确第一阶段所有输出写入 mock TX sink 或 mock reclaim sink，不写真实 TX Ring / reclaim ring。
- [ ] 将小核核心逻辑与共享内存 IPC 层解耦，避免业务逻辑直接依赖 `put_shm_descriptor_t`。
- [ ] 保留第二阶段适配点，后续把 mock source/sink 替换为 `rtos_shm_ipc_*` API。

验收标准：

- 小核路由核心可以在 host 单元测试中运行。
- 第一阶段代码不需要真实共享内存 region。
- mock 输入输出结构足够覆盖路由、丢弃、心跳和统计路径。

### 1.2 内部抽象接口

- [ ] 定义 `rtos_route_input_t`，表示小核路由输入的抽象帧描述。
- [ ] 定义 `rtos_route_output_t`，表示小核路由输出的目标接口、处理结果和 drop reason。
- [ ] 定义 `rtos_tx_sink`，第一阶段写 mock TX 队列，第二阶段调用 `rtos_shm_ipc_enqueue_tx_descriptor()`。
- [ ] 定义 `rtos_reclaim_sink`，第一阶段写 mock reclaim 队列，第二阶段调用 `rtos_shm_ipc_reclaim_frame()`。
- [ ] 定义 `rtos_time_source`，用于 TTL、heartbeat、Recovery 和 latency 统计。
- [ ] 定义小核内部 drop reason，并提供到 `put_shm_reclaim_reason_t` 的映射。

drop/reclaim 映射必须覆盖：

| 小核语义 | IPC reclaim reason |
| ---- | ---- |
| 端到网关心跳被消费 | `PUT_SHM_RECLAIM_REASON_HEARTBEAT_CONSUMED` |
| 无路由 / 保留 CID / 未定义广播 CID | `PUT_SHM_RECLAIM_REASON_NO_ROUTE` |
| TTL 过期 | `PUT_SHM_RECLAIM_REASON_TTL_EXPIRED` |
| epoch 不匹配 | `PUT_SHM_RECLAIM_REASON_EPOCH_MISMATCH` |
| 非法 anyMSG / 鉴权失败 / 完整性失败 / 重放失败 | `PUT_SHM_RECLAIM_REASON_INVALID_FRAME` |
| 本地队列满 / 目标 TX ring 满 / Recovery 丢弃 | `PUT_SHM_RECLAIM_REASON_QUEUE_FULL` |

验收标准：

- route input 不包含共享内存指针和 Frame Pool 所有权操作。
- TX / reclaim 输出都可在测试中替换为 mock。
- 内部 drop reason 能稳定映射到公共 reclaim reason。

### 1.3 CID 路由规则

- [ ] 使用 `anymsg_frame.h` 中定义的 CID 地址段作为路由依据。
- [ ] 实现 `destination_cid[0]` 到目标接口的固定映射。
- [ ] `0x20 ~ 0x3F` 路由到 CAN。
- [ ] `0x40 ~ 0x5F` 路由到 Ethernet。
- [ ] `0x60 ~ 0x7F` 路由到 Wi-Fi。
- [ ] `0x80 ~ 0x9F` 路由到 Bluetooth。
- [ ] `0xA0 ~ 0xBF` 路由到 4G。
- [ ] `0xC0 ~ 0xDF` 路由到 RS485。
- [ ] `0x00 ~ 0x1F` 和 `0xE0 ~ 0xFF` 作为保留地址，不进入 Router Scheduler。
- [ ] 未定义广播 CID 暂不扩展广播语义，统一按 `NO_ROUTE` 处理。

验收标准：

- 每个合法 CID 地址段都有确定目标接口。
- 保留地址和未定义广播地址不会进入 TX mock。
- 无路由帧写 mock reclaim，并记录 `NO_ROUTE` 统计。

### 1.4 Priority 队列与调度

- [ ] 实现 priority 0/1/2/3 四级本地队列。
- [ ] priority 数值越小优先级越高。
- [ ] priority 超出 0/1/2/3 时按非法输入处理，不进入调度队列。
- [ ] 实现严格优先级 + 配额的防饥饿调度。
- [ ] 默认配额：priority 0 每轮 16 帧，priority 1 每轮 12 帧，priority 2 每轮 8 帧，priority 3 每轮 4 帧。
- [ ] 本地队列满时优先丢弃低优先级帧，并写 mock reclaim。
- [ ] TX mock 拥塞时按 priority 执行 bounded retry 或丢弃策略。

验收标准：

- priority 0/1 能优先进入 TX mock。
- priority 2/3 在持续高优先级流量下不会永久饿死。
- 非法 priority 不会进入调度队列。
- 本地队列满或 TX mock 拥塞时有确定 drop reason 和统计。

### 1.5 TTL、epoch 与可信性状态

- [ ] route input 入队前检查 epoch。
- [ ] route input 入队前检查 TTL。
- [ ] route input 写 TX mock 前再次检查 TTL。
- [ ] `ttl == 0` 表示不启用过期检查。
- [ ] 鉴权失败、完整性失败、重放失败的输入不得进入调度队列。
- [ ] 非法 anyMSG header、非法 CID、非法 type 统一走丢弃统计和 mock reclaim。
- [ ] descriptor 级 CRC、Frame Pool 边界和 ring 接口一致性校验不在第一阶段实现，由第二阶段 IPC 适配层和 `rtos_shm_ipc_*` API 承担。

验收标准：

- TTL 过期帧不会进入 TX mock。
- `ttl == 0` 的帧不会因 TTL 检查被误丢弃。
- epoch 不匹配帧不会进入 TX mock。
- 鉴权、完整性、重放失败帧不会进入 TX mock。
- 所有可回收丢弃路径都写 mock reclaim。

### 1.6 端到网关心跳

- [ ] 识别 `type = 0x00` 的端到网关心跳帧。
- [ ] 使用 `source_cid` 作为端设备身份更新端心跳表。
- [ ] 仅当 `source_cid[0]` 位于 `0x20 ~ 0xDF` 时进入端心跳表。
- [ ] 校验 `destination_cid` 是否匹配已配置 gateway CID 或 gateway alias。
- [ ] gateway CID 未配置时，不更新端心跳表，并写 mock reclaim。
- [ ] 心跳消费后不进入 Router Scheduler、不写 TX mock。
- [ ] 小核不生成 `type = 0x01` 网关到端心跳。

验收标准：

- 合法端心跳更新端在线表。
- 非法端心跳只更新错误统计并写 mock reclaim。
- 心跳帧不会出现在 TX mock 中。

### 1.7 状态机、Recovery 与降级

- [ ] 实现小核内部状态机：`BOOT`、`INIT_BOARD`、`INIT_ROUTER_TABLE`、`NORMAL`、`DEGRADED`、`RECOVERY`。
- [ ] 第一阶段 Recovery 只清理小核本地队列和本地状态，不操作共享内存。
- [ ] Recovery 清理本地队列引用时写 mock reclaim，公共映射为 `PUT_SHM_RECLAIM_REASON_QUEUE_FULL`。
- [ ] Linux heartbeat、Mailbox 异常、共享内存 epoch 变化等触发条件在第一阶段用 mock event 注入。
- [ ] 局部降级覆盖本地队列满、目标 TX mock 拥塞、某类 CID 持续无路由。

验收标准：

- Recovery 后本地队列没有遗留待转发旧引用。
- DEGRADED 状态不继续普通业务路由。
- mock event 能覆盖状态切换路径。

### 1.8 统计与观测

- [ ] 维护 route success、route miss、drop reason、priority、heartbeat、queue full、recovery 等统计。
- [ ] 按接口、priority、drop reason 拆分关键统计。
- [ ] 记录小核内部延迟：mock RX 进入时间到 mock TX 输出时间。
- [ ] 第一阶段统计输出到小核本地 snapshot 或测试可读取结构。
- [ ] Web、共享内存 stats/event area 暂不在第一阶段实现。

验收标准：

- 单元测试能读取统计并断言计数。
- drop、route、heartbeat、recovery 统计与行为一致。
- priority 0/1、priority 2 至少记录 max/count 延迟。

---

## 2. 第二阶段：基于现有 RTOS IPC v2 API 的共享内存适配

目标：将第一阶段的小核核心逻辑接入 `rtos_firmware/ipc/rtos_shm_ipc.*` 提供的现有 v2 descriptor API，形成 RTOS 侧真实 RX drain、TX enqueue 和 reclaim 闭环。

### 2.1 IPC 初始化与输入适配

- [ ] 在小核启动流程中接入 `rtos_shm_ipc_attach()`，绑定 BSP/linker 提供的共享内存 region。
- [ ] IPC Event Task 周期读取 RX pending 状态，并对 6 个接口执行 drain 兜底。
- [ ] 使用 `rtos_shm_ipc_dequeue_rx_descriptor()` 从指定接口 RX ring 获取 descriptor。
- [ ] 使用 `rtos_shm_ipc_get_frame_const()` 只读访问 Frame Pool 中的完整 anyMSG。
- [ ] 把真实 descriptor 和 anyMSG header 转换为第一阶段的 `rtos_route_input_t`。
- [ ] IPC API 返回 CRC、Frame Pool 边界、接口一致性等错误时，不把 descriptor 交给路由核心。
- [ ] descriptor 未达到可信 frame reference 时，不由路由层读取 `frame_id` 或写 reclaim，进入 IPC recovery/error 路径。

验收标准：

- RX Descriptor Ring 能被 drain 到路由核心。
- 小核核心仍不直接依赖共享内存结构体细节。
- 非法 descriptor 不会把不可信 `frame_id` 交给路由核心。
- Doorbell 丢失时仍可依靠 pending bitmap 和周期 drain 兜底。

### 2.2 TX Writer 与 reclaim 适配

- [ ] 将第一阶段 `rtos_tx_sink` 的真实实现接到 `rtos_shm_ipc_enqueue_tx_descriptor()`。
- [ ] 将第一阶段 `rtos_reclaim_sink` 的真实实现接到 `rtos_shm_ipc_reclaim_frame()`。
- [ ] route success 写目标 TX Descriptor Ring。
- [ ] 消费或丢弃但不转发的帧写 reclaim ring。
- [ ] TX descriptor 和 reclaim descriptor 只能引用 Linux 已发布给 RTOS 的 RX frame。
- [ ] 小核不释放 Frame Pool，不清零 payload，不修改 Linux free list。

验收标准：

- 成功路由帧出现在目标 TX Ring。
- 心跳、无路由、TTL 过期、epoch mismatch、非法 anyMSG 等路径出现在 reclaim ring。
- Frame Pool 最终释放仍由 Linux 完成。

### 2.3 Pending Bitmap、Doorbell 与拥塞

- [ ] pending bitmap 原子 OR/AND、诊断计数原子 ADD、descriptor CRC、cache sync 和 Doorbell 发布继续由 `rtos_shm_ipc_*` API 承担。
- [ ] 路由层只把 pending bitmap 和 Doorbell 作为事件来源，不直接普通 RMW 共享 pending line。
- [ ] TX Ring 满时执行 bounded retry 或按 priority 丢弃，并最终写 reclaim reason `PUT_SHM_RECLAIM_REASON_QUEUE_FULL`。
- [ ] reclaim ring 满时暂停继续消费 RX descriptor，进入 `DEGRADED_RECLAIM_FULL` 或等价状态，等待 Linux drain 后恢复。
- [ ] Doorbell 失败不作为 descriptor 已发布失败处理；依赖 pending bitmap 和周期 drain 兜底。

验收标准：

- pending bit 不会因路由层普通读改写被覆盖。
- TX ring full 有确定 retry、drop 和 reclaim 行为。
- reclaim ring 满不会导致新的不可回收 Frame Buffer 引用继续产生。

### 2.4 共享内存 Recovery

- [ ] 接入 Linux epoch 变化检测。
- [ ] 接入共享内存 magic/version 变化检测。
- [ ] 接入 route table / gateway CID / control area 更新。
- [ ] Recovery 时暂停新帧投递、冻结本地队列、分批写 reclaim。
- [ ] reclaim ring 满时 Recovery 不继续写 reclaim，等待 Linux drain 后补写。
- [ ] Recovery 后重新 attach 或刷新本地 IPC/route/gateway 状态。

验收标准：

- Linux 重启或共享内存重建后，小核不会继续转发旧 epoch 帧。
- Recovery 后重新加载 Ring 映射、路由表和 gateway CID。
- 本地旧引用要么已写 reclaim，要么被冻结等待补写，不会静默丢失。

---

## 3. 第三阶段：Linux/RTOS host 闭环、板端联调和压测

目标：联调 Linux IPC library、RTOS IPC library、小核路由层和 Linux 出口层，验证端到端行为。第三阶段先完成 host 闭环，再推进板端 reserved-memory、cache maintenance 和 Mailbox/CMDQU。

### 3.1 Host 闭环场景

- [ ] Linux 使用 `linux_shm_frame_alloc()` 分配 Frame Pool block。
- [ ] Linux 写入完整 anyMSG 后调用 `linux_shm_frame_commit_rx()` 发布 RX descriptor。
- [ ] RTOS 使用 `rtos_shm_ipc_dequeue_rx_descriptor()` drain RX ring。
- [ ] RTOS 路由成功时调用 `rtos_shm_ipc_enqueue_tx_descriptor()`。
- [ ] Linux 使用 `linux_shm_dequeue_tx_descriptor()` 读取目标 TX descriptor 和只读 frame。
- [ ] RTOS 消费或丢弃时调用 `rtos_shm_ipc_reclaim_frame()`。
- [ ] Linux 使用 `linux_shm_dequeue_reclaim_descriptor()` ack reclaim 并最终释放 Frame Pool block。
- [ ] 验证 `RX_QUEUED` frame 不可由 Linux 公开 release API 直接释放。
- [ ] 验证 RTOS 回写 TX/reclaim 只能引用 Linux 已发布给 RTOS 的 frame。

验收标准：

- CAN RX 能路由到 RS485 TX。
- `type = 0x00` 心跳被 RTOS 消费，Linux 能从 reclaim ring 回收。
- 无路由、TTL 过期、epoch mismatch、invalid frame 进入 reclaim。
- TX ring full 后 bounded retry 失败，最终 reclaim `PUT_SHM_RECLAIM_REASON_QUEUE_FULL`。
- reclaim ring full 时 RTOS 暂停继续消费 RX。

### 3.2 板端联调场景

- [ ] 确认 Linux DTS `reserved-memory` 与 RTOS BSP/linker 使用同一物理共享内存区域。
- [ ] 替换 host no-op cache ops 为板端真实 cache flush/invalidate。
- [ ] 替换 host no-op notify 为 Mailbox/CMDQU 或内核 ioctl doorbell。
- [ ] 验证 Doorbell 只作为唤醒信号，pending bitmap 和 ring 状态仍是唯一可信数据状态。
- [ ] 验证 Doorbell 丢失、notify 失败、Linux 出口暂时不 drain 时周期 drain 能兜底。
- [ ] 验证 Linux 重启或共享内存重建触发 RTOS Recovery。

验收标准：

- 真实 TX Ring 和 Linux 出口层能收到正确目标帧。
- 消费或丢弃帧最终能被 Linux 回收 Frame Buffer。
- 路由结果、drop reason、统计和日志一致。
- cache maintenance 和 Mailbox 平台 ops 的错误能被统计和降级处理。

### 3.3 压测场景

- [ ] 高负载 RX Ring drain 下 priority 0/1 小核内部延迟。
- [ ] 单一高吞吐入口打满时，其他入口和高优先级帧仍可处理。
- [ ] 目标 TX Ring 拥塞下 bounded retry、`QUEUE_FULL` reclaim 和统计行为。
- [ ] reclaim ring 积压和满载恢复。
- [ ] Linux 出口阻塞时，小核 TX enqueue 延迟与 Linux send done 延迟分开统计。
- [ ] 长时间压测后 Frame Pool 使用量回落到稳定水平。

验收标准：

- priority 0/1 在小核内获得优先调度。
- priority 2/3 在拥塞时按策略丢弃或降级。
- Frame Pool 不出现持续增长的疑似泄漏。
- 压测报告包含最大延迟、p95/p99、drop reason 和水位线。

---

## 4. 已有测试基线

RTOS IPC 基线：

- [x] `rtos_firmware/test/rtos_shm_ipc_test.c` 覆盖 ABI size、format/attach。
- [x] 覆盖 RX/TX descriptor ring enqueue/dequeue。
- [x] 覆盖 descriptor CRC 错误消费。
- [x] 覆盖 Frame Pool 边界校验。
- [x] 覆盖 reclaim descriptor 写入。
- [x] 覆盖 notify 失败后 descriptor 保持已发布。
- [x] 覆盖 pending bit 清除与并发入队竞态。
- [x] 覆盖跨接口 pending bit 设置不被清位覆盖。
- [x] 覆盖 RX/TX ring 接口不一致时消费坏 descriptor 并计数。

Linux IPC 基线：

- [x] `linux_app/test/linux_shm_ipc_test.c` 覆盖 format/attach、host map/unmap。
- [x] 覆盖 Frame Pool alloc/release/quota/global full。
- [x] 覆盖 RX commit、notify partial-success 和重复 frame 拒绝。
- [x] 覆盖 `RX_QUEUED` frame 不允许直接 release。
- [x] 覆盖 TX dequeue 成功、坏 descriptor 消费、未发布 frame 拒绝。
- [x] 覆盖 reclaim dequeue、reclaim ack 和来源/目标元数据错配拒绝。
- [x] 覆盖 pending clear 的跨接口和同接口竞态。

---

## 5. 新增测试清单

### 5.1 第一阶段路由核心测试

- [ ] CID 首字节路由到 CAN / Ethernet / Wi-Fi / Bluetooth / 4G / RS485。
- [ ] 保留 CID、未定义广播 CID、无路由进入 drop / mock reclaim。
- [ ] priority 0/1 优先，priority 2/3 不长期饿死。
- [ ] 非法 priority 不进入调度队列。
- [ ] TTL 过期不进入 TX mock。
- [ ] `ttl == 0` 不触发 TTL 过期。
- [ ] epoch mismatch 不进入 TX mock。
- [ ] auth failed、integrity failed、replay dropped 不进入 TX mock。
- [ ] invalid anyMSG 不进入 TX mock。
- [ ] `type = 0x00` 心跳更新端在线表，不进入 TX mock。
- [ ] gateway CID 未配置时心跳不更新端在线表。
- [ ] 本地队列满时按优先级丢弃并写 mock reclaim。
- [ ] TX mock 拥塞时按 priority 策略重试或丢弃。
- [ ] Recovery 清理本地队列引用并写 mock reclaim。
- [ ] 统计计数与实际处理路径一致。

### 5.2 第二阶段 IPC 适配测试

- [ ] RX Descriptor Ring drain 到第一阶段路由核心。
- [ ] Frame Pool 只读访问完整 anyMSG header。
- [ ] TX Ring 写入目标接口 descriptor。
- [ ] reclaim ring 写入公共 reclaim reason。
- [ ] IPC API 返回 CRC / bounds / interface 错误时不进入路由核心。
- [ ] Doorbell 失败后 descriptor 仍可通过 pending bitmap 和周期 drain 消费。
- [ ] reclaim ring 满时暂停继续消费 RX descriptor。
- [ ] Frame Pool 引用不在小核释放或清零。
- [ ] epoch 变化触发 Recovery。

### 5.3 第三阶段跨库闭环测试

- [ ] Linux `alloc + commit_rx`，RTOS drain/route，Linux `dequeue_tx`。
- [ ] CAN RX 到 RS485 TX。
- [ ] `type = 0x00` 心跳被 RTOS 消费，Linux 从 reclaim ring 回收。
- [ ] 无路由、TTL 过期、epoch mismatch、invalid frame 进入 reclaim。
- [ ] TX ring full 后 bounded retry 失败，最终 reclaim `PUT_SHM_RECLAIM_REASON_QUEUE_FULL`。
- [ ] reclaim ring full 时 RTOS 暂停继续消费 RX。
- [ ] 长时间闭环后 Linux Frame Pool `used` 回落稳定，无持续增长疑似泄漏。

---

## 6. 关键假设

- 本计划只指导 `rtos_firmware/` 小核后续实现，不修改公共 ABI。
- 第二阶段共享内存 ABI 以 `common/include/shared_memory_ipc.h`、接口文档和架构设计文档为准，不以第一阶段 mock 类型作为 ABI。
- `rtos_firmware/ipc` 当前实现视为第二阶段可用底座；后续若 API 返回码或平台 ops 细节变化，只调整 IPC 适配层和本 TODO 对应条目。
- `linux_app/ipc` 当前实现视为 host 闭环的 Linux 侧基线；真实协议入口和物理出口接入可在第三阶段逐步替换 mock。
- 板端 cache maintenance、Mailbox/CMDQU doorbell 以后续内核驱动或 ioctl 接口为准。

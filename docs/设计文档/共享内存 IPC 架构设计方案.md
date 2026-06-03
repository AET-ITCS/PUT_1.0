# 共享内存 IPC v2 架构设计方案

## Summary

- 共享内存 IPC 主线升级为 v2：`anyMSG + Frame Pool + 每接口 Descriptor Ring + Pending Bitmap + Mailbox Doorbell`。
- v1 `unified_frame_t + 128B slot + CAN direct` 只作为历史原型，不再作为后续主开发接口。
- Linux 负责真实物理收发、Frame Pool 分配和最终释放；FreeRTOS 小核只移动 descriptor、读取完整 anyMSG、执行路由调度并写 TX/reclaim descriptor。
- Web 仍只读 Linux 状态快照和日志，不直接访问共享内存。

## Frozen ABI

- 公共头文件：[common/include/shared_memory_ipc.h](/home/yuki/projects/PUT_1.0/common/include/shared_memory_ipc.h)。
- `PUT_SHM_IPC_VERSION = 2`，region 总大小固定 `64 KiB`。
- Frame Pool 固定为 `64 * 512B`，当前单个完整 anyMSG 最大承载 512B。
- 支持六类接口：CAN、Ethernet、Wi-Fi、Bluetooth、4G、RS485。
- 每个接口有独立 RX ring 和 TX ring，共 12 个 descriptor ring，每 ring 深度 8。
- descriptor 固定 64B，保存 `frame_id/frame_offset/frame_length/interface/cid/type/priority/ttl/epoch/flags/crc16`。
- `rx_pending_bitmap`、`tx_pending_bitmap`、`reclaim_pending` 分别独占一个 cache line。
- pending bitmap 的 `bits` 必须通过平台 atomic bit operation 更新，不能使用整条 cache line 的普通 RMW。
- reclaim ring 由 RTOS 写、Linux 读，用于通知 Linux 回收 Frame Pool block。
- descriptor `flags` 低 5 位冻结为入口可信性 ABI：`AUTH_OK`、`INTEGRITY_OK`、`REPLAY_OK`、`INTERNAL_TRUSTED`、`CONTROL_ALLOWED`。

## Data Flow

Linux 接入方向：

```text
物理接口 RX
  ↓ decode / reassemble
完整 anyMSG
  ↓ Linux 分配 Frame Pool block
写 Frame Pool
  ↓ 写对应接口 RX descriptor
设置 rx_pending_bitmap
  ↓ empty -> non-empty 时 Mailbox Doorbell
RTOS IPC Event Task
```

RTOS 路由方向：

```text
读取 rx_pending_bitmap
  ↓ drain 对应 RX ring
读取 descriptor 并定位 Frame Pool 中完整 anyMSG
  ↓ anyMSG header / epoch / TTL / heartbeat / CID route
进入 Router Scheduler
  ↓ 写目标接口 TX descriptor
设置 tx_pending_bitmap
  ↓ empty -> non-empty 时 Mailbox Doorbell
Linux 出口层真实发送
```

RTOS 丢弃或消费但不转发的帧：

```text
心跳消费 / 无路由 / TTL 过期 / epoch 不匹配 / 非法帧
  ↓
写 reclaim descriptor
  ↓
Linux 回收 Frame Pool block
```

## Runtime Rules

- `write_seq` 只由生产者写，`read_seq` 只由消费者写。
- `empty = write_seq == read_seq`。
- `full = write_seq - read_seq >= depth`。
- ring 满时丢弃最新 descriptor，不覆盖旧 descriptor，并递增 `drop_count`。
- descriptor CRC 使用 CRC-16/CCITT-FALSE，覆盖 `descriptor_crc16` 之前的字节。
- Doorbell 只在 ring 从 empty 变为 non-empty 时触发；队列持续非空时依赖 pending bitmap。
- Doorbell 失败发生在 `write_seq` 已发布之后时，不作为可重试发送失败返回，只递增 `notify_fail_count`。
- 清 pending bit 前必须二次确认当前 ring 仍为空，清位本身必须使用平台原子 AND；清位后还要重新读取 `producer.write_seq`，若发现同接口有新 descriptor，则必须原子 OR 置回 pending bit。
- descriptor 出队时按 ring 类型校验接口一致性：RX 要求 `source_interface == ring.interface_id`，TX 要求 `target_interface == ring.interface_id`。
- RTOS 不释放 Frame Pool，不清零 payload，只通过 reclaim ring 给 Linux 明确回收原因。
- priority 是 descriptor 元数据，不写入 anyMSG 保留字段。
- Wi-Fi、Ethernet、Bluetooth、4G 外部入口默认不可信；只有 Linux 接入层完成鉴权、完整性和重放检查后，才可设置 `AUTH_OK | INTEGRITY_OK | REPLAY_OK`。
- 外部入口访问 priority 0/1 或 CAN/RS485 控制路径时必须额外设置 `CONTROL_ALLOWED`，否则 RTOS 按非法帧 reclaim。
- CAN、RS485 等内部入口可由 Linux 标记 `INTERNAL_TRUSTED`，RTOS 不再无条件把所有 descriptor 视为 `AUTH_OK`。

## RTOS Implementation

- `rtos_firmware/` 是正式小核固件目录；`freertos/` 只作为历史参考。
- `rtos_firmware/ipc/rtos_shm_ipc.*` 提供 v2 descriptor API：
  - `rtos_shm_ipc_format_region()`
  - `rtos_shm_ipc_attach()`
  - `rtos_shm_ipc_dequeue_rx_descriptor()`
  - `rtos_shm_ipc_enqueue_tx_descriptor()`
  - `rtos_shm_ipc_reclaim_frame()`
  - `rtos_shm_ipc_get_frame_const()`
- IPC 层只校验共享内存 ABI、descriptor CRC、Frame Pool 边界和接口一致性，不解析 anyMSG payload。
- 小核后续路由任务在 IPC 层之上实现：RX drain、heartbeat table、route table、scheduler、TX writer、statistics。

## Linux Implementation

- `linux_app/ipc/linux_shm_ipc.*` 提供 Linux 侧 v2 IPC library，当前阶段先作为独立 static library 和 host 单测目标，不直接接入真实协议入口主流程。
- Linux 侧是 Frame Pool 唯一分配者和最终释放者，维护 64-bit allocation bitmap、每 frame 元数据、每接口 quota、pending reclaim、泄漏可疑计数和 full/high-watermark 统计。
- Linux 侧写 RX descriptor、读 TX descriptor、读 reclaim descriptor；ring 满时只返回错误并记录统计，不自动释放 frame，调用方负责决定丢弃、重试或释放。
- Linux 本地上下文需要通过 `linux_shm_ipc_init()` 或 `linux_shm_ipc_map()` 初始化；`format_region/attach` 只保留带初始化 magic 的映射生命周期字段。
- Frame Pool release 受状态机约束：`RX_QUEUED` frame 必须等待 RTOS reclaim，不能由公开 release API 直接释放。
- TX descriptor 和 reclaim descriptor 是 RTOS 回写入口，只能引用 `RX_QUEUED` frame；Linux 必须用本地 frame 状态和接口元数据拒绝未发布、已释放或来源/目标错配的 descriptor。
- `linux_shm_platform.*` 提供 host/mock 和 `/dev/mem` 两类后端。`/dev/mem` 后端当前只完成 `open("/dev/mem", O_RDWR | O_SYNC)` 与 `mmap()`，cache flush/invalidate 暂为 no-op，后续应由 kernel driver/ioctl 替换真实 cache maintenance 和 Mailbox/CMDQU doorbell。
- pending bitmap 清除规则与 RTOS 保持一致：atomic clear 后重新读取 `producer.write_seq`，若 ring 又变为非空，必须 atomic OR 置回对应 bit。

## Test Plan

- ABI 测试：`sizeof`、offset、region 64KiB、descriptor 64B、ring 控制行 64B、anyMSG header 40B。
- ring 测试：空读、写满、满写拒绝、顺序保持、CRC 错误消费、pending bit 设置/清除。
- Frame Pool 测试：合法 descriptor 能返回只读 anyMSG 指针；frame_id、offset、length 越界返回错误。
- reclaim 测试：心跳消费、无路由、TTL 过期、epoch 不匹配、非法帧等原因能写入 reclaim ring。
- notify 测试：empty -> non-empty 触发 doorbell；notify 失败后 descriptor 仍可消费并累计 `notify_fail_count`。
- Linux host 测试：reserved-memory mock 映射、Frame Pool alloc/release/quota/full、RX enqueue、TX dequeue、reclaim ack、pending clear 竞态和 notify partial-success。
- 集成测试：任一入口 anyMSG 能进入 RX ring，小核路由到目标 TX ring，Linux 出口层读取并释放 Frame Pool。

## Assumptions

- 当前阶段保留 64KiB region；后续硬件 DTS/BSP/linker 如需扩容，需要升级 ABI 小版本。
- 单帧 512B 是 v2 初始默认值；大于 512B 的外部数据由 Linux 物理适配层分片重组或后续扩容处理。
- Linux 是 Frame Pool 唯一分配者和最终释放者。
- 小核不直接操作 CAN、RS485、Ethernet、Wi-Fi、Bluetooth、4G 硬件。

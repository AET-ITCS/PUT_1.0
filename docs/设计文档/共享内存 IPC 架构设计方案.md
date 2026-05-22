# 共享内存 IPC 架构设计方案

## Summary

- 当前工程没有实际 `rtos_firmware/`，小核代码在 `freertos/cvitek/task/comm/`；README 中的 `rtos_firmware` 是目标结构描述。
- v1 架构以 `unified_frame_t` 作为 Linux → RTOS 的正式业务 payload，长度固定 96 字节；共享内存层只负责传输、cache 同步、通知和错误返回，不解释 CAN 业务。
- 采用双向 SPSC ring：`linux_to_rtos` 传业务帧和 heartbeat，`rtos_to_linux` 传 CAN RX、status、event；Web 仍只读 Linux 生成的 `/run/put/status/` 快照，不直接访问共享内存。

## 已冻结接口（第一步）

- `rtos_firmware/` 是后续正式小核固件目标目录；`freertos/` 只作为历史参考实现和行为参考，后续主开发不直接在 `freertos/` 上继续堆叠。
- `common/include/unified_frame.h` 保持当前 v1 定义不改：`UNIFIED_FRAME_LENGTH = 96`、`UNIFIED_FRAME_VERSION = 0x01`、CRC 覆盖前 94 字节。
- `common/include/shared_memory_ipc.h` 已冻结共享内存 IPC v1 ABI：64 KiB region、双向 SPSC ring、32 个 256 字节 slot、128 字节 payload、64 字节 cache line 对齐。
- Linux → RTOS 的业务消息固定使用 `PUT_SHM_MESSAGE_TYPE_UNIFIED_FRAME`，payload 为完整 `unified_frame_t`。
- RTOS → Linux 的 CAN RX 回传固定使用 `PUT_SHM_MESSAGE_TYPE_CAN_RX`，payload 为 `put_shm_can_rx_payload_t`，不复用 `UNIFIED_FRAME`。
- RTOS → Linux 的状态和事件使用公共 packed payload：`put_shm_status_payload_t` 和 `put_shm_event_payload_t`，不裸传 `freertos/` 私有结构。
- heartbeat / hello / ready 使用 `put_shm_heartbeat_payload_t`，Linux 重启后通过递增 `linux_epoch` 触发重新握手。
- v1 CAN 边界冻结为经典 CAN：标准 ID `0x000 ~ 0x7FF`，扩展 ID `0x00000000 ~ 0x1FFFFFFF`，DLC `0 ~ 8`，不支持 CAN FD、BRS、RTR。
- IPC 专用错误码已冻结在 `UNIFIED_ERR_IPC_*` 区间；队列满、未就绪、通知失败、offline 等状态必须返回明确错误码。
- 第一阶段只冻结接口和文档，不实现 ring 读写、不替换 `ipc_to_rtos_send()` stdout stub、不接入 cmdqu/mailbox、不创建完整 `rtos_firmware/` 工程骨架。

## Key Changes

- 在 [common/include/shared_memory_ipc.h](/home/yuki/projects/PUT_1.0/common/include/shared_memory_ipc.h) 固化公共 ABI：
  - `PUT_SHM_IPC_VERSION = 1`
  - `PUT_SHM_PAYLOAD_MAX_LEN = 128`
  - `PUT_SHM_SLOT_SIZE = 256`
  - `PUT_SHM_L2R_DEPTH = 32`
  - `PUT_SHM_R2L_DEPTH = 32`
  - reserved shared memory 总大小按 `64 KiB` 规划，物理地址由 DTS 和 RTOS BSP 配置共同提供。
- slot 使用固定头 + payload：
  - `magic/version/type/sequence/length/crc16/payload[128]/reserved`
  - slot 按 64-byte cache line 对齐；ring 的 producer-owned 和 consumer-owned 索引分离到不同 cache line。
- ring 使用单生产者单消费者单调计数：
  - `write_seq` 只由生产者写，`read_seq` 只由消费者写。
  - empty：`write_seq == read_seq`
  - full：`write_seq - read_seq >= depth`
  - 满队列丢弃最新消息，不覆盖旧帧。
- 消息类型：
  - `UNIFIED_FRAME`：Linux → RTOS，payload 为 `unified_frame_t`
  - `CAN_RX`：RTOS → Linux，payload 为 `put_shm_can_rx_payload_t`
  - `HEARTBEAT`：Linux 周期下发，RTOS 更新 `rtos_recovery`
  - `STATUS`：RTOS 周期回传公共 packed status
  - `EVENT`：RTOS 回传错误事件
- 错误码补齐：
  - `UNIFIED_ERR_IPC_QUEUE_EMPTY`
  - `UNIFIED_ERR_IPC_QUEUE_FULL`
  - `UNIFIED_ERR_IPC_NOT_READY`
  - `UNIFIED_ERR_IPC_NOTIFY_FAILED`
  - `UNIFIED_ERR_IPC_OFFLINE`

## Data Flow

- Linux 发送路径保持现有业务边界：
  - `protocol_manager` → `frame_packer_pack()` → `ipc_to_rtos_send()`
  - 只替换 [linux_app/ipc/ipc_to_rtos.c](/home/yuki/projects/PUT_1.0/linux_app/ipc/ipc_to_rtos.c) 的 stdout stub 为 shared memory enqueue + doorbell。
- RTOS 接收路径接入现有小核边界：
  - shared ring dequeue → `rtos_ipc_poll_linux_payload()` → `rtos_protocol_adapter_linux_payload_to_can()` → CAN TX queue。
- cache / barrier 顺序固定：
  - writer：写 slot → flush slot → memory barrier → 更新 `write_seq` → flush producer line → doorbell。
  - reader：invalidate producer line → 判断可读 → invalidate slot → 校验 slot CRC 和 payload length → 处理 → 更新 `read_seq` → flush consumer line。
- 通知方式：
  - v1 用 `/dev/cvi-rtos-cmdqu` 或 BSP mailbox 作为 doorbell，只通知“对应 ring 有新数据”，不承载业务 payload。
  - 通知丢失时，接收端通过周期 drain 兜底；ring 是唯一数据源。
  - 若消息 slot 和 `write_seq` 已发布，doorbell 失败不再作为可重试发送失败返回；发送方递增 `notify_fail_count`，接收方依靠周期 drain 读取已入队消息。

## Recovery

- Linux 启动或重启时递增 `linux_epoch`，清空 shared rings，发送 `HELLO/HEARTBEAT`。
- RTOS 发现新 epoch 后丢弃旧 TX 队列、进入握手流程，READY 后才允许业务帧进入 CAN TX。
- Linux heartbeat 超过 `RTOS_LINUX_HEARTBEAT_TIMEOUT_MS` 后，RTOS 进入 fail-safe offline：
  - 禁止 TX
  - 清空 CAN TX queue
  - abort XL2515 TX buffer
  - 切 Listen-Only
  - 保留 CAN RX、status、event 回传能力

## Test Plan

- ABI 测试：`sizeof`、offset、slot 256-byte、payload 128-byte、`unified_frame_t` 96-byte 兼容。
- ring 测试：空读、写满、满写拒绝、回绕、顺序保持、CRC 错误、magic/version 错误。
- Linux 单测：`ipc_to_rtos_send()` 对 NULL、未初始化、队列满返回正确错误码；notify 在 `write_seq` 发布后失败时必须验证消息已入队且不会触发重试语义。
- RTOS host 测试：真实 ring 替换 mock queue 后，现有 `rtos_protocol_adapter` 和 CAN forward 测试继续通过。
- 硬件联调：cache 一致性、cmdqu doorbell、Linux 重启重握手、heartbeat timeout、CAN 高负载下不发送旧帧。

## Assumptions

- v1 只支持经典 CAN 发送；`unified_frame_t` 中 CAN FD flag 继续由 RTOS adapter 拒绝。
- 共享内存物理地址暂不硬编码在业务层，由 Linux DTS reserved-memory 和 RTOS BSP/linker 配置统一注入。
- status/event 不直接复用 RTOS 私有结构裸传，已在 `common/include/shared_memory_ipc.h` 中冻结为 packed 公共 payload，避免 bool/padding ABI 差异。

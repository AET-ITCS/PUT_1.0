# 大小核共享内存 IPC 接口预留说明

## 1. 文档定位

本文不是共享内存实现方案，也不是完整共享内存 ABI 定义。

当前项目中，共享内存模块暂未实现。本文件只说明大核协议转换层和后续共享内存模块之间需要预留的最小对接接口，方便共享内存负责人后续接入。

当前占位头文件为：

```text
common/include/shared_memory_ipc.h
```

当前最小接口为：

```c
typedef unified_error_t (*shared_memory_ipc_send_fn_t)(const unified_frame_t *frame);
```

含义是：共享内存模块后续需要提供一个“可以发送一帧 `unified_frame_t`”的能力。

---

## 2. 当前项目状态

当前大核发送链路为：

```text
外部协议 parser
    ↓
protocol_parsed_msg_t
    ↓
frame_packer_pack()
    ↓
unified_frame_t
    ↓
ipc_to_rtos_send()
```

其中：

```text
linux_app/ipc/ipc_to_rtos.c
```

目前只是打印 stub，并没有写共享内存。

共享内存模块完成后，只需要替换 `ipc_to_rtos_send()` 内部实现，或接入一个兼容 `shared_memory_ipc_send_fn_t` 的发送函数。协议 parser、`frame_packer` 和 `unified_frame_t` 格式不需要因为共享内存实现细节而修改。

---

## 3. 最小接口契约

共享内存发送函数需要满足：

| 项目 | 要求 |
| ---- | ---- |
| 输入 | `const unified_frame_t *frame` |
| 帧长度 | 固定为 `UNIFIED_FRAME_LENGTH`，即 96 字节 |
| 成功返回 | `UNIFIED_OK` |
| 失败返回 | `unified_error_t` 公共错误码 |
| 输入所有权 | 调用方保留，发送函数不得修改 `frame` |
| 阻塞要求 | 不应长时间阻塞大核协议转换层 |
| 内部实现 | 可由共享内存负责人选择 ring、mailbox、DMA 或其他结构 |

调用方只关心：

```text
这一帧是否成功交给小核发送通道
```

调用方不关心：

```text
共享内存物理地址
ring queue 元数据
cache flush / invalidate API
cmdqu / mailbox ioctl 格式
doorbell 具体实现
```

---

## 4. 职责边界

| 模块 | 负责 | 不负责 |
| ---- | ---- | ------ |
| 大核协议 parser | 解析外部协议，生成 `protocol_parsed_msg_t` | 共享内存写入 |
| `frame_packer` | 生成完整 `unified_frame_t`，填 CRC | 共享内存通知 |
| `ipc_to_rtos_send()` | 调用共享内存发送能力 | 外部协议解析 |
| 共享内存模块 | 写共享内存、cache 同步、通知小核 | CAN 业务语义 |
| FreeRTOS 小核 | 接收并校验 `unified_frame_t`，转发 CAN | Linux 外部协议细节 |
| Web 模块 | 读取 Linux 生成的状态快照 | 直接访问共享内存或小核 |

---

## 5. 对共享内存负责人的接入要求

后续共享内存模块实现时，需要保证：

1. 能接收一帧完整 `unified_frame_t`。
2. 不修改调用方传入的 `frame`。
3. 写入共享内存前后自行处理必要的 cache flush / memory barrier。
4. 使用 cmdqu / mailbox / doorbell 通知小核时，不要求大核业务层感知通知细节。
5. 队列满、设备未就绪、参数错误等失败场景返回公共错误码。
6. Linux 心跳超时或小核 offline 时，不继续发送旧业务帧。

建议错误语义：

| 场景 | 建议返回 |
| ---- | -------- |
| `frame == NULL` | `UNIFIED_ERR_NULL` |
| 共享内存未初始化 | `UNIFIED_ERR_INVALID_ARG` 或后续新增专用错误码 |
| 发送队列满 | 复用队列满语义或后续新增专用错误码 |
| 小核 offline | 后续新增专用错误码 |
| 通知小核失败 | `UNIFIED_ERR_INVALID_ARG` 或后续新增专用错误码 |

当前公共错误码还没有专门的 IPC 错误段，后续共享内存模块实现时可以再补充，例如：

```text
UNIFIED_ERR_IPC_NOT_READY
UNIFIED_ERR_IPC_QUEUE_FULL
UNIFIED_ERR_IPC_NOTIFY_FAILED
UNIFIED_ERR_IPC_OFFLINE
```

---

## 6. 暂不在本接口中定义的内容

以下内容由共享内存负责人后续决定，本接口预留文档不提前写死：

- 共享内存物理地址。
- 共享内存总大小。
- Linux 设备树 `reserved-memory` 名称。
- FreeRTOS linker script 段名。
- ring queue 的 metadata 布局。
- slot 数量。
- cmdqu / mailbox 的具体命令格式。
- cache flush / invalidate 的具体 SDK API。
- RTOS -> Linux 回传是否复用同一共享内存通道。

---

## 7. 与小核方案的关系

`docs/设计文档/FreeRTOS核plan.md` 中的小核方案只假设：

```text
小核最终能够收到完整 unified_frame_t
```

至于这帧是通过共享内存 ring、cmdqu doorbell、mailbox 通知还是其他底层机制送达，不属于小核 CAN 转发业务逻辑本身。

小核收到帧后仍按以下流程处理：

```text
接收 unified_frame_t
    ↓
Frame Validator
    ↓
CAN TX Queue
    ↓
CAN_TX_Task / XL2515
```

---

## 8. 后续验收建议

共享内存模块完成后，至少需要验证：

- 大核调用 `ipc_to_rtos_send()` 后，小核能收到同一帧 `unified_frame_t`。
- 小核校验 `magic/version/CRC/can_dlc/can_id` 正常。
- 发送队列满时不会覆盖旧帧。
- 小核 offline / Linux 心跳超时时不会继续发送旧业务帧。
- Web 仍只通过 `/run/put/status/` 查看状态，不直接访问共享内存。


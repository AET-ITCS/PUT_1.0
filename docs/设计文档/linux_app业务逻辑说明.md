# linux_app 业务逻辑说明

## 1. 当前定位

`linux_app` 是大核 Linux 侧的协议转换应用。

当前第一版目标是跑通大核侧最小链路：

```text
外部协议输入
  ↓
协议解析
  ↓
协议中间消息 protocol_parsed_msg_t
  ↓
统一帧打包 unified_frame_t
  ↓
发送给小核接口 ipc_to_rtos_send
```

目前已经实现的是 **以太网 UDP 调试帧 → 统一帧** 的最小闭环。

真实共享内存发送暂时还没有接入，当前 `ipc_to_rtos_send()` 先用打印方式作为 stub。

---

## 2. 当前目录结构

```text
linux_app/
├── CMakeLists.txt
├── main.c
├── core/                    # 大核协议转换核心流程
│   ├── protocol_manager.c   # 协议接收、解析、打包、发送的调度逻辑
│   ├── protocol_manager.h
│   ├── protocol_parsed_msg.h# 协议解析后的中间消息结构
│   ├── frame_packer.c       # protocol_parsed_msg_t → unified_frame_t
│   └── frame_packer.h
├── 以太网/                  # 以太网相关协议解析
│   ├── ethernet_udp.c       # UDP 调试帧解析实现
│   ├── ethernet_udp.h
│   ├── ethernet_status.c    # Web 业务状态快照写出
│   └── ethernet_status.h
└── ipc/                     # 大核到小核发送接口
    ├── ipc_to_rtos.c        # 当前为打印 stub，后续替换为共享内存发送
    └── ipc_to_rtos.h
```

目录分工：

| 目录 | 作用 |
|---|---|
| `core/` | 与具体外部协议无关的核心流程，例如协议调度、统一帧打包 |
| `以太网/` | 以太网协议相关解析，目前是 UDP 调试帧 |
| `ipc/` | 大核到小核发送接口，目前等待共享内存实现接入 |

---

## 3. 当前完整业务流

当前业务流如下：

```text
tools/send_udp_frame.py
  ↓
发送 UDP 调试帧到 linux_app
  ↓
main.c
  ↓
protocol_manager_run_udp()
  ↓
recvfrom() 接收 UDP payload
  ↓
ethernet_udp_parse_frame()
  ↓
protocol_parsed_msg_t
  ↓
frame_packer_pack()
  ↓
unified_frame_t
  ↓
ipc_to_rtos_send()
  ↓
当前打印 unified_frame_t，后续写入共享内存
```

---

## 4. main.c

`main.c` 是程序入口。

主要职责：

```text
1. 解析命令行参数
2. 初始化 frame_packer 的 sequence
3. 启动 UDP 协议管理循环
```

支持参数：

```bash
./build/linux_app/linux_app --udp-port 5000
```

表示监听 UDP 端口 `5000`。

也可以用于测试：

```bash
./build/linux_app/linux_app --udp-port 5000 --max-packets 1
```

表示只处理 1 个 UDP 包后退出。

Web 状态快照默认写入：

```bash
/run/put/status
```

开发机上如果没有 `/run/put/status` 写权限，可以指定临时目录：

```bash
./build/linux_app/linux_app --udp-port 5000 --status-dir /tmp/put/status
```

如果只想验证协议链路、不写 Web 状态文件：

```bash
./build/linux_app/linux_app --udp-port 5000 --disable-status
```

---

## 5. protocol_manager.c

`protocol_manager.c` 是当前业务主流程调度器。

它负责串联各个模块：

```text
创建 UDP socket
  ↓
bind 到指定端口
  ↓
循环 recvfrom 等待 UDP 数据
  ↓
调用 ethernet_udp_parse_frame()
  ↓
调用 frame_packer_pack()
  ↓
调用 ipc_to_rtos_send()
```

当前只接入了以太网 UDP 调试帧。

同时，`protocol_manager.c` 会把以太网 UDP 链路的运行状态交给
`ethernet_status.c`，周期写出给 Web 后端只读展示的快照：

```text
/run/put/status/modules.json
/run/put/status/ipc_status.json
/run/put/status/can_status.json
/run/put/status/events.jsonl
```

写文件采用“临时文件 + rename”的原子替换方式，避免 Web 后端读到半截 JSON。
当前第一版只有以太网 UDP 真实统计，4G/WiFi/蓝牙/RS485 暂时输出 `unknown`，
小核 CAN 回传暂未接入时 `can_status.json` 也输出 `unknown`。

后续如果增加 RS485、TCP、WiFi、蓝牙、4G，不应该改变后面的统一打包流程，而是新增各自的 parser：

```text
RS485 原始帧  → rs485_parse_frame()      → protocol_parsed_msg_t
TCP 私有帧    → ethernet_tcp_parse()     → protocol_parsed_msg_t
WiFi 数据     → wifi_parse_frame()       → protocol_parsed_msg_t
蓝牙数据      → bluetooth_parse_frame()  → protocol_parsed_msg_t
4G/MQTT 数据  → four_g_parse_frame()     → protocol_parsed_msg_t
```

然后统一走：

```text
protocol_parsed_msg_t → frame_packer_pack() → ipc_to_rtos_send()
```

---

## 6. ethernet_udp.c

`ethernet_udp.c` 负责解析调试用 UDP 简单二进制帧。

该协议只是第一阶段调试入口，不是最终正式业务协议。

解析时会检查：

```text
1. 输入指针是否为空
2. 长度是否为 76 字节
3. magic 是否为 0x55AA
4. version 是否为 0x01
5. CRC16 是否正确
6. vehicle_type 是否合法
7. can_dlc 是否符合 CAN / CAN FD 规则
8. can_id 是否符合标准帧 / 扩展帧范围
```

解析成功后输出：

```text
protocol_parsed_msg_t
```

其中：

```text
source_protocol = PROTOCOL_TYPE_ETHERNET
source_id = 0
destination_id = 0
```

其他字段来自 UDP 调试帧：

```text
vehicle_type
can_flags
can_dlc
can_id
can_data
```

## 7. ethernet_status.c

`ethernet_status.c` 负责把 `linux_app` 第一版以太网 UDP 链路的业务状态写成
Web 后端可读取的 JSON/JSONL 快照。

它记录：

```text
1. UDP 接收包数 rx_count
2. 成功送入 ipc_to_rtos_send 的帧数 tx_count
3. 接收字节数 rx_bytes
4. 解析错误、打包错误、IPC 发送错误计数
5. 最近接收时间 last_seen_ms
6. 最近发送时间 last_tx_ms
7. 最近错误时间、阶段和错误码
```

生成文件：

| 文件 | 作用 |
|---|---|
| `modules.json` | 4G/WiFi/蓝牙/以太网/RS485 模块状态；当前以太网为真实统计，其它为 `unknown` |
| `ipc_status.json` | 大核到小核发送路径统计；当前基于 `ipc_to_rtos_send()` stub 返回值 |
| `can_status.json` | 小核 CAN 回传状态；当前共享内存回传未接入，输出 `unknown` 占位 |
| `events.jsonl` | 解析、打包、IPC 等异常事件，每行一个 JSON |

`linux_app` 仍然不提供 HTTP API，也不托管 Web 页面；这些文件只供后续
Rust `put-webd` 后端只读读取。

---

## 8. protocol_parsed_msg_t

`protocol_parsed_msg_t` 是协议解析后的中间消息。

定义位置：

```text
linux_app/core/protocol_parsed_msg.h
```

它的作用是承接各类外部协议解析结果：

```c
typedef struct {
    protocol_type_t source_protocol;
    uint8_t vehicle_type;
    uint32_t source_id;
    uint32_t destination_id;
    uint32_t can_id;
    uint8_t can_dlc;
    uint8_t can_flags;
    uint8_t can_data[64];
} protocol_parsed_msg_t;
```

可以简单理解为：

```text
protocol_parsed_msg_t 只描述“要发什么 CAN 数据”
```

它不关心：

```text
unified_frame_t.magic
unified_frame_t.version
unified_frame_t.sequence
unified_frame_t.timestamp_ms
unified_frame_t.crc16
```

这些由 `frame_packer` 统一处理。

---

## 9. frame_packer.c

`frame_packer.c` 负责把：

```text
protocol_parsed_msg_t
```

转换成：

```text
unified_frame_t
```

它负责填写统一帧公共字段：

```text
magic = 0xA55A
version = 0x01
frame_type = UNIFIED_FRAME_TYPE_CAN_DATA
source_protocol
vehicle_type
can_dlc
can_flags
sequence
timestamp_ms
source_id
destination_id
can_id
can_data
crc16
```

同时统一做校验：

```text
source_protocol 是否合法
vehicle_type 是否合法
can_dlc 是否符合 CAN / CAN FD
can_id 是否符合标准帧 / 扩展帧范围
```

这样可以保证后续所有协议使用同一套统一帧打包规则。

---

## 10. ipc_to_rtos.c

`ipc_to_rtos.c` 是大核发送给小核的接口。

当前实现是 stub：

```text
收到 unified_frame_t
  ↓
打印 sequence、source_protocol、vehicle_type、can_id、dlc、flags、crc 和 data
```

示例输出：

```text
[ipc_stub] seq=1 src_proto=0x04 vehicle=0x49 can_id=0x123 dlc=8 flags=0x00 crc=0x8359 data=01 02 03 04 05 06 07 08
```

后续队友完成共享内存模块后，只需要替换：

```c
ipc_to_rtos_send()
```

函数内部实现即可。

调用方不需要改。

---

## 11. 当前已经完成的能力

当前已经完成：

```text
UDP 调试帧接收
UDP 调试帧解析
协议中间消息结构
统一帧打包
CRC16 计算
sequence 递增
timestamp_ms 填充
CAN DLC 校验
CAN ID 校验
IPC 发送接口 stub
Web 状态快照文件写出
单元测试
协议文档
```

也就是说：

```text
大核协议转换层第一版骨架已经跑通
```

---

## 12. 当前还没完成的能力

当前还没有完成：

```text
真实共享内存发送
TCP 模式
RS485 / Modbus
WiFi
蓝牙
4G / MQTT
CAN 状态回传
配置文件读取
日志系统
Rust Web 后端 / Vue 前端
```

---

## 13. 后续扩展原则

后续新增协议时，不要让每个协议模块直接生成 `unified_frame_t`。

推荐统一遵守：

```text
某协议原始数据 → protocol_parsed_msg_t → frame_packer_pack() → unified_frame_t
```

这样做的好处：

```text
协议解析逻辑和统一帧打包逻辑分离
统一帧规则只在 frame_packer.c 维护
新增协议时只需要新增 parser
测试时能分别定位 parser 问题和 packer 问题
```

---

## 14. 运行和测试

构建大核应用：

```bash
./scripts/build_linux_app.sh
```

运行单元测试：

```bash
./scripts/test_linux_app.sh
```

启动 UDP 接收端：

```bash
./build/linux_app/linux_app --udp-port 5000
```

发送一帧测试数据：

```bash
./tools/send_udp_frame.py 127.0.0.1 --port 5000 --vehicle light --can-id 0x123 --data 0102030405060708
```

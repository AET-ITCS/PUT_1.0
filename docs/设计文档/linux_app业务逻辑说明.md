# linux_app 业务逻辑说明

## 1. 当前定位

`linux_app` 是大核 Linux 侧的多协议 CAN 网关应用。当前定位是 **只做数据转发，不解释应用层业务**：

```text
UDP / RS485 / 后续其它外部通道
  ↓
提取 CAN 字段：can_id / can_flags / can_dlc / can_data
  ↓
protocol_parsed_msg_t(vehicle_type = RAW_CAN)
  ↓
frame_packer_pack() 打包 unified_frame_t
  ↓
ipc_to_rtos_send() 交给小核
  ↓
小核发送真实 CAN / CAN FD
```

灯光、车窗、座椅等业务含义由使用网关的应用方决定，`linux_app` 不再内置寄存器映射或业务规则。

真实共享内存发送暂时还没有接入，当前 `ipc_to_rtos_send()` 先用打印方式作为 stub。

---

## 2. 当前目录结构

```text
linux_app/
├── CMakeLists.txt
├── main.c
├── config/
│   └── device_config.ini
├── core/
│   ├── app_config.c/.h        # INI 配置解析
│   ├── can_direct_frame.c/.h  # UDP/RS485 共用 CAN direct 网关帧解析
│   ├── protocol_manager.c/.h  # 多协议 worker 调度和公共 pipeline
│   ├── status_collector.c/.h  # Web 状态快照写出
│   ├── protocol_parsed_msg.h  # 协议解析后的中间消息结构
│   └── frame_packer.c/.h      # protocol_parsed_msg_t → unified_frame_t
├── ethernet/
│   └── ethernet_udp.c/.h      # UDP 入口，调用 can_direct parser
├── rs485/
│   └── rs485_debug.c/.h       # RS485 入口，按 AA 55 同步 CAN direct 帧
└── ipc/
    └── ipc_to_rtos.c/.h       # 大核到小核发送接口，当前为 stub
```

目录分工：

| 目录 | 作用 |
|---|---|
| `core/` | 与具体外部通道无关的核心流程，例如 CAN direct 解析、协议调度、统一帧打包 |
| `ethernet/` | UDP socket 接收入口 |
| `rs485/` | RS485 串口接收和固定帧同步 |
| `ipc/` | 大核到小核发送接口，目前等待共享内存实现接入 |

---

## 3. 当前完整数据流

```text
main.c
  ↓
读取 linux_app/config/device_config.ini
  ↓
protocol_manager_run()
  ↓
按配置启动 UDP worker / RS485 worker
  ↓
ethernet_udp_parse_frame() / rs485_debug_parse_frame()
  ↓
can_direct_parse_frame()
  ↓
protocol_parsed_msg_t
  ↓
frame_packer_pack()
  ↓
unified_frame_t
  ↓
ipc_to_rtos_send()
```

公共 pipeline 使用 mutex 保护 `frame_packer` sequence 和 IPC 发送路径，避免多个协议线程并发打包/发送造成竞态。

---

## 4. 配置与启动

推荐启动：

```bash
./build/linux_app/linux_app --config linux_app/config/device_config.ini
```

示例配置：

```ini
[ethernet_udp]
enabled = true
port = 5000
protocol = can_direct

[rs485]
enabled = true
dev = /dev/ttyS1
baud = 115200
protocol = can_direct

[status]
enabled = true
dir = /run/put/status
```

兼容命令行参数：

```bash
./build/linux_app/linux_app --udp-port 5000 --max-packets 1
./build/linux_app/linux_app --udp-port 5000 --status-dir /tmp/put/status
./build/linux_app/linux_app --udp-port 5000 --disable-status
```

---

## 5. CAN direct 网关帧

UDP payload 和 RS485 串口帧共用 76 字节 CAN direct 格式：

```text
magic(2)     = 0x55AA，小端，线上字节 AA 55
version(1)   = 0x01
reserved(1)  = 0
can_flags(1)
can_dlc(1)
can_id(4)    = 小端
can_data(64)
crc16(2)     = CRC-16/CCITT-FALSE，小端，覆盖前 74 字节
```

解析规则：

```text
1. 长度必须为 76 字节
2. magic 必须为 0x55AA
3. version 必须为 0x01
4. reserved 必须为 0
5. CRC16 必须正确
6. 普通 CAN：can_dlc <= 8
7. CAN FD：can_dlc <= 64
8. 标准帧：can_id <= 0x7FF
9. 扩展帧：can_id <= 0x1FFFFFFF
```

解析成功后：

```text
source_protocol = PROTOCOL_TYPE_ETHERNET 或 PROTOCOL_TYPE_RS485
vehicle_type    = VEHICLE_MSG_TYPE_RAW_CAN
source_id       = 0
destination_id  = 0
```

---

## 6. Web 状态快照

`status_collector.c` 负责把多协议链路状态写到：

```text
/run/put/status/modules.json
/run/put/status/ipc_status.json
/run/put/status/can_status.json
/run/put/status/events.jsonl
```

当前 UDP 和 RS485 输出真实收发/错误统计，4G/WiFi/蓝牙暂时输出 `unknown/disabled`。

Web 状态中：

```text
ethernet.protocol = can_direct
rs485.protocol    = can_direct
```

---

## 7. 测试

构建和测试：

```bash
cmake --build build/linux_app
cmake --build build/linux_app_test
ctest --test-dir build/linux_app_test --output-on-failure
```

发送一帧 UDP CAN direct 测试数据：

```bash
./tools/send_udp_frame.py 127.0.0.1 --port 5000 --can-id 0x123 --data 0102030405060708
```

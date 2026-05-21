# CAN direct 网关帧协议（UDP / RS485 共用）

## 1. 协议定位

本文档描述 `linux_app` 纯网关模式下的外部承载帧。UDP 和 RS485 使用同一种 76 字节二进制格式，payload 只承载 CAN 字段，不解释灯光、车窗、座椅等应用含义。

主流程：

```text
UDP / RS485 收到 CAN direct 网关帧
  ↓
提取 can_id / can_flags / can_dlc / can_data
  ↓
protocol_parsed_msg_t(vehicle_type = RAW_CAN)
  ↓
frame_packer_pack() 打包 unified_frame_t
  ↓
ipc_to_rtos_send() 交给小核
  ↓
小核按 CAN 字段发送真实 CAN / CAN FD
```

## 2. 基本参数

| 项目 | 说明 |
|---|---|
| 协议名称 | CAN direct 网关帧 |
| 当前用途 | 纯网关转发 CAN 字段 |
| 传输方式 | UDP payload / RS485 字节流 |
| UDP 默认端口 | `5000` |
| RS485 分帧 | 按 `AA 55` 帧头同步，固定读取 76 字节 |
| 帧长度 | 固定 `76` 字节 |
| 字节序 | little-endian |
| 接收解析代码 | `linux_app/core/can_direct_frame.c` |
| UDP 入口 | `linux_app/ethernet/ethernet_udp.c` |
| RS485 入口 | `linux_app/rs485/rs485_debug.c` |
| 发送测试工具 | `tools/send_udp_frame.py` |

## 3. 帧格式

所有多字节字段均为 **little-endian**。

```text
+--------+---------+----------+-----------+---------+--------+--------------+-------+
| magic  | version | reserved | can_flags | can_dlc | can_id | can_data[64] | crc16 |
| 2 Byte | 1 Byte  | 1 Byte   | 1 Byte    | 1 Byte  | 4 Byte | 64 Byte      | 2Byte |
+--------+---------+----------+-----------+---------+--------+--------------+-------+
```

| 偏移 | 长度 | 字段 | 类型 | 说明 |
|---:|---:|---|---|---|
| 0 | 2 | `magic` | `uint16_t` | 固定 `0x55AA`，线上字节为 `AA 55` |
| 2 | 1 | `version` | `uint8_t` | 当前固定 `0x01` |
| 3 | 1 | `reserved` | `uint8_t` | 保留，当前必须为 `0x00` |
| 4 | 1 | `can_flags` | `uint8_t` | CAN 标志位 |
| 5 | 1 | `can_dlc` | `uint8_t` | CAN 数据字节数 |
| 6 | 4 | `can_id` | `uint32_t` | CAN 标准 ID 或扩展 ID |
| 10 | 64 | `can_data` | `uint8_t[64]` | CAN / CAN FD 数据区，不足 64 字节补 0 |
| 74 | 2 | `crc16` | `uint16_t` | CRC-16/CCITT-FALSE，覆盖前 74 字节 |

## 4. 字段说明

### 4.1 `can_flags`

| 值 | 名称 | 含义 |
|---:|---|---|
| `0x00` | `UNIFIED_CAN_FLAG_NONE` | 普通 CAN 标准数据帧 |
| `0x01` | `UNIFIED_CAN_FLAG_EXTENDED_ID` | 使用 29-bit 扩展 ID |
| `0x02` | `UNIFIED_CAN_FLAG_FD` | 使用 CAN FD，允许 `can_dlc <= 64` |
| `0x04` | `UNIFIED_CAN_FLAG_RTR` | 远程帧，预留 |
| `0x08` | `UNIFIED_CAN_FLAG_BRS` | CAN FD BRS，预留 |

### 4.2 合法性规则

| 项目 | 条件 | 合法范围 |
|---|---|---|
| 普通 CAN 长度 | `can_flags` 不包含 `UNIFIED_CAN_FLAG_FD` | `can_dlc <= 8` |
| CAN FD 长度 | `can_flags` 包含 `UNIFIED_CAN_FLAG_FD` | `can_dlc <= 64` |
| 标准帧 ID | `can_flags` 不包含 `UNIFIED_CAN_FLAG_EXTENDED_ID` | `can_id <= 0x7FF` |
| 扩展帧 ID | `can_flags` 包含 `UNIFIED_CAN_FLAG_EXTENDED_ID` | `can_id <= 0x1FFFFFFF` |

### 4.3 `crc16`

| 参数 | 值 |
|---|---:|
| 算法 | CRC-16/CCITT-FALSE |
| poly | `0x1021` |
| init | `0xFFFF` |
| refin/refout | `false / false` |
| xorout | `0x0000` |
| 覆盖范围 | 偏移 `0 ~ 73`，共 74 字节 |

## 5. 解析成功后的映射关系

| CAN direct 字段 | `protocol_parsed_msg_t` 字段 |
|---|---|
| 输入通道 | `source_protocol = PROTOCOL_TYPE_ETHERNET` 或 `PROTOCOL_TYPE_RS485` |
| 固定 RAW CAN | `vehicle_type = VEHICLE_MSG_TYPE_RAW_CAN` |
| `can_flags` | `can_flags` |
| `can_dlc` | `can_dlc` |
| `can_id` | `can_id` |
| `can_data[64]` | `can_data[64]` |
| 无 | `source_id = 0` |
| 无 | `destination_id = 0` |

`linux_app` 不解释 `can_data` 的业务含义。应用方自己决定 CAN ID 和数据内容代表什么。

## 6. 示例

发送一帧普通 CAN 标准帧：

```text
can_flags = 0x00
can_dlc   = 8
can_id    = 0x123
can_data  = 01 02 03 04 05 06 07 08
```

对应 76 字节 payload 开头为：

```text
AA 55 01 00 00 08 23 01 00 00
01 02 03 04 05 06 07 08
... 后续补 0 到 can_data[64]，最后 2 字节为 CRC16
```

可用工具发送 UDP 示例：

```bash
./tools/send_udp_frame.py 127.0.0.1 --port 5000 --can-id 0x123 --data 0102030405060708
```

# 以太网 UDP 调试帧协议

## 1. 协议定位

本文档描述大核 Linux 协议转换层第一版使用的 **调试用 UDP 简单二进制帧**。

该协议不是最终业务协议，也不是 CAN 官方帧格式。它的作用是先跑通大核侧最小链路：

```text
PC / 上位机 UDP 发包
  ↓
linux_app 接收 UDP 数据
  ↓
ethernet_udp_parse_frame 解析调试帧
  ↓
protocol_parsed_msg_t 协议中间消息
  ↓
frame_packer_pack 打包 unified_frame_t
  ↓
ipc_to_rtos_send 交给小核发送通道
```

后续 RS485、WiFi、4G、蓝牙或正式上位机协议接入后，可以复用同一套：

```text
外部协议数据 → protocol_parsed_msg_t → unified_frame_t
```

## 2. 使用位置

| 项目 | 说明 |
|---|---|
| 协议名称 | 以太网 UDP 调试帧协议 |
| 当前用途 | 大核协议转换层调试和回归测试 |
| 传输方式 | UDP |
| 默认端口 | `5000` |
| 帧长度 | 固定 `76` 字节 |
| 字节序 | little-endian |
| 数据出口 | `unified_frame_t` |
| 接收解析代码 | `linux_app/以太网/ethernet_udp.c` |
| 发送测试工具 | `tools/send_udp_frame.py` |

## 3. 帧格式

所有多字节字段均为 **little-endian**。

```text
+--------+---------+--------------+-----------+---------+--------+--------------+-------+
| magic  | version | vehicle_type | can_flags | can_dlc | can_id | can_data[64] | crc16 |
| 2 Byte | 1 Byte  | 1 Byte       | 1 Byte    | 1 Byte  | 4 Byte | 64 Byte      | 2Byte |
+--------+---------+--------------+-----------+---------+--------+--------------+-------+
```

字段表：

| 偏移 | 长度 | 字段 | 类型 | 说明 |
|---:|---:|---|---|---|
| 0 | 2 | `magic` | `uint16_t` | 固定 `0x55AA`，内存字节顺序为 `AA 55` |
| 2 | 1 | `version` | `uint8_t` | 当前固定 `0x01` |
| 3 | 1 | `vehicle_type` | `uint8_t` | 车身业务类型，取值见 `vehicle_msg_type_t` |
| 4 | 1 | `can_flags` | `uint8_t` | CAN 标志位，取值见 `unified_can_flag_t` |
| 5 | 1 | `can_dlc` | `uint8_t` | CAN 数据字节数 |
| 6 | 4 | `can_id` | `uint32_t` | CAN 标准 ID 或扩展 ID |
| 10 | 64 | `can_data` | `uint8_t[64]` | CAN / CAN FD 数据区，不足 64 字节补 0 |
| 74 | 2 | `crc16` | `uint16_t` | CRC-16/CCITT-FALSE，覆盖前 74 字节 |

总长度：

```text
2 + 1 + 1 + 1 + 1 + 4 + 64 + 2 = 76 字节
```

## 4. 字段说明

### 4.1 magic

固定为：

```text
0x55AA
```

由于本协议使用 little-endian，实际 UDP 数据中的前两个字节是：

```text
AA 55
```

### 4.2 version

当前固定为：

```text
0x01
```

如果后续调试帧格式变化，需要提升版本号。

### 4.3 vehicle_type

该字段对应 `common/include/protocol_type.h` 中的 `vehicle_msg_type_t`。

常用值：

| 值 | 名称 | 含义 |
|---:|---|---|
| `0x47` | `VEHICLE_MSG_TYPE_SEAT_CONTROL` | 座椅控制 |
| `0x49` | `VEHICLE_MSG_TYPE_LIGHT_CONTROL` | 灯光控制 |
| `0x51` | `VEHICLE_MSG_TYPE_WINDOW_CONTROL` | 车窗控制 |

### 4.4 can_flags

该字段对应 `unified_can_flag_t`，是 bitmask。

| 值 | 名称 | 含义 |
|---:|---|---|
| `0x00` | `UNIFIED_CAN_FLAG_NONE` | 普通 CAN 标准数据帧 |
| `0x01` | `UNIFIED_CAN_FLAG_EXTENDED_ID` | 使用 29-bit 扩展 ID |
| `0x02` | `UNIFIED_CAN_FLAG_FD` | 使用 CAN FD，允许 `can_dlc <= 64` |
| `0x04` | `UNIFIED_CAN_FLAG_RTR` | 远程帧，预留 |
| `0x08` | `UNIFIED_CAN_FLAG_BRS` | CAN FD BRS，预留 |

### 4.5 can_dlc

本项目内部直接把 `can_dlc` 当作 CAN 数据字节数。

| 类型 | 条件 | 合法范围 |
|---|---|---|
| 普通 CAN | `can_flags` 不包含 `UNIFIED_CAN_FLAG_FD` | `0 ~ 8` |
| CAN FD | `can_flags` 包含 `UNIFIED_CAN_FLAG_FD` | `0 ~ 64` |

### 4.6 can_id

小核最终发送 CAN 时使用的 CAN ID。

| 类型 | 条件 | 合法范围 |
|---|---|---|
| 标准帧 | `can_flags` 不包含 `UNIFIED_CAN_FLAG_EXTENDED_ID` | `0x000 ~ 0x7FF` |
| 扩展帧 | `can_flags` 包含 `UNIFIED_CAN_FLAG_EXTENDED_ID` | `0x00000000 ~ 0x1FFFFFFF` |

### 4.7 can_data

固定占用 64 字节。

- 普通 CAN 只使用前 `can_dlc` 字节；
- CAN FD 最多使用 64 字节；
- 未使用字节填 `0x00`。

### 4.8 crc16

CRC 参数：

| 参数 | 值 |
|---|---:|
| 算法 | CRC-16/CCITT-FALSE |
| poly | `0x1021` |
| init | `0xFFFF` |
| refin | `false` |
| refout | `false` |
| xorout | `0x0000` |
| 覆盖范围 | 偏移 `0 ~ 73`，共 74 字节 |

## 5. 解析成功后的映射关系

`ethernet_udp_parse_frame()` 解析成功后，会输出 `protocol_parsed_msg_t`。

映射关系如下：

| UDP 调试帧字段 | `protocol_parsed_msg_t` 字段 |
|---|---|
| 固定来源 | `source_protocol = PROTOCOL_TYPE_ETHERNET` |
| `vehicle_type` | `vehicle_type` |
| `can_flags` | `can_flags` |
| `can_dlc` | `can_dlc` |
| `can_id` | `can_id` |
| `can_data[64]` | `can_data[64]` |
| 无 | `source_id = 0` |
| 无 | `destination_id = 0` |

之后由 `frame_packer_pack()` 继续打包为 `unified_frame_t`。

## 6. 接收端校验顺序

大核收到 UDP 数据后按以下顺序校验：

```text
1. 检查指针是否为空
2. 检查长度是否等于 76 字节
3. 检查 magic 是否为 0x55AA
4. 检查 version 是否为 0x01
5. 计算 CRC16 并和 crc16 字段比较
6. 检查 vehicle_type 是否合法
7. 检查 can_dlc 是否符合 can_flags
8. 检查 can_id 是否符合标准帧 / 扩展帧范围
9. 输出 protocol_parsed_msg_t
```

## 7. 示例

### 7.1 语义示例

发送一帧普通 CAN 标准帧：

```text
vehicle_type = 0x49       # 灯光控制
can_flags    = 0x00       # 普通 CAN 标准帧
can_dlc      = 8
can_id       = 0x123
can_data     = 01 02 03 04 05 06 07 08
```

### 7.2 完整 UDP 数据示例

上述数据对应的 76 字节 UDP payload 为：

```text
AA 55 01 49 00 08 23 01 00 00
01 02 03 04 05 06 07 08
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00
D2 D2
```

其中：

```text
crc16 = 0xD2D2
```

### 7.3 使用工具发送

先启动大核应用：

```bash
./build/linux_app/linux_app --udp-port 5000
```

再发送测试帧：

```bash
./tools/send_udp_frame.py 127.0.0.1 --port 5000 --vehicle light --can-id 0x123 --data 0102030405060708
```

期望大核打印类似：

```text
[ipc_stub] seq=1 src_proto=0x04 vehicle=0x49 can_id=0x123 dlc=8 flags=0x00 crc=0x8359 data=01 02 03 04 05 06 07 08
```

注意：这里打印出来的 `crc=0x8359` 是 `unified_frame_t` 的 CRC，不是 UDP 调试帧的 `crc16=0xD2D2`。

## 8. 相关文件

| 文件 | 作用 |
|---|---|
| `linux_app/以太网/ethernet_udp.h` | UDP 调试帧常量和解析接口 |
| `linux_app/以太网/ethernet_udp.c` | UDP 调试帧解析实现 |
| `tools/send_udp_frame.py` | 生成并发送 UDP 调试帧 |
| `tests/linux_app_test/test_ethernet_udp.c` | UDP 调试帧单元测试 |
| `linux_app/core/protocol_manager.c` | UDP 接收、解析、打包、发送流程 |
| `linux_app/core/frame_packer.c` | `protocol_parsed_msg_t` 到 `unified_frame_t` 的打包 |

## 9. 后续替换原则

这个协议只是第一阶段调试入口。后续接入正式业务协议时，不要求继续使用该 UDP 帧格式。

新的协议模块只需要保证最终输出：

```text
protocol_parsed_msg_t
```

即可复用后面的统一打包和发送流程。

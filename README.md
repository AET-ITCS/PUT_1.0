# 多协议统一终端 (Multi-Protocol Unified Terminal)

> **基于 Milk-V Duo 256M 的多协议智能网关**
>
> 面向"多协议车载/工业智能网关"设计比赛项目

---

## 目录

- [项目概述](#项目概述)
- [系统架构](#系统架构)
- [核心设计原则](#核心设计原则)
- [硬件平台](#硬件平台)
- [仓库目录结构](#仓库目录结构)
- [各模块职责](#各模块职责)
- [anyMSG 统一业务帧](#anymsg-统一业务帧)
- [共享内存 IPC 目标形态](#共享内存-ipc-目标形态)
- [v1 原型与目标架构关系](#v1-原型与目标架构关系)
- [开发阶段规划](#开发阶段规划)
- [快速开始](#快速开始)
- [分支管理](#分支管理)
- [项目亮点](#项目亮点)
- [相关文档](#相关文档)
- [许可证](#许可证)

---

## 项目概述

本项目设计并实现一款**多协议 anyMSG 智能网关**，面向车载网络、工业现场和比赛演示场景。系统固定面向六类物理接口：

```text
CAN / Ethernet / Wi-Fi / Bluetooth / 4G / RS485
```

目标架构不再把系统定位为单向的某类协议转换器，而是让外部设备把完整 `anyMSG` 放入对应物理协议载荷中。Linux 大核负责真实物理收发、协议适配、解包封包、分片重组和状态生成；FreeRTOS 小核负责共享内存中的完整 `anyMSG` 路由、心跳消费、优先级调度和实时控制；Linux 出口层再按目标物理接口完成真实发送。

主控平台采用 **Milk-V Duo 256M**，利用其 Linux 大核与 FreeRTOS 小核的异构架构拆分复杂 I/O 与实时调度。Web 模块作为旁路监控服务运行在 Linux 侧，只读展示状态、资源和日志。C51 低功耗控制作为规划或外部配套模块，不参与协议转换和数据转发。

### 核心链路

```text
外部设备
  ↓ 完整 anyMSG 放入物理协议载荷，必要时分片
Linux 大核接入层
  ↓ 解包 / 重组 / 校验出完整 anyMSG
共享内存 RX Ring + Frame Pool
  ↓ Mailbox Doorbell 唤醒
FreeRTOS 小核路由调度
  ↓ anyMSG 头部校验 / 心跳消费 / 路由 / 优先级调度
共享内存 TX Ring + Frame Pool
  ↓ Mailbox Doorbell 通知
Linux 大核出口层
  ↓ 按目标物理接口封包 / 分片 / 发送
目标设备
```

---

## 系统架构

### 分层职责

| 模块 | 运行位置 | 负责 | 不负责 |
| ---- | -------- | ---- | ------ |
| Linux 接入层 | Milk-V Duo 256M 大核 | 六类物理接口监听、解包、分片重组、完整 `anyMSG` 校验、写共享内存 RX Ring | 小核路由调度 |
| 共享内存 IPC | 大小核共享 ABI | Frame Pool、Descriptor Ring、Pending Bitmap、cache 同步、Mailbox Doorbell | 解释业务 payload |
| FreeRTOS 小核 | Milk-V Duo 256M 小核 | RX Ring drain、`anyMSG` 头部校验、心跳消费、CID 路由、优先级调度、写 TX Ring | 真实物理接口收发 |
| Linux 出口层 | Milk-V Duo 256M 大核 | 读 TX Ring、目标协议封包、必要时分片、真实物理发送、释放帧资源 | 修改小核路由结果 |
| Web 模块 | Linux 大核 | 只读展示模块状态、系统资源、日志和异常事件 | 直接读写共享内存或控制物理接口 |
| C51 低功耗 | 外部或规划模块 | 上电、唤醒、低功耗控制 | 协议转换、路由调度、物理发送 |

### 系统模块全景

```text
┌──────────────────────────────────────────────────────────────┐
│                 多协议 anyMSG 智能网关                       │
└──────────────────────────────────────────────────────────────┘

┌─ 六类物理接口 ────────────────────────────────────────────────┐
│ CAN │ Ethernet │ Wi-Fi │ Bluetooth │ 4G │ RS485               │
└───────────────────────────────────────────────────────────────┘
        ↓             ↓          ↓          ↓        ↓
┌─ Linux 大核接入层 ────────────────────────────────────────────┐
│ physical_interface_adapter_t                                  │
│ decode / reassemble / validate complete anyMSG                │
└───────────────────────────────────────────────────────────────┘
        ↓
┌─ 共享内存 IPC ────────────────────────────────────────────────┐
│ Frame Pool + RX Descriptor Rings + Pending Bitmap             │
│ Mailbox Doorbell only wakes peer; it never carries payload     │
└───────────────────────────────────────────────────────────────┘
        ↓
┌─ FreeRTOS 小核路由调度层 ─────────────────────────────────────┐
│ anyMSG header check / heartbeat / CID route / priority queue   │
└───────────────────────────────────────────────────────────────┘
        ↓
┌─ 共享内存 IPC ────────────────────────────────────────────────┐
│ Frame Pool + TX Descriptor Rings + Pending Bitmap             │
└───────────────────────────────────────────────────────────────┘
        ↓
┌─ Linux 大核出口层 ────────────────────────────────────────────┐
│ encapsulate / fragment_tx / send through target interface      │
└───────────────────────────────────────────────────────────────┘
        ↓
┌─ 目标设备 ────────────────────────────────────────────────────┐
│ CAN │ Ethernet │ Wi-Fi │ Bluetooth │ 4G │ RS485               │
└───────────────────────────────────────────────────────────────┘

┌─ Web 监控旁路 ────────────────────────────────────────────────┐
│ 读取 /run/put/status/ 状态快照和 /var/log/put/ 日志            │
└───────────────────────────────────────────────────────────────┘
```

---

## 核心设计原则

1. **完整 `anyMSG` 是统一业务帧**
   - 外部链路可以按物理接口能力分片承载 `anyMSG`。
   - Linux 接入层必须重组并校验出完整 `anyMSG` 后，才能写共享内存。
   - 小核永远只处理完整 `anyMSG`，不处理物理分片。

2. **Linux 负责真实物理收发和协议差异**
   - 六类接口的私有头、流式分帧、MTU、分片重组、发送封包都放在 Linux 物理接口适配层。
   - 新增物理协议时优先新增适配器，不把协议差异写进共享内存层或小核路由层。

3. **FreeRTOS 小核负责共享内存内的路由调度**
   - 小核根据 `destination_cid`、`type`、priority、TTL 等描述符和帧头信息做调度。
   - 小核不直接处理网卡、串口、蓝牙、4G 模块等真实 I/O。

4. **Mailbox 只做 Doorbell**
   - Mailbox 只负责跨核唤醒，不承载业务数据。
   - Ring 和 Pending Bitmap 是唯一可信的数据状态来源。
   - Doorbell 丢失时，接收方必须可以通过周期 drain 兜底。

5. **Web 与 C51 不改变主链路**
   - Web 只读 Linux 生成的状态快照和日志。
   - C51 只负责低功耗和唤醒控制，不参与协议转换、共享内存通信或路由调度。

---

## 硬件平台

| 组件 | 说明 |
| ---- | ---- |
| 主控板 | Milk-V Duo 256M，大核 Linux + 小核 FreeRTOS |
| CAN | 外接 CAN 收发器或底板 CAN 接口 |
| Ethernet | 底板 RJ45 接口 |
| Wi-Fi | USB Wi-Fi 模块或蓝牙/Wi-Fi 二合一模块 |
| Bluetooth | USB 蓝牙模块或蓝牙/Wi-Fi 二合一模块 |
| 4G | USB 4G 模块 |
| RS485 | 底板 RS485 接口 |
| 低功耗管理 | C51 单片机、电源控制电路、唤醒检测电路，当前作为规划或外部配套模块 |
| 状态指示 | LED 状态指示灯 |
| 外壳 | 黑盒封装，保留必要接口 |

---

## 仓库目录结构

当前仓库采用 monorepo 结构，已落地的主要目录如下：

```text
PUT_1.0/
├── README.md
├── flake.nix
├── shell.nix
├── default.nix
├── nix/
│   ├── riscv64-linux-toolchain.cmake
│   ├── riscv64-elf-toolchain.cmake
│   └── aarch64-linux-toolchain.cmake
│
├── docs/
│   ├── 设计文档/
│   │   ├── 整体架构设计.md
│   │   ├── 统一数据帧设计.md
│   │   ├── 共享内存 IPC 架构设计方案.md
│   │   ├── freeRTOS核设计.md
│   │   ├── web模块设计.md
│   │   └── 蓝牙模块设计.md
│   ├── 接口文档/
│   │   ├── 大小核共享内存IPC接口.md
│   │   └── web接口文档.md
│   ├── tasks/
│   ├── 手册/
│   └── 原理图/
│
├── common/
│   ├── include/
│   │   ├── anymsg_frame.h
│   │   ├── shared_memory_ipc.h
│   │   ├── unified_frame.h
│   │   ├── protocol_type.h
│   │   ├── error_code.h
│   │   └── crc16.h
│   └── src/
│       └── crc16.c
│
├── linux_app/
│   ├── main.c
│   ├── CMakeLists.txt
│   ├── core/
│   ├── ipc/
│   ├── ethernet/
│   ├── bluetooth/
│   ├── rs485/
│   └── config/
│
├── rtos_firmware/
│   ├── main.c
│   ├── CMakeLists.txt
│   ├── bsp/
│   ├── can/
│   ├── include/
│   ├── ipc/
│   ├── src/
│   ├── test/
│   └── watchdog/
│
├── web/
│   ├── backend/
│   ├── frontend/
│   ├── config/
│   ├── mock_status/
│   ├── mock_logs/
│   └── README.md
│
├── freertos/
└── scripts/
    ├── build_linux_app.sh
    ├── build_rtos.sh
    ├── build_web.sh
    └── test_linux_app.sh
```

说明：

- `common/include/anymsg_frame.h` 和 `common/include/shared_memory_ipc.h` 已作为目标主线公共 ABI。
- `common/include/unified_frame.h` 仅保留为 v1 历史原型参考，不再作为共享内存主链路。
- 根目录当前未落地独立的 `c51_low_power/`、`tools/`、`tests/`、`third_party/` 目录；相关能力在设计文档中作为目标或建议保留。

---

## 各模块职责

### 大核 Linux 应用 (`linux_app/`)

Linux 应用负责真实物理接口和协议适配，是系统接入层与出口层的主体。

核心职责：

- 监听 CAN、Ethernet、Wi-Fi、Bluetooth、4G、RS485 六类物理接口。
- 在接入方向完成 decode、分片重组、完整 `anyMSG` 校验和共享内存 RX Ring 写入。
- 在出口方向读取 TX Ring，按目标物理接口封包、分片并真实发送。
- 生成 `/run/put/status/` 状态快照和 `/var/log/put/` 日志，供 Web 模块只读展示。
- 管理接口状态、错误计数、分片重组统计和发送失败统计。

### 小核 FreeRTOS 固件 (`rtos_firmware/`)

FreeRTOS 小核定位为共享内存多 Ring 实时路由调度核心。

核心职责：

- 接收 Mailbox Doorbell，读取 RX Pending Bitmap。
- 按预算 drain 六路 RX Ring，定位 Frame Pool 中的完整 `anyMSG`。
- 校验长度、CID、type、epoch、TTL 等基础字段。
- 消费 `type = 0x00` 的端到网关心跳并维护在线表。
- 根据 `destination_cid` 查询目标出口，按 priority 和防饥饿配额调度。
- 写目标 TX Ring，设置 TX Pending Bitmap，并通过 Mailbox Doorbell 通知 Linux。
- 记录无路由、TTL 过期、非法长度、Ring 满等异常统计。

### Web 监控模块 (`web/`)

Web 模块用于比赛演示、现场调试和后续运维查看。

核心职责：

- 后端使用 Rust + Axum/Tokio，前端使用 Vue3 / Vite。
- 读取 Linux 生成的状态快照、系统资源和日志。
- 展示各接口连通性、收发计数、错误计数、最近通信时间。
- 展示共享内存队列状态、小核路由丢弃原因和关键异常事件。

设计边界：

- 不解析外部业务协议。
- 不直接访问共享内存。
- 不向小核发送控制命令。
- 不控制 CAN、串口、蓝牙、4G、网络接口。

### 公共代码层 (`common/`)

`common/` 存放大核和小核共同使用的稳定公共 ABI 和基础工具。

当前包含：

- `anymsg_frame.h`：anyMSG 40B 固定帧头、CID 地址段、type 和基础校验 helper。
- `shared_memory_ipc.h`：v2 共享内存 IPC 公共 ABI，包含 Frame Pool、Descriptor Ring、Pending Bitmap 和 reclaim ring。
- `unified_frame.h`：v1 原型业务帧定义，仅作历史参考。
- `protocol_type.h`：协议类型枚举。
- `error_code.h`：公共错误码。
- `crc16.h` / `crc16.c`：CRC-16 基础工具。

边界要求：

- 继续避免把具体物理协议业务代码放入 `common/`。

### C51 低功耗控制

C51 低功耗控制当前作为规划或外部配套模块描述，根目录尚未落地独立工程。

目标职责：

- 检测外部唤醒信号。
- 控制 Milk-V Duo 256M 上电或唤醒。
- 检测系统空闲状态并控制低功耗。
- 控制外设电源和 LED 状态指示。

---

## anyMSG 统一业务帧

目标统一业务帧为 `anyMSG`。完整定义见 [统一数据帧设计](docs/设计文档/统一数据帧设计.md)。

`anyMSG` 由 40B 固定帧头和可变长度 payload 组成：

```text
msg_length        2B   完整帧长度，必须等于 40 + payload_length
retries           1B   重试次数，当前默认 1
__RESERVED__      1B   保留字段，当前填 0
__SRCHLD__        4B   源标记，当前保留
destination_cid   4B   目的通信地址
source_cid        4B   源通信地址
local_time        4B   本地时间戳
verify_string    16B   校验字段，当前算法未定义
payload_length    2B   payload 字节数
type              1B   payload 类型
__PADDING__       1B   填充字段，当前填 0
payload         可变   业务负载
```

CID 地址首字节用于区分设备或接口地址段：

| 地址首字节 | 地址段 |
| ---------- | ------ |
| `0x20 ~ 0x3F` | CAN 设备地址段 |
| `0x40 ~ 0x5F` | 以太网设备地址段 |
| `0x60 ~ 0x7F` | Wi-Fi 设备地址段 |
| `0x80 ~ 0x9F` | 蓝牙设备地址段 |
| `0xA0 ~ 0xBF` | 4G 蜂窝设备地址段 |
| `0xC0 ~ 0xDF` | RS485 设备地址段 |

`type` 只描述 payload 语义，不直接规定物理出口或转发策略。典型类型包括心跳、健康度、网络鉴权、Modbus、RAW_CAN、CAN_FD、UDS、J1939、CANopen 等。

---

## 共享内存 IPC 目标形态

目标共享内存不再把完整业务帧塞进固定长度 slot，而是采用 `put_shm_region_t` v2 ABI：

```text
put_shm_region_t
├── region_header
├── frame_pool[64][512]
├── rx_rings
│   ├── CAN_RX_RING
│   ├── ETH_RX_RING
│   ├── WIFI_RX_RING
│   ├── BT_RX_RING
│   ├── LTE_RX_RING
│   └── RS485_RX_RING
├── tx_rings
│   ├── CAN_TX_RING
│   ├── ETH_TX_RING
│   ├── WIFI_TX_RING
│   ├── BT_TX_RING
│   ├── LTE_TX_RING
│   └── RS485_TX_RING
├── rx_pending_bitmap
├── tx_pending_bitmap
├── reclaim_pending
├── reclaim_ring
└── reserved
```

核心组件：

- **Frame Pool**：保存完整 `anyMSG` 字节，当前冻结为 `64 * 512B`。
- **Descriptor Ring**：保存帧 ID、偏移、长度、来源接口、目标接口、CID、type、priority、TTL、epoch、CRC 和 flags 等元数据，单 descriptor 固定 64B。
- **Pending Bitmap**：表示哪些 RX/TX Ring 非空。
- **Mailbox Doorbell**：只做跨核唤醒，不传输业务数据。
- **Reclaim Ring**：小核消费或丢弃但不转发时通知 Linux 回收 Frame Pool。

Frame Pool 资源由 Linux 分配和最终释放；小核只移动描述符、更新消费状态、写入调度结果或 reclaim descriptor。Ring 满时不覆盖旧描述符，必须丢弃新帧或按策略丢弃低优先级帧并记录统计。

### 物理接口适配器

各物理协议的解包和封包差异由 Linux 侧适配器隔离，目标接口形态参考：

```c
typedef struct {
    const char *name;
    uint8_t interface_id;

    size_t (*get_mtu)(void *ctx);
    int (*decode_rx)(void *ctx, const uint8_t *input, size_t input_len, adapter_rx_result_t *out);
    int (*reassemble)(void *ctx, const adapter_fragment_t *fragment, anymsg_buffer_t *out_complete_msg);
    int (*encapsulate)(void *ctx, const anymsg_buffer_t *msg, adapter_tx_packet_t *out_packet);
    int (*fragment_tx)(void *ctx, const anymsg_buffer_t *msg, adapter_tx_packet_list_t *out_packets);
    int (*send)(void *ctx, const adapter_tx_packet_t *packet);
    int (*status)(void *ctx, adapter_status_t *out_status);
} physical_interface_adapter_t;
```

新增物理协议时应新增适配器，而不是修改小核路由核心。

---

## v1 原型与目标架构关系

旧 v1 原型为：

```text
Linux 协议适配层
  ↓
96B unified_frame_t
  ↓
128B shared memory slot payload
  ↓
历史小核 CAN direct 输出路径
```

该方案适合早期功能验证，但不是当前目标架构。当前主线已经迁移到 v2 共享内存 ABI。v1 主要限制是：

- `unified_frame_t` 固定长度，无法表达完整 `anyMSG` 的可变 payload。
- 固定 slot payload 无法承载更大的业务帧。
- CAN direct 路径把小核绑定到具体物理出口，不符合六类接口统一路由边界。
- 以 CAN 字段为中心的帧结构无法自然支持多接口之间的统一寻址和转发。

主线关系：

```text
历史原型: fixed slot payload + unified_frame_t
当前主线: Frame Pool + Descriptor Ring + complete anyMSG
```

---

## 开发阶段规划

| 阶段 | 目标 | 主要产出 |
| ---- | ---- | -------- |
| 第一阶段 | 需求分析与总体设计 | 总体架构、anyMSG 帧定义、职责边界、文档体系 |
| 第二阶段 | v1 原型和硬件验证 | 基础通信链路、共享内存 v1、CAN direct 原型、外设可用性确认 |
| 第三阶段 | anyMSG 与接口适配层 | `anyMSG` 公共定义、六类物理接口适配器、分片重组策略 |
| 第四阶段 | 共享内存 v2 | Frame Pool、Descriptor RX/TX Ring、Pending Bitmap、Mailbox Doorbell、reclaim ring |
| 第五阶段 | 小核路由调度 | CID 路由、心跳消费、优先级队列、防饥饿调度、异常统计 |
| 第六阶段 | Web 监控与整机集成 | 只读状态接口、前端展示、日志与事件、低功耗配套、样机封装 |

---

## 快速开始

### 环境准备

#### 硬件准备

- Milk-V Duo 256M 开发板
- CAN / Ethernet / Wi-Fi / Bluetooth / 4G / RS485 对应测试设备或模块
- CAN 收发器
- 调试串口和供电设备

#### 开发环境：Nix（推荐）

本项目使用 [Nix](https://nixos.org/) 管理开发环境与交叉编译工具链，确保开发者使用一致、可复现的工具版本。

先决条件：安装 Nix >= 2.8 并启用 flakes。

```bash
cd PUT_1.0
nix develop
```

进入环境后，shell 会自动打印项目结构、工具链信息和构建命令提示。

##### 提供的工具链

| 类别 | 组件 | 用途 |
| ---- | ---- | ---- |
| 基础构建 | `cmake`, `make`, `gcc`, `gdb` | 本地编译与调试 |
| RISC-V Linux 交叉编译 | `riscv64-unknown-linux-gnu-gcc` | 大核 Linux 应用编译 |
| RISC-V musl 交叉编译 | `riscv64-unknown-linux-musl-gcc` | Web 后端静态链接 |
| RISC-V 裸机交叉编译 | `riscv64-none-elf-gcc` | 小核 FreeRTOS 固件编译 |
| ARM64 交叉编译 | `aarch64-linux-gnu-gcc` | 备选大核目标 |
| Rust | `rustc`, `cargo`, `rustup` | Web 后端和 Rust 工具 |
| Web 前端 | `node`, `npm` | Vue3 / Vite 构建 |
| Python 工具 | `python3`, `pyserial` | 调试脚本运行 |
| 代码分析 | `clang-tools`, `cppcheck`, `clippy`, `rustfmt` | 代码质量检查 |

##### CMake 交叉编译

```bash
# 编译大核 Linux 程序
cmake -B build_linux -S linux_app \
      -DCMAKE_TOOLCHAIN_FILE=../nix/riscv64-linux-toolchain.cmake
cmake --build build_linux

# 编译小核 FreeRTOS 固件
cmake -B build_rtos -S rtos_firmware \
      -DCMAKE_TOOLCHAIN_FILE=../nix/riscv64-elf-toolchain.cmake
cmake --build build_rtos
```

##### Rust 交叉编译

进入 `nix develop` 后，Cargo 会通过环境变量配置交叉编译 linkers 和别名，可直接使用：

```bash
cargo build-riscv64-linux
cargo build-riscv64-linux-musl
cargo build-riscv64-elf
cargo check-riscv64-linux
```

##### Web 监控模块

```bash
npm --prefix web/frontend ci
npm --prefix web/frontend run build
cargo test --manifest-path web/backend/Cargo.toml
cargo run --manifest-path web/backend/Cargo.toml -- --config web/config/web_config.dev.toml
```

开发配置读取 `web/mock_status/` 与 `web/mock_logs/`；生产配置读取 `/run/put/status/` 与 `/var/log/put/`。

##### 可用 Shell 环境

```bash
nix develop          # 完整开发环境
nix develop .#minimal # 最小环境
nix-shell            # nix-shell 兼容入口
```

##### 关键环境变量

| 变量 | 说明 |
| ---- | ---- |
| `RISCV64_LINUX_CC` | RISC-V Linux C 编译器路径 |
| `RISCV64_LINUX_MUSL_CC` | RISC-V Linux musl C 编译器路径 |
| `RISCV64_ELF_CC` | RISC-V 裸机 C 编译器路径 |
| `AARCH64_LINUX_CC` | ARM64 C 编译器路径 |
| `RUST_TARGET_RISCV64_LINUX` | Rust RISC-V Linux target |
| `RUST_TARGET_RISCV64_LINUX_MUSL` | Rust RISC-V Linux static Web target |
| `RUST_TARGET_RISCV64_ELF` | Rust RISC-V bare-metal target |

#### 开发环境：传统方式（备选）

如果无法使用 Nix，可以手动安装以下工具链：

- Linux 交叉编译工具链：`riscv64-unknown-linux-gnu-gcc`
- FreeRTOS 交叉编译工具链：`riscv64-none-elf-gcc`
- CMake >= 3.13
- Python 3 + `pyserial`
- Rust + `rustup`
- Node.js / npm

Milk-V Duo 官方 SDK 地址：https://github.com/milkv-duo/duo-buildroot-sdk

### 常用脚本

```bash
# 编译大核 Linux 应用
./scripts/build_linux_app.sh

# 编译小核 FreeRTOS 固件
./scripts/build_rtos.sh

# 编译 Web 前后端
./scripts/build_web.sh

# 运行 Linux 应用侧测试
./scripts/test_linux_app.sh
```

---

## 分支管理

本项目采用简单分支策略：

```text
main              # 稳定版本
develop           # 日常开发版本
feature/xxx       # 功能开发分支
```

推荐分支示例：

```text
feature/anymsg-frame
feature/shared-memory-v2
feature/interface-adapters
feature/rtos-router
feature/web-monitor
feature/low-power
```

开发流程：

```text
个人在 feature 分支开发
  ↓
测试通过
  ↓
合并到 develop
  ↓
阶段性稳定后
  ↓
合并到 main
```

---

## 项目亮点

1. **六类物理接口统一接入**：覆盖 CAN、Ethernet、Wi-Fi、Bluetooth、4G、RS485。
2. **完整 `anyMSG` 业务帧**：统一寻址、统一类型语义，支持可变 payload。
3. **大小核清晰分工**：Linux 处理真实 I/O 和协议差异，FreeRTOS 处理共享内存内的路由和调度。
4. **共享内存 v2 目标架构**：Frame Pool + Descriptor Ring 适合大帧、分片重组和多接口路由。
5. **Mailbox Doorbell 简化跨核同步**：Mailbox 只唤醒，数据状态以 Ring 和 Pending Bitmap 为准。
6. **Web 旁路监控**：只读展示状态、日志和异常事件，不影响主通信链路。
7. **低功耗扩展边界清晰**：C51 独立负责唤醒和电源控制，不侵入协议链路。

---

## 相关文档

- [整体架构设计](docs/设计文档/整体架构设计.md)
- [统一数据帧设计](docs/设计文档/统一数据帧设计.md)
- [共享内存 IPC 架构设计方案](docs/设计文档/共享内存%20IPC%20架构设计方案.md)
- [FreeRTOS 小核设计](docs/设计文档/freeRTOS核设计.md)
- [Web 模块设计](docs/设计文档/web模块设计.md)
- [大小核共享内存 IPC 接口](docs/接口文档/大小核共享内存IPC接口.md)
- [Web 接口文档](docs/接口文档/web接口文档.md)
- [项目计划与任务](docs/tasks/多协议统一终端项目计划.md)

---

## 许可证

本项目仅供学习和比赛使用。当前仓库尚未提供 `LICENSE` 文件，正式发布或参赛提交前请补充授权说明。

---

> **项目状态：** 开发中

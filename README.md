# 多协议统一终端 (Multi-Protocol Unified Terminal)

> **基于 Milk-V Duo 256M 的多协议转 CAN 智能网关**
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
  - [大核 Linux 应用 (`linux_app/`)](#大核-linux-应用-linux_app)
  - [小核 RTOS 固件 (`rtos_firmware/`)](#小核-rtos-固件-rtos_firmware)
  - [C51 低功耗管理 (`c51_low_power/`)](#c51-低功耗管理-c51_low_power)
  - [公共代码层 (`common/`)](#公共代码层-common)
- [统一数据帧设计](#统一数据帧设计)
- [数据流一览](#数据流一览)
- [开发阶段规划](#开发阶段规划)
- [快速开始](#快速开始)
- [分支管理](#分支管理)
- [项目亮点](#项目亮点)
- [许可证](#许可证)

---

## 项目概述

本项目设计并实现一款**多协议统一终端**，将多种外部通信协议（4G、WiFi、蓝牙、以太网、RS485）统一接入，并最终转换为 **CAN 总线数据**，适用于车载网络和工业现场通信场景。

主控平台采用 **Milk-V Duo 256M**，利用其**异构双核架构**（大核 + 小核）实现协议解析与实时控制的分离，并额外集成 **C51 单片机** 实现低功耗唤醒管理。

### 核心思路

```text
外部多协议数据接入（4G / WiFi / 蓝牙 / 以太网 / RS485）
        ↓
大核 Linux 解析协议帧 → 统一封装为内部标准帧
        ↓
大小核通信传递数据
        ↓
小核 RTOS 实时转发到 CAN 总线
        ↓
C51 低功耗唤醒管理（整机电源控制）
```

---

## 系统架构

### 三层协同架构

| 层级 | 处理器 | 运行环境 | 核心职责 |
|------|--------|----------|----------|
| **大核 (协议接入与解析层)** | Milk-V Duo 256M (大核) | Linux | 复杂协议接入、数据解析、统一帧封装 |
| **小核 (实时转发层)** | Milk-V Duo 256M (小核) | RTOS | CAN 报文实时发送/接收、总线状态管理 |
| **C51 (低功耗管理层)** | C51 单片机 | 裸机 | 低功耗状态控制、外部唤醒检测、电源管理 |

### 系统模块全景

```text
┌─────────────────────────────────────────────────────────┐
│                    多协议统一终端系统                   │
└─────────────────────────────────────────────────────────┘

┌─ 外部协议接入层 ─────────────────────────────────────────┐
│  4G 模块  │  WiFi 模块  │  蓝牙模块  │  以太网  │  RS485 │
└──────────────────────────────────────────────────────────┘
        ↓           ↓            ↓          ↓          ↓
┌─ 大核 Linux 协议解析层 ──────────────────────────────────┐
│  ┌──────────┐  ┌────────────┐  ┌──────────────────────┐  │
│  │  4G 解析 │  │ WiFi 解析  │  │  RS485 CAN direct 解析 │  │
│  └──────────┘  └────────────┘  └──────────────────────┘  │
│  ┌──────────┐  ┌────────────┐  ┌──────────────────────┐  │
│  │ 蓝牙解析 │  │ 以太网解析 │  │  协议管理 & 统一打包 │  │
│  └──────────┘  └────────────┘  └──────────────────────┘  │
└──────────────────────────────────────────────────────────┘
                        ↓
             ┌──────────────────────┐
             │  统一数据帧封装      │
             │  unified_frame_t     │
             └──────────────────────┘
                        ↓
┌─ 大小核通信层 ───────────────────────────────────────────┐
│             大核 → 小核 (数据帧下发)                     │
│             小核 → 大核 (CAN 状态回传)                   │
└──────────────────────────────────────────────────────────┘
                        ↓
┌─ 小核 RTOS 实时 CAN 转发层 ─────────────────────────────┐
│  ┌──────────────┐  ┌──────────────────┐                 │
│  │  CAN 报文发送│  │  CAN 报文接收    │                 │
│  └──────────────┘  └──────────────────┘                 │
│  ┌──────────────┐  ┌──────────────────┐                 │
│  │  错误处理    │  │  状态上报        │                 │
│  └──────────────┘  └──────────────────┘                 │
└─────────────────────────────────────────────────────────┘
                        ↓
                   ┌──────────┐
                   │ CAN 总线 │
                   └──────────┘

┌─ C51 低功耗管理层 ──────────────────────────────────────────┐
│  低功耗检测  →  唤醒信号检测  →  主控电源控制  →  LED 指示  │
└─────────────────────────────────────────────────────────────┘
```

---

## 核心设计原则

1. **大核负责复杂通信，不做 CAN 实时操作**
   - 大核运行 Linux，负责 4G/WiFi/蓝牙/以太网/RS485 的协议解析
   - 将不同来源数据统一封装为 `unified_frame_t`，再传给小核
   - 避免 Linux 调度不确定性影响 CAN 实时性

2. **小核专注实时 CAN 转发**
   - 小核运行 RTOS，只识别 `unified_frame_t` 一种协议格式
   - 不关心数据来自哪种外部协议，只负责实时转发到 CAN 总线

3. **C51 独立管理低功耗**
   - 无数据时控制系统进入低功耗状态
   - 检测到外部数据到来时唤醒主控系统
   - 不参与协议转换和 CAN 数据发送

---

## 硬件平台

| 组件 | 说明 |
|------|------|
| **主控板** | Milk-V Duo 256M（大核 Linux + 小核 RTOS） |
| **4G 通信** | USB 转 4G 模块 |
| **WiFi 通信** | USB 蓝牙/WiFi 二合一模块（WiFi 功能） |
| **蓝牙通信** | USB 蓝牙/WiFi 二合一模块（蓝牙功能） |
| **以太网** | 底板自带 RJ45 接口 |
| **RS485** | 底板自带 RS485 接口 |
| **CAN** | 外接 CAN 收发器 / 底板 CAN 接口 |
| **低功耗管理** | C51 单片机 + 电源控制电路 + 唤醒检测电路 |
| **状态指示** | LED 状态指示灯 |
| **外壳** | 黑盒封装，保留必要接口 |

---

## 仓库目录结构

```
PUT_1.0/
├── README.md                       # 项目说明（本文件）
├── LICENSE                         # 许可证
├── .gitignore                      # Git 忽略规则
│
├── docs/                           # 项目文档
│   ├── 多协议统一终端项目计划.md       # 项目计划与需求分析
│   └── 架构设计.md                   # 仓库结构与架构设计说明
│
├── common/                         # 大核与小核公共代码
│   ├── include/
│   │   ├── unified_frame.h         # 统一协议帧定义（核心）
│   │   ├── shared_memory_ipc.h     # 共享内存 IPC 公共 ABI
│   │   ├── protocol_type.h         # 协议类型枚举
│   │   └── error_code.h            # 公共错误码
│   └── src/
│
├── linux_app/                      # 大核 Linux 应用程序
│   ├── CMakeLists.txt
│   ├── main.c                      # 主入口
│   ├── include/                    # 头文件
│   ├── src/
│   │   ├── protocol_manager.c      # 协议管理层
│   │   ├── frame_packer.c          # 统一帧打包
│   │   ├── ipc_to_rtos.c           # 大小核通信（大核→小核）
│   │   └── config.c                # 配置管理
│   ├── protocols/                  # 各协议接入模块
│   │   ├── four_g_client.c         # 4G 通信
│   │   ├── wifi_server.c           # WiFi TCP/UDP 服务
│   │   ├── bluetooth_server.c      # 蓝牙通信
│   │   ├── ethernet_tcp.c          # 以太网 TCP/UDP
│   │   └── rs485_can_direct.c           # RS485 CAN direct
│   ├── drivers/                    # 驱动封装
│   │   ├── uart_linux.c            # Linux 串口驱动
│   │   ├── usb_device.c            # USB 设备管理
│   │   └── gpio_linux.c            # GPIO 控制
│   └── config/
│       └── device_config.json      # 设备配置文件
│
├── rtos_firmware/                  # 小核 RTOS 固件
│   ├── CMakeLists.txt
│   ├── main.c                      # 主入口
│   ├── include/                    # 头文件
│   ├── src/
│   │   ├── ipc_rx_task.c           # 接收大核数据
│   │   ├── frame_parser.c          # 解析统一数据帧
│   │   ├── can_task.c              # CAN 实时发送任务
│   │   ├── can_driver.c            # CAN 底层驱动
│   │   └── watchdog.c              # 看门狗
│   └── bsp/                        # 板级支持包
│       ├── board.c
│       ├── clock.c
│       ├── pinmux.c
│       └── interrupt.c
│
├── c51_low_power/                  # C51 低功耗唤醒程序
│   ├── main.c                      # 主程序
│   ├── wakeup.c                    # 唤醒检测
│   ├── power_ctrl.c                # 电源控制
│   └── README.md
│
├── tools/                          # 调试与测试工具
│   ├── frame_debugger.py           # 统一帧调试工具
│   ├── can_test.py                 # CAN 测试脚本
│   ├── serial_test.py              # 串口测试脚本
│   └── log_parser.py               # 日志分析工具
│
├── scripts/                        # 构建与部署脚本
│   ├── build_linux_app.sh          # 编译大核程序
│   ├── build_rtos.sh               # 编译小核固件
│   ├── flash_rtos.sh               # 烧录小核固件
│   ├── package_release.sh          # 打包发布
│   └── clean.sh                    # 清理构建产物
│
├── tests/                          # 测试代码
│   ├── protocol_frame_test/        # 统一帧测试
│   ├── linux_app_test/             # 大核模块测试
│   ├── can_loopback_test/          # CAN 回环测试
│   └── integration_test/           # 集成测试
│
├── third_party/                    # 第三方依赖
│   └── README.md
│
└── output/                         # 构建输出目录
    └── .gitkeep
```

---

## 各模块职责

### 大核 Linux 应用 (`linux_app/`)

大核运行 Linux 系统，负责所有**复杂协议接入与数据解析**工作。

**核心职责：**

- 4G 模块联网与 TCP/MQTT 数据收发
- WiFi 模块 TCP/UDP 服务
- 蓝牙模块数据收发
- 以太网 TCP/UDP 通信
- RS485 CAN direct 数据接收与转发
- 将不同协议数据统一封装为 `unified_frame_t`
- 通过大小核通信接口将数据帧发送给小核

**设计要点：** 大核**不直接操作 CAN**，只做"不同协议数据 → 统一帧"的转换。

### 小核 RTOS 固件 (`rtos_firmware/`)

小核运行 RTOS，专注于 **CAN 实时转发**。

**核心职责：**

- 接收大核发送的统一数据帧
- 校验帧头、长度、CRC
- 解析 CAN ID、DLC 和数据区
- 按实时要求发送 CAN 报文
- 接收 CAN 总线的返回数据，回传给大核
- CAN 错误检测与处理
- 看门狗保护

**设计要点：** 小核**不关心**外部协议细节（4G/WiFi/蓝牙等），只识别 `unified_frame_t`。

### C51 低功耗管理 (`c51_low_power/`)

C51 作为独立的低功耗管理单元。

**核心职责：**

- 检测系统空闲状态，控制进入低功耗
- 检测 4G/WiFi/蓝牙/以太网/RS485 唤醒信号
- 唤醒 Milk-V Duo 256M 主控系统
- 控制外设电源通断
- LED 状态指示
- 心跳检测

**设计要点：** C51 **不参与协议转换**，只负责电源管理。

### 公共代码层 (`common/`)

存放**大核和小核共同使用**的公共代码。

**包含内容：**

- `unified_frame.h` — 统一数据帧结构定义（最核心文件）
- `protocol_type.h` — 协议类型枚举
- `error_code.h` — 错误码定义

**设计要点：** 只放**稳定、通用**的内容，不包含具体业务协议代码。

---

## 统一数据帧设计

大核与小核之间通过统一的内部数据帧进行通信，定义在 `common/include/unified_frame.h` 中。

### 帧结构

```c
typedef struct {
    uint16_t magic;             // 帧头 (0xA55A)
    uint8_t  protocol_type;     // 来源协议类型
    uint8_t  frame_type;        // 帧类型：控制帧/数据帧/心跳帧
    uint32_t can_id;            // 目标 CAN ID
    uint8_t  can_dlc;           // CAN 数据长度
    uint8_t  data[64];          // 数据内容
    uint16_t crc;               // CRC 校验
} unified_frame_t;
```

### 协议类型枚举

| 枚举值 | 宏定义 | 说明 |
|--------|--------|------|
| `0x01` | `PROTOCOL_4G` | 4G 网络 |
| `0x02` | `PROTOCOL_WIFI` | WiFi |
| `0x03` | `PROTOCOL_BLUETOOTH` | 蓝牙 |
| `0x04` | `PROTOCOL_RS485` | RS485 |
| `0x05` | `PROTOCOL_ETHERNET` | 以太网 |

### 设计优势

- **统一接口**：小核只需解析一种帧格式，降低复杂度
- **易于扩展**：新增协议只需在大核添加解析模块，不影响小核
- **可追溯**：`protocol_type` 字段标明数据来源，便于调试

---

## 数据流一览

### 4G → CAN

```
云端服务器 / MQTT / TCP
    ↓
USB 4G 模块
    ↓
大核 Linux → 协议解析 → 统一封装
    ↓
小核 RTOS → CAN 报文发送
    ↓
CAN 总线设备
```

### WiFi → CAN

```
PC 上位机
    ↓ UDP/TCP
WiFi 模块
    ↓
大核 Linux → 协议解析 → 统一封装
    ↓
小核 RTOS → CAN 报文发送
    ↓
CAN 总线设备
```

### 蓝牙 → CAN

```
手机蓝牙
    ↓
USB 蓝牙模块
    ↓
大核 Linux → 蓝牙数据解析 → 统一封装
    ↓
小核 RTOS → CAN 报文发送
    ↓
CAN 总线设备
```

### 以太网 → CAN

```
PC 上位机
    ↓ TCP/UDP
以太网接口
    ↓
大核 Linux → 协议解析 → 统一封装
    ↓
小核 RTOS → CAN 报文发送
    ↓
CAN 总线设备
```

### RS485 → CAN

```
RS485 设备 (CAN direct 网关帧)
    ↓
RS485 接口
    ↓
大核 Linux → 串口读取 → 协议解析 → 统一封装
    ↓
小核 RTOS → CAN 报文发送
    ↓
CAN 总线设备
```

---

## 开发阶段规划

| 阶段 | 目标 | 主要产出 |
|------|------|----------|
| **第一阶段** | 需求分析与总体设计 | 架构图、数据流图、模块分工表、统一帧定义 |
| **第二阶段** | 硬件验证 | 各模块连通性测试、接口测试代码、外设可用性确认 |
| **第三阶段** | 大核协议接入开发 | 多协议接入程序、统一解析模块、数据模拟测试工具 |
| **第四阶段** | 小核 RTOS CAN 转发开发 | CAN 驱动、CAN 转发/接收任务、错误处理模块 |
| **第五阶段** | 大小核联调 | 完整通信链路演示、联调问题记录与解决 |
| **第六阶段** | 低功耗与整机集成 | 低功耗唤醒演示、样机、黑盒封装、稳定性测试 |

---

## 快速开始

### 环境准备

#### 硬件准备

- **Milk-V Duo 256M** 开发板
- 各通信模块（4G / WiFi+蓝牙 / 以太网 / RS485）
- CAN 收发器

#### 开发环境 — Nix（推荐）

本项目使用 [Nix](https://nixos.org/) 管理开发环境与交叉编译工具链，确保所有开发者使用一致的、可复现的工具链版本。

> **先决条件**：安装 Nix (>= 2.8) 并启用 [flakes](https://nixos.wiki/wiki/Flakes) 支持。

```bash
# 进入统一的 Nix 开发环境（自动配置所有工具链）
cd PUT_1.0
nix develop
```

进入环境后，shell 会自动打印项目结构、工具链信息和构建命令提示。

##### 提供的工具链

| 类别 | 组件 | 用途 |
|------|------|------|
| **基础构建** | `cmake`, `make`, `gcc`, `gdb` | 本地编译与调试 |
| **RISC-V Linux 交叉编译** | `riscv64-unknown-linux-gnu-gcc` | 大核 Linux 应用编译 |
| **RISC-V musl 交叉编译** | `riscv64-unknown-linux-musl-gcc` | Web 后端静态链接 |
| **RISC-V 裸机交叉编译** | `riscv64-none-elf-gcc` | 小核 RTOS 固件编译 |
| **ARM64 交叉编译** | `aarch64-linux-gnu-gcc` | 备选大核目标 |
| **C51 编译器** | `sdcc` | C51 低功耗程序编译 |
| **Rust** | `rustc`, `cargo`, `rustup` | Rust 语言支持 |
| **Rust 交叉编译目标** | `riscv64gc-unknown-linux-gnu`、`riscv64gc-unknown-linux-musl` 等 | Rust 交叉编译 |
| **Web 前端** | `node`, `npm` | Vue3 / Vite 构建 |
| **Python 工具** | `python3`, `pyserial` | 调试脚本运行 |
| **代码分析** | `clang-tools`, `cppcheck`, `clippy`, `rustfmt` | 代码质量检查 |

##### 使用 CMake 工具链文件进行交叉编译

```bash
# 编译大核 Linux 程序 (RISC-V)
cmake -B build_linux -S linux_app \
      -DCMAKE_TOOLCHAIN_FILE=../nix/riscv64-linux-toolchain.cmake
cmake --build build_linux

# 编译小核 RTOS 固件 (RISC-V bare-metal)
cmake -B build_rtos -S rtos_firmware \
      -DCMAKE_TOOLCHAIN_FILE=../nix/riscv64-elf-toolchain.cmake
cmake --build build_rtos
```

##### Rust 交叉编译

进入 `nix develop` 后，Cargo 会通过环境变量配置交叉编译 linkers 和别名，可直接使用：

```bash
cargo build-riscv64-linux        # 编译 RISC-V Linux 目标
cargo build-riscv64-linux-musl   # 编译 Web 后端静态目标
cargo build-riscv64-elf          # 编译 RISC-V bare-metal 目标
cargo check-riscv64-linux        # 仅检查（无需完整编译）
```

##### Web 监控模块

```bash
npm --prefix web/frontend ci
npm --prefix web/frontend run build
cargo test --manifest-path web/backend/Cargo.toml
cargo run --manifest-path web/backend/Cargo.toml -- --config web/config/web_config.dev.toml
```

开发配置会读取 `web/mock_status/` 与 `web/mock_logs/`；生产配置仍读取 `/run/put/status/` 与 `/var/log/put/`。

##### 可用的 Shell 环境

```bash
# 完整开发环境（默认，包含所有工具）
nix develop

# 最小环境（仅基础构建工具，加载更快）
nix develop .#minimal

# nix-shell 兼容入口（仍需启用 flakes，推荐优先使用 nix develop）
nix-shell
```

##### 环境变量

进入 Nix shell 后，以下环境变量可供 CMake / 脚本使用：

| 变量 | 值示例 | 说明 |
|------|--------|------|
| `RISCV64_LINUX_CC` | `/nix/store/.../bin/riscv64-unknown-linux-gnu-gcc` | RISC-V Linux C 编译器路径 |
| `RISCV64_LINUX_MUSL_CC` | `/nix/store/.../bin/riscv64-unknown-linux-musl-gcc` | RISC-V Linux musl C 编译器路径 |
| `RISCV64_ELF_CC` | `/nix/store/.../bin/riscv64-none-elf-gcc` | RISC-V 裸机 C 编译器路径 |
| `AARCH64_LINUX_CC` | `/nix/store/.../bin/aarch64-linux-gnu-gcc` | ARM64 C 编译器路径 |
| `RUST_TARGET_RISCV64_LINUX` | `riscv64gc-unknown-linux-gnu` | Rust RISC-V Linux target |
| `RUST_TARGET_RISCV64_LINUX_MUSL` | `riscv64gc-unknown-linux-musl` | Rust RISC-V Linux static Web target |
| `RUST_TARGET_RISCV64_ELF` | `riscv64gc-unknown-none-elf` | Rust RISC-V bare-metal target |

#### 开发环境 — 传统方式（备选）

如果无法使用 Nix，也可以手动安装以下工具链：

- **Linux 交叉编译工具链**：`riscv64-unknown-linux-gnu-gcc` (大核目标)
- **RTOS 交叉编译工具链**：`riscv64-none-elf-gcc` (小核目标)
- **C51 编译器**：`sdcc` 或 Keil C51
- **CMake** >= 3.13
- **Python 3** + `pyserial` (调试工具依赖)
- **Rust** (可选)：通过 `rustup` 安装，并添加 target：
  ```bash
  rustup target add riscv64gc-unknown-linux-gnu
  rustup target add riscv64gc-unknown-linux-musl
  rustup target add riscv64gc-unknown-none-elf
  ```
- **Node.js / npm**：用于构建 `web/frontend`。

Milk-V Duo 官方 SDK 地址：https://github.com/milkv-duo/duo-buildroot-sdk

### 构建大核程序

```bash
cd linux_app
mkdir build && cd build
cmake ..
make
```

### 构建小核固件

```bash
cd rtos_firmware
mkdir build && cd build
cmake ..
make
```

### 烧录小核固件

```bash
./scripts/flash_rtos.sh
```

### 运行测试

```bash
# 统一帧测试
cd tests/protocol_frame_test && ./run_test.sh

# CAN 回环测试
cd tests/can_loopback_test && ./run_test.sh

# 集成测试
cd tests/integration_test && ./run_test.sh
```

---

## 分支管理

本项目采用简单的分支策略：

```
main              # 稳定版本
develop           # 日常开发版本
feature/xxx       # 功能开发分支
```

### 推荐分支示例

```text
feature/rs485-to-can
feature/wifi-to-can
feature/bluetooth-to-can
feature/4g-to-can
feature/ethernet-to-can
feature/low-power
feature/rtos-can-send
```

### 开发流程

```text
个人在 feature 分支开发
    ↓ 测试通过
合并到 develop
    ↓ 阶段性稳定
合并到 main
```

---

## 项目亮点

1. **多协议统一接入** — 支持 4G、WiFi、蓝牙、以太网、RS485 五种通信方式，覆盖主流车载和工业场景
2. **统一协议封装** — 不同来源数据经过标准化处理后统一转发，系统扩展性和可维护性高
3. **大小核协同架构** — 充分利用 Milk-V Duo 256M 异构多核能力，实现"大核解析 + 小核实时"
4. **RTOS 保证 CAN 实时性** — 小核专用 RTOS 处理 CAN 通信，避免 Linux 调度延迟影响
5. **低功耗唤醒设计** — C51 独立管理电源，无数据时低功耗待机，有数据时自动唤醒
6. **黑盒网关形态** — 最终作品封装为独立网关，只暴露必要接口，符合工业化设计要求

---

## 许可证

本项目仅供学习和比赛使用，具体许可证见 `LICENSE` 文件。

---

> **项目状态：** 开发中
>
> **相关文档：**
> - [项目计划与需求分析](docs/多协议统一终端项目计划.md)
> - [架构设计与仓库结构说明](docs/架构设计.md)

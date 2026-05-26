# 架构隐患 TODO 清单

来源：`docs/设计文档/整体架构设计.md` 架构审查  
日期：2026-05-26  
排序规则：按风险优先级从高到低排列。

---

## P0 高风险

### 1. 补齐 Frame Pool 回收闭环

- [ ] 明确 Frame Pool 的完整生命周期：分配、RX Ring 入队、小核消费、小核丢弃、TX Ring 出队、Linux 发送完成、最终释放。
- [ ] 为小核消费但不进入 TX Ring 的帧补充回收路径，例如心跳帧、无路由帧、TTL 过期帧、epoch 不匹配帧、非法帧。
- [ ] 设计 `reclaim/free ring` 或等价的“可回收队列 + frame_id + drop reason + ack”机制。
- [ ] 明确小核只写回收标记还是写回收描述符，Linux 何时扫描并释放 Frame Buffer。
- [ ] 增加 Frame Pool 泄漏检测统计：allocated、released、pending_reclaim、leaked_suspect、drop_reason。

验收标准：

- 心跳帧被小核消费后，Frame Buffer 能被 Linux 回收。
- 无路由、TTL 过期、epoch 不匹配等丢弃帧不会长期占用 Frame Pool。
- 压测后 Frame Pool 使用量能回落到稳定水平。

### 2. 冻结共享内存 v2 ABI，消除 v1/v2 分叉

- [ ] 明确 `shared_memory_region_v2` 的正式 ABI 文档位置。
- [ ] 固化 Frame Pool、Descriptor Ring、Pending Bitmap、stats/event area 的结构体定义。
- [ ] 明确 descriptor 字段大小、对齐、字节序、CRC 覆盖范围、状态机和版本号。
- [ ] 明确 v1 `unified_frame_t` / 128B slot / CAN direct 路径仅作为历史参考。
- [ ] 更新仍引用 v1 语义的文档，尤其是 `大小核共享内存IPC接口.md` 和 `web模块设计.md`。
- [ ] 在 README 中标出目标架构为 anyMSG + Frame Pool + Descriptor Ring。

验收标准：

- 新开发代码只依赖 v2 ABI。
- v1 文档不会被误认为后续主开发接口。
- Linux 和 FreeRTOS 使用同一份公共头文件定义共享内存结构。

### 3. 定义 anyMSG 可信性、完整性和入口鉴权策略

- [ ] 定义 `verify_string[16]` 的真实算法或明确替代校验字段。
- [ ] 明确 anyMSG 是否需要端到端 CRC、MAC、签名或 token。
- [ ] 区分链路层 CRC、共享内存 descriptor CRC、anyMSG 业务完整性校验的职责边界。
- [ ] 为 Ethernet、Wi-Fi、4G、Bluetooth 等外部入口定义最小鉴权策略，防止伪造 CID 和伪造控制帧。
- [ ] 定义重放保护策略，例如 timestamp 窗口、sequence、nonce 或 session_id。
- [ ] 明确非法鉴权、非法 CID、非法 type、校验失败的丢弃和统计规则。

验收标准：

- 外部输入不能仅凭构造合法长度字段就进入小核调度。
- 高优先级控制帧有明确可信来源判断。
- Web/日志能看到鉴权失败、完整性失败、重放丢弃等统计。

---

## P1 中高风险

### 4. 补充背压、容量和水位线策略

- [ ] 定义 Frame Pool 总容量、单帧最大长度、单接口可占用上限。
- [ ] 定义每个 RX Ring、TX Ring、重组缓存、本地优先级队列的容量。
- [ ] 为高吞吐入口设置限流策略，避免 Ethernet/4G 抢占全部共享内存。
- [ ] 为 priority 0/1 控制帧预留 Frame Pool 和 TX Ring 水位。
- [ ] 明确 Ring 满、Frame Pool 满、重组缓存满时的丢弃顺序。
- [ ] 定义按接口、按 priority、按会话的 drop 统计。

验收标准：

- 任一高流量入口打满时，不影响高优先级控制帧基本转发。
- 重组缓存耗尽时有确定的淘汰策略。
- 压测能验证队列水位、丢弃计数和恢复行为。

### 5. 冻结 CID 路由规则

- [ ] 固化 `destination_cid` 到目标 TX Ring 的映射规则。
- [ ] 确认 CID 地址首字节段与 FreeRTOS 路由表示例保持一致。
- [ ] 定义网关 CID、广播 CID、保留 CID、非法 CID 的处理方式。
- [ ] 明确路由表来自固定编译配置、Linux 初始化写入，还是共享内存控制区动态更新。
- [ ] 定义路由表更新时的版本号、epoch、原子切换和回滚策略。

验收标准：

- 小核只按一套确定规则选择目标 TX Ring。
- 保留地址和非法地址不会被错误转发。
- 路由表更新不会造成新旧规则混用。

---

## P2 中风险

### 6. 明确端到端实时性边界

- [ ] 定义小核 priority 调度与 Linux 出口真实发送之间的关系。
- [ ] 为 Linux 出口线程或 event loop 定义调度优先级、队列策略和最大阻塞时间。
- [ ] 明确分片发送时高优先级帧是否可以插队。
- [ ] 定义每类接口的目标延迟指标，例如 CAN、RS485、Ethernet、Wi-Fi、Bluetooth、4G。
- [ ] 增加端到端延迟统计：进入 RX Ring、出 TX Ring、Linux 实际发送完成时间。
- [ ] 增加实时性压测用例，覆盖高负载和 TX Ring 拥塞场景。

验收标准：

- priority 0/1 帧不只是在小核内优先，也能在 Linux 出口层获得优先发送。
- 文档能说明哪些接口能提供实时保证，哪些只能提供尽力而为。
- 压测报告能给出端到端最大延迟和丢弃原因。

### 7. 补充安全边界和运维暴露面说明

- [ ] 明确 Web 只读接口是否只绑定可信局域网地址。
- [ ] 定义日志接口是否需要脱敏，避免暴露 CID、token、网络配置等敏感信息。
- [ ] 明确状态快照目录和日志目录的文件权限。
- [ ] 定义生产部署时防火墙、iptables 或上级路由限制建议。

验收标准：

- Web 只读不等于公网安全，文档中有明确部署边界。
- 日志和状态快照不会无意暴露鉴权材料。

---

## 后续整理建议

- [ ] 将本 TODO 清单中的 P0 项拆成独立设计文档或接口文档。
- [ ] 每完成一项，同步更新 `整体架构设计.md`、相关模块设计文档和 README。
- [ ] 在测试计划中补充对应验收用例，避免 TODO 只停留在文档层面。

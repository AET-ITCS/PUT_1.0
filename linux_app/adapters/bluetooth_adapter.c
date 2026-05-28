#include "bluetooth_adapter.h"
#include "physical_interface_adapter.h"
#include "status_collector.h"
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

/**
 * @brief 蓝牙模块内部上下文结构体
 * 
 * 维护蓝牙模块运行所需的底层描述符、线程状态以及统计指标上下文。
 */
typedef struct {
    int listen_fd;                 /**< 蓝牙服务端监听套接字描述符 */
    int client_fd;                 /**< 当前配对连接的客户端套接字描述符 */
    pthread_t thread;              /**< 蓝牙服务异步接收子线程句柄 */
    bool running;                  /**< 蓝牙服务端运行状态标志 */
    bluetooth_status_t status;     /**< 集中上报的蓝牙状态与统计指标数据结构 */
    status_collector_t *collector; /**< 全局集中式状态快照收集器指针 */
} bt_ctx_t;

/**
 * @brief 蓝牙模块全局唯一私有上下文实例
 */
static bt_ctx_t g_bt_ctx = { .listen_fd = -1, .client_fd = -1, .running = false };

/**
 * @brief 获取当前系统单调时间戳（毫秒）
 * 
 * @return uint64_t 当前时间的毫秒数
 */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

/* ---------- 适配器回调接口实现 ---------- */

/**
 * @brief 获取蓝牙物理接口的最大传输单元 (MTU)
 * 
 * 蓝牙适配器受限于 v2 共享内存单 frame_buffer 的 512B 限制。
 * 
 * @param ctx 蓝牙私有上下文指针 (未显式使用)
 * @return int 返回最大传输单元，固定为 512 字节
 */
static int bluetooth_get_mtu(void *ctx) {
    (void)ctx;
    return 512; 
}

/**
 * @brief 物理接收数据包的解析与合法性校验
 * 
 * 校验 anyMSG 帧的总长度是否落在合法的 [40, 512] 字节区间。
 * 
 * @param ctx 蓝牙私有上下文指针 (未显式使用)
 * @param input 待解析的原始数据缓冲区
 * @param input_len 原始数据缓冲区长度
 * @param out 解析与校验后的 anyMSG 结构输出指针 (待具体业务逻辑填充)
 * @return int 0 成功，-1 长度校验失败
 */
static int bluetooth_decode_rx(void *ctx, const uint8_t *input, size_t input_len, adapter_rx_result_t *out) {
    if (input_len < 40 || input_len > 512) return -1;
    // 占位逻辑：未来在此处进行 anyMSG 头部的严格校验与输出结构填充
    (void)ctx; (void)out;
    return 0;
}

/**
 * @brief 经典蓝牙流式物理链路下的分包重组接口
 * 
 * 蓝牙采用两阶段精准 `recv_all` 的防黏包分包切帧策略，因此直接调用解码即可。
 * 
 * @param ctx 蓝牙私有上下文指针
 * @param input 接收数据缓冲区
 * @param input_len 数据长度
 * @param out 重组及校验输出结果
 * @return int 0 成功，非 0 失败
 */
static int bluetooth_reassemble(void *ctx, const uint8_t *input, size_t input_len, adapter_rx_result_t *out) {
    return bluetooth_decode_rx(ctx, input, input_len, out);
}

/**
 * @brief 将 ready 的 anyMSG 帧打包为 RFCOMM 物理发送封包
 * 
 * @param ctx 蓝牙私有上下文指针
 * @param msg 待发送的 anyMSG 数据帧
 * @param out_packet 打包后的输出物理包
 * @return int 0 成功，非 0 失败
 */
static int bluetooth_encapsulate(void *ctx, const anymsg_buffer_t *msg, adapter_tx_packet_t *out_packet) {
    (void)ctx; (void)msg; (void)out_packet;
    // 业务占位：若有蓝牙特有的物理打包封包逻辑在此实现
    return 0;
}

/**
 * @brief 发送帧超出物理 MTU 时的拆包分片接口
 * 
 * @param ctx 蓝牙私有上下文指针
 * @param msg 待拆包的 anyMSG 数据帧
 * @param out_packets 拆分后的物理分包链表
 * @return int 0 成功，非 0 失败
 */
static int bluetooth_fragment_tx(void *ctx, const anymsg_buffer_t *msg, adapter_tx_packet_list_t *out_packets) {
    (void)ctx; (void)msg; (void)out_packets;
    // 业务占位：若 anyMSG 载荷超过最大物理单元，可在此执行分片拆包
    return 0;
}

/**
 * @brief 通过经典蓝牙 RFCOMM 通道真实发送物理数据包
 * 
 * @param ctx 蓝牙私有上下文指针（指向内部 `bt_ctx_t`）
 * @param packet 待发送的物理包描述符
 * @return int 0 发送成功，-1 链路未连接或物理发送失败
 */
static int bluetooth_send(void *ctx, const adapter_tx_packet_t *packet) {
    bt_ctx_t *c = (bt_ctx_t *)ctx;
    if (!c->status.connected) return -1;
    ssize_t sent = send(c->client_fd, packet->data, packet->len, 0);
    if (sent != (ssize_t)packet->len) return -1;
    c->status.tx_count++;
    c->status.last_tx_ms = now_ms();
    return 0;
}

/**
 * @brief 获取蓝牙模块当前最新的运行状态快照
 * 
 * @param ctx 蓝牙私有上下文指针
 * @param out 外部状态输出结构体指针
 * @return int 0 获取成功，-1 参数非法
 */
static int bluetooth_get_status(void *ctx, bluetooth_status_t *out) {
    if (!out) return -1;
    bt_ctx_t *c = (bt_ctx_t *)ctx;
    memcpy(out, &c->status, sizeof(bluetooth_status_t));
    return 0;
}

/* ---------- 蓝牙服务后台接收线程 ---------- */

/**
 * @brief 蓝牙服务端后台独立接收线程函数
 * 
 * 采用“动态两阶段阻塞接收”防御式设计：
 * 1. 阶段一：阻塞读取前 2 字节以获取 anyMSG 的 `msg_length`；
 * 2. 对 `msg_length` 进行边界范围校验（[40, 512] 字节），异常时断开链路触发重连；
 * 3. 阶段二：根据解析出的长度，精准读满剩余字节，规避流式套接字黏包和滑窗错位。
 * 
 * @param arg 传入的 `bt_ctx_t` 上下文指针
 * @return void* 线程返回值 (NULL)
 */
static void *bluetooth_thread(void *arg) {
    bt_ctx_t *c = (bt_ctx_t *)arg;
    while (c->running) {
        uint16_t msg_len = 0;
        // 阶段一：读取 2 字节的消息长度字段
        ssize_t r = recv(c->client_fd, &msg_len, 2, MSG_WAITALL);
        if (r != 2) { goto reconnect; }
        
        // 字节序转换（小端转主机字节序）
        msg_len = le16toh(msg_len);
        
        // 消息长度合法性校验，避免脏报文影响或内存越界
        if (msg_len < 40 || msg_len > 512) { goto reconnect; }
        
        uint8_t buf[512];
        *(uint16_t *)buf = htole16(msg_len);
        
        // 阶段二：精准接收剩余的 bytes
        r = recv(c->client_fd, buf + 2, msg_len - 2, MSG_WAITALL);
        if (r != (ssize_t)(msg_len - 2)) { goto reconnect; }
        
        // 完整包解析与校验
        adapter_rx_result_t rx_res;
        if (bluetooth_decode_rx(c, buf, msg_len, &rx_res) == 0) {
            // 业务占位：此处分配共享内存（Frame Pool），并将帧写入大小核 Descriptor RX Ring
            c->status.rx_count++;
            c->status.rx_bytes += msg_len;
            c->status.last_seen_ms = now_ms();
        } else {
            c->status.parse_error_count++;
        }
        continue;

    reconnect:
        // 当物理链路断开、读取失败或流对齐损坏时进入重连处理
        close(c->client_fd);
        c->client_fd = -1;
        c->status.connected = false;
        
        // 阻塞等待外部蓝牙调试终端发起连接
        struct sockaddr_rc rem_addr;
        socklen_t opt = sizeof(rem_addr);
        c->client_fd = accept(c->listen_fd, (struct sockaddr *)&rem_addr, &opt);
        if (c->client_fd < 0) { sleep(1); continue; }
        
        c->status.connected = true;
        // 将配对客户端的 MAC 地址转换为可读字符串，存入运行状态中
        ba2str(&rem_addr.rc_bdaddr, c->status.connected_client_addr);
    }
    return NULL;
}

/* ---------- 外部生命周期管理 API ---------- */

/**
 * @brief 启动大核经典蓝牙 RFCOMM 服务端监听线程并注册至集中式状态收集器
 * 
 * @param collector 全局集中式状态收集器指针
 * @return int 0 启动成功，-1 启动失败
 */
int bluetooth_server_start(status_collector_t *collector) {
    if (g_bt_ctx.running) return 0;
    
    // 创建经典蓝牙 RFCOMM 协议族的套接字
    int s = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (s < 0) return -1;
    
    struct sockaddr_rc loc_addr = { 0 };
    loc_addr.rc_family = AF_BLUETOOTH;
    loc_addr.rc_bdaddr = *BDADDR_ANY;
    loc_addr.rc_channel = 1; // 默认绑定 RFCOMM Channel 1
    
    if (bind(s, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0) { close(s); return -1; }
    if (listen(s, 1) < 0) { close(s); return -1; }
    
    g_bt_ctx.listen_fd = s;
    g_bt_ctx.client_fd = -1;
    g_bt_ctx.running = true;
    g_bt_ctx.collector = collector;
    g_bt_ctx.status.enabled = true;
    g_bt_ctx.status.listening = true;
    g_bt_ctx.status.started_at_ms = now_ms();
    
    if (collector) {
        // 注册到全局状态收集器，便于 put-webd 读取蓝牙通道的统计与连通性快照
        status_collector_register_adapter(collector, "bluetooth", &g_bt_ctx.status);
    }
    
    // 创建异步后台服务接收线程，保证数据切帧逻辑不阻塞主业务线程
    pthread_create(&g_bt_ctx.thread, NULL, bluetooth_thread, &g_bt_ctx);
    return 0;
}

/**
 * @brief 优雅关闭蓝牙监听服务，断开当前连接，释放系统套接字与子线程资源
 */
void bluetooth_server_stop(void) {
    if (!g_bt_ctx.running) return;
    g_bt_ctx.running = false;
    
    if (g_bt_ctx.client_fd >= 0) close(g_bt_ctx.client_fd);
    if (g_bt_ctx.listen_fd >= 0) close(g_bt_ctx.listen_fd);
    
    pthread_join(g_bt_ctx.thread, NULL);
    g_bt_ctx.status.stopped = true;
    
    if (g_bt_ctx.collector) {
        status_collector_unregister_adapter(g_bt_ctx.collector, "bluetooth");
    }
}

/**
 * @brief 统一物理接口适配器全局实例 (Bluetooth)
 * 
 * 挂载至协议管理器后，使其拥有与其他 5 类物理介质完全一致的路由寻址和收发特征。
 */
physical_interface_adapter_t bluetooth_adapter = {
    .name = "Bluetooth",
    .interface_id = 0x03,
    .get_mtu = bluetooth_get_mtu,
    .decode_rx = bluetooth_decode_rx,
    .reassemble = bluetooth_reassemble,
    .encapsulate = bluetooth_encapsulate,
    .fragment_tx = bluetooth_fragment_tx,
    .send = bluetooth_send,
    .status = (int(*)(void*, void*))bluetooth_get_status
};


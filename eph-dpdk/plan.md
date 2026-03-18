## DPDK 库架构设计

先定义边界：这个库做什么，不做什么。

```
[交易应用层]
    │  send_order(payload, len)
    ▼
┌─────────────────────────────────┐
│         dpdk_transport          │  ← 你要写的库
│  ┌──────────┐  ┌─────────────┐  │
│  │ TLS ctx  │  │  TX engine  │  │
│  └──────────┘  └─────────────┘  │
│  ┌──────────────────────────┐   │
│  │      DPDK EAL / PMD      │   │
│  └──────────────────────────┘   │
└─────────────────────────────────┘
    │
    ▼  DMA
  [ NIC ring ]
```

**库的职责边界**：
- **做**：EAL 初始化、端口配置、mbuf 池管理、TX 零拷贝路径、用户态 TLS 集成
- **不做**：TLS 握手（用 BoringSSL/wolfSSL 的现有实现）、WebSocket 协议解析、业务逻辑

---

## 分层架构

```
┌─────────────────────────────────────────────────┐
│  Layer 4: Public API                            │
│  dpdk_transport_{init, send, poll_rx, destroy}  │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  Layer 3: Session Layer                         │
│  TLS session 绑定、WebSocket frame 封装          │
│  用户态 TLS record layer (BoringSSL kTLS mode)   │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  Layer 2: TX Engine                             │
│  mbuf 生命周期管理、批量发包、ring 写入           │
│  单 lcore 独占、busy poll 模式                   │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  Layer 1: DPDK Platform                         │
│  EAL init、port/queue config、mempool            │
│  RSS/flowdir 配置、统计采集                      │
└─────────────────────────────────────────────────┘
```

---

## Layer 1：DPDK Platform 关键逻辑链

### 1.1 EAL 初始化顺序

这是最容易出错的部分，顺序严格不可调换：

```
rte_eal_init()
    │  ← 必须最先调用，且只能调用一次，全局副作用
    │  ← argv 必须包含 --proc-type, --lcores, --huge-dir
    │
    ▼
rte_eth_dev_count_avail()
    │  ← EAL init 之后才能枚举到 PMD 绑定的网卡
    │  ← 此处要 WARN 日志：如果 count == 0，说明 VFIO 绑定失败
    │
    ▼
rte_pktmbuf_pool_create()
    │  ← mempool 必须在 port configure 之前创建
    │  ← pool size 计算：(nb_rx_desc + nb_tx_desc + burst_size) * nb_ports + CACHE_SIZE
    │  ← 常见错误：size 不是 2^n - 1，会被 DPDK 静默向上取整
    │
    ▼
rte_eth_dev_configure()
    │  ← 关键：rxmode/txmode offload flags 必须与 NIC capabilities 取交集
    │  ← 用 rte_eth_dev_info_get() 先查 dev_info.rx_offload_capa
    │  ← 直接写死 offload flags 是最常见的移植性 bug
    │
    ▼
rte_eth_rx_queue_setup() / rte_eth_tx_queue_setup()
    │  ← nb_rx_desc / nb_tx_desc 必须在 dev_info.{rx,tx}_desc_{lim} 范围内
    │  ← 否则 DPDK 会返回 -EINVAL，但 errno 信息极其模糊
    │
    ▼
rte_eth_dev_start()
    │  ← 启动后立即 rte_eth_link_get_nowait()
    │  ← link up 可能需要等待（光口 ~100ms），需要 poll + timeout 逻辑
    │
    ▼
rte_eth_promiscuous_enable()  // 可选，调试用
```

**日志分层设计**：
```
EAL init 参数展开          → TRACE（敏感，可能含 hugepage 路径）
port/queue 配置参数        → DEBUG（每次启动记录一次，用于复现）
link up/down 事件          → INFO（运营关注）
offload 不匹配降级         → WARN（性能影响，需人工确认）
EAL/port/queue 返回负值    → ERROR（附上 rte_strerror(ret)）
```

### 1.2 Hugepage 陷阱

这是云 VM 和开发机上最高频的坑：

```
/sys/kernel/mm/hugepages/ 配置
    │  ← 必须在 EAL init 之前配好，DPDK 无法运行时扩容
    │
    ▼
rte_eal_init 尝试 mmap hugepages
    │  ← 失败时返回 -1，但 rte_errno 不一定被正确设置
    │  ← 必须检查 /dev/hugepages 挂载点，记录实际可用 huge page 数
    │  ← WARN：如果 available_hugepages < required，会 fallback 到 4K page
    │      4K page 下 TLB miss 剧增，latency 恶化但不崩溃，极难排查
```

---

## Layer 2：TX Engine 关键逻辑链

### 2.1 mbuf 生命周期（最容易内存泄漏的地方）

```
rte_pktmbuf_alloc(pool)
    │  ← 失败（pool 耗尽）：ERROR + 当前 pool 使用率
    │  ← 不要在热路径 alloc，应预分配 burst buffer
    │
    ▼  填充 payload（直接写 rte_pktmbuf_mtod 返回的指针）
    │
    ▼
rte_eth_tx_burst(port, queue, &mbuf, 1)
    │  ← 返回值 nb_sent 可能 < nb_to_send（队列满）
    │  ← 未发送的 mbuf 必须手动 rte_pktmbuf_free()，否则 pool 泄漏
    │  ← 这是 DPDK 新手最高频的 bug
    │
    ▼
    │  mbuf 发送后所有权转给 NIC driver，禁止再访问
    │  ← 不需要也不能手动 free 已发送的 mbuf
```

**日志分层**：
```
alloc 失败                 → ERROR（附上 rte_mempool_avail_count）
tx_burst 返回 < n         → WARN（附上 dropped count，每 1000 次汇总一次避免刷屏）
mbuf pool 使用率 > 80%    → WARN（预警，防止 alloc 失败）
每次发包                  → TRACE（热路径，compile-time filter 掉）
```

### 2.2 lcore 线程模型

DPDK 的 lcore 不是普通线程，有严格约束：

```
主线程 (master lcore)
    │  rte_eal_init() → rte_eal_mp_remote_launch()
    │
    ├── TX lcore（独占，SCHED_FIFO + isolcpus）
    │       busy poll loop：rte_eth_tx_burst()
    │       ← 禁止在此 lcore 上调用任何阻塞 syscall
    │       ← 禁止 malloc/free（会触发 ptmalloc lock）
    │       ← 禁止 spdlog 异步 sink（其内部有锁），只能用无锁 ring 暂存日志
    │
    └── RX lcore（可选，或与 TX 共用）
            busy poll loop：rte_eth_rx_burst()
```

**关键约束**：热路径 lcore 上**不能直接调用 spdlog**，否则锁竞争会破坏 latency。需要设计一个无锁 log ring：

```
TX lcore → [lock-free SPSC ring] → 日志 lcore → spdlog sink
```

日志 ring 的写入只在 WARN/ERROR 级别触发，TRACE/DEBUG 在 compile time 直接消除。

---

## Layer 3：TLS Session 关键逻辑链

这是整个库里最复杂的部分，因为 DPDK 无 socket，TLS 库的 BIO 层需要完全替换。

### 3.1 自定义 BIO 对接

```
TLS 握手（在主线程，允许阻塞）:
    BoringSSL/wolfSSL
        │  ← 替换默认 socket BIO → custom DPDK BIO
        │  ← custom BIO 的 write 调用 dpdk_send_raw()
        │  ← custom BIO 的 read 调用 dpdk_recv_raw()（等待 RX burst）
        │
        ▼  握手完成后
    SSL_get_current_cipher() → 记录 cipher suite  [INFO]
    提取 session key / IV / seq  → 用于 record layer
        │
        ▼
    切换到 TX lcore 热路径:
        TLS record layer 直接在 mbuf 上加密
        ← 加密完成直接 rte_eth_tx_burst()，零拷贝
```

### 3.2 握手与热路径的切换边界

```
握手阶段（主线程）        → 热路径阶段（TX lcore）
        │                           │
        ▼                           ▼
  允许系统调用              禁止系统调用
  允许 heap alloc          禁止 heap alloc
  允许 spdlog 直接写       只能写 log ring
  可以失败重试             失败直接计数+丢弃（不重试）
```

切换时序有一个经典竞态：

```
主线程: session 初始化完毕，设置 ready flag
TX lcore: 读取 ready flag → 开始发包
          │
          ← 必须用 atomic release/acquire 语义
          ← 不能只用 volatile，不能只用普通 bool
          ← 否则：TX lcore 看到 ready=true，但 session key 还未对主线程可见
```

---

## Layer 4：Public API 设计

```cpp
// 配置结构（编译期 vs 运行期分离）
struct DpdkConfig {
    // 运行期：从 CLI/env 读取
    uint16_t port_id;
    uint16_t tx_queue_id;
    uint16_t nb_tx_desc;     // 建议 512
    uint16_t nb_rx_desc;     // 建议 256
    uint32_t mbuf_pool_size; // 建议 4096 - 1
    std::string_view remote_host;
    uint16_t remote_port;
    LogLevel log_level;      // runtime configurable

    // 编译期：影响代码生成
    // 通过 SPDLOG_ACTIVE_LEVEL 控制
};

// 三个核心生命周期函数
Result<DpdkTransport> dpdk_transport_init(const DpdkConfig&);
//  └── 完成 EAL init → port config → TLS 握手 → lcore 启动
//  └── 失败时保证资源全部释放（RAII 包裹）

int dpdk_transport_send(DpdkTransport*, const void* payload, size_t len);
//  └── 热路径，从应用线程推入 SPSC ring → TX lcore 消费
//  └── 非阻塞，ring 满返回 -EAGAIN

void dpdk_transport_destroy(DpdkTransport*);
//  └── 发送 EOS 到 TX lcore → 等待 lcore 退出 → 释放 mempool → rte_eal_cleanup
```

---

## spdlog 集成策略

### Compile-time filter 配置

```cmake
# xmake.lua 中
-- Release/production 构建：只编译 INFO 及以上
add_defines("SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO")

-- Debug 构建：允许到 TRACE
add_defines("SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE")
```

### Logger 命名层级

```
dpdk.platform    ← EAL、端口、hugepage 相关
dpdk.txengine    ← mbuf、burst、ring 相关
dpdk.tls         ← 握手、cipher、session 切换
dpdk.api         ← 公开接口的入口/出口
```

好处：可以对不同子系统设置不同的 runtime level，比如生产上 `dpdk.platform=INFO` 但排查 TX 问题时临时把 `dpdk.txengine` 调到 `DEBUG`。

### 热路径日志规则

```
热路径（TX lcore busy poll loop）:
  TRACE/DEBUG → compile time 完全消除（SPDLOG_ACTIVE_LEVEL 控制）
  WARN/ERROR  → 写入无锁 SPSC ring，异步输出
  原则：热路径内任何日志调用展开后必须是 if (false) {} 或无锁写
```

---

## 最容易出错的逻辑链汇总

| # | 位置 | 错误现象 | 根因 |
|---|------|---------|------|
| 1 | mbuf tx_burst 后不 free 未发出的 | pool 缓慢耗尽，数小时后崩溃 | 返回值未检查 |
| 2 | TLS session key 切换无 acquire | 偶发加密错误，极难复现 | memory order 缺失 |
| 3 | lcore 热路径调用 spdlog 直接写 | latency 突刺 +50µs，p99 恶化 | logger 内部有互斥锁 |
| 4 | hugepage 不够 fallback 4K page | latency 正常但 p99 变差 3-5x | DPDK 静默降级 |
| 5 | offload flags 未与 NIC capa 取交集 | port start 失败，错误信息无意义 | 硬编码 flags |
| 6 | EAL init 在 fork 之后调用 | 子进程 crash，parent 正常 | EAL 是全局状态 |

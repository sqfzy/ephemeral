# Ephemeral HFT Library — Development Plan

## Current State

ephemeral 已有优秀的底层基础设施：
- **eph-containers**: SPSC lock-free 队列（BoundedQueue、EvictingQueue）+ 变长消息包装
- **eph-utils**: TSC 硬件计时器、CPU 亲和性、Hugepage、HdrHistogram、SystemStats
- **eph-net**: Socket/DPDK 双后端 TCP + TLS 1.3 + WebSocket + 多种 Framer
- **eph-dpdk**: 用户态 TCP、ARP、DNS、连接器
- **eph-itch**: ITCH 5.0 零拷贝解析器
- **eph-fix**: FIX 协议解析/构建

**核心短板**：应用层几乎完全缺失（order book、OMS、exchange adapter、风控），无法直接用于交易。

## Target Market

以**加密货币 HFT** 为首要目标市场（WebSocket API 覆盖主流交易所），后续扩展到传统市场。

---

## Phase 1 — Minimum Viable HFT System

> 目标：在 Binance 上跑通完整交易流程（订阅行情 → 策略决策 → 下单 → 收确认）
> 性能目标：tick-to-trade < 5μs

### 1.0 前置条件：CI & Quality Gate

- [ ] GitHub Actions CI pipeline
  - 多编译器：GCC 13+, Clang 17+
  - 构建模式：Debug + Release
  - Sanitizer：ASan, UBSan, TSan
- [ ] 所有现有 benchmark 纳入 CI，确立性能基线
- [ ] 性能回归阈值：P50 退化 > 5% 或 P99 退化 > 10% 阻止 merge

### 1.1 eph-json — JSON 支持

- [ ] 集成 simdjson（需确认 Apache 2.0 许可证兼容性）
- [ ] JSON 解析：On-Demand API，零拷贝提取字段
- [ ] JSON 构建：轻量 builder（交易所 REST 请求体构造）
- [ ] Benchmark：解析 Binance depth/trade 消息的吞吐量

### 1.2 eph-net 扩展 — HTTP Client

- [ ] 基于现有 SocketTransport + TLS 实现 HTTP/1.1 client
  - POST/GET/PUT/DELETE
  - 请求签名钩子（用于交易所 API 认证）
  - 响应 JSON 解析
- [ ] 连接复用（HTTP keep-alive）
- [ ] 超时 + 重试策略

### 1.3 eph-exchange — Binance Adapter（参考实现）

- [ ] **Market Data**
  - WebSocket 订阅：depth stream（增量 + snapshot）、trade stream、ticker
  - 自动重连 + 心跳管理
  - 消息序列号检查（Binance lastUpdateId gap detection）
- [ ] **Trading**
  - REST API：下单、撤单、查询订单、查询持仓
  - HMAC-SHA256 签名（基于 aws-lc）
  - WebSocket User Data Stream（订单/持仓推送）
  - 自动适配 Binance API 限速规则
- [ ] **API 设计**（polling 优先）
  ```cpp
  // Low-level polling API (Phase 1)
  auto exchange = eph::binance::create(config);
  while (running) {
      exchange.poll_market_data([&](const Quote& q) { /* ... */ });
      exchange.poll_user_data([&](const Fill& f) { /* ... */ });
      if (should_trade) {
          auto result = exchange.place_order(order);
      }
  }
  ```

### 1.4 eph-book — Price-Level Order Book

- [ ] Price-level aggregated book（每个价格跟踪总量 + 订单数 + 时间戳）
- [ ] Cache-friendly 设计：单个 price level ≤ 64 bytes
- [ ] 性能目标：< 50ns/update
- [ ] 从 Binance depth stream 增量构建 + snapshot 初始化
- [ ] BBO (Best Bid/Offer) O(1) 提取
- [ ] 深度 N 切片方法
- [ ] Cross-validation：定期与交易所 snapshot 对账

### 1.5 eph-oms — Basic Order Management

- [ ] Order 生命周期：New → Acknowledged → PartiallyFilled → Filled / Cancelled / Rejected
- [ ] Order 类型：Limit, Market, IOC, FOK, Post-Only
- [ ] 预分配 Order 对象池（避免热路径 new/delete）
- [ ] 64-bit 单调递增 Order ID
- [ ] Position tracking：实时持仓 + 未实现 PnL
- [ ] Fill management：成交回报处理

### 1.6 基本风控（强制，不可绕过）

- [ ] **Kill switch** — 一键全部撤单 + 停止新订单（< 1μs）
- [ ] **Position limit** — 硬性仓位上限
- [ ] **Rate limiter** — token bucket，适配交易所限速
- [ ] **Price sanity check** — 订单价格偏离 mid price 超过阈值则拒绝
- [ ] 总热路径开销 < 20ns

### 1.7 Prometheus Metrics Exporter

- [ ] 利用现有 HdrHistogram + SystemStats
- [ ] Prometheus text format (`/metrics` endpoint 或文件输出)
- [ ] 核心指标：tick-to-trade 延迟分布、消息吞吐量、order book 深度、持仓/PnL

### Phase 1 Acceptance Criteria

```cpp
auto exchange = eph::binance::create(config);
exchange.subscribe({"BTC-USDT"});
while (running) {
    exchange.poll_market_data([&](const Quote& q) {
        book.update(q);
        if (strategy.should_buy(book)) {
            auto order = Order::limit_buy("BTC-USDT", price, qty);
            // 风控自动检查：position limit + rate limit + price sanity
            auto result = exchange.place_order(order);
        }
    });
    exchange.poll_user_data([&](const Fill& f) {
        oms.on_fill(f);
    });
}
// kill switch: exchange.cancel_all();
```

---

## Phase 2 — Production Hardening

> 目标：生产环境可靠运行，具备审计和调试能力

### 2.1 Binary Message Journal

- [ ] mmap ring buffer，顺序写入 < 100ns/msg
- [ ] 所有收发消息的零拷贝二进制日志
- [ ] Nanosecond 级时间戳（共享 TSC 读取，与 metrics 合并）
- [ ] Deterministic replay：给定相同事件序列产出完全相同的状态
- [ ] 架构预留：Phase 1 在消息路径预留 `journal_hook()` 调用点

### 2.2 Memory Management

- [ ] **Object Pool / Slab Allocator** — 固定大小对象的 O(1) 分配/释放
- [ ] **Arena Allocator** — 请求级别内存管理，一次 reset
- [ ] 默认行为即最优，高级用户可通过配置调整
- [ ] 后续考虑 PMR (Polymorphic Memory Resource) 统一接口

### 2.3 Complete Risk Management Framework

- [ ] Loss limit — 单日/单笔最大亏损触发自动平仓
- [ ] 异常检测 — 价格跳变超过阈值暂停交易
- [ ] 可插拔自定义规则框架（核心风控仍强制）
- [ ] 编译时风控级别：minimal / standard / paranoid

### 2.4 Testing Enhancement

- [ ] **Fuzz test** — libFuzzer 覆盖所有 parser（ITCH、FIX、WebSocket、JSON、HTTP）
- [ ] **Mock TCP** — 标准 mock 实现，模拟延迟/丢包/partial send
- [ ] **Long-running stability test** — nightly，小时级运行，检测内存泄漏/timer 漂移
- [ ] **Property-based testing** — encode(decode(x)) == x 验证

### 2.5 Developer Experience

- [ ] CMake find_package 支持（消费者模式）
- [ ] API 文档（Doxygen，header 内注释）
- [ ] Getting Started 教程（安装 → 构建 → 运行 → 连接交易所）
- [ ] DPDK 部署指南（SR-IOV、hugepage、CPU 隔离、内核参数）
- [ ] Changelog（conventional commits + 自动生成）

### 2.6 High-Level Callback API

- [ ] `exchange.run(handlers)` — polling 之上的 thin wrapper
- [ ] 适用于不需要微秒级优化的策略

### 2.7 Second Exchange Adapter (OKX or Bybit)

- [ ] 从 Binance + 第二个 adapter 中提炼标准化接口
- [ ] 标准化事件模型：Quote, Trade, OrderBookUpdate, Fill, OrderAck

### 2.8 NUMA-Aware Allocation

- [ ] 确保 queue buffer、packet buffer 分配在正确的 NUMA node
- [ ] 对多 socket 机器有 2-3x 延迟改善

---

## Phase 3 — Traditional Market & Advanced Features

> 目标：覆盖传统交易所，提供高级功能

### 3.1 Network Layer Extension

- [ ] **UDP Multicast** — MulticastReceiver, IGMP join, source-specific multicast
- [ ] **MoldUDP64** — NASDAQ 传输层协议
- [ ] **DatagramTransport concept** — 与 StreamTransport 并列的传输概念
- [ ] **io_uring 后端** — socket 路径消除系统调用开销（busy-poll 一等公民）
- [ ] **AF_XDP** — 轻量级 kernel bypass（不需要完整 DPDK 环境）
- [ ] **连接管理器** — 多连接生命周期管理、健康检查、自动重连策略
- [ ] **TLS session ticket** — 重连延迟从 ~2 RTT 降到 ~1 RTT

### 3.2 Protocol Extension

- [ ] **SBE (Simple Binary Encoding)** — CME 等交易所使用
- [ ] **OUCH** — NASDAQ 低延迟订单入口
- [ ] **FAST** — 传统交易所市场数据压缩（长期）
- [ ] **Exchange concept 体系** — 从多个 adapter 提炼的统一概念

### 3.3 Advanced Order Book

- [ ] **Order-by-order book** — 跟踪每个独立订单（做市商需求）
- [ ] **基于价格数组 O(1) 查找**（适用于已知价格范围的场景）
- [ ] **Implied order book** — 合成期权/期货 spread book（长期）

### 3.4 Testing & Simulation

- [ ] **Mini exchange simulator** — 简单 matching engine + WebSocket 接口
- [ ] **Market data replay** — 从 pcap 或 journal 加载历史数据
- [ ] **Latency injection** — 模拟不同网络延迟
- [ ] **Order fill simulation** — 队列模型、概率模型

### 3.5 Advanced Infrastructure

- [ ] **Shared Memory IPC** — ShmQueue，进程间通信
- [ ] **编译时可观测性级别** — EPHEMERAL_OBSERVE_LEVEL (OFF / BASIC / FULL)
- [ ] **Instrument trait/concept** — 统一可观测性接口，template 注入分发
- [ ] **MPSC 队列** — 多策略线程向同一发送线程提交订单
- [ ] **完整合规框架** — MiFID II / Reg NMS 监管报告

---

## Open Decisions

以下权衡需要在实施前确定：

| 决策项 | 选项 A | 选项 B | 判断依据 |
|--------|--------|--------|----------|
| Order Book 实现 | sorted array O(1) | sorted vector + binary search | 目标更新频率：百万级/s 选 A，千级/s 选 B |
| JSON 库 | simdjson (Apache 2.0) | 其他 | 许可证兼容性确认 |
| DPDK TCP 重传 | 维持检测丢包→重连 | 添加轻量重传 | 部署网络质量 |

---

## Architecture Principles

1. **Polling 优先** — 底层 API 用 busy-polling，callback 是 thin wrapper
2. **先实现后抽象** — Phase 1 硬编码 Binance，Phase 2 从多 adapter 提炼 concept
3. **风控不可绕过** — 核心风控（kill switch、position limit）是强制的
4. **编译时可配** — 日志级别、风控级别、可观测性级别均为编译时开关
5. **默认即最优** — 用户无需配置即可获得合理性能，高级用户可调优
6. **零分配热路径** — 热路径上使用预分配对象池，禁止 new/delete

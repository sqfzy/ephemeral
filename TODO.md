# TODO

## 大功能

- [x] **eph-gateway（连接管理层）** — Gateway: 多 Transport 生命周期管理 + 健康监控 + 优先级 + 20 tests。
- [ ] **UDP multicast receiver** — equity 行情接收（CME MDP3.0、Nasdaq TotalView via MoldUDP64）。`MulticastReceiver<DpdkImpl>` + IGMP join。中复杂度，2-3h。
- [ ] **io_uring 后端** — 填补 socket/DPDK 中间层。极高复杂度，需 kernel API + fallback。**优先级低，暂不实施**。

## 中功能

- [x] **MetricsSink concept** — MetricsSink concept + NullSink + ConsoleSink + 18 tests。
- [x] **eph-audit 审计日志骨架** — AuditLog: ring buffer 审计日志 + 14 event types + 文件持久化 + 14 tests。
- [x] **Crash handler + kill switch** — KillSwitch: 协调多 Transport 紧急关闭 + 信号处理 + 15 tests。

## 小改进

### 代码质量
- [x] MapBook `double` key 与 ArrayBook `epsilon` 语义不一致 — MapBook 已加 epsilon-tolerant find_approx
- [x] `RateLimiter::available()` 中 `const_cast` — 改为非 const 方法
- [x] `format_mac` 在 `arp.hpp` 和 `tcp.hpp` 中重复 — 合并到 `net_header.hpp`
- [x] 删除 `SharedRxDispatcher`（`shared_rx.hpp`）— 已删除，benchmark 迁移到 Reactor

### 文档
- [x] `summary.md` 更新 — 反映新模块（eph-core、eph-json、eph-book）和新架构（Reactor、flow_steering）
- [x] `docs/` 目录新增模块指南 — json-guide.md、orderbook-guide.md、reactor-guide.md
- [x] eph-json、eph-book 模块级 README

### 测试
- [ ] DPDK TCP 状态机行为测试 — 需要 mock 环境模拟 SYN/ACK/FIN/RST
- [ ] Reactor + flow_steering 集成测试 — 需要真实 RSS NIC（非 ENA）

## 审计遗留项 (2026-04-01)

> Critical/Major 已全部修复。以下 Minor/Nit 项不阻塞，按优先级排列。

### Minor — 安全/正确性
- [x] eph-dpdk: `ip_id` 非线程安全递增 — 文档化单线程约束 (net_header.hpp:224-227)
- [x] eph-dpdk: ARP 无 spoofing 防护 — 文档化 gateway_mac 缓解方案 (arp.hpp)
- [x] eph-dpdk: DNS 16-bit txid 可暴力 — 文档化 CSPRNG + 替代方案 (dns.hpp)
- [x] eph-net: WebSocket masking key fallback PRNG 有偏 — 改用 SplitMix64 (websocket.hpp)
- [x] eph-net: HTTP Content-Length 无范围验证 — 加 256MiB 上限 (http_client.hpp)
- [x] eph-fix: Leap second 验证不完整 — 拒绝 second=60 (parser.hpp)
- [x] eph-fix: Risk check 应拒绝 non-finite notional/avg_price — 加 kInvalidInput (risk_check.hpp)
- [x] eph-fix: PossDupFlag 仅 log 不过滤 — 文档化调用方责任 (session.hpp)

### Minor — 性能
- [x] eph-dpdk: TCP reorder buffer drain O(N²) — 文档化 ReorderSlots=64 可接受 (tcp.hpp)
- [x] eph-dpdk: Reactor 线性扫描 O(N)/packet — 文档化 N<8 最优 (reactor.hpp)
- [x] eph-json: BookTicker::mid_price()/spread() 每次重新解析 — 缓存 bid/ask 价格 (binance.hpp)
- [x] eph-net: SOCKS5 handshake 堆分配改为栈 buffer (proxy.hpp)

### Minor — API/设计
- [x] eph-core: `DecodedFrame::payload` 指针生命周期文档化 (framer_concept.hpp)
- [x] eph-core: `msg_type` 字段语义文档化 (framer_concept.hpp)
- [x] eph-core: `poll_rx` callback uint16_t 文档化容量限制 (tcp_concept.hpp)
- [x] eph-json: binance/okx/bybit subscribe_message() — 文档化无法抽象原因
- [x] eph-json: Binance REST get_depth() limit 参数验证 (binance_rest.hpp)
- [x] eph-json: MapBook 加 bids()/asks() range view (map_book.hpp)
- [x] eph-utils: AuditLog::record() 返回 bool (audit_log.hpp)
- [x] eph-utils: TSC 校准方差可通过 get_calibration_cv() 获取 (time.hpp)
- [x] eph-utils: `get_cpu_base_frequency()` 返回 optional (cpu.hpp)
- [x] eph-itch: Logger 返回类型统一为 const shared_ptr& (moldudp64.hpp)
- [x] eph-itch: Price 转换常量命名 kItchPriceDivisor/kLuldPriceDivisor (messages.hpp)

### Nit
- [x] eph-core: TCP state enum 注明 "client-side only" (tcp_concept.hpp)
- [x] eph-core: MessageFramer encode() buffer 前置条件文档化 (framer_concept.hpp)
- [x] eph-containers: stats() clamping 加 debug log (bounded_queue.hpp)
- [x] eph-containers: RingBuffer 线程安全 HB chain 文档化 (ring_buffer.hpp)
- [x] eph-net: ws::build_pong/ping_frame 移除多余 null guard (websocket.hpp)
- [x] eph-fix: Session 线程模型文档化 (session.hpp)
- [x] eph-fix: Repeating group 截断加 log (parser.hpp)
- [x] eph-fix: OrderManager 处理 OrderCancelReject (order_manager.hpp)
- [x] eph-itch: OUCH builder 移除冗余 assert (ouch.hpp)
- [x] eph-utils: HdrHistogram 常量命名 kMaxCountsLen (hdr_histogram.hpp)

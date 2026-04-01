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
- [ ] `summary.md` 更新 — 反映新模块（eph-core、eph-json、eph-book）和新架构（Reactor、flow_steering）
- [ ] `docs/` 目录新增模块指南 — eph-json 使用指南、eph-book 订单簿指南、Reactor 多连接指南
- [ ] eph-json、eph-book 模块级 README

### 测试
- [ ] DPDK TCP 状态机行为测试 — 需要 mock 环境模拟 SYN/ACK/FIN/RST
- [ ] Reactor + flow_steering 集成测试 — 需要真实 RSS NIC（非 ENA）

## 审计遗留项 (2026-04-01)

> Critical/Major 已全部修复。以下 Minor/Nit 项不阻塞，按优先级排列。

### Minor — 安全/正确性
- [ ] eph-dpdk: `ip_id` 非线程安全递增 — 改 atomic 或文档约束 (net_header.hpp:277)
- [ ] eph-dpdk: ARP 无 spoofing 防护 — 文档化 gateway_mac 缓解方案 (arp.hpp:148-180)
- [ ] eph-dpdk: DNS 16-bit txid 可暴力 + 无 MAC 验证 (dns.hpp:493-618)
- [ ] eph-net: WebSocket masking key fallback PRNG 有偏 (websocket.hpp:276-288)
- [ ] eph-net: HTTP Content-Length 无范围验证 (http.hpp:185-186)
- [ ] eph-fix: Leap second 验证不完整 — 任意日期接受 second=60 (parser.hpp:141)
- [ ] eph-fix: Risk check 应拒绝 non-finite notional/avg_price (risk_check.hpp:144-148)
- [ ] eph-fix: PossDupFlag 仅 log 不过滤 — 文档化调用方责任 (session.hpp:307-329)

### Minor — 性能
- [ ] eph-dpdk: TCP reorder buffer drain O(N²) — profile ReorderSlots=64 (tcp.hpp:1008-1028)
- [ ] eph-dpdk: Reactor 线性扫描 O(N)/packet — benchmark >8 连接场景 (reactor.hpp:244-281)
- [ ] eph-json: BookTicker::mid_price()/spread() 每次重新解析 (binance.hpp:142-158)
- [ ] eph-net: SOCKS5 handshake 堆分配可改为栈 buffer (proxy.hpp:235,266,320)

### Minor — API/设计
- [ ] eph-core: `DecodedFrame::payload` 指针生命周期未文档化 (framer_concept.hpp:44-49)
- [ ] eph-core: `msg_type` 字段混淆 WS/ITCH/FIX 语义 (framer_concept.hpp:46-47)
- [ ] eph-core: `poll_rx` callback uint16_t 对 jumbo frame 不够 (tcp_concept.hpp:84-87)
- [ ] eph-json: binance/okx/bybit subscribe_message() 大量重复代码
- [ ] eph-json: Binance REST get_depth() 不验证 limit 参数 (binance_rest.hpp:316-317)
- [ ] eph-json: MapBook 缺 bids()/asks() span — 与 signals.hpp 不兼容 (map_book.hpp)
- [ ] eph-utils: AuditLog::record() 溢出静默 — 考虑 bool 返回 (audit_log.hpp:132-149)
- [ ] eph-utils: TSC 校准方差仅 log 不返回调用方 (time.hpp:310-315)
- [ ] eph-utils: `get_cpu_base_frequency()` 返回 fallback 1.0GHz 而非 optional (cpu.hpp:395)
- [ ] eph-itch: Logger 返回类型不一致：raw ptr vs shared_ptr ref (moldudp64.hpp:48-54)
- [ ] eph-itch: Price 转换魔法常量应命名为常量 (messages.hpp)

### Nit
- [ ] eph-core: TCP state enum 应注明 "client-side only" (tcp_concept.hpp:18-29)
- [ ] eph-core: MessageFramer encode() buffer 前置条件未强制 (framer_concept.hpp:69)
- [ ] eph-containers: stats() clamping 可能隐藏 bug — 加 debug log (bounded_queue.hpp:900-904)
- [ ] eph-containers: RingBuffer 线程安全注释应解释 HB chain (ring_buffer.hpp:28-32)
- [ ] eph-net: ws::build_pong/ping_frame 多余 null guard (websocket.hpp:607-619)
- [ ] eph-fix: Session 混合 memory ordering 策略 — 文档化线程模型
- [ ] eph-fix: Repeating group 静默截断应 log (parser.hpp:440-486)
- [ ] eph-fix: OrderManager 未跟踪 OrderCancelReject 状态转换
- [ ] eph-itch: OUCH builder 中 assert + runtime null check 冗余 (ouch.hpp:118,177,222)
- [ ] eph-utils: HdrHistogram 魔法常量 10'000'000 应命名 (hdr_histogram.hpp:129)

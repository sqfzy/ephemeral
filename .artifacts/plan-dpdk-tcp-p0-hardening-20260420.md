# Plan: eph-net-dpdk TCP 生产级 P0 补强

> 把用户态 TCP 栈从"HFT 数据中心 happy path 够用"提升到"任何线上事故都不会让人怀疑栈本身"的等级。补 6 项 P0 缺口 + 一张状态机 conformance 表作为安全网。

---

## Context

**为什么做**：上一次 `/discuss` 评估了 `eph::dpdk::TcpSession` 的 RFC 793 状态机覆盖度——9/11 状态实现得很扎实，但**有几个工程缺口在事故发生时会非常致命**：

- 跨网段 / vSwitch / VLAN tagged 路径上对端 MSS 与本地不一致 → 包静默黑洞
- NAT/防火墙 idle 超时 → 静默单向断开 → 应用以为连接活着
- 高频重连复用同一 src_port → TIME_WAIT 内 4-tuple 冲突 → 协议未定义行为
- 进程 panic / 析构时不发 RST → 对端要等数十秒才知道连接死了
- ICMP Frag Needed 完全无视 → 路径 MTU 变化时只能"延迟突然变高+丢包"靠人脑诊断

调研后**修正**了三个原以为缺失但其实已经存在的项：
- ✅ DF bit 已经设了（`packet_template.hpp:154, 250` → `kIpDontFragment=0x4000`）
- ✅ SYN 已经发送 MSS / WSCALE / SACK_PERM option（`packet_core.hpp:281-300`）—— 但 RX 不解析
- ✅ `reset()` 已经发真 RST（`tcp.hpp:1298-1319`）—— 但 destructor 不发

**意图结果**：补完这 6 项后，DPDK TCP 栈在数据中心场景下的"已知不补会出事的项"清空。仍然**坚定不实现**拥塞控制 / SACK / Timestamps（P2）和 Listen/Accept / Window Scaling（P1，下迭代再看）。

---

## 定位与边界

### In scope
- `TcpConfig` 新增 keepalive 字段 + `effective_mss` 协商存储
- 解析 incoming SYN-ACK 的 TCP options（MSS / WSCALE）
- 用 `effective_mss` 替换所有 `config_.mss` 的 send 路径决策
- `TcpSession` 加 keepalive timer + probe send + 死亡判定
- `DpdkTcpStream::create` 在 `src_port=0` 时调 `poller->pick_src_port`
- `DpdkPoller` 增加 ICMP callback API；fallback 路径解析 ICMP Type 3 Code 4
- `~TcpSession()` 在 `state != Closed` 时发 RST 兜底
- `StreamMetric` 增加 7 项 `tcp.*` / `icmp.*`；`DpdkTcpStream::metric()` lazy-read 这些条目
- 新增 TCP 状态机 conformance 表驱动测试（`tests/legacy/test_tcp_conformance.cpp`）

### Out of scope（明确不做）
- **Listen / Accept**（P1，留给下迭代——目前是纯 client 栈）
- **Window Scaling**（P1，跨地域链路才需要，本迭代先不动）
- **拥塞控制 / Reno / Cubic / BBR**（永不做：HFT 专线场景反而有害）
- **SACK**（永不做：丢一段直接重连比精细恢复快）
- **TCP Timestamps / PAWS**（永不做：短连接 + 频繁重连 seq wrap 不可达）
- **IPv6**（暂不做：交易所主流仍 IPv4）
- **多 RX queue / RSS**（性能扩展，不是正确性问题）

---

## 技术选型

| 类别 | 选择 | 理由 |
|------|------|------|
| MSS 协商存储 | `TcpSession::effective_mss_` (uint16_t) | 单值即可；任何降级（peer/ICMP）都 `std::min` 进去 |
| Keepalive 触发器 | 调用方驱动 `tick_keepalive(now_tsc)` | 与 `flush_pending_ack` 一致的 pull 模型；不引入定时器线程 |
| Keepalive 报文 | 零长度 ACK with `seq = snd_nxt-1` | 标准 Linux 风格 keepalive probe，对端必回 ACK |
| Auto src_port | `DpdkTcpStream::create()` 调 `poller->pick_src_port` | poller 已实现；只需在 create 路径绕开 `validate()` 的 src_port=0 拒绝 |
| ICMP 接收路径 | `DpdkPoller` fallback path + callback | Poller 是包入口；session 看不到全局 ICMP |
| ICMP → session 路由 | 解析 ICMP payload 中的 embedded 4-tuple 反查 | RFC 792 标准；不依赖 sequence number |
| Destructor RST | `~TcpSession() if (state_ != Closed) reset()` | reset() 已经处理 best-effort send + state 推进 |
| Stats → metric | `metric()` 内 switch，TCP/ICMP 槽位直读 `sess_.tcp_stats()` | 避免与已有 `stats_.X++` 重复计数 + 避免 hot path 双原子操作 |
| Conformance test | 仿 `tests/legacy/test_tcp_state_machine.cpp` 的 `FakeTcpMbuf` 模式 | 已验证可行；纯 mempool=nullptr，无 NIC 依赖 |

---

## 架构设计

### 模块影响

| 模块 / 文件 | 改动 | 大小估计 |
|---|---|---|
| `eph-net/include/eph/net/stream_metrics.hpp` | enum 加 7 项 + name table | +20 行 |
| `eph-net-dpdk/include/eph/dpdk/packet_parse.hpp` | 新增 `parse_tcp_options()` + `parse_icmp()` | +80 行 |
| `eph-net-dpdk/include/eph/dpdk/tcp.hpp` | TcpConfig + TcpSession 新成员/方法 | +200 行 |
| `eph-net-dpdk/include/eph/net/dpdk/poller.hpp` | ICMP callback API + fallback 路径 | +60 行 |
| `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` | auto src_port + metric() lazy-read switch + ICMP callback wiring | +50 行 |
| `tests/integration/test_stream_metrics.cpp` | 扩展覆盖 7 项新 metric | +40 行 |
| `eph-net-dpdk/tests/legacy/test_tcp_conformance.cpp`（新） | 表驱动状态机 conformance | ~400 行 |

### 数据流（新增的 ICMP 反馈）

```
NIC RX
  │
  ├─► rte_eth_rx_burst()
  │
  └─► DpdkPoller::poll()
        │
        ├─► lookup_by_5tuple_(mbuf)
        │     │
        │     ├─ HIT → entry.process_burst_fn()      ← TCP/UDP 主路径
        │     │
        │     └─ MISS ──► fallback_path_(mbuf)       ← 新增
        │                   │
        │                   ├─ proto == ICMP?
        │                   │    ├─ parse_icmp() → Type 3 Code 4?
        │                   │    │    ├─ extract embedded 4-tuple + next_hop_mtu
        │                   │    │    ├─ icmp_callback_(this_user, embedded_4tuple, mtu)
        │                   │    │    └─ ┌────────────────────────────────────────┐
        │                   │    │      │ DpdkTcpStream::on_icmp_frag_needed_     │
        │                   │    │      │  ├─ match embedded_4tuple == sess_.tuple│
        │                   │    │      │  ├─ sess_.on_icmp_frag_needed(mtu)      │
        │                   │    │      │  └─ ++stream metric                     │
        │                   │    │      └────────────────────────────────────────┘
        │                   │    └─ else: drop (Type 8 echo, etc.)
        │                   └─ else: drop (existing behavior)
```

### 数据流（新增的 keepalive driver）

```
DpdkTcpStream::poll_once_  (caller's poll loop)
  │
  ├─► sess_.poll_rx(...)
  │     └─ updates last_rx_tsc_ on any TCP packet
  │
  ├─► sess_.flush_pending_ack()
  │
  └─► sess_.tick_keepalive(now_tsc)         ← 新增
        │
        ├─ if state != Established or interval == 0 → no-op
        ├─ if (now - last_rx_tsc_) < interval → no-op
        ├─ else if probes_sent_ == max_probes → state = Closed
        └─ else → send_keepalive_probe_(); ++probes_sent_; ++metric
```

---

## 接口设计

### 公共 API（用户可见 / 测试可断言）

```cpp
// ── TcpConfig 新字段 ─────────────────────────────────────────────────────────
struct TcpConfig {
    /* existing fields */ ...

    /// Keepalive interval. Zero = disabled (default). Typical: 30s for
    /// idle-sensitive paths sitting behind NAT / firewall connection tables.
    std::chrono::milliseconds keepalive_interval = std::chrono::milliseconds::zero();

    /// Consecutive missed probes before declaring dead. Default 3.
    uint8_t keepalive_probes = 3;

    /// validate() now also bounds-checks keepalive_probes ∈ [1, 10] when
    /// keepalive_interval > 0.
};

// ── TcpSession 新公共 API ────────────────────────────────────────────────────
class TcpSession {
public:
    /// Effective MSS in use. Equals config_.mss before connect; after
    /// SYN-ACK, equals min(config_.mss, peer_advertised_mss). May be
    /// further reduced by ICMP Frag Needed.
    [[nodiscard]] uint16_t effective_mss() const noexcept;

    /// Whether peer advertised an MSS option in SYN-ACK.
    [[nodiscard]] bool peer_mss_negotiated() const noexcept;

    /// Apply ICMP Frag Needed feedback. next_hop_mtu is the value reported
    /// by the ICMP message; effective_mss_ becomes min(current, mtu - 40).
    /// Increments stats_.icmp_frag_needed_received.
    void on_icmp_frag_needed(uint16_t next_hop_mtu) noexcept;

    /// Periodic keepalive driver. Called every poll cycle by the stream.
    /// No-op if state != Established or keepalive disabled.
    void tick_keepalive(uint64_t now_tsc) noexcept;
};

// ── TcpSession::Stats 新字段 ─────────────────────────────────────────────────
struct Stats {
    /* existing fields */ ...
    uint64_t keepalive_probes_sent       = 0;
    uint64_t icmp_frag_needed_received   = 0;
    uint64_t mss_negotiations_applied    = 0;  // 1 if peer MSS < config (per connect)
};

// ── DpdkPoller 新 API ────────────────────────────────────────────────────────
template <>
class DpdkPoller<void> {
public:
    /// Callback invoked when an ICMP Type 3 Code 4 (Frag Needed) packet
    /// arrives. The poller has already parsed the embedded IP+TCP header
    /// to extract the original connection's destination 4-tuple.
    using IcmpFragNeededCallback = void(*)(void* user,
                                            uint32_t embedded_src_ip,
                                            uint32_t embedded_dst_ip,
                                            uint16_t embedded_src_port,
                                            uint16_t embedded_dst_port,
                                            uint8_t  embedded_proto,
                                            uint16_t next_hop_mtu) noexcept;

    /// Set ICMP callback. Pass nullptr to disable. Single callback for the
    /// whole poller; the callback dispatches to the right stream.
    void set_icmp_callback(IcmpFragNeededCallback cb, void* user) noexcept;

    /// Counter exposed for diagnostics — number of ICMP messages dispatched
    /// to the callback (Type 3 Code 4 only; other ICMP types are dropped).
    [[nodiscard]] uint64_t icmp_frag_needed_dispatched() const noexcept;
};

// ── StreamMetric 新条目 ──────────────────────────────────────────────────────
enum class StreamMetric : std::size_t {
    /* existing 6 */ ...
    kTcpResetsReceived,         // "net.stream.tcp.resets_received"
    kTcpOutOfOrderSegments,     // "net.stream.tcp.out_of_order_segments"
    kTcpReorderBufferHits,      // "net.stream.tcp.reorder_buffer_hits"
    kTcpReorderBufferOverflows, // "net.stream.tcp.reorder_buffer_overflows"
    kTcpKeepaliveProbesSent,    // "net.stream.tcp.keepalive_probes_sent"
    kTcpMssNegotiationApplied,  // "net.stream.tcp.mss_negotiation_applied"
    kIcmpFragNeededReceived,    // "net.stream.icmp.frag_needed_received"
    kCount
};
```

### 注入位点（具体文件:行）

| 改动 | File:Line | 操作 |
|---|---|---|
| TCP options 解析 | `packet_parse.hpp` (new fn) | `parse_tcp_options(mbuf, ParsedPacket&)` 抽 MSS/WSCALE |
| effective_mss 应用 | `tcp.hpp:649, 723, 869` | `config_.mss` → `effective_mss_` |
| MSS 协商触发 | `tcp.hpp:603-625`（`connect()` SYN-ACK 处理块） | parse options，set `effective_mss_`，计 metric |
| Auto src_port | `tcp_stream.hpp:373-549`（`create()` 入口） | `if cfg.src_port==0` → 调 poller.pick_src_port |
| Destructor RST | `tcp.hpp:421-426` | `if (state_ != Closed) reset();` |
| Keepalive driver | `tcp.hpp` 新方法 + `tcp_stream.hpp:poll_once_` | `tick_keepalive` 加入 poll loop |
| ICMP fallback | `poller.hpp:lookup_by_5tuple_` 失败路径 | 新 fallback 函数解析 ICMP 并触发 callback |
| ICMP wiring | `tcp_stream.hpp:create()` 注册 callback | poller->set_icmp_callback(&dispatch, this) |
| metric() lazy-read | `tcp_stream.hpp:754-757` | 改 switch；TCP/ICMP 槽读 sess_.tcp_stats() |

---

## 编码规范

| 维度 | 规范 |
|------|------|
| 新方法命名 | `effective_mss()`, `tick_keepalive()`, `on_icmp_frag_needed()` —— 动词或名词，与项目既有风格一致 |
| 新成员命名 | `effective_mss_`, `peer_mss_`, `last_rx_tsc_`, `keepalive_probes_sent_`, `last_keepalive_tsc_` —— 后缀下划线 |
| 新 StreamMetric | 严格 OTel 层级 `net.stream.<sub>.<dim>`；`tcp.*` 集中归类，`icmp.*` 独立 |
| Keepalive 关闭判定 | `keepalive_interval == 0ms` 表 disabled（不引入额外 enum） |
| RST 失败处理 | destructor 内异常静默吞下（log DEBUG），mempool 可能已不可用 |
| ICMP callback 错误 | 所有 callback noexcept；内部异常不允许逃逸 |
| 注释 | 仅在"为什么"处加：(a) 选择 effective_mss 单值而非维护多源最小值 (b) keepalive probe 用 `seq=snd_nxt-1` 的 RFC 依据 (c) 析构 RST 是 best-effort 而非保证 |
| 日志级别 | MSS 协商：DEBUG；keepalive probe 发送：TRACE；keepalive 死亡判定：WARN；ICMP Frag Needed 收到：INFO（属事件，不噪） |
| 测试 | 每 phase 一个新增 test case；conformance test 表驱动以减少代码量 |

---

## 实施计划

### Phase A — MSS Option 协商（RX 解析 + 应用）

- **交付物**：
  - `packet_parse.hpp::parse_tcp_options(mbuf, ParsedPacket&)`
  - `TcpSession::effective_mss_` 成员 + `effective_mss()` accessor
  - `connect()` SYN-ACK 处理路径调用 `parse_tcp_options` → 设置 `effective_mss_`
  - `send()` / `send_batch()` / RX 段长度判定改用 `effective_mss_`
  - `Stats::mss_negotiations_applied`
- **验收**：
  - 单元测试：构造 SYN-ACK with MSS=1200 → `session.effective_mss() == 1200`
  - 单元测试：构造 SYN-ACK 无 MSS option → `session.effective_mss() == config_.mss`
  - 回归：`test_tcp_state_machine` / `test_kernel_tcp_stream` 全绿
- **预估**：~3 小时

### Phase B — TIME_WAIT-aware 自动 src_port

- **交付物**：
  - `DpdkTcpStream::create()` 在 `cfg.legacy.tuple.src_port == 0` 时调 `poller->pick_src_port`
  - `TcpConfig::validate()` 仍拒绝 src_port=0（防止裸 session 用户漏掉）
  - 新 INFO 日志：`"DpdkTcpStream::create: auto-allocated src_port={}"`
- **验收**：
  - 集成测试：`StreamConfig` 不指定 src_port → create 成功，端口落在 [32768, 60999]
  - 集成测试：连续 100 次 create+close → 无重复 4-tuple 冲突
- **预估**：~1.5 小时

### Phase C — Destructor RST 兜底

- **交付物**：
  - `~TcpSession()` 在 `state_ != Closed` 时调 `reset()`
  - try-catch 包裹（mempool 可能已被 Platform 销毁，记 DEBUG 不抛）
- **验收**：
  - 单元测试：建连后 raw destruct（不调 close）→ 对端 mock 收到 RST
  - 单元测试：mempool 提前销毁场景 → destructor 不 crash
- **预估**：~1 小时

### Phase D — TCP Keepalive

- **交付物**：
  - `TcpConfig::keepalive_interval` + `keepalive_probes`
  - `TcpSession`: `last_rx_tsc_`, `last_keepalive_tsc_`, `keepalive_misses_` 状态
  - `tick_keepalive(now_tsc)` 实现
  - `send_keepalive_probe_()` 私有方法
  - `process_rx` 末尾更新 `last_rx_tsc_`
  - `DpdkTcpStream::poll_once_` 调 `sess_.tick_keepalive(TSC::now())`
  - `Stats::keepalive_probes_sent`
- **验收**：
  - 单元测试：keepalive_interval=10ms，模拟无 RX 11ms → tick 后发出 probe
  - 单元测试：3 次连续 probe 无响应 → state == Closed
  - 单元测试：probe 发出后立即收到 ACK → `keepalive_misses_` 重置
  - 默认配置（interval=0）→ tick_keepalive 永远 no-op，不影响延迟
- **预估**：~3 小时

### Phase E — ICMP Frag Needed (Path MTU Discovery)

- **交付物**：
  - `packet_parse.hpp::parse_icmp(mbuf)` → 抽 type/code/embedded 4-tuple/next_hop_mtu
  - `DpdkPoller::set_icmp_callback()` API + 内部 callback storage
  - `DpdkPoller::poll()` fallback 路径：lookup_by_5tuple_ 失败 + IPv4 proto=1 → parse_icmp
  - Type 3 Code 4 → 触发 callback；其他 ICMP 类型沿用现有 drop 行为
  - `TcpSession::on_icmp_frag_needed(mtu)` 实现：`effective_mss_ = min(effective_mss_, mtu - 40)`
  - `DpdkTcpStream` 在 `create()` 注册 callback；callback 用 embedded 4-tuple 匹配自身
  - `Stats::icmp_frag_needed_received`
- **验收**：
  - 单元测试：注入 ICMP Type 3 Code 4 with MTU=576 → `session.effective_mss() == 536`（576-20-20=536）
  - 单元测试：注入 ICMP Echo Request（Type 8）→ 不触发 callback，不 crash
  - 单元测试：注入 ICMP Type 3 Code 4 with embedded 4-tuple 不匹配 → 计入 poller 计数器，不影响 session
- **预估**：~4 小时（最大块）

### Phase F — Stats → StreamMetric 整合

- **交付物**：
  - `stream_metrics.hpp` enum + 名字表加 7 项；static_assert 同步
  - `DpdkTcpStream::metric()` 改 switch，新条目直读 `sess_.tcp_stats()`
  - 其他 backend（Kernel TCP/UDP, DPDK UDP）的 `metric()` 对新条目返回 0（与现有 `kReasmOverflows`/UDP 模式一致）
  - `tests/integration/test_stream_metrics.cpp` 扩展用例覆盖每个新条目
- **验收**：
  - `publish_metrics(*dpdk_stream, sink)` 输出包含全部 13 项 counter
  - 触发 RST → `kTcpResetsReceived` 增加；触发 ICMP frag needed → `kIcmpFragNeededReceived` 增加
  - Kernel stream 输出新 7 项均为 0（不破坏既有契约）
- **预估**：~2 小时

### Phase G — TCP State Machine Conformance Test（安全网）

- **交付物**：
  - 新文件 `eph-net-dpdk/tests/legacy/test_tcp_conformance.cpp`
  - 仿照 `test_tcp_state_machine.cpp` 的 `FakeTcpMbuf` 模式（mempool=nullptr，mbuf 栈构造）
  - 表驱动：每行 = `(initial_state, incoming_flags, payload, expected_final_state, expected_response, expected_seq_advance)`
  - 覆盖 9 个实现状态 × 主要 incoming 类型（SYN / SYN+ACK / ACK / FIN / FIN+ACK / RST / PSH+ACK with payload / 乱序 ACK / 重复 SEQ）
  - 至少 60 行测试用例
- **验收**：
  - 表全过
  - **回归保护**：故意把 `Phase A-F` 任一改动注入一个错误（例如 effective_mss 用错 source）→ conformance test 必须 catch
- **预估**：~4 小时

### 总计 ~18.5 小时实施 + 各 phase 收尾的 bench 回归（每 phase < 30 分钟）

### 提交节奏（每 phase 一个 commit）

```
1. feat(net-dpdk): parse SYN-ACK options + negotiate effective MSS
2. feat(net-dpdk): auto-allocate src_port via DpdkPoller::pick_src_port
3. feat(net-dpdk): RST cleanup in ~TcpSession() destructor
4. feat(net-dpdk): TCP keepalive (default off, configurable interval/probes)
5. feat(net-dpdk): ICMP Frag Needed PMTU feedback path
6. feat(net,net-dpdk): expose TCP/ICMP session counters as StreamMetric tcp.* / icmp.*
7. test(net-dpdk): TCP state-machine conformance table (regression safety net)
```

---

## 关键决策记录

### D-1: ICMP path 走 Poller fallback + callback，而非直接挂 stream

- **问题**：ICMP 包归谁处理？
- **选项**：A. 每个 stream 自己看 NIC RX；B. Poller fallback 检测 + callback；C. 单独的 IcmpHandler 类
- **决策**：**B**
- **理由**：单 stream 看不到全局 ICMP；Poller 已经是包入口；callback 模式符合项目既有风格（无 vtable，function pointer）
- **验收**：Phase E 单元测试覆盖 callback 触发路径

### D-2: TCP/ICMP stats 走 lazy-read，不维护与 sess_.stats_ 平行的 atomic 计数

- **问题**：要不要在 DpdkTcpStream 的 hot path 加 inc_<M>()，让所有 metric 走统一的 atomic counter 数组？
- **选项**：A. 平行 inc_<M>()（统一）；B. lazy-read（DpdkTcpStream::metric switch 到 sess_.tcp_stats()）
- **决策**：**B**
- **理由**：避免重复计数（事实源单一）；避免 hot path 多一次原子操作；TcpSession 是单线程访问，lazy-read 安全
- **代价**：metric() 不再是单一数组下标，要写 switch；可接受
- **验收**：Phase F 测试同时断言 sess_.tcp_stats().resets_received 和 publish_metrics 出来的 kTcpResetsReceived 一致

### D-3: Keepalive 默认关闭

- **问题**：默认开启 30s 还是默认关闭？
- **决策**：**默认 keepalive_interval=0ms 表关闭**
- **理由**：HFT 应用层心跳（FIX heartbeat / WS ping、ITCH 周期消息）已经覆盖大多数场景；默认开启可能在测试环境引入意外的 probe 流量；keep 行为需要用户显式 opt-in 才有合理预期
- **验收**：默认 TcpConfig 路径 `tick_keepalive` 永远 no-op，不引入测量回归

### D-4: src_port=0 的语义在 stream 层 = 自动分配，session 层 = 仍报错

- **问题**：`TcpConfig::validate()` 是否要松绑允许 src_port=0？
- **决策**：**保持 validate() 拒绝；语义上 auto-alloc 是 stream 层 (`DpdkTcpStream::create`) 的高层责任**
- **理由**：裸 `TcpSession` 用户没有 poller 上下文，无法 auto-alloc；保持低层 API 严格能 catch "忘配 src_port" 的真实错误
- **验收**：Phase B 测试覆盖两条路径

### D-5: Destructor RST 是 best-effort 不是保证

- **问题**：destructor 发 RST 失败要不要抛？
- **决策**：**catch & log DEBUG，不抛**
- **理由**：destructor 不允许抛异常（项目设 SPDLOG_NO_EXCEPTIONS）；mempool 可能已被 Platform 销毁；强保证 RST = 引入"析构顺序耦合"的运维负担
- **验收**：Phase C 测试覆盖 mempool 提前销毁场景

### D-6: Conformance test 走 legacy 模式（mempool=nullptr）

- **问题**：测试架在哪一层？
- **选项**：A. tests/legacy/ FakeTcpMbuf 模式；B. tests/integration/ 走 DpdkTcpStream + 真 mempool；C. 走 net_null vdev
- **决策**：**A**
- **理由**：legacy 模式已验证可行；纯状态机测不需 NIC；FakeTcpMbuf 栈构造无 mempool 依赖；测试运行最快、隔离性最强
- **验收**：与既有 `test_tcp_state_machine.cpp` 同目录、同 build 集成

### D-7: 性能门槛维持 ≤1% 延迟回归

- **问题**：6 项改动累计是否会撞 hot path？
- **决策**：**强制 `lat tcp --dpdk` 在 Phase D + Phase E + Phase F 收尾各跑一次对比**
- **理由**：keepalive tick / ICMP fallback / metric switch 都触及 poll loop；CLAUDE.md 明确性能门槛
- **验收**：每 phase 收尾时 bench 报告 ≤1% 回归方放行；超则回滚或局部优化

---

## Verification（端到端验收清单）

### 构建
```bash
xmake -m release
xmake build -g tests
```

### 单元 / 集成测试
```bash
# 新增 / 扩展
xmake run test_tcp_conformance               # Phase G 新增
xmake run test_stream_metrics                # Phase F 扩展

# 回归（不应破坏）
xmake run test_tcp_state_machine             # legacy
xmake run test_tcp_close_reset               # legacy
xmake run test_tcp_fault_tolerance           # legacy
xmake run test_kernel_tcp_stream             # 不影响 kernel 路径
xmake run test_kernel_tcp_stream_behavioral  # 不影响 kernel 路径

# DPDK e2e（如有 NIC）
sudo ./tests/integration/dpdk_e2e
```

### 性能 bench（每个 phase 收尾必跑）
```bash
# Baseline（开始前记录一次）
sudo ./benchmarks/latency/lat tcp --dpdk     # 记 p50/p99
sudo ./benchmarks/latency/lat ws  --dpdk     # 记 p50/p99

# Phase D / E / F 收尾各跑一次，比 baseline 回归 ≤1%
```

### 兜底 grep（确认 keepalive 注释 stale 已修）
```bash
# 头注释 "implements: ... keepalive" 应改为真实状态
grep -n "keepalive" eph-net-dpdk/include/eph/dpdk/tcp.hpp
# 不再有 "implements ... keepalive" 在头注释里，但有真实 send_keepalive_probe_ / tick_keepalive 实现
```

---

## 未覆盖项（明确不做，留待后续迭代）

- **Listen / Accept**（被动打开）→ 下迭代评估，需求依赖是否要做内部服务通信 / 简化 DPDK↔DPDK 测试
- **Window Scaling (RFC 1323)** → 跨 region/AZ 链路或大 BDP 场景才补
- **拥塞控制 / SACK / Timestamps** → 永不在 HFT 数据中心场景做
- **IPv6** → 交易所主流仍 IPv4
- **多 RX queue + RSS** → 性能扩展，单连接不需要
- **应用层心跳框架**（FIX / WS ping 等）→ 不在 stream 层职责
- **OpenTelemetry SDK 适配器** → 用户写自己的 sink 即可

---

## 退出准则（DoD）

实施全部完成的标志：

1. ✅ 7 个 commits 全部 build & test 绿
2. ✅ `lat tcp --dpdk` / `lat ws --dpdk` 累计回归 ≤1%
3. ✅ Conformance test 表至少 60 个 case，全过
4. ✅ `tcp.hpp` 头注释更新（修 stale "implements keepalive"）
5. ✅ `summary.md` + `eph-net-dpdk/summary.md` 同步新增 P0 features 段落
6. ✅ `CLAUDE.md` "Available public surface" 增补 keepalive / effective_mss / ICMP feedback 三个用户可见 API

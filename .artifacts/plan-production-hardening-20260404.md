# Plan: 生产级加固 — Phase A (P0) + Phase B (P1)

> 修复 10 个 CRITICAL/HIGH 级问题，使 eph-net/eph-transport/eph-dpdk/eph-utils 达到生产部署标准。

创建时间：2026-04-04
状态：已完成
来源：discuss-20260404-production-level.md（5 角色 6 轮讨论收敛结果）
关联：production-readiness-20260404.plan.md（已完成，本 plan 为其后续）

---

## 定位与边界

**目标**：修复代码审计发现的 10 个生产安全/正确性/可靠性缺陷，每项修复附带回归测试。

**In scope**：
- 5 个 P0 修复（密码学安全 + 数据正确性，阻塞上线）
- 5 个 P1 修复（可靠性 + 输入验证，上线后 1 周内）
- 10 个回归测试

**Out of scope**：
- Phase C 架构重构（reconnect 互斥协议统一、Gateway RAII 重设计、AuditLog MPSC）→ 下个 milestone
- DNSSEC（降级 P2，专线网络风险缓解）
- IPv6、HTTP keep-alive、TLS KeyUpdate

**DPDK 环境约束**：P0-5（RST 验证）修改 eph-dpdk 代码，可结构验证但无法编译运行。

---

## 实施计划

> **Commit 策略**：每个阶段完成并通过验收后，执行 `/git` 提交。

> **性能约束**：所有修复合计热路径开销 < 5ns/msg。每阶段完成后跑 bench_transport_types + bench_e2e_latency 确认无回归。

### 阶段 1: P0-1 — stop/reconnect 竞态守护

**问题**：`transport.hpp:784` 的 `stop()` 与 RX 线程的 `do_reconnect_()`（`transport.hpp:1186`）存在竞态。`running=false` 到线程 join 之间的窗口内，RX 线程可能进入 reconnect 路径访问已释放的 crypto/tcp 指针。

**修复方案**：在 `stop()` 中 `running=false` 之后、线程 join 之前，添加 spin-wait 等待 `reconnecting==false`，带 5s 超时 + forced reset 兜底。

```cpp
// transport.hpp stop() 修改：
void stop() noexcept {
    bool was_running = core_.running.exchange(false, std::memory_order_acq_rel);
    if (!was_running) return;

    auto log = detail::transport_logger();
    SPDLOG_LOGGER_INFO(log, "Stopping transport");

    // 等待 RX 线程退出 reconnect 路径，防止 join 时 crypto/tcp 仍被访问
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (core_.reconnecting.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) {
            SPDLOG_LOGGER_ERROR(log,
                "stop: reconnect still in progress after 5s, forcing TCP reset");
            if (core_.tcp) core_.tcp->reset();
            break;
        }
        eph::utils::cpu_relax();
    }

    tx_.stop();
    rx_.stop();
    // ... 其余不变 ...
}
```

**同步修改**：`do_reconnect_()` 在设置 `reconnecting=true` 之前须检查 `running`：
```cpp
// transport.hpp do_reconnect_() 开头补充：
if (!core_.running.load(std::memory_order_acquire)) {
    return false;  // stop() 已调用，不再 reconnect
}
core_.reconnecting.store(true, std::memory_order_release);
```

- 交付物：transport.hpp 修改 + test_stop_during_reconnect
- 验收标准：TSan 下 100 次随机 timing stop/reconnect 不触发 data race
- 测试要点：用 FakeTcpTransport + set_connect_error 使 reconnect 阻塞，主线程调 stop()，验证 5s 内返回且无 crash
- 推荐 skill：`/design auto`
- 预估：0.5 天

### 阶段 2: P0-2 — TLS RX 序列号 95% 自动 reconnect

**问题**：`rx_worker.hpp:815-826` 在 TLS read 序列号达 90% 时仅 WARN，不触发 reconnect。如果 server 大量发送数据（RX 先耗尽），nonce 碰撞 = 密码学完全失效。TX 侧在 `tx_worker.hpp:451` 已有 95% reconnect。

**依赖**：阶段 1（stop/reconnect guard 必须先到位，因为 RX 触发的 reconnect 也经过 do_reconnect_() 路径）

**修复方案**：在 `rx_worker.hpp` 的 90% warning 检查之后，追加 95% reconnect：

```cpp
// rx_worker.hpp TLS decrypt loop 内，紧接 90% warning 后：
if (!rx_seq_warning_logged_) {
    uint64_t rseq = core_.crypto->read_seq();
    if (rseq >= tls_record::kSequenceWarnThreshold) {
        SPDLOG_LOGGER_WARN(log, ...);
        rx_seq_warning_logged_ = true;
    }
}
// 新增 95% reconnect：
{
    if (!core_.crypto) continue;
    uint64_t rseq = core_.crypto->read_seq();
    if (rseq >= tls_record::kSequenceReconnectThreshold) {
        SPDLOG_LOGGER_WARN(log,
            "TLS read sequence at {}/{} (95%%), "
            "triggering preemptive reconnect for key refresh",
            rseq, tls_record::kMaxSequenceNumber);
        reassembly_len = 0;
        ws_reassembly_len = 0;
        frame_processor_->reset();
        if (!callbacks_.do_reconnect()) {
            core_.running.store(false, std::memory_order_release);
            break;
        }
        rx_seq_warning_logged_ = false;  // 重置 warning flag
        continue;
    }
}
```

**优化**：合并 90% 和 95% 检查，只读一次 `read_seq()`。

- 交付物：rx_worker.hpp 修改 + test_tls_rx_seq_exhaustion
- 验收标准：mock crypto read_seq() 返回 95% 阈值时触发 reconnect
- 推荐 skill：`/design auto`
- 预估：0.25 天

### 阶段 3: P0-3 — 证书 pin 默认拒绝

**问题**：`tls_session.hpp:754-758` 当有 pin 列表但无 `on_pin_mismatch` 回调时，WARN 后继续连接（soft pin）。MITM 攻击直接成功。

**修复方案**：有 pin 列表 + 无回调 = 默认拒绝。

```cpp
// tls_session.hpp pin 验证 else 分支改为：
} else {
    // 有 pin 列表但无回调 = 默认硬拒绝（safe default）
    SPDLOG_LOGGER_ERROR(log,
        "SPKI pin mismatch: actual={}, no on_pin_mismatch callback set — "
        "rejecting connection (configure callback to override)",
        actual_hash);
    return std::unexpected(ConnectionErrorInfo{
        ConnectionError::kTlsHandshakeFailed,
        std::format("SPKI pin mismatch (actual={}), no override callback",
                     actual_hash)});
}
```

**补充**：在 `TransportConfig::warnings()` 中增加提示：
```cpp
if (!pinned_spki_sha256.empty() && !on_pin_mismatch)
    w.emplace_back("pinned_spki_sha256 is set but on_pin_mismatch is null — "
                   "pin mismatch will reject the connection (hard pin mode)");
```

- 交付物：tls_session.hpp + transport_types.hpp 修改 + test_pin_mismatch_default_reject
- 验收标准：配置 pin 列表但不设回调时，pin mismatch 返回 kTlsHandshakeFailed
- 推荐 skill：`/design auto`
- 预估：0.25 天

### 阶段 4: P0-4 — timestamp assert→runtime check

**问题**：`timestamp.hpp:28,39` 用 `assert()` 守护负数→uint64 转换。release 构建 `-DNDEBUG` 后 assert 消失，负数静默变成巨大 uint64。

**修复方案**：替换 assert 为 runtime clamp：

```cpp
// timestamp.hpp:
[[nodiscard]] constexpr uint64_t ms_to_ns(int64_t ms) noexcept {
    if (ms < 0) [[unlikely]] return 0;
    return static_cast<uint64_t>(ms) * 1'000'000;
}

[[nodiscard]] constexpr uint64_t us_to_ns(int64_t us) noexcept {
    if (us < 0) [[unlikely]] return 0;
    return static_cast<uint64_t>(us) * 1'000;
}
```

- 交付物：timestamp.hpp 修改 + test_negative_timestamp_conversion
- 验收标准：ms_to_ns(-1) == 0, us_to_ns(-100) == 0
- 推荐 skill：`/design auto`
- 预估：0.1 天

### 阶段 5: P0-5 — RST 序列号验证

**问题**：`tcp.hpp:860` 接受任何 RST 不验证序列号。RFC 5961 要求 RST 的 SEQ 必须在接收窗口内 `[RCV.NXT, RCV.NXT + RCV.WND)`。

**修复方案**：在 RST 处理前加窗口验证：

```cpp
// tcp.hpp process_rx, RST 处理前：
if (parsed.has_flag(net::kTcpRst)) {
    // RFC 5961 §3.2: validate RST sequence number is within receive window
    uint32_t rst_seq = parsed.seq();
    if (rst_seq != rcv_nxt_ &&
        !(seq_after(rst_seq, rcv_nxt_) &&
          !seq_after(rst_seq, rcv_nxt_ + rcv_wnd_))) {
        SPDLOG_LOGGER_DEBUG(log,
            "RST seq {} outside receive window [{}, +{}), ignored (RFC 5961)",
            rst_seq, rcv_nxt_, rcv_wnd_);
        free_list[free_count++] = pkts[i];
        continue;
    }
    // ... 原有 RST 处理逻辑不变 ...
}
```

**成本**：2 次 uint32 减法 + 1 次比较 ≈ 1ns。

- 交付物：tcp.hpp 修改 + test_rst_out_of_window_rejected（单元测试，mock 数据包）
- 验收标准：out-of-window RST 被丢弃，in-window RST 正常关闭连接
- 注意：eph-dpdk 无法编译运行，仅结构验证 + 单元测试（不依赖 DPDK API）
- 推荐 skill：`/design auto`
- 预估：0.25 天

### 阶段 6: P1-1 — Gateway void*→function 回调

**问题**：`gateway.hpp:100` 用 `void*` 存储 transport 指针，transport 析构后 `check_health()` 解引用是 UB。

**修复方案**：用 `std::function` 回调替代 `void*` + function pointer。移除 `transport_ptr`，改为 lambda 捕获 transport 引用：

```cpp
struct GatewayConnection {
    std::string tag;
    // 替代 void* + function pointer 的 type-erased 回调
    std::function<void()> stop_fn{};
    std::function<bool()> is_running_fn{};
    std::function<void()> start_threads_fn{};
    std::function<void()> reconnect_fn{};
    std::function<uint64_t()> rx_packets_fn{};
    // ... degraded 检测字段不变 ...
};
```

`register_transport()` 模板方法改为让 caller 自行绑定 lambda，transport 生命周期由 caller 保证。如果 caller 用 `shared_ptr` 管理 transport，则在 lambda 中捕获 `weak_ptr` 即可实现安全失效检测。

**API 变更**：这是 breaking change——所有 `register_transport()` 调用方需要更新。但 Gateway 的调用方很少（通常只有主程序入口 1-2 处），影响范围可控。

- 交付物：gateway.hpp 重构 + test_gateway_transport_lifetime + 更新所有调用方
- 验收标准：transport 析构后 is_running_fn 返回 false 而非 crash
- 推荐 skill：`/design auto`
- 预估：0.5 天

### 阶段 7: P1-2 — AuditLog record_mt per-slot committed flag

**问题**：`audit_log.hpp:191` `fetch_add` 分配 slot 后，199-209 行的字段写入无同步。两个线程可能写入同一 slot。

**修复方案**：每个 Entry 添加 `std::atomic<bool> committed{false}` 字段。写入流程：
1. `fetch_add` 分配 idx
2. 写入所有字段
3. `committed.store(true, release)` 标记完成
4. 读端检查 `committed.load(acquire)` 后再读

```cpp
struct Entry {
    std::atomic<bool> committed{false};
    // ... 现有字段 ...
};

bool record_mt(...) noexcept {
    size_t idx = head_.fetch_add(1, std::memory_order_acq_rel);
    auto& entry = entries_[idx & kMask];
    // 等待前一次写入完成（如果 ring buffer wrap 回来了）
    while (entry.committed.load(std::memory_order_acquire)) {
        eph::utils::cpu_relax();
    }
    entry.tsc = TSC::now();
    // ... 写入其他字段 ...
    entry.committed.store(true, std::memory_order_release);
    return idx < Capacity;
}
```

读端在 `dump()` / `to_json()` 中检查 committed 再读。

- 交付物：audit_log.hpp 修改 + test_auditlog_concurrent_writers
- 验收标准：TSan 下 4 线程并发写入无 data race
- 推荐 skill：`/design auto`
- 预估：0.5 天

### 阶段 8: P1-3 — HttpClient check-before-append

**问题**：`http_client.hpp:776-778` 先 append 数据到 buffer，后检查大小限制。buffer 可暂时超限。

**修复方案**：检查移到 append 前：

```cpp
// http_client.hpp recv_all:
ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
// ... error handling ...
// 先检查再 append
if (buf.size() + static_cast<size_t>(n) > config_.max_response_size) {
    return std::unexpected(std::format(
        "Response exceeds max_response_size ({}+{}B > {}B)",
        buf.size(), n, config_.max_response_size));
}
buf.append(chunk, static_cast<size_t>(n));
```

对 `ssl_recv_all()` 做同样修改。

- 交付物：http_client.hpp 修改 + test_http_response_size_boundary
- 验收标准：response 恰好等于 max_response_size 成功，超出 1 byte 失败
- 推荐 skill：`/design auto`
- 预估：0.25 天

### 阶段 9: P1-4 — RateLimiter 时钟回退守护

**问题**：`rate_limiter.hpp:261-262` 如果 `now < last_refill_`（NTP 校准），elapsed_ns 可能是负数或巨大正数（unsigned 溢出）。

**修复方案**：在 refill_locked 中 clamp elapsed_ns：

```cpp
void refill_locked() noexcept {
    const auto now = Clock::now();
    // Guard against clock rewind (NTP adjustment, etc.)
    if (now <= last_refill_) return;
    const auto elapsed_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - last_refill_).count());
    // ... 其余不变 ...
}
```

- 交付物：rate_limiter.hpp 修改 + test_ratelimiter_clock_rewind
- 验收标准：mock clock 回退后 tokens 不变（不增不减）
- 推荐 skill：`/design auto`
- 预估：0.25 天

### 阶段 10: P1-5 — Histogram establish_size 溢出检查

**问题**：`hdr_histogram.hpp:1015` 的 `(buckets_needed + 1) * (sub_bucket_count_ / 2)` 可能溢出 int32。

**修复方案**：在 int64 上检查后再 cast：

```cpp
void establish_size(uint64_t max_value) {
    int32_t buckets_needed = get_buckets_needed_to_cover_value(max_value);
    bucket_count_ = buckets_needed;
    int64_t product = (static_cast<int64_t>(buckets_needed) + 1)
                    * (sub_bucket_count_ / 2);
    if (product > kMaxCountsLen) {
        throw std::invalid_argument(std::format(
            "Histogram too large: counts_len {} exceeds max {}",
            product, kMaxCountsLen));
    }
    counts_len_ = static_cast<int32_t>(product);
}
```

- 交付物：hdr_histogram.hpp 修改 + test_histogram_extreme_params
- 验收标准：极端参数（highest_trackable=UINT64_MAX, sig_figs=5）抛异常而非 UB
- 推荐 skill：`/design auto`
- 预估：0.25 天

---

## 关键决策记录

### D-1: stop/reconnect 同步方式
- **问题**：如何防止 stop() 和 do_reconnect_() 竞态
- **选项**：A. mutex / B. spin-wait + 超时 / C. state machine enum
- **决策**：B — spin-wait on `reconnecting` flag + 5s 超时 + forced TCP reset
- **理由**：non-blocking（R3 要求），超时兜底（R1 要求），不改变现有 flag 语义
- **验收标准**：TSan 下 100 次随机 timing 无 data race

### D-2: cert pin 默认行为
- **问题**：有 pin 列表但无回调时的行为
- **选项**：A. 默认放行（soft pin）/ B. 默认拒绝（hard pin）/ C. 配置项控制
- **决策**：B — 默认拒绝
- **理由**：safe default 原则（R10）；需要 soft pin 的用户显式设置回调返回 true
- **验收标准**：无回调 + pin mismatch → kTlsHandshakeFailed

### D-3: Gateway 生命周期方案
- **问题**：如何解决 void* 悬垂指针
- **选项**：A. shared_ptr<void> / B. function 回调替代 void* / C. weak_ptr 观察者
- **决策**：B — function 回调
- **理由**：零引用计数开销（R3），零 API 概念变更（仍是 type erasure），caller 用 weak_ptr 捕获即可安全检测失效
- **验收标准**：transport 析构后回调返回 false 而非 crash

### D-4: AuditLog 多线程同步
- **问题**：record_mt 数据竞争修复方案
- **选项**：A. per-slot mutex / B. per-slot committed flag / C. SeqLock
- **决策**：B — per-slot atomic committed flag
- **理由**：写端 1 次 store(release)，读端 1 次 load(acquire)，最低开销；Phase C 评估 SeqLock
- **验收标准**：TSan 下 4 线程并发写入无 data race

### D-5: stop() 超时后行为
- **问题**：5s spin-wait 超时后是 continue 还是 abort
- **选项**：A. log ERROR + forced reset + continue / B. log ERROR + abort()
- **决策**：A（生产环境）— forced TCP reset 使 reconnect 退出，然后正常 join
- **理由**：可恢复优于 crash；超时本身说明有更深的 bug，ERROR 日志 + 监控告警足以引起运维注意
- **验收标准**：超时后 stop() 仍能正常返回，不死循环

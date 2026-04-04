# Plan: eph-net / eph-transport / eph-dpdk 生产就绪改造

> 三大网络模块从"功能可用"提升到"生产级别"：修复可靠性缺陷、统一错误体系、安全加固、测试覆盖、性能优化。

创建时间：2026-04-04
状态：已完成

---

## 定位与边界

**目标**：使 eph-net、eph-transport、eph-dpdk 达到 HFT 加密货币交易系统的生产部署标准。

**In scope**：
- TLS 序列号上限修复
- 连接降级检测
- 跨模块错误类型统一
- 安全加固（证书 pinning、日志脱敏）
- 测试基础设施（ASan/UBSan、fuzzing、E2E 测试）
- 性能优化（TX 批量、HWM 追踪、阈值回调、延迟 benchmark）
- 日志命名空间统一
- ARP 缓存刷新

**Out of scope**：
- IPv6 支持
- HTTP chunked encoding / keep-alive / connection pool
- TLS session resumption / 0-RTT
- TLS 1.3 KeyUpdate（make-before-break）
- 审计日志（取决于合规要求，非本次范围）
- HMAC 密钥安全内存（mlock + zeroing，取决于部署环境）
- Prometheus / OpenTelemetry 集成

**DPDK 环境约束**：
当前不具备 DPDK 运行环境。以下子项可以完成代码修改但无法编译验证和运行测试，标记为"待 DPDK 环境验证"：
- 1c（eph-dpdk 部分）：connector.hpp、tcp.hpp、arp.hpp、dns.hpp 的返回类型迁移
- 2b：ARP 缓存定时刷新
- 5a：TX 批量发送

其余 12 项可在当前环境完整实施和验证（eph-net + eph-transport）。

---

## 架构设计

### 错误体系统一

**现状**：eph-dpdk 和 eph-net 的 `connect()` 返回 `std::expected<T, std::string>`，eph-transport 返回 `std::expected<T, ConnectionErrorInfo>`。

**目标**：所有模块的 `connect()` 统一返回 `std::expected<T, ConnectionErrorInfo>`。

扩展 `ConnectionError` 枚举（eph-core/transport_errors.hpp）：

```cpp
enum class ConnectionError : uint8_t {
    // 现有
    kInvalidConfig,
    kFactoryFailed,
    kTcpNotEstablished,
    kTlsSessionFailed,
    kTlsHandshakeFailed,
    kTlsKeyExportFailed,
    kWsUpgradeFailed,
    kWsUpgradeRejected,
    kWsAcceptInvalid,

    // 新增
    kDnsResolveFailed,       // DNS 解析失败（超时、NXDOMAIN）
    kArpResolveFailed,       // ARP 解析失败（仅 DPDK）
    kTcpConnectTimeout,      // TCP 连接超时
    kTcpConnectRefused,      // TCP 连接被拒绝
    kProxyHandshakeFailed,   // SOCKS5/HTTP CONNECT 代理握手失败
    kPlatformInitFailed,     // DPDK Platform 初始化失败
};
```

`ConnectionErrorInfo` 结构体不变，枚举值从 9 → 15。所有现有测试中对 `std::string` 错误的断言同步迁移。

### Gateway 降级检测

`GatewayConnection` 新增字段：

```cpp
struct GatewayConnection {
    // ... 现有字段 ...
    uint64_t (*rx_packets_fn)(void*) = nullptr;  // type-erased stats 访问
    uint64_t last_rx_packets = 0;                 // 上次检查时的 RX 计数
    uint64_t degraded_since_ns = 0;               // 首次检测到无新数据的时间
};
```

`register_transport()` 自动绑定：
```cpp
conn.rx_packets_fn = [](void* p) {
    return static_cast<Transport*>(p)->stats().rx_packets;
};
```

`check_health()` 三态逻辑：
- `is_running() == false` → Disconnected
- `is_running() == true` && `rx_packets delta > 0` → Healthy（重置 degraded_since_ns）
- `is_running() == true` && `rx_packets delta == 0` 且持续 > `degraded_threshold` → Degraded

### TLS 序列号

修改 `tls_constants.hpp`：
```cpp
inline constexpr uint64_t kMaxSequenceNumber = (1ULL << 32);  // 从 2^24 → 2^32
```

前提验证：确认 nonce 构造是 deterministic 模式（`seq XOR implicit_iv`），非 random nonce。NIST SP 800-38D 对 deterministic nonce 的限制是 2^32。

### Soft Certificate Pinning

`TransportConfig` 新增：
```cpp
std::vector<std::string> pinned_spki_sha256;  // Base64-encoded SPKI hashes
std::function<bool(std::string_view)> on_pin_mismatch;  // 返回 true 继续，false 断开
```

行为：空列表 = 不做 pinning；不匹配 = WARN 日志 + 回调。

### 日志命名空间

| 模块 | 修改前 | 修改后 |
|------|--------|--------|
| eph-net | `net.socket`, `net.http_client`, ... | 不变 |
| eph-transport | `net.transport` | `transport.core` |
| eph-transport | `net.tls` | `transport.tls` |
| eph-transport | `net.tls_record` | `transport.tls_record` |
| eph-dpdk | `dpdk.tcp`, `dpdk.arp`, ... | 不变 |

仅修复 eph-transport 的 `net.*` → `transport.*`。

---

## 接口设计

### FakeTcpTransport（测试基础设施）

定义在 eph-core 或 eph-transport 的 test 目录中，满足 `TcpTransport` concept：

```cpp
class FakeTcpTransport {
public:
    // TcpTransport concept 要求的方法
    std::expected<void, std::string> connect(std::chrono::milliseconds timeout);
    std::expected<size_t, std::string> send(const void* data, size_t len);
    std::expected<int, std::string> poll_rx(auto callback);
    void close();
    void reset();
    bool is_established() const;

    // 可编程控制接口
    void inject_rx(std::span<const uint8_t> data);    // 注入接收数据
    void inject_error(std::string error);               // 下次操作返回错误
    void inject_disconnect();                            // 模拟对端断连
    void set_connect_behavior(ConnectBehavior b);       // 控制连接行为
    // ...
};
```

### 阈值回调（性能监控）

`TransportConfig` 新增：
```cpp
struct ThresholdConfig {
    uint64_t rx_drop_rate_threshold = 0;     // 0 = 禁用
    uint64_t rtt_p99_ns_threshold = 0;       // 0 = 禁用
    std::function<void(std::string_view metric, uint64_t value, uint64_t threshold)> on_breach;
};
ThresholdConfig thresholds;
```

Worker 线程每 1024 次迭代采样 stats 并做阈值比较。

---

## 实施计划

> **Commit 策略**：每个阶段完成并通过验收后，执行 `/git` 提交。commit message 标注阶段编号（如 `plan: 完成阶段 1 — 基础设施与类型系统`）。

### 阶段 1: 基础设施 & 类型系统

所有后续阶段的前置依赖。

- **1a. TLS 序列号上限**：验证 nonce 构造为 deterministic 模式 → 修改 `kMaxSequenceNumber` 为 `1ULL << 32` → 更新阈值常量 → 更新相关测试
- **1b. 时钟审查**：grep 全项目 `system_clock` 使用，确认 RateLimiter、ReconnectPolicy、Gateway 均使用 `steady_clock`。发现问题则修复。
- **1c. ConnectionError 枚举扩展**：扩展枚举 → 修改 `connection_error_name()` → 迁移 eph-net `SocketTransport::connect()`、`HttpClient`、`proxy.hpp` 的返回类型 → 迁移 eph-dpdk `connector.hpp::connect()`、`tcp.hpp::connect()`、`arp.hpp::resolve()`、`dns.hpp::resolve()` 的返回类型 → 更新所有受影响的测试断言。**注：eph-dpdk 部分代码修改完成但无法编译验证，待 DPDK 环境补验。**
- **1d. ASan/UBSan + Fuzzer 编译模式**：xmake.lua 新增 `--sanitize=address,undefined` 模式和 `--sanitize=fuzzer` 支持
- **1e. 日志命名空间统一**：修改 eph-transport 中 3 个 logger 名称 → grep 全项目确认无硬编码的旧名称引用
- 交付物：编译通过 + 全部现有测试在 normal 和 ASan 模式下通过
- 推荐 skill：`/design auto`
- 预估：1-2 天

### 阶段 2: 连接可靠性

- **2a. Gateway 降级检测**：`GatewayConnection` 新增 `rx_packets_fn` / `last_rx_packets` / `degraded_since_ns` 字段 → `register_transport()` 绑定 stats 访问 → `check_health()` 实现三态逻辑 → 单元测试覆盖 Healthy → Degraded → Healthy 转换
- **2b. ARP 缓存定时刷新**（eph-dpdk）：在 `TcpSession` 或 `Connector` 中增加可选的定时 gratuitous ARP 请求（默认 60s 间隔，可配置，0 = 禁用）。**待 DPDK 环境验证。**
- **2c. 日志脱敏审查**：审查所有 `dump()` / `to_json()` 方法 → 确保密钥、密码、证书路径等字段被脱敏（如 `"api_key": "***"`）→ 参照 `proxy.hpp` 中密码字段的 "never logged" 模式统一应用
- 交付物：Gateway 降级测试通过 + 日志输出无敏感信息
- 推荐 skill：`/design auto`
- 预估：1-2 天

### 阶段 3: 安全加固 + 测试基础

- **3a. Soft certificate pinning**：`TransportConfig` 新增 `pinned_spki_sha256` 和 `on_pin_mismatch` → TLS 握手后在 `transport_core.hpp` 中提取 peer cert SPKI SHA-256 → 与 pin 列表比较 → 不匹配时 WARN + 回调 → 单元测试
- **3b. FakeTcpTransport**：定义满足 TcpTransport concept 的 mock → 支持 inject_rx / inject_error / inject_disconnect → `static_assert(TcpTransport<FakeTcpTransport>)` 编译验证
- 交付物：pin mismatch 回调测试通过 + FakeTcpTransport concept 满足
- 推荐 skill：`/design auto`
- 预估：2-3 天

### 阶段 4: 测试覆盖

- **4a. E2E 集成测试（mock）**：基于 FakeTcpTransport 的完整生命周期测试，覆盖场景：
  - connect → send → recv → close（happy path）
  - 连接中途对端 RST
  - TLS 握手超时
  - WS upgrade 被拒（HTTP 403）
  - RX 队列溢出 + on_rx_drop 回调
  - reconnecting 状态下 send() 返回 kNotConnected
- **4b. E2E 集成测试（loopback socket）**：基于 eph-net SocketTransport 的真实网络测试，验证 connect → send → recv → close 的端到端数据正确性
- **4c. 3 个 libFuzzer harness**：
  - `fuzz_ws_decode.cpp`：fuzzing `websocket.hpp::decode_frame()`
  - `fuzz_http_parse.cpp`：fuzzing `http_message.hpp::parse_http_response()`
  - `fuzz_dns_reply.cpp`：fuzzing `dns.hpp` 的 reply parser
- 交付物：E2E 测试全通过（normal + ASan）+ fuzzer 运行 10 分钟无 crash
- 推荐 skill：`/test` + `/design auto`
- 预估：3-4 天

### 阶段 5: 性能优化

- **5a. TX 批量发送**（eph-dpdk）：`TcpSession` 新增 `send_batch()` 方法，攒包后调用 `rte_eth_tx_burst()` 批量发送。保留现有 `send()` 单包接口不变。**待 DPDK 环境验证。**
- **5b. HWM 实时追踪**：将 Transport 中 HWM 从每 64 次采样改为 atomic max-CAS（`compare_exchange_weak` loop），确保捕捉瞬时峰值
- **5c. 周期性阈值回调**：Worker 线程每 1024 次迭代读取 stats → 与 `ThresholdConfig` 中的阈值比较 → 触发 `on_breach` 回调
- **5d. E2E 延迟 benchmark**：新增 `bench_e2e_latency.cpp`，基于 loopback socket 测量 send-to-wire 和 wire-to-recv 的完整延迟分布（P50/P99/P99.9）
- 交付物：现有 benchmark 无回归 + 新 benchmark 基线数据
- 推荐 skill：`/bench` + `/design auto`
- 预估：3-4 天

---

## 关键决策记录

### D-1: TLS 序列号上限 2^24 → 2^32
- **问题**：当前 2^24 上限在 100K msg/sec 下仅 167 秒，是否安全提升
- **选项**：A. 提升到 2^32 / B. 实现 make-before-break / C. TLS KeyUpdate / D. A+B
- **决策**：A
- **理由**：nonce 使用 deterministic 构造（seq XOR implicit_iv），NIST SP 800-38D 对此模式实际限制是 2^32。提升后 100K/s 下可用 ~12 小时，远超交易所的维护周期（通常 24h 强制断连）。实现成本极低（改常量），风险极低
- **验收标准**：确认 nonce 构造代码为 `seq XOR implicit_iv` 且 seq 类型为 uint64_t；修改后 TLS 加解密测试全通过

### D-2: 错误体系统一策略
- **问题**：eph-dpdk/eph-net 用 string 错误，eph-transport 用结构化错误
- **选项**：A. 全部迁移到 ConnectionErrorInfo / B. 新建通用 Error 类型 / C. 仅迁移 connect()
- **决策**：A
- **理由**：一步到位，所有调用点都能程序化匹配错误类别。ConnectionErrorInfo 已在 eph-core 定义，只需扩展枚举
- **验收标准**：项目中不再有 `std::expected<T, std::string>` 作为 connect 类函数的返回类型；所有 ConnectionError 枚举值都有对应的 `connection_error_name()` 条目

### D-3: Gateway 降级检测方式
- **问题**：如何检测"连接活着但数据停滞"
- **选项**：A. Transport 回调驱动 / B. Stats 轮询驱动 / C. 双信号（+RTT）
- **决策**：A 的变体——通过 type-erased `rx_packets_fn` 在 monitor 线程中轮询 stats
- **理由**：零侵入（不修改 Transport 核心代码），复用现有的 type-erasure 注册模式，rx_packets 是 atomic 读取无锁开销
- **验收标准**：Gateway 单元测试覆盖 Healthy ↔ Degraded 双向转换 + on_health_change 回调触发

### D-4: Soft Certificate Pinning
- **问题**：如何平衡安全性和运维成本
- **选项**：硬 pin（阻断） / soft pin（告警+回调） / CT Log 监控
- **决策**：Soft pin（告警+回调，应用层决定是否断开）
- **理由**：交易所证书轮换频繁（Let's Encrypt 90 天），硬 pin 的运维断连风险高于 MiTM 攻击概率。Soft pin 允许应用层灵活响应
- **验收标准**：pin 匹配时静默通过；不匹配时 WARN 日志 + on_pin_mismatch 回调触发；空 pin 列表时完全跳过检查

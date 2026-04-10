# Plan: Phase 9 — HFT-pragmatic Feature & Test Recovery

> 回迁 v3.3 重构期间意外丢失的功能和测试。HFT-pragmatic scope：6 个功能组件 + ~770 测试（51% baseline 覆盖），所有回迁**基于 v3.3 新架构重新实现**并**向 Tokio 风格对齐**。

创建时间：2026-04-10
状态：草案（待执行）
分支：refactor/transport-api
基于 commit：a50f2c0
上游文档：
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — v3.3 架构 SSOT
- `.artifacts/discuss-20260410-174332.md` — scope debate，从 1332→770 cases
- `.temp/baseline-pre-v3.3/` — 删除源代码归档（v3.4 发布前保留）

---

## 定位与边界

**目标**：Phase 9 不是新功能也不是重构，是 **recovery**——把 v3.3 Phase 0-8 重构过程中意外丢失的代码和测试找回来，但按新架构重新实现（不是 copy）。

**用户**：HFT 客户端开发者。写交易策略，需要：
- wss:// 连到交易所（WS HTTP 握手 + TLS）
- REST API 签名（HMAC-SHA256）
- 通过 Prime Broker 的 HTTP CONNECT proxy 访问交易所
- 合规要求的 KillSwitch
- 交易所 rate limit 的 TokenBucket

**In scope（6 组件 + ~770 测试）**：
- HTTP parser 子集（只支持 WS upgrade 响应 + REST GET/POST）
- WS HTTP 握手（透明集成到 TcpStream::create）
- HMAC-SHA256（typed Key / Tag）
- HTTP CONNECT proxy（kernel 透明，DPDK 拒绝）
- KillSwitch minimal（atomic + callback）
- RateLimiter TokenBucket minimal（thread-safe）
- P0 security tests (64) + P0 regression tests (~30) + P1 parser tests (~420) + P2 stream behavioral rewrites (~140) + Proxy tests (~35) + KS/RL tests (~35)

**Out of scope（明确放弃，归档到 scope-decision.md）**：
- Gateway（强制组合不符合 HFT 实践；baseline 无消费者）
- CircuitBreaker（opinions 分歧；HFT 写 per-venue 状态机）
- HTTP chunked transfer / Transfer-Encoding / cookies / redirect / Expect: 100-continue / multipart
- SOCKS5 proxy（HFT 无用例）
- 旧 alias 系统测试：`test_transport_types.cpp` (211 cases) + `test_transport.cpp` (110 cases)
- 旧 Gateway/CB 测试（157 cases）
- HFT 不用的 HTTP edge case 测试（~85 cases）

---

## 技术选型

继承 v3.3。无新选择。

| 类别 | 选择 |
|---|---|
| 语言 | C++23（`std::expected`、`std::format`、concepts） |
| 构建 | xmake |
| 测试 | gtest（继承 `eph-test` rule） |
| 加密 | aws-lc（HMAC、TLS 共享依赖） |
| 日志 | spdlog + SPDLOG_ACTIVE_LEVEL 编译期过滤 |
| 构建风格 | header-only，无 .cpp under include/ |

---

## 架构设计

### 模块归属（不新增模块，11 不变）

| 组件 | 模块 | 路径 | 公开度 |
|---|---|---|---|
| HTTP parser 子集 | eph-net | `eph-net/include/eph/net/http.hpp` | **public** |
| WS HTTP 握手 helper | eph-net | `eph-net/include/eph/net/detail/ws_handshake.hpp` | internal |
| HMAC-SHA256 | eph-net | `eph-net/include/eph/net/hmac.hpp` | **public** |
| HTTP CONNECT proxy helper | eph-net | `eph-net/include/eph/net/detail/http_connect.hpp` | internal |
| Proxy config 类型 | eph-net | `eph-net/include/eph/net/proxy.hpp` | **public** |
| KillSwitch | eph-utils | `eph-utils/include/eph/utils/kill_switch.hpp` | **public** |
| RateLimiter TokenBucket | eph-utils | `eph-utils/include/eph/utils/rate_limiter.hpp` | **public** |

### 集成点

1. **WS 握手集成**：`eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp` 和 `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` 的 `create()` 在 TLS handshake 之后、返回之前，如果 `cfg.ws_path` 非空则调用 `detail::perform_ws_handshake`
2. **Proxy 集成**：`eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp` 的 `create()` 在 TCP connect 之后、TLS handshake 之前，如果 `cfg.proxy` 非空则调用 `detail::perform_http_connect`；DPDK 版本遇到非空 `cfg.proxy` 直接返回 `ErrorInfo{Error::InvalidConfig, "proxy not supported on DPDK backend"}`
3. **StreamConfig 新字段**：两个 backend 的 `StreamConfig` 新增
   - `std::string ws_path` (空字符串 = 不做 WS upgrade)
   - `std::optional<ProxyConfig> proxy` (none = 不通过 proxy)
   - `std::vector<HttpHeader> ws_extra_headers` (可选，用户自定义 upgrade request header)

### Error enum 扩展

`eph-core/include/eph/core/error.hpp` 追加 3 个 Error 值（在 Phase 0 已定义的 enum 末尾新增）：

```cpp
enum class Error : uint8_t {
    // ... existing (Phase 0) ...
    ProxyConnectFailed,     // TCP connect to proxy server failed
    ProxyHandshakeFailed,   // proxy returned non-200 (e.g. 502)
    ProxyAuthRequired,      // 407 Proxy Auth Required (Basic auth missing/wrong)
};
```

与对应的 `error_name()` 字符串。放在 sub-phase 9.6 内改动。

### 模块依赖变更

无。Phase 9 只增加文件，不改依赖图。验证：`eph-net` 仍只依赖 eph-core + eph-containers + aws-lc；`eph-utils` 仍独立。

---

## 接口设计

### 1. HTTP Parser 子集（`eph-net/include/eph/net/http.hpp`）

```cpp
namespace eph::net {

struct HttpHeader {
    std::string_view name;
    std::string_view value;
};

struct HttpRequest {
    std::string_view              method;        // "GET", "POST", "CONNECT"
    std::string_view              target;        // "/api/v3/order?symbol=BTC"
    uint8_t                       version_minor; // 0 = HTTP/1.0, 1 = HTTP/1.1
    std::span<const HttpHeader>   headers;
    std::span<const uint8_t>      body;          // 空或 Content-Length 指定
};

struct HttpResponse {
    uint16_t                      status_code;
    std::string_view              reason_phrase;
    uint8_t                       version_minor;
    std::span<const HttpHeader>   headers;
    std::span<const uint8_t>      body;
};

template <class T>
struct ParseResult {
    T      value;
    size_t consumed;               // 调用者从 buffer 移除这么多字节
};

// 增量解析：
//   Ok(Some(ParseResult))  — 解析出完整消息
//   Ok(None)               — 数据不够，调用者拿更多字节再试
//   Err(ErrorInfo)         — 协议错误（不可恢复）
// 
// 重要约束：
// - HttpRequest/HttpResponse 的 string_view/span 字段生命周期绑定到 buffer
// - header_storage 由调用者提供（HFT 零堆分配原则）
// - 不支持 chunked transfer encoding, Transfer-Encoding, cookies, redirect, Expect
[[nodiscard]]
std::expected<std::optional<ParseResult<HttpRequest>>, core::ErrorInfo>
parse_http_request(
    std::span<const uint8_t>      buffer,
    std::span<HttpHeader>         header_storage
) noexcept;

[[nodiscard]]
std::expected<std::optional<ParseResult<HttpResponse>>, core::ErrorInfo>
parse_http_response(
    std::span<const uint8_t>      buffer,
    std::span<HttpHeader>         header_storage
) noexcept;

// 构造（写入用户 buffer，返回写入字节数）
[[nodiscard]]
std::expected<size_t, core::ErrorInfo>
build_http_request(
    uint8_t*                      buf,
    size_t                        cap,
    std::string_view              method,
    std::string_view              target,
    std::span<const HttpHeader>   headers,
    std::span<const uint8_t>      body = {}
) noexcept;

[[nodiscard]]
std::expected<size_t, core::ErrorInfo>
build_http_response(
    uint8_t*                      buf,
    size_t                        cap,
    uint16_t                      status_code,
    std::string_view              reason_phrase,
    std::span<const HttpHeader>   headers,
    std::span<const uint8_t>      body = {}
) noexcept;

} // namespace eph::net
```

**设计要点**（Tokio 对齐）：
- 增量 `expected<optional<T>>` 匹配 `httparse::Status::Complete/Partial` + v3.3 `StreamCodec::decode`
- 零堆分配（caller-provided storage）
- Zero-copy（span / string_view 指向输入 buffer）
- 不做 HeaderMap 类型化（HFT 头数量 ≤20，span 线性查找够用）
- 所有函数 `noexcept`

### 2. WS HTTP 握手（`eph-net/include/eph/net/detail/ws_handshake.hpp`）

```cpp
namespace eph::net::detail {

// Perform WebSocket HTTP handshake (Sec-WebSocket-Key generation,
// Sec-WebSocket-Accept verification, 101 Switching Protocols check) over
// an already-connected (and optionally TLS-wrapped) byte sink.
//
// ByteSink is duck-typed: must provide
//   std::expected<size_t, ErrorInfo> send(std::span<const uint8_t>);
//   std::expected<size_t, ErrorInfo> recv(uint8_t*, size_t);
//
// Called by KernelTcpStream::create and DpdkTcpStream::create when
// cfg.ws_path is non-empty.
template <class ByteSink>
[[nodiscard]]
std::expected<void, core::ErrorInfo> perform_ws_handshake(
    ByteSink&                      io,
    std::string_view               host,         // Host header value
    std::string_view               ws_path,      // "/ws/btcusdt@trade"
    std::span<const HttpHeader>    extra_headers = {},
    std::chrono::milliseconds      timeout = std::chrono::seconds{10}
) noexcept;

} // namespace eph::net::detail
```

### 3. HMAC-SHA256（`eph-net/include/eph/net/hmac.hpp`）

```cpp
namespace eph::net {

// Typed HMAC-SHA256 key. Normalizes input at construction (keys > 64 bytes
// are SHA-256 hashed; keys < 64 bytes are zero-padded). Runtime sign calls
// are zero-alloc and noexcept.
//
// Key material is cleared on destruction (防 swap/coredump 泄露).
// Non-copyable to avoid stack sprawl of sensitive material.
class HmacSha256Key {
    alignas(64) uint8_t normalized_[64];
public:
    explicit HmacSha256Key(std::span<const uint8_t> raw) noexcept;
    explicit HmacSha256Key(std::string_view raw) noexcept;
    
    HmacSha256Key(const HmacSha256Key&)            = delete;
    HmacSha256Key& operator=(const HmacSha256Key&) = delete;
    HmacSha256Key(HmacSha256Key&&)                 = default;
    HmacSha256Key& operator=(HmacSha256Key&&)      = default;
    
    ~HmacSha256Key() noexcept;   // explicit_bzero(normalized_, 64)
};

// Typed tag (32 bytes of MAC output).
struct HmacSha256Tag {
    std::array<uint8_t, 32> bytes;
    
    // Write hex encoding (64 ASCII chars) to caller-provided buffer.
    // Returns number of bytes written (always 64).
    size_t to_hex(std::span<uint8_t, 64> out) const noexcept;
    
    // Convenience: returns owned std::string. Allocates — do NOT use on
    // HFT hot path. Prefer the span-based to_hex above for signing in a
    // per-request fashion.
    [[nodiscard]] std::string to_hex() const;
};

// One-shot sign. Zero-alloc, noexcept. Safe on HFT hot path.
[[nodiscard]]
HmacSha256Tag hmac_sha256_sign(
    const HmacSha256Key&     key,
    std::span<const uint8_t> data
) noexcept;

// Convenience overload
[[nodiscard]]
HmacSha256Tag hmac_sha256_sign(
    const HmacSha256Key& key,
    std::string_view     data
) noexcept;

} // namespace eph::net
```

**设计要点**（Tokio/ring 对齐）：
- `HmacSha256Key` 是 RAII 类型，析构清零
- 不可 copy，避免敏感 key 在栈上散播
- 构造时 normalize，runtime zero-alloc
- Tag 是 typed struct（不是原始 array），可带方法

### 4. ProxyConfig + HTTP CONNECT（`eph-net/include/eph/net/proxy.hpp` + `detail/http_connect.hpp`）

```cpp
// eph-net/include/eph/net/proxy.hpp
namespace eph::net {

struct ProxyConfig {
    std::string                    host;               // "proxy.example.com"
    uint16_t                       port{0};
    std::optional<std::string>     basic_auth_user;    // Basic auth username
    std::optional<std::string>     basic_auth_pass;    // Basic auth password
    std::chrono::milliseconds      timeout{std::chrono::seconds{10}};
};

} // namespace eph::net

// eph-net/include/eph/net/detail/http_connect.hpp
namespace eph::net::detail {

// Perform HTTP CONNECT handshake over an already-connected byte sink.
// Called by KernelTcpStream::create when cfg.proxy is non-empty,
// between TCP connect and TLS handshake.
template <class ByteSink>
[[nodiscard]]
std::expected<void, core::ErrorInfo> perform_http_connect(
    ByteSink&            io,
    const ProxyConfig&   proxy,
    std::string_view     target_host,
    uint16_t             target_port
) noexcept;

} // namespace eph::net::detail
```

### 5. KillSwitch（`eph-utils/include/eph/utils/kill_switch.hpp`）

```cpp
namespace eph::utils {

// Irreversible kill switch. Once tripped, stays tripped.
//
// Use cases:
//   1. Polling: check tripped() in main loops
//   2. Callback: pass on_trip at construction for immediate notification
//
// Thread-safe: trip() and tripped() use acquire-release ordering.
//
// Design note: does NOT support reset() — by design, a kill switch is
// a one-way action for compliance/risk reasons.
class KillSwitch {
    std::atomic<bool>       tripped_{false};
    std::function<void()>   on_trip_;
public:
    explicit KillSwitch(std::function<void()> on_trip = nullptr) noexcept;
    
    // Poll
    [[nodiscard]] bool tripped() const noexcept {
        return tripped_.load(std::memory_order_acquire);
    }
    
    // Idempotent. First call invokes on_trip callback (if set); subsequent
    // calls are no-ops. Safe to call from multiple threads — only one will
    // win the CAS and fire the callback.
    void trip() noexcept;
};

} // namespace eph::utils
```

### 6. RateLimiter TokenBucket（`eph-utils/include/eph/utils/rate_limiter.hpp`）

```cpp
namespace eph::utils {

// Thread-safe token bucket rate limiter.
//
// Use cases:
//   1. Per-venue rate limit (Binance 1200 req/min, OKX 100 req/sec, etc.)
//   2. Global trading throughput cap
//
// Supports weighted requests (Binance's weight-based rate limit).
//
// Design constraints:
//   - Configuration fixed at construction time (no dynamic reconfig)
//   - Mutex-based (~20ns uncontended) — adequate for HFT at N<10 
//     threads/bucket; lock-free variant can be added if profiling shows it
class TokenBucket {
public:
    struct Config {
        uint32_t capacity;             // max burst size (tokens)
        double   refill_per_second;    // sustained rate (tokens/sec)
    };
    
    explicit TokenBucket(Config cfg) noexcept;
    
    // Non-blocking. Returns true if weight tokens were deducted,
    // false if insufficient tokens. Never sleeps.
    [[nodiscard]] bool try_acquire(uint32_t weight = 1) noexcept;
    
    // Query current token count (estimate; may change immediately after).
    [[nodiscard]] double available_tokens() const noexcept;
    
    // Getters
    uint32_t capacity() const noexcept    { return capacity_; }
    double   refill_rate() const noexcept { return refill_per_sec_; }

private:
    const uint32_t capacity_;
    const double   refill_per_sec_;
    
    mutable std::mutex                             mu_;
    double                                         tokens_;
    std::chrono::steady_clock::time_point          last_refill_;
    
    void refill_locked_() noexcept;
};

} // namespace eph::utils
```

**设计要点**（Tokio/governor 对齐）：
- 线程安全默认（mutex 版本，简单可靠）
- `std::chrono::steady_clock` 时钟源（不是 TSC，秒级精度足够）
- 支持 weighted request（Binance 兼容）
- 无 dynamic reconfig（minimal 原则）

---

## 测试迁移策略

### P0 Security tests (64 cases) — 保持 baseline 拆分

| Baseline file | cases | → New file |
|---|---|---|
| `test_http_request_cl_te_injection.cpp` | 9 | `eph-net/tests/test_http_cl_te_injection.cpp` |
| `test_http_request_crlf_injection.cpp` | 15 | `eph-net/tests/test_http_crlf_injection.cpp` |
| `test_http_request_whitespace.cpp` | 14 | `eph-net/tests/test_http_whitespace.cpp` |
| `test_http_te_edge_cases.cpp` | 13 | `eph-net/tests/test_http_te_edge.cpp` |
| `test_build_upgrade_request_injection.cpp` | 13 | `eph-net/tests/test_ws_upgrade_injection.cpp` |

迁移方式：**copy**，只改 include / namespace / API 调用签名（`parse_http_request` 新签名含 header_storage）。输入字节序列不变。

### P0 Regression tests (~30 cases) — 从 commit hash 反向定位

对每个 fix commit：
```
08dacb1 fix(transport/ws): replace forbidden received close code
80d5a3b fix(transport/ws): reject 1-byte close frame body (RFC 6455 §5.5.1)
3bed1b3 fix(transport/ws): enforce RFC 6455 §5.2 extended length encoding rules
e108fcb fix(net/http): trim trailing OWS in header field-name (RFC 7230 §3.2.4)
5137fee fix(net/http): enforce RFC 7230 3-digit status code in parse_http_response
8dfe9f6 fix(eph-transport): CRLF/whitespace injection defense in build_upgrade_request
e336990 fix(eph-net): reject whitespace in build_http_request method/host/path
282f0e2 fix(eph-net): Content-Length must be entirely-numeric
54b056f fix(fix/framer): size_t underflow in OOB-read bounds check
```

Procedure:
1. `git show <hash> -- '**/test_*.cpp'` 看该 fix 增加了哪些具体 TEST
2. 从 baseline 对应文件 grep 这些 TEST
3. 迁到 v3.3 位置：WS 相关 → `eph-net/tests/test_websocket_wire.cpp`，HTTP 相关 → `eph-net/tests/test_http.cpp`，fix 相关 → 已有 `eph-fix/tests/test_fix.cpp`
4. 8dfe9f6 / e336990 / 282f0e2 可能与 P0 security 重合，避免重复

### P1 Parser hot-path (~420 cases) — 文件级映射

| Baseline | cases | → New file | 迁 cases | 方式 |
|---|---|---|---|---|
| `eph-net/tests/test_http.cpp` + `test_http_client.cpp` + `test_http_response_complete_adv.cpp` | 49+146+40 | `eph-net/tests/test_http.cpp` + `test_http_client.cpp` | **~150** | copy；砍含 chunked/TE/cookie/redirect 的 TEST |
| `eph-net/tests/test_websocket.cpp` | 122 | `eph-net/tests/test_websocket_wire.cpp` | **~100** | copy parser-level cases；stream 集成丢 |
| `eph-net/tests/test_tls_record.cpp` | 56 | `eph-net/tests/test_tls_record.cpp` | **56** | copy |
| `eph-transport/tests/test_tls_record_roundtrip.cpp` | 22 | 同上 | **22** | copy |
| `eph-transport/tests/test_tls_config.cpp` | 47 | `eph-net/tests/test_tls_config.cpp` | **47** | copy |
| `eph-net/tests/test_hmac.cpp` | 45 | `eph-net/tests/test_hmac.cpp` | **45** | copy；断言保持（RFC 4231 vectors），改 API 到 typed Key/Tag |

**合计 ~420 cases**

HTTP parser 砍的具体规则：
```
grep -n "chunked\|Transfer-Encoding\|Cookie:\|Set-Cookie\|Location:\|Expect: 100\|multipart" \
  .temp/baseline-pre-v3.3/eph-net/tests/test_http*.cpp
```
命中的 TEST 整体跳过（不是部分跳）。

### P2 Stream behavioral (~140 cases) — 完全重写

| Baseline | cases | → New file | 迁 cases | 方式 |
|---|---|---|---|---|
| `test_socket_transport.cpp` | 86 | `eph-net-kernel/tests/test_kernel_tcp_stream_behavioral.cpp` | **~60** | **完全重写**：读 baseline 断言含义，用 FakeStream/TestPoller/localhost loopback 重写。去掉 raw fd 断言 |
| `test_transport_config.cpp` | 83 | `eph-net-kernel/tests/test_stream_config_validation.cpp` | **~60** | 改类名 `TransportConfig` → `StreamConfig`，其余断言原样 |
| `test_tcp_concept.cpp` | 46 | 合并到 `eph-net/tests/test_concepts.cpp` | **~20** | 增补新 concept 的断言 |
| `test_transport.cpp` (110) + `test_transport_types.cpp` (211) | 321 | **丢弃** | 0 | 旧 alias 系统不存在，强迁无意义 |

**合计 ~140 cases**

### Proxy tests (~35 cases) — 三层拆分

| 层 | New file | cases |
|---|---|---|
| Proxy URL 解析 | `eph-net/tests/test_proxy_url.cpp` | ~15 |
| HTTP CONNECT handshake | `eph-net/tests/test_http_connect.cpp` | ~12 |
| KernelTcpStream + proxy 集成 | `eph-net-kernel/tests/test_kernel_proxy_integration.cpp` | ~8 |

**合计 ~35 cases**。其余 49 cases（SOCKS5 + old class state management）丢。

### KillSwitch + RateLimiter (~35 cases) — 基于新 API 重写

- `eph-utils/tests/test_kill_switch.cpp` — **~15 cases**
- `eph-utils/tests/test_rate_limiter.cpp` — **~20 cases**

**不从 baseline copy**（baseline 有 reset / multi-subscribe / audit log 等，与 minimal 设计差异大）。

### 总计

**~770 cases** = 64 P0 security + 30 P0 regression + 420 P1 parser + 140 P2 behavioral + 35 Proxy + 35 KS/RL - ~45 重复去重。

---

## 编码规范

继承 CLAUDE.md + `feedback_tokio_style.md`：

| 维度 | 规范 |
|---|---|
| 命名 | Tokio-aligned：`HmacSha256Key`, `HmacSha256Tag`, `TokenBucket::Config`，不用 C 风格 `hmac_ctx_t` |
| 错误处理 | `std::expected<T, core::ErrorInfo>` for fallible APIs；noexcept everywhere possible |
| Parser 模式 | 增量式 `expected<optional<T>>` 匹配 `StreamCodec::decode` |
| Heap allocation | Hot path 零堆分配；caller-provided storage 优于内部分配 |
| 线程安全 | 共享 primitive (RateLimiter) 默认线程安全；per-connection (TokenBucket per thread) 由 caller 决策 |
| 敏感数据 | Key material 析构清零（`explicit_bzero`），不可 copy |
| 日志 | `SPDLOG_DEBUG/WARN/ERROR`，非 trivial 函数要有 entry/error 日志 |
| 头文件风格 | header-only，无 .cpp under include/；detail/ 子目录放内部 helper |
| 注释 | 每个 public 函数有 `///` 文档注释说明前置条件、线程安全、生命周期约束 |

---

## 实施计划

> **Commit 策略**：每 sub-phase 完成并通过 6 层 verification gate 后，subagent 自动创建 commit，格式 `recover(phase-9.N): <标题>`。每个 sub-phase 是独立回滚点。

### 执行方式

使用 `/repeat` + subagent 模式（与 Phase 0-8 一致）。每 sub-phase 一个 general-purpose subagent，主循环负责派发和 until 条件检查。

**Verification gates（每 sub-phase 必须全绿）**：
1. **Build gate**: `xmake build <targets>` pass
2. **New tests gate**: 新增 tests 全通过
3. **Regression gate**: `xmake run -g tests` 零 FAIL
4. **Coverage gate**: `grep -c "TEST(" <new_file>` ≥ 承诺数
5. **Tokio style gate**: subagent self-review 对照 `feedback_tokio_style.md`
6. **Deliverable checklist**: sub-phase 特有清单全部勾选

**Rollback 策略**：subagent 内部自动 fix 直到 APPROVE；连续 3 次 fix 失败 → STATUS: BLOCKED，暂停等用户介入。

### Sub-phase 9.1 — eph-utils: KillSwitch + RateLimiter

**交付物**：
- `eph-utils/include/eph/utils/kill_switch.hpp`（~30 行）
- `eph-utils/include/eph/utils/rate_limiter.hpp`（~60 行）
- `eph-utils/tests/test_kill_switch.cpp`（~15 cases）
- `eph-utils/tests/test_rate_limiter.cpp`（~20 cases）

**验收 checklist**：
- [ ] KillSwitch：atomic + callback，single-fire，idempotent，thread-safe
- [ ] KillSwitch 析构 test（无内存泄露）
- [ ] TokenBucket：线程安全，weighted acquire，refill 精度测试（用 mock clock）
- [ ] TokenBucket 多线程 CAS 测试
- [ ] 两个类都是 header-only
- [ ] 无新的 eph-utils 依赖（aws-lc 等）

**依赖**：无
**预估**：10-15 min subagent

### Sub-phase 9.2 — eph-net: HMAC

**交付物**：
- `eph-net/include/eph/net/hmac.hpp`（~80 行）
- `eph-net/tests/test_hmac.cpp`（从 baseline copy 45 cases，改 API）

**验收 checklist**：
- [ ] HmacSha256Key RAII + 非 copy + 析构清零
- [ ] `hmac_sha256_sign(key, data) -> HmacSha256Tag` 一次性 signature
- [ ] `HmacSha256Tag::to_hex(span<uint8_t, 64>)` 零堆分配版本
- [ ] RFC 4231 test vectors 全部通过（45 cases）
- [ ] 析构清零行为验证（memset 检查）
- [ ] 复用 aws-lc，不引入新依赖

**依赖**：无（aws-lc 已由 eph-net 使用）
**预估**：10-15 min subagent

### Sub-phase 9.3 — eph-net: HTTP Parser 子集

**交付物**：
- `eph-net/include/eph/net/http.hpp`（~400 行）
- `eph-net/tests/test_http.cpp`（基础 smoke tests ~30 cases，完整迁移在 9.4）

**验收 checklist**：
- [ ] `parse_http_request` / `parse_http_response` 增量模式 `expected<optional<ParseResult>>`
- [ ] `build_http_request` / `build_http_response` 零堆分配
- [ ] caller-provided `header_storage`
- [ ] Smoke tests: 基本 GET/POST request、200 OK response、incomplete buffer 返回 None
- [ ] **明确拒绝**：chunked transfer encoding 检测到返回 `ErrorInfo{Error::CodecBad, "chunked not supported"}`
- [ ] `parse_http_request` 支持 CONNECT method（给 9.6 用）

**依赖**：无
**预估**：20-30 min subagent

### Sub-phase 9.4 — HTTP Parser 完整测试迁移

**交付物**：
- `eph-net/tests/test_http.cpp`（补充到 ~80 cases，P1 hot-path）
- `eph-net/tests/test_http_client.cpp`（~70 cases）
- P0 security 5 文件：
  - `test_http_cl_te_injection.cpp`（9 cases）
  - `test_http_crlf_injection.cpp`（15 cases）
  - `test_http_whitespace.cpp`（14 cases）
  - `test_http_te_edge.cpp`（13 cases）
  - `test_ws_upgrade_injection.cpp`（13 cases）（虽然叫 ws_upgrade，但构造请求用 http.hpp）

**验收 checklist**：
- [ ] P0 security 64 cases 全量 migrate
- [ ] P1 HTTP parser ~150 cases migrate（按砍 chunked 规则）
- [ ] Coverage gate: `grep -c TEST` ≥ 214
- [ ] 断言语义保持（输入 bytes 不变，只改 API 调用方式）
- [ ] 编译期验证 `parse_http_request` 接受 CONNECT method

**依赖**：9.3
**预估**：30-40 min subagent

### Sub-phase 9.5 — WS Handshake 集成

**交付物**：
- `eph-net/include/eph/net/detail/ws_handshake.hpp`（~200 行，template function）
- 修改 `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp` `create()`：TLS 之后如果 `cfg.ws_path` 非空调用 `perform_ws_handshake`
- 修改 `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` `create()`：同上
- 修改 `eph-net-kernel/include/eph/net/kernel/config.hpp` `StreamConfig`：新增 `std::string ws_path` + `std::vector<HttpHeader> ws_extra_headers`
- 修改 `eph-net-dpdk/include/eph/net/dpdk/config.hpp`：同上
- `eph-net/tests/test_ws_handshake.cpp`（~10 cases：成功握手、Accept 错、状态码非 101、超时）
- `eph-net-kernel/tests/test_kernel_ws_upgrade.cpp`（~5 integration cases）

**验收 checklist**：
- [ ] `perform_ws_handshake` 是 template function，ByteSink duck-typed
- [ ] Sec-WebSocket-Key base64 随机生成（用 getrandom）
- [ ] Sec-WebSocket-Accept 正确验证（SHA1 + magic GUID + base64）
- [ ] StreamConfig.ws_path 空 = 不做 upgrade（向后兼容）
- [ ] Kernel + DPDK 两边都能构造 WS stream（DPDK TLS 在 Phase 7 已解锁）
- [ ] Integration test：连到 localhost WebSocket echo server 做一次完整握手

**依赖**：9.3（`perform_ws_handshake` 使用 `parse_http_response`）
**预估**：25-35 min subagent

### Sub-phase 9.6 — HTTP CONNECT Proxy

**交付物**：
- `eph-core/include/eph/core/error.hpp`：追加 3 个 Error 值（ProxyConnectFailed, ProxyHandshakeFailed, ProxyAuthRequired）
- `eph-net/include/eph/net/proxy.hpp`（公开 ProxyConfig struct）
- `eph-net/include/eph/net/detail/http_connect.hpp`（~150 行 template helper）
- 修改 `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp` `create()`：TCP connect 之后、TLS handshake 之前如果 `cfg.proxy` 非空调用 `perform_http_connect`
- 修改 `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` `create()`：非空 `cfg.proxy` → 返回 `InvalidConfig`
- 修改 StreamConfig：`std::optional<ProxyConfig> proxy`
- `eph-net/tests/test_proxy_url.cpp`（~15 cases：URL 解析 / Basic auth 编码）
- `eph-net/tests/test_http_connect.cpp`（~12 cases：成功、407、502、timeout、连接失败）
- `eph-net-kernel/tests/test_kernel_proxy_integration.cpp`（~8 cases：端到端、reconnect、错误传播）

**验收 checklist**：
- [ ] `Error` enum 3 个新值 + `error_name()` 对应字符串
- [ ] `ProxyConfig` 公开且 `noexcept` 构造
- [ ] `perform_http_connect` 是 template，复用 `build_http_request` + `parse_http_response`
- [ ] Basic auth 用 `HmacSha256Key` 同样的 base64 实现（或独立 base64 helper）
- [ ] DPDK backend 明确拒绝 proxy
- [ ] 35 cases 全绿
- [ ] 无 SOCKS5 代码

**依赖**：9.3（用 HTTP parser）+ 9.2（base64 可能从 HMAC 借用）
**预估**：25-35 min subagent

### Sub-phase 9.7 — Parser Regression 测试迁移

**交付物**：
- `eph-net/tests/test_websocket_wire.cpp`（~100 cases from baseline test_websocket.cpp 的 wire 层 cases）
- `eph-net/tests/test_tls_record.cpp`（56 cases from baseline test_tls_record.cpp）
- 补充 `eph-net/tests/test_tls_record.cpp`（22 cases from baseline test_tls_record_roundtrip.cpp，合并）
- `eph-net/tests/test_tls_config.cpp`（47 cases from baseline）
- 将 P0 regression 的 WS/HTTP/TLS cases 分散插入对应文件

**验收 checklist**：
- [ ] test_websocket_wire.cpp ≥ 100 cases（包含 close code 1007 / 8-byte extended length / 1-byte close / RFC §5.2 等近期 bug fix regression tests）
- [ ] test_tls_record.cpp ≥ 78 cases（含 roundtrip）
- [ ] test_tls_config.cpp ≥ 47 cases
- [ ] P0 regression 9 个 fix commits 每个都有对应 TEST
- [ ] 全部通过（现有 eph-net/detail/{websocket,tls_*} 是 Phase 7 migration 的产物，行为应与 baseline 等价）

**依赖**：无（测试的是 Phase 7 已存在的代码）
**预估**：35-45 min subagent

### Sub-phase 9.8 — P2 Stream Behavioral 重写

**交付物**：
- `eph-net-kernel/tests/test_kernel_tcp_stream_behavioral.cpp`（~60 cases 重写）
- `eph-net-kernel/tests/test_stream_config_validation.cpp`（~60 cases）
- 补充 `eph-net/tests/test_concepts.cpp`（增加 ~20 cases from baseline test_tcp_concept.cpp）

**验收 checklist**：
- [ ] 不 copy baseline 代码；基于 FakeStream/TestPoller/localhost loopback 重写
- [ ] 连接生命周期、send/recv、close_gracefully、错误传播、reconnect 触发
- [ ] StreamConfig validation（remote_host 非空、timeout 正、reconnect_config 合法等）
- [ ] 140 cases 全绿
- [ ] 不依赖 raw fd 细节（新 API 不暴露 socket internals）
- [ ] 测试覆盖带 ws_path 的 StreamConfig（Sub-phase 9.5 新增 field）
- [ ] 测试覆盖带 proxy 的 StreamConfig（Sub-phase 9.6 新增 field）

**依赖**：9.5（WS handshake）+ 9.6（proxy）
**预估**：35-45 min subagent

### Sub-phase 9.9 — Phase 9 Closing

**交付物**：
- `.artifacts/phase-9-scope-decision.md` — 归档"明确不迁"清单 + 理由
- CLAUDE.md 更新（补 eph-utils 新增组件、新 HTTP/HMAC public API）
- `eph-core/CHANGELOG.md`（Error enum 新增值）
- `eph-net/CHANGELOG.md`（HTTP/HMAC/proxy/WS handshake）
- `eph-utils/CHANGELOG.md`（KillSwitch + RateLimiter）
- `eph-net-kernel/CHANGELOG.md`（StreamConfig 新字段）
- `eph-net-dpdk/CHANGELOG.md`（StreamConfig 新字段 + proxy 拒绝）
- Final grep audit：确认没有 baseline 能用的 public symbol 但 v3.3 缺失
- `.temp/baseline-pre-v3.3/` 生命周期记录（保留到 v3.4 发布）

**scope-decision.md 必须包含**：
- Out-of-scope 列表（Gateway / CircuitBreaker / chunked / SOCKS5 等，含理由）
- "如果将来需要" 恢复指南（从 .temp/baseline 哪个文件找起，按哪个 v3.3 pattern 重新设计）
- `.temp/` 删除时间点：**v3.4 release 时**，由 release 脚本或手动 checklist 执行

**验收 checklist**：
- [ ] 完整 clean build: `xmake clean && xmake build`
- [ ] 完整 test pass: `xmake run -g tests` 零 FAIL
- [ ] Examples build: `xmake build -g examples`
- [ ] Benchmarks build: `xmake build -g benchmarks`
- [ ] `lat_tcp` / `lat_ws` / `lat_ex_market` sanity run (~1 min each)
- [ ] Grep audit: `grep -rn "parse_http_response\|build_http_request\|HmacSha256\|ProxyConfig\|KillSwitch\|TokenBucket" --include="*.hpp"` 找到所有新 symbol 在 v3.3
- [ ] scope-decision.md 格式正确
- [ ] .artifacts/INDEX.md 更新

**依赖**：所有之前 sub-phase
**预估**：15-20 min subagent

### 总预估

9 sub-phases × 平均 25 min = **~3.5 小时**（subagent 顺序执行，与 Phase 0-8 节奏一致）。

---

## 关键决策记录

### D-1: HTTP parser 是否支持 chunked transfer encoding

**问题**：Baseline 的 HTTP parser 支持 chunked TE（146 cases 中约 40 cases 涉及），HFT 交易所 API 不用 chunked。

**选项**：
- A. 全支持（baseline 等价）— ~500 行代码 + 40 tests
- B. 子集 + 明确拒绝 chunked — ~400 行代码 + `Error::CodecBad`

**决策**：B

**理由**：
- HFT 交易所（Binance/OKX/Coinbase/Kraken/FTX/Bitget）的 REST API 全部用 Content-Length 响应（2026-04 抽样确认）
- chunked 的解析复杂度是非线性的（trailer headers、chunk extensions、chunk size parsing）
- 明确拒绝 > 静默 undefined 行为
- baseline 的 chunked tests (40 cases) 测的是 eph 接收不会用的特性

**验收标准**：`parse_http_response` 遇到 `Transfer-Encoding: chunked` header 返回 `ErrorInfo{Error::CodecBad, "chunked transfer encoding not supported"}`；有对应 test 验证这个行为。

---

### D-2: WS handshake 是否透明集成到 TcpStream::create

**问题**：Tokio 是独立 `connect_async(url)` 函数，eph 可以选择同样做法或 config 驱动透明集成。

**选项**：
- A. 独立 `create_websocket(cfg)` 函数（Tokio 风格）
- B. Config 驱动：`StreamConfig.ws_path` 非空时自动做 upgrade

**决策**：B

**理由**：
- v3.3 已选 "collapsed design"（TLS 是 Stream 的 bool 模板参数而非 TlsStream 装饰器）
- 保持内在一致性比绝对 Tokio 对齐更重要
- 用户 API 更简洁：一个 `create(cfg)` 处理 TCP + TLS + WS 全栈
- HFT 用户只关心"给我一个能发送/接收 WS 消息的 stream"，不关心中间的 upgrade 细节

**验收标准**：`KernelTcpStream::create(cfg)` 在 `cfg.ws_path` 非空时自动做 WS handshake；空时不做。DPDK 同理。

---

### D-3: KillSwitch 是否支持 reset

**问题**：Baseline 的 KillSwitch 支持 `reset()`。新 minimal 不支持。

**选项**：
- A. 支持 reset（baseline 等价）
- B. 不支持 reset（新 minimal）

**决策**：B

**理由**：
- Kill switch 的本义是"单向紧急停机动作"
- 支持 reset 鼓励误用（trip → fix bug → reset → 重新启动策略），这破坏合规语义
- 合规场景：一旦 tripped，整个 process 应该 shutdown 后人工 review 才重启，不能 in-process reset
- 如果需要 "可 reset 的状态标志"，那是 `std::atomic<bool>`，不是 KillSwitch

**验收标准**：`KillSwitch` 没有 `reset()` / `untrip()` / `clear()` 方法；`trip()` 后 `tripped()` 永远返回 true。

---

### D-4: RateLimiter 是否线程安全默认

**问题**：HFT 典型用例是单线程 per bucket，线程安全有 mutex overhead。但非线程安全会导致用户忘加 mutex 出 bug。

**选项**：
- A. 非线程安全（minimal，快）
- B. 线程安全（governor-style）
- C. 模板参数选择

**决策**：B

**理由**：
- Tokio `governor` 默认线程安全，用户期望一致
- mutex uncontended cost ~20ns，HFT 可接受
- 用户 bug 避免 > 纳秒优化
- C 增加模板复杂度，minimal 哲学拒绝

**验收标准**：`TokenBucket::try_acquire` 从多线程调用不崩溃，计数正确；性能测试 mutex overhead < 50ns。

---

### D-5: Gateway / CircuitBreaker 是否迁

**问题**：Baseline 有完整实现 + 测试，HFT 用户代言人承认在实践中不用通用框架。

**选项**：
- A. 迁（全量等价）
- B. 不迁（scope 外）
- C. 迁骨架（~30 行）

**决策**：B（不迁）

**理由**：
- Baseline 的 grep 证据：0 example 消费 Gateway/CircuitBreaker
- HFT 团队写 per-venue 状态机而非通用 framework
- Gateway 强制组合形态导致用户不用
- 迁移成本高（93 + 64 = 157 cases），收益零

**验收标准**：`scope-decision.md` 明确列出 Gateway/CircuitBreaker 在 out-of-scope，并给出"未来需要时的恢复路径"。

---

### D-6: `.temp/baseline-pre-v3.3/` 保留到何时

**问题**：baseline 是 refactor 期间的参考资料，不应该永久占空间，但过早删除会失去未来参考。

**选项**：
- A. refactor 完成后立即删除
- B. merge 到 main 后删除
- C. v3.4 release 后删除

**决策**：C（v3.4 release）

**理由**：
- 给 Phase 9 的 recovery 工作做参考（核心用途）
- v3.4 发布意味着 v3.3 已在生产稳定一段时间，发现更多回归需求的概率很低
- `.gitignore` 已排除，不污染 git 历史，成本几乎为零

**验收标准**：`scope-decision.md` 包含 "`.temp/baseline-pre-v3.3/` 保留至 v3.4 release" 条款；v3.4 release checklist 包含手动删除步骤。

---

## 完成信号

Phase 9 完成的标志：
1. 9 个 sub-phases 全部 commit，最后 review APPROVE
2. 完整 clean build 通过
3. 完整 test suite 零 FAIL（含所有 Phase 0-8 测试 + Phase 9 新增 ~770 cases）
4. `lat_*` benchmark sanity run 通过（无崩溃、延迟合理）
5. `.artifacts/phase-9-scope-decision.md` 存在且格式正确
6. CLAUDE.md 更新完成
7. `git log --oneline d67c276^..HEAD` 显示 1 design 锚点 + 9 v3.3 phases + 9 v9 sub-phases，共 19 commits

此后 refactor/transport-api 分支 ready for merge to main。

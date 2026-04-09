# Plan: TLS hot-path AEAD crypto init ordering fix

> 修复 `TransportCore::do_connect` 在 `do_ws_upgrade` 之前 snapshot SSL 序列号导致的 TLS 1.3 nonce desync bug；抽出 `arm_aead_crypto()` helper、修 4 个 callsite、加 e2e 回归测试、加接口契约文档。

创建时间：2026-04-09
状态：已完成（commits 4eab3fb fix + 4751ad7 tests）

参考文档：`.artifacts/discuss-20260409-173817.md`（5 角色对抗讨论 + 4 个质疑点的逐一击穿）

---

## 定位与边界

**目标**：消除 commit `9788c4a8` 引入的 P0 协议层 bug，并堵上让该 bug 潜伏 7 天的测试覆盖盲区，保证未来同类 refactor 不会再无声破坏。

**用户**：所有使用 `Transport` / `DirectTxTransport` + `WsFramer` + `use_tls=true` 的下游 —— 包括 `simple_hft`、`binance_book`、`simple_hft_dpdk`、`ws_echo_client` 这些 example，以及任何照 README 抄的外部用户。

**In scope**：
- 抽出 `TransportCore::arm_aead_crypto()` helper 并在 4 个 callsite 调用（Transport::create / Transport reconnect / DirectTxTransport::create / DirectTxTransport reconnect）
- `DirectTransport` 重构为也调用同一 helper（统一接口、防回归）
- 新增 in-process TLS+WS echo server fixture（`tests/support/tls_ws_echo_server.hpp`）
- 新增 e2e 集成测试（`tests/integration/test_transport_tls_ws_e2e.cpp`），覆盖 3 个 transport 变体的 round-trip + reconnect 场景
- 在 `TlsSession::extract_hot_state` 加 doxygen pre-condition；修正 line 575 自相矛盾的注释
- Hotfix 之前手跑 `simple_hft` 记录 bug 指纹；修复后再跑确认症状消失

**Out of scope**（作为 follow-up issue）：
- 把 example 纳入 nightly CI smoke test
- DPDK 路径的 TLS+WS 单元测试覆盖（DPDK 测试需要 root + NIC，由 `lat_ws --dpdk` 这条 latency bench 路径在手动场景下覆盖）
- 重命名 `extract_hot_state` → `snapshot_hot_state`（噪音变更，仅文档锁住契约）
- 新建 `arm_aead_crypto` 之外的任何 TransportCore API 重构

---

## 技术选型

| 类别 | 选择 | 理由 |
|---|---|---|
| 语言 | C++23（不变） | 跟随项目 |
| 构建 | xmake（不变） | 跟随项目 |
| 测试框架 | gtest（不变，沿用 `eph-test` rule） | 跟随项目 |
| 测试 TLS server | 进程内同进程线程，aws-lc `SSL_CTX` 直接 accept | 零外部依赖、零子进程、< 1 秒 run time，符合 codebase header-only + zero-dep 风格 |
| 测试证书 | aws-lc `EVP_PKEY` + `X509` API 在测试启动时 in-memory 生成 ephemeral 证书 | 无 fixture 文件、无 CI 环境依赖 |
| 测试客户端 TLS 校验 | `verify_peer = false` | 测试两端都自己控制，pin / CA 校验不在本测试覆盖范围；简化 fixture |

---

## 架构设计

### 修改前后调用流程对比

**修改前**（buggy，Transport / DirectTxTransport 路径）：
```
TransportCore::do_connect()
  ├─ TCP connect
  ├─ TLS handshake
  ├─ extract_hot_state()           ← snapshot here, seq=(0, 0/?)
  └─ TlsRecordCrypto::create()     ← hot crypto frozen at stale seq
TransportCore::do_ws_upgrade<WsFramer>()
  ├─ tls->handshake_write(GET)     ← SSL write_seq → 1
  └─ tls->handshake_read(101)      ← SSL read_seq → 1+
[hot path begins]
  └─ first encrypt uses nonce(seq=0), server expects nonce(seq=1) → bad_record_mac
```

**修改后**（fixed，统一所有 3 个 transport 变体）：
```
TransportCore::do_connect()
  ├─ TCP connect
  └─ TLS handshake (no hot crypto extract)
TransportCore::do_ws_upgrade<WsFramer>()
  ├─ tls->handshake_write(GET)     ← SSL write_seq → 1
  └─ tls->handshake_read(101)      ← SSL read_seq → 1+
TransportCore::arm_aead_crypto()   ← snapshot here, seq matches SSL state
  ├─ extract_hot_state()
  └─ TlsRecordCrypto::create()
[hot path begins]
  └─ first encrypt uses nonce matching SSL's next-record seq → OK
```

### 修改的文件

| 文件 | 修改类型 | 说明 |
|---|---|---|
| `eph-transport/include/eph/transport/detail/transport_core.hpp` | 抽离 + 新增 | 从 `do_connect` 删除 line 155-169；新增 `arm_aead_crypto()` 方法 |
| `eph-transport/include/eph/transport/transport.hpp` | 调用顺序 | `Transport::create` 在 line 207 后插入 `arm_aead_crypto()`；reconnect 路径（line ~1248）同样 |
| `eph-transport/include/eph/transport/direct_tx_transport.hpp` | 调用顺序 | `DirectTxTransport::create` 在 line 131 后插入 `arm_aead_crypto()`；reconnect 路径（line ~733）同样 |
| `eph-transport/include/eph/transport/direct_transport.hpp` | 重构（统一接口） | `do_connect_()` line 988-1011 改为调用 `core_.arm_aead_crypto()`，删除 inline 实现 |
| `eph-transport/include/eph/transport/detail/tls_session.hpp` | 注释 | line 575 自相矛盾注释修正；`extract_hot_state` 加 doxygen pre-condition |
| `tests/support/tls_ws_echo_server.hpp` | 新增 fixture | header-only in-process TLS+WS echo server，~200 行 |
| `tests/integration/test_transport_tls_ws_e2e.cpp` | 新增 测试 | 3 个 transport 变体 × 2 个场景（round-trip + reconnect）的 typed test，~150 行 |
| `tests/integration/xmake.lua` | 新增 target | 注册 `test_transport_tls_ws_e2e` 测试 target |

---

## 接口设计

### 新 API: `TransportCore::arm_aead_crypto()`

```cpp
/// Snapshot the TLS application traffic state from the SSL session and
/// initialize the hot-path AEAD crypto context (TlsRecordCrypto).
///
/// **Pre-condition**: this method MUST be called after **all**
/// SSL_write/SSL_read application-data operations on this session are
/// complete (e.g., after the WebSocket HTTP Upgrade exchange). After this
/// call, no further SSL_write/SSL_read may occur on this session — all
/// subsequent application data must go through the returned crypto
/// context. Violating this pre-condition will cause TLS sequence number
/// desynchronization and immediate `bad_record_mac` on the first
/// hot-path record.
///
/// **No-op if** `config.use_tls == false` (returns success).
///
/// **Idempotency**: not idempotent. Calling twice will reset the hot-path
/// seq counter to whatever SSL_get_*_sequence currently reports, which
/// may itself be wrong if SSL_write/SSL_read happened between calls.
/// Reconnect paths must call this exactly once after each successful
/// handshake + WS upgrade cycle.
///
/// @return success on hot crypto initialized, ConnectionErrorInfo on
///         key extraction or AEAD context construction failure.
[[nodiscard]] std::expected<void, ConnectionErrorInfo>
arm_aead_crypto() noexcept;
```

**字段写入**：
- `crypto = std::make_unique<TlsRecordCrypto>(...)` —— 新建
- 不修改 `tls_version` / `cipher_name` / `last_handshake_ns` —— 这些字段保留在 `do_connect` 中由 TLS handshake 完成时立即写入（它们不依赖 hot-state，TLS 握手完成后就稳定）

**错误码**：
- `ConnectionError::kTlsKeyExportFailed`（沿用现有错误码，不引入新枚举）

### 修改的注释: `TlsSession::extract_hot_state`

`tls_session.hpp` 当前 line 575 注释：
```
"...Reads current seq numbers from SSL so TlsRecordCrypto stays in sync
after any SSL_write/SSL_read usage."
```

改为：
```
"...Reads current seq numbers from SSL. **Caller must guarantee that no
further SSL_write/SSL_read will occur on this session after this call.**
This is a one-shot snapshot of the application traffic state — once the
returned TlsRecordCrypto is in use, the SSL session's application
sequence counters and the hot-path counters will diverge if any SSL
operation runs in parallel."
```

### 测试 fixture: `tls_ws_echo_server.hpp`

```cpp
namespace eph::test {

/// In-process TLS 1.3 + WebSocket echo server for transport e2e tests.
/// Spawns a single accept thread, runs aws-lc TLS handshake, parses the
/// HTTP Upgrade request, sends 101 response, then echoes WebSocket frames
/// back until the client closes.
///
/// Uses an ephemeral self-signed certificate generated in-memory at
/// construction. Test clients should set `verify_peer = false`.
///
/// Lifecycle: construct → start() → port() returns bound port → stop()
/// in destructor.
class TlsWsEchoServer {
public:
    TlsWsEchoServer();   // generates cert + key, binds 127.0.0.1:0
    ~TlsWsEchoServer();  // stops accept thread, closes listener

    void start();              // launches accept thread
    void stop();               // signals stop, joins thread
    uint16_t port() const noexcept;
    bool send_close_to_clients();  // for reconnect tests: kill all sessions

private:
    // ephemeral cert + SSL_CTX setup
    // listener socket
    // accept thread + per-connection handler
};

} // namespace eph::test
```

### 测试结构: `test_transport_tls_ws_e2e.cpp`

```cpp
template <typename TransportT>
class TransportTlsWsTest : public ::testing::Test {
protected:
    void SetUp() override { server_.start(); }
    void TearDown() override { server_.stop(); }
    eph::test::TlsWsEchoServer server_;
};

using TransportTypes = ::testing::Types<
    eph::net::DefaultTransport<eph::net::SocketTransport>,
    eph::net::DirectTxDefaultTransport<eph::net::SocketTransport>,
    eph::net::DirectDefaultTransport<eph::net::SocketTransport>
>;
TYPED_TEST_SUITE(TransportTlsWsTest, TransportTypes);

TYPED_TEST(TransportTlsWsTest, RoundTripSendReceive) {
    // 1. build TcpFactory targeting 127.0.0.1:server.port()
    // 2. TransportConfig{ use_tls=true, verify_peer=false, ws_path="/" }
    // 3. TransportT::create(...)
    // 4. send_text("hello")
    // 5. recv() with timeout, expect "hello"
}

TYPED_TEST(TransportTlsWsTest, ReconnectAfterServerClose) {
    // 1. connect + send/recv "hello1" + verify
    // 2. server.send_close_to_clients()
    // 3. wait for client to detect disconnect + reconnect (poll state)
    // 4. send/recv "hello2" + verify
    // (this test exercises the 4 reconnect callsites of arm_aead_crypto)
}
```

---

## 编码规范

跟随 `CLAUDE.md` 和现有 codebase 约定，本次新增/修改部分的特定约定：

| 维度 | 规范 |
|---|---|
| 命名 | 新 helper 命名 `arm_aead_crypto`（snake_case，与 `do_connect` / `do_ws_upgrade` 对齐） |
| 错误处理 | `arm_aead_crypto` 返回 `std::expected<void, ConnectionErrorInfo>`，沿用 `ConnectionError::kTlsKeyExportFailed` |
| 日志 | `arm_aead_crypto` 在成功时 emit `SPDLOG_LOGGER_DEBUG`（带 cipher、key_len、seq 值）；失败 `SPDLOG_LOGGER_ERROR`。沿用 `detail::transport_logger()` |
| 注释 | 在 `arm_aead_crypto` 头部 doxygen 块内**显式写出 pre-condition**，包含违反后果（"immediate bad_record_mac"），让未来的 reader 一眼看出契约 |
| 测试命名 | typed test：`TransportTlsWsTest_RoundTripSendReceive`、`_ReconnectAfterServerClose` |
| Test fixture 头文件 | header-only，命名空间 `eph::test`，无可变全局状态，每个测试独立持有 server 实例 |

---

## 实施计划

> **Commit 策略**：单 PR，3 个 commit。每个 commit 是独立回滚点。本地按 tests-first 验证（手动 cherry-pick fix 看从红变绿），但提交顺序按 fix → tests → doc 保证 CI 历史绿。

### 阶段 0: 现场症状记录（pre-fix 验证）

- **交付物**：bug 指纹文档（保存为本 plan 文件的 follow-up note 或 commit message 的一部分）
- **动作**：
  1. `xmake -m debug` 构建 simple_hft（debug 模式，SPDLOG_LEVEL_TRACE）
  2. 准备 testnet credentials 或临时改 host 到 `echo.websocket.org`（绕过 Binance 鉴权要求）
  3. `xmake run simple_hft` 跑 ~30 秒，捕获完整日志
  4. 记录症状：TLS handshake 完成 → WS upgrade 完成 → 第一次 send/recv 后多少 ns 内出现什么 error
- **预期症状**：log 显示 "TLS handshake in X.Xms" → "WS upgrade in X.Xms" → 然后客户端 RX 看到 TLS decrypt error 或 TCP read 0 / RST，整体进入 reconnect 死循环
- **验收标准**：症状被记录到 `.artifacts/discuss-20260409-173817.md` 的 follow-up section 或新建 `.artifacts/fix-tls-ordering-symptoms-20260409.txt`，作为后续 commit message 引用证据
- **预估**：~5 分钟

### 阶段 1: Hotfix（fix-only commit）

- **交付物**：4 个 callsite 修复 + helper 抽离 + DirectTransport 重构 + 注释修正
- **动作**：
  1. 修改 `transport_core.hpp`：删除 `do_connect` 中 line 155-169；新增 `arm_aead_crypto()` public 方法（实现就是搬过来的 5 行）
  2. 修改 `transport.hpp`：在 `Transport::create` 的 line 207 之后插入 `t->core_.arm_aead_crypto()` 调用（含错误处理）；reconnect 路径 line ~1248 同样
  3. 修改 `direct_tx_transport.hpp`：`DirectTxTransport::create` line 131 之后 + reconnect 路径 line ~733 同样
  4. 修改 `direct_transport.hpp`：`do_connect_()` line 988-1011 删除 inline 实现，改为 `core_.arm_aead_crypto()`
  5. 修改 `tls_session.hpp`：line 575 注释修正 + `extract_hot_state` doxygen 加 pre-condition 块
  6. **本地验证（暂不 commit）**：手动跑 `xmake run simple_hft`，确认症状消失（连接稳定、消息正常收发）
- **验收标准**：
  - 编译通过：`xmake build -g tests` 全绿
  - 现有测试不退化：`xmake run test_transport`、`xmake run test_tls_record`、`xmake run test_websocket`、`xmake run test_transport_e2e` 全绿
  - simple_hft / binance_book 在 testnet 跑通（或至少看到第一条 application record 被成功收/发）
- **推荐 skill**：手工 Edit（变更范围明确、无设计自由度）
- **预估**：~30 分钟

### 阶段 2: E2E 回归测试（tests commit）

- **交付物**：
  - `tests/support/tls_ws_echo_server.hpp` —— in-process TLS+WS echo server fixture
  - `tests/integration/test_transport_tls_ws_e2e.cpp` —— typed test，3 变体 × 2 场景
  - `tests/integration/xmake.lua` —— 注册新 target
- **动作**：
  1. 写 `TlsWsEchoServer`：
     - 构造函数：用 aws-lc `EVP_PKEY_keygen` + `X509_*` 生成 ECDSA P-256 自签证书；构造 `SSL_CTX` 配置 TLS 1.3；socket bind 127.0.0.1:0 + listen
     - `start()`：spawn accept thread；每个连接：`SSL_accept` → 读 HTTP Upgrade 请求 → 计算 `Sec-WebSocket-Accept` (SHA-1 + base64) → 写 101 response → 进入 echo loop（unmask client → echo 回去）
     - `stop()`：原子标志 + close listener fd + join thread
     - `send_close_to_clients()`：遍历活跃 session，每个 SSL_shutdown + close（用于 reconnect 测试）
  2. 写 `test_transport_tls_ws_e2e.cpp`：
     - typed test fixture 持有 server
     - `RoundTripSendReceive`：3 变体各跑一遍 send "hello" → recv "hello"
     - `ReconnectAfterServerClose`：连接 → 发收 → server kill clients → 等 reconnect → 再发收
  3. 注册到 `tests/integration/xmake.lua`
  4. **预 fix 验证（在 stash 阶段 1 修改的状态下）**：跑新测试，期望红 —— 证明测试能捕获 bug
  5. **post fix 验证**：恢复阶段 1 修改，跑新测试，期望全绿
- **验收标准**：
  - 6 个 typed test instance 全部通过（3 transport × 2 scenario）
  - 测试运行时间 < 5 秒
  - `xmake run test_transport_tls_ws_e2e` 单独可运行
  - 在阶段 1 的 fix 被回退的前提下，至少 4 个 instance 失败（Transport 和 DirectTxTransport 的两个场景），证明回归保护到位
- **推荐 skill**：手工 Write（fixture 和 typed test 是新写的），可参考 `eph-transport/tests/test_tls_record.cpp` 的 aws-lc 用法风格
- **预估**：~2 小时（fixture 200 行 + test 150 行 + 调试）

### 阶段 3: 文档与契约（doc commit）

- **交付物**：契约文档更新 + plan 状态翻为已完成
- **动作**：
  1. （已在阶段 1 中完成）`tls_session.hpp` 的 `extract_hot_state` doxygen pre-condition —— 但作为单独 commit 时，将文档相关变更从阶段 1 切出来
  2. 更新 `.artifacts/plan-tls-aead-ordering-fix-20260409-175353.md` 的 `状态：` 字段为 `已完成`
  3. 在 `.artifacts/INDEX.md` 追加 fix 相关行
- **验收标准**：plan 文件状态字段更新；INDEX.md 更新
- **推荐 skill**：手工 Edit
- **预估**：~5 分钟

### 阶段 4: 最终确认与 PR

- **动作**：
  1. `git log --oneline -5` 确认 3 commit 拓扑（fix → tests → doc）
  2. 手动再跑一次 simple_hft 在 testnet 上，确认 fix 持久有效（不只是 unit test 绿）
  3. 调用 `/git` 或手工创建 PR，title `fix(transport): defer hot-path AEAD crypto init until after WS upgrade`
  4. PR description 引用 `.artifacts/discuss-20260409-173817.md` 和本 plan，并贴阶段 0 记录的 bug 指纹日志
- **验收标准**：PR 已创建，CI 全绿，等待 review
- **预估**：~10 分钟

---

## 关键决策记录

### D-1: 新 helper 命名为 `arm_aead_crypto`
- **问题**：抽离的 hot-state 提取 helper 应该叫什么
- **选项**：A) `arm_aead_crypto`  B) `finalize_crypto`  C) `take_over_application_data`  D) `snapshot_aead_state`
- **决策**：**A**
- **理由**：简短、准确、表达"装填弹药 / 装好之后才能开火"的语义；和 codebase 现有 `do_connect` / `do_ws_upgrade` snake_case 风格一致；避免 "finalize" 在 RAII 语境下的歧义
- **验收标准**：方法名 = `arm_aead_crypto`；类内 public 成员；返回类型 `std::expected<void, ConnectionErrorInfo>`

### D-2: helper 归属 TransportCore，不抽出自由函数
- **问题**：helper 应该是 TransportCore 成员还是自由函数
- **选项**：A) `TransportCore` 成员  B) 自由函数 `arm_aead_crypto(TransportCore<TcpImpl>&)`  C) 把更多握手后初始化打包成 `do_post_handshake_setup`
- **决策**：**A**
- **理由**：和 `do_connect` / `do_ws_upgrade` 风格一致；避免破坏 TransportCore 封装；C 把无关动作打包反而难维护
- **验收标准**：`transport_core.hpp` 中 `TransportCore` 类内新增 public 方法

### D-3: `extract_hot_state` 仅加 doxygen pre-condition，不改名
- **问题**：是否把 `extract_hot_state` 改名为 `snapshot_hot_state` 强调一次性语义
- **选项**：A) 只加 doxygen pre-condition，不改名  B) 加 pre-condition + 改名  C) 不动
- **决策**：**A**
- **理由**：改名是噪音变更（grep 噪音、git 历史污染、所有调用方都要改）；契约由 doxygen pre-condition 锁住即可。同时修正 line 575 的自相矛盾注释
- **验收标准**：`tls_session.hpp:579` 加 pre-condition 块，含违反后果说明（"immediate bad_record_mac"）

### D-4: TLS 元信息字段（tls_version / cipher_name / last_handshake_ns）保留在 do_connect
- **问题**：这些字段是搬到 arm_aead_crypto 还是留在 do_connect
- **选项**：A) 留在 do_connect（TLS 握手完成后立刻填）  B) 搬到 arm_aead_crypto
- **决策**：**A**
- **理由**：这些是元信息字段，TLS 握手一完成就稳定，没必要等到 ws upgrade 之后才填；修复粒度更小、对现有调用方影响最少

### D-5: 测试 server 用进程内同进程线程 + aws-lc 直接 accept
- **问题**：怎么搭建测试用的 TLS+WS server
- **选项**：A) 进程内线程 + aws-lc  B) `mock_binance_server.py` 加 TLS 包装  C) `openssl s_server` 子进程  D) 复用 HttpClient + 自己写 SSL_CTX server
- **决策**：**A**
- **理由**：零外部依赖，符合 codebase header-only + zero-dep 风格；fixture 可复用给未来的集成测试；< 1 秒 run time
- **验收标准**：`tests/support/tls_ws_echo_server.hpp` 头文件存在，无任何外部进程依赖、无 fixture 文件依赖

### D-6: 测试证书 in-memory 生成，不 check-in fixture
- **问题**：自签证书怎么处理
- **选项**：A) 测试启动时 in-memory 生成 ephemeral 证书  B) check-in 测试证书  C) fork openssl req 子进程
- **决策**：**A**
- **理由**：完全 in-memory，无 fixture 文件、无环境依赖；ECDSA P-256 + ephemeral private key，每个测试 run 独立
- **验收标准**：`TlsWsEchoServer` 构造函数生成证书 + 私钥；客户端用 `verify_peer = false` 跳过校验

### D-7: 测试覆盖 3 个 transport 变体 + 2 个场景
- **问题**：测试矩阵的范围
- **选项**：A) 3 变体 × 2 场景  B) 2 变体 × 1 场景（最小修复保护）  C) 1 变体 × 1 场景
- **决策**：**A**
- **理由**：3 变体共享 server 代码、共享证书代码，多跑测试只是几行 typed test 模板；DirectTransport 当前虽然没 bug，但加测试可以防止将来回归；reconnect 路径在讨论中明确是同一类 bug，必须有回归保护
- **验收标准**：`TransportTypes` typedef 包含 3 个变体；2 个 `TYPED_TEST` 实例（RoundTrip + Reconnect）

### D-8: 测试归属 `tests/integration/`
- **问题**：测试文件放哪
- **选项**：A) `tests/integration/test_transport_tls_ws_e2e.cpp`  B) `eph-transport/tests/`  C) `eph-net/tests/`
- **决策**：**A**
- **理由**：跨 eph-transport + eph-net + aws-lc 的真集成测试，标准位置就是 `tests/integration/`；和 `test_transport_e2e.cpp` 并列，互补地补上 "TLS+WS" 这条它显式排除的路径

### D-9: 修复和测试合一个 PR，3 commit
- **问题**：commit / PR 拓扑
- **选项**：A) 同 PR 3 commit（fix / tests / doc）  B) 修复先 ship，测试后续 PR  C) 单 commit
- **决策**：**A**
- **理由**：测试是必备不是 nice-to-have（讨论中 R13 强论点）；同 PR 保证修复和回归保护一起合入，避免 "修了但忘了加测试" 的情况；3 commit 拆开是因为它们是逻辑独立的回滚点；本地按 tests-first 验证保留 TDD 证据链

### D-10: Hotfix 之前手跑 simple_hft 记录症状
- **问题**：是否在合 fix 之前先记录现场症状
- **选项**：A) 跑（成本 ~30 秒）  B) 不跑直接修
- **决策**：**A**
- **理由**：R11 在讨论中保留的"实证"主张；修复后再跑一次确认症状消失，比口头宣称 "fix 生效" 更有说服力；symptoms 写进 commit message / PR description 形成证据链

---

## Phase 2 一致性检查

| 检查项 | 结果 |
|---|---|
| 决策之间是否矛盾 | 通过 — `arm_aead_crypto` 是同步阻塞 API，与 codebase 全部同步阻塞的 transport API 一致 |
| 修复路径是否覆盖所有受影响 callsite | 通过 — 4 个 callsite（Transport::create + reconnect、DirectTxTransport::create + reconnect）全部列出，且 DirectTransport 也重构以统一接口防回归 |
| 测试是否覆盖所有修改的代码路径 | 通过 — RoundTripSendReceive 覆盖 4 个 callsite 中的 2 个（Transport::create、DirectTxTransport::create），ReconnectAfterServerClose 覆盖另外 2 个（reconnect 路径） |
| 测试是否能够在没有 fix 的情况下捕获 bug | 通过 — 阶段 2 验收标准明确要求：在 fix 被回退的前提下，至少 4 个 instance 失败 |
| 命名约定是否一致 | 通过 — `arm_aead_crypto`、`TlsWsEchoServer`、`TransportTlsWsTest` 都符合 codebase 命名风格 |
| 错误码体系是否一致 | 通过 — 沿用 `ConnectionError::kTlsKeyExportFailed`，无新增枚举 |
| Doxygen 契约是否覆盖隐式假设 | 通过 — `arm_aead_crypto` 和 `extract_hot_state` 都明确写出 pre-condition + 违反后果 |
| 实施计划是否可独立回滚 | 通过 — 3 commit 各自是独立回滚点，且阶段顺序保证 CI 历史绿 |

---

## plan.md 完成度检查

- ✅ 定位与边界：已确定（修复 4 个 callsite + 加测试 + 加文档 + 不动 example smoke test 等 follow-up）
- ✅ 技术选型：已确定（沿用现有，测试 server 用 aws-lc 进程内方案）
- ✅ 架构设计：已确定（修改前后调用流程对比清晰，9 个修改文件全部列出）
- ✅ 接口设计：已确定（`arm_aead_crypto` 完整签名 + doxygen 契约 + fixture API + typed test 框架）
- ✅ 编码规范：已确定（命名 / 错误处理 / 日志 / 注释 全部对齐 codebase）
- ✅ 实施计划：已确定（5 阶段：症状记录 → fix → tests → doc → PR）
- ✅ 关键决策记录：已确定（10 个决策，每个有理由 + 验收标准）
- ✅ 一致性检查：通过

---

## 跨会话上下文（恢复指引）

如果实施分多个会话，下次启动时：
1. 读本 plan
2. 检查 `.artifacts/INDEX.md` 看是否有 fix-* 报告记录已完成的阶段
3. 跑 `git log --oneline -10` 看是否已经有 `fix(transport): defer hot-path AEAD...` 相关 commit
4. 跑 `xmake run test_transport_tls_ws_e2e` 看测试是否已存在并通过

恢复点：
- 0 commit：从阶段 0 开始
- 1 commit (fix only)：从阶段 2 开始（缺测试）
- 2 commits (fix + tests)：从阶段 3 开始（缺文档/状态更新）
- 3 commits：plan 已完成，更新本文件 `状态：` 为 `已完成`

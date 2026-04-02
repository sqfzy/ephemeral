# Code Review Report — eph-dpdk & eph-net

## 元信息
- 时间：2026-04-02 06:19:16
- 耗时：~6 min
- Diff 来源：branch `origin/main...dev` (scoped to `eph-dpdk/`, `eph-net/`)
- 审查范围：34 files, +9253 / -3275 lines, ~160 commits
- 审查维度：all
- 构建状态：✅ 通过 (`xmake build` clean)
- 测试状态：❌ 全部链接失败 (GCC 11.5 ABI mismatch — `_M_replace_cold` / `__cxa_call_terminate` undefined)

---

## Review 摘要

### 变更概况
- 文件数：34 (15 eph-dpdk, 19 eph-net)
- 增删：+9253 / -3275
- 主要变更：大幅扩展 DPDK 网络栈（DNS resolver, multicast RX, flow steering, Reactor）和 eph-net 模块（HTTP client, proxy, circuit breaker, kill switch, gateway, HMAC, socket transport）

### 总体评价
两个模块功能覆盖广泛，代码风格一致，可观测性（spdlog 日志）覆盖充分。主要风险集中在三个方面：(1) Reactor 的 `mark_reconnected` 存在真实的数据竞争，(2) KillSwitch 析构后信号处理器悬垂指针，(3) 测试全部无法链接，等同于零回归保护。性能敏感路径的设计（零拷贝分发、TSC 传播、线性扫描 hash pre-filter）合理。

### 问题统计
- 🔴 Critical：5
- 🟡 Major：8
- 🔵 Minor：6
- 💬 Nit：2

### 结论
**REQUEST_CHANGES** — 存在 Critical 正确性/安全问题，且测试无法运行。

---

## 🔴 Critical

### [C0a] Reactor `hash_tuple` 方向不对称 — hash pre-filter 完全失效

**文件**：`eph-dpdk/include/eph/dpdk/reactor.hpp:86-93`
**类型**：正确性
**描述**：`hash_tuple` 使用 FNV-1a 按 `mix(src_ip) → mix(dst_ip) → mix(src_port<<16|dst_port)` 顺序哈希。注册的 tuple 为 `{local→server}`，而入站包为 `{server→local}`（src/dst 互换）。FNV-1a 是顺序敏感的，导致 `hashes_[j] != pkt_hash` **始终为 true**——hash pre-filter 拒绝所有合法包，Reactor dispatch 完全失效。
**建议**：改为 XOR + sum 对称哈希，确保 hash(A→B) == hash(B→A)。

---

### [C0b] `transport_frame.hpp` kDirect 模式编译错误

**文件**：`eph-transport/include/eph/transport/detail/transport_frame.hpp:140,243,380,579,617,642`
**类型**：正确性
**描述**：`rx_enqueue()` 有 `requires (kHasRxQueue)` 约束，`tx_queue_` 在 kDirect 模式下为 `detail::Empty` 类型。transport_frame.hpp 中 6 处调用未被 `if constexpr` 保护：4 处 `rx_enqueue`（close frame 投递、fast-path data enqueue、deliver_message fallback）和 2 处 `tx_queue_.try_produce`（handle_ping pong 回复、handle_close 回复）。kDirect 模板实例化时编译失败。
**建议**：所有 6 处用 `if constexpr (kHasRxQueue)` / `if constexpr (kHasTxQueue)` 保护；kDirect 模式下 close/pong 回复改用 `send_direct()`。

---

### [C1] Reactor `mark_reconnected` 数据竞争 — 可导致 use-after-free

**文件**：`eph-dpdk/include/eph/dpdk/reactor.hpp:190-210`
**类型**：正确性 / 并发安全
**描述**：`mark_reconnected` 的同步协议依赖 seq_cst fence 确保 RX 线程在 session 指针被修改前"已观察到 connected=false"。但这在 C++ 内存模型下不成立：如果 RX 线程（line 284）在 writer 执行 `store(false, release)` **之前**已通过 acquire-load 看到 `connected=true`，它将继续解引用 session（line 291-303），而 writer 同时在 line 203 修改 session 指针。seq_cst fence 仅在与另一个 seq_cst fence/operation 配对时提供全序保证，与 RX 线程的 acquire load 不构成同步关系。

实际后果：RX 线程调用 `process_rx` 时 session 指针可能已被替换为新地址，旧 session 可能已析构 → use-after-free。

**建议**：
1. 在 RX loop 的 dispatch 路径加读锁（`shared_mutex` 或 seqlock），或
2. 使用 RCU-like 方案：mark_reconnected 设置新 session 为 pending，在下一次 burst_complete 回调中原子交换，或
3. 最简方案：在 mark_reconnected 中 `std::atomic_thread_fence(seq_cst)` 后加一个 busy-wait 等待 RX 线程完成当前 iteration（需要 RX 线程维护一个 per-iteration epoch counter）

---

### [C2] KillSwitch 析构后信号处理器悬垂指针

**文件**：`eph-net/include/eph/net/kill_switch.hpp:75, 164-169, 226-236`
**类型**：安全性 / 正确性
**描述**：`~KillSwitch()` 调用 `shutdown()` 但**不**将 `s_instance_` 重置为 nullptr，也不恢复默认信号处理器。析构后若收到 SIGINT/SIGTERM，signal_handler (line 228) 会 `s_instance_.load(acquire)` 得到悬垂指针，然后调用 `request_shutdown()` 写入已释放内存 → UB。

**建议**：在 `shutdown()` 末尾或析构函数中：
```cpp
std::signal(SIGINT, SIG_DFL);
std::signal(SIGTERM, SIG_DFL);
s_instance_.store(nullptr, std::memory_order_release);
```

---

### [C3] 测试全部链接失败 — 零回归保护

**文件**：所有 `tests/dpdk/` 和 `tests/net/` 目标
**类型**：测试
**描述**：GCC 11.5 与项目所需的 libstdc++ ABI 不兼容（`_M_replace_cold` 是 GCC 13+ 的符号，`__cxa_call_terminate` 需要更新的 libcxxabi）。所有 22 个测试目标均链接失败。`test_transport` 还有编译错误（`ConnectionErrorInfo` 类型不匹配）。在此状态下合入意味着对 ~160 个 commit 的 9000+ 行变更没有任何自动化验证。

**建议**：
1. 确认 CI 环境 GCC 版本 ≥ 13 并在 CI 上验证通过
2. 修复 `test_transport.cpp:278` 的编译错误
3. 在 xmake.lua 中添加最低编译器版本检查

---

## 🟡 Major

### [M1] HTTP `extra_headers` 参数未验证 CRLF 注入

**文件**：`eph-net/include/eph/net/http_client.hpp:135-138`
**类型**：安全性
**描述**：`build_http_request()` 直接 `req.append(extra_headers)` 无任何校验。若调用方传入包含 `\r\n\r\n` 的字符串，可注入任意 HTTP body 或额外请求（HTTP request smuggling）。虽然 API 文档说"each terminated with \r\n"，但没有代码保证。

**建议**：验证 `extra_headers` 不包含连续的 `\r\n\r\n`，或改为接受 `std::span<std::pair<string_view, string_view>>` 的 header map。

---

### [M2] `SSL_set_fd()` 返回值未检查

**文件**：`eph-net/include/eph/net/http_client.hpp:537`
**类型**：正确性
**描述**：`SSL_set_fd(ssl, fd)` 可能失败（返回 0），但代码直接进入 handshake 循环。若 fd 无效或 BIO 创建失败，SSL_connect 会产生难以诊断的错误。

**建议**：
```cpp
if (!SSL_set_fd(ssl, fd)) {
    return std::unexpected(std::format("SSL_set_fd failed: {}", detail::ssl_last_error()));
}
```

---

### [M3] Gateway `check_health` 持锁回调用户代码 — 潜在死锁

**文件**：`eph-net/include/eph/net/gateway.hpp:241-263`
**类型**：设计 / 正确性
**描述**：`check_health()` 持 `mu_` 期间调用 `is_running_fn(transport_ptr)` 和 `on_health_change` 回调。若回调中再调用 Gateway 的任何方法（如 `unregister_connection`），会死锁（非递归 mutex）。

**建议**：先在锁内收集状态快照，释放锁后再调用回调：
```cpp
std::vector<std::tuple<std::string, ConnHealth, ConnHealth>> changes;
{
    std::lock_guard lock(mu_);
    // ... detect changes, push to `changes` ...
}
for (auto& [tag, old_h, new_h] : changes) {
    config_.on_health_change(tag, old_h, new_h);
}
```

---

### [M4] DNS resolver 缺少 rdlength 上限验证

**文件**：`eph-dpdk/include/eph/dpdk/dns.hpp:340-342`
**类型**：安全性
**描述**：`parse_dns_response` 校验 `offset + rdlength > dns_len`（正确），但未对 rdlength 本身设上限。恶意 DNS 响应可声明 rdlength=65535（uint16_t 最大值），导致后续 `offset += rdlength` 跳过大量数据。虽然循环会正常终止（an_count 有限），但配合精心构造的包可引导解析器跳到伪造的 A 记录。

**建议**：增加 `if (rdlength > kMaxDnsPacketLen) return std::unexpected("DNS: oversized RDATA");`

---

### [M5] HTTP Content-Length 缺失时依赖连接关闭 — 可能无限缓冲

**文件**：`eph-net/include/eph/net/http_client.hpp:772-774`
**类型**：正确性
**描述**：`is_response_complete()` 在无 Content-Length 时返回 false，recv 循环持续读取直到超时或连接关闭。对于 Transfer-Encoding: chunked 的响应（crypto exchange API 常见），解析器永远不会返回 complete，只能等超时。256 MiB 上限仅在有 Content-Length 时检查。

**建议**：检测 `Transfer-Encoding: chunked` 并解析 chunked 编码的结束标记（`0\r\n\r\n`），或在无 Content-Length 时也对总缓冲量设上限。

---

### [M6] Multicast UDP 解析缺少 IHL 最小值验证

**文件**：`eph-dpdk/include/eph/dpdk/multicast.hpp:176-180`
**类型**：正确性
**描述**：代码检查 `(ip->version_ihl >> 4) != 4` 验证 IPv4 版本，但 IHL 字段（低 4 位）仅隐式通过后续 `< kIpv4HeaderLen` 检查。IHL=1 会计算为 4 字节（< 20），被拒绝，这是安全的。但 IHL 值 5-15 中某些值与选项字段的解析交互可能导致 `udp` 指针偏移到无效位置。

**建议**：显式检查：`uint8_t ihl = (ip->version_ihl & 0x0F) << 2; if (ihl < kIpv4HeaderLen || kEtherHeaderLen + ihl + kUdpHeaderLen > pkt_len) return {};`

---

### [M7] `net_header.hpp parse_packet` 对恶意 ip_total 防御充分，但缺少注释

**文件**：`eph-dpdk/include/eph/dpdk/net_header.hpp:517-534`
**类型**：设计
**描述**：`parse_packet` 有三层防御：(1) `data_start > ip_total` 检查拒绝 TCP doff 过大，(2) `kEtherHeaderLen + ip_total > pkt_len` 拒绝 IP 声明超出 mbuf，(3) `ip_total > data_start` guard 确保 payload_len 不下溢。这些检查正确且完整，但缺少注释说明为何这三个条件足以阻止所有整数溢出场景。

**建议**：为三个 guard 各加一行 `// Prevents: ...` 注释，解释每个检查阻止的具体攻击向量。

---

### [M8] Shell scripts 缺少 cleanup trap

**文件**：`eph-dpdk/dpdk_setup.sh`, `eph-dpdk/dpdk_teardown.sh`
**类型**：设计 / 可靠性
**描述**：`dpdk_setup.sh` 若在 hugepages 分配后、NIC 绑定前失败退出，系统留在不一致状态。`dpdk_teardown.sh` 若某步失败仍报"teardown complete"。两个脚本均未使用 `trap ... EXIT` 进行清理。

**建议**：在两个脚本中加入 `trap cleanup EXIT` 模式，在异常退出时回滚已完成的步骤（至少释放 hugepages、解绑 NIC）。

---

## 🔵 Minor

### [m1] `SocketTransport` move 操作缺少 `noexcept` 声明

**文件**：`eph-net/include/eph/net/socket_transport.hpp:99-113`
**类型**：设计
**描述**：Move constructor/assignment 未标记 `noexcept`，阻止 `std::vector<SocketTransport>` 使用 move 优化。
**建议**：添加 `noexcept`。

---

### [m2] CircuitBreaker `state()` 与 `allow()` 可返回不一致状态

**文件**：`eph-net/include/eph/net/circuit_breaker.hpp:170-176`
**类型**：设计
**描述**：`state()` 检查超时并可能报告 HalfOpen，但不执行状态迁移（留给 `allow()`）。调用方可能先 `state()` 看到 HalfOpen，随后 `allow()` 看到 Open（因为另一个线程已重置）。已有文档但易误用。
**建议**：在 API 文档中明确说明 `state()` 是快照，不驱动状态迁移。

---

### [m3] `ProxyConfig::validate()` 未检查 hostname 中的控制字符

**文件**：`eph-net/include/eph/net/socket_config.hpp:204-234`
**类型**：输入验证
**描述**：`validate()` 仅检查 host 非空，但 `from_url()` 已对控制字符做了检查。直接构造的 `SocketConfig` 可绕过。
**建议**：在 `validate()` 中增加控制字符检查，或文档说明需通过 `from_url()` 构造。

---

### [m4] ARP 分配失败日志缺少 mempool 状态

**文件**：`eph-dpdk/include/eph/dpdk/arp.hpp:117-121`
**类型**：可观测性
**描述**：`rte_pktmbuf_append` 失败时仅日志 frame_len，未包含 mempool 剩余数量，难以区分"包太大"与"池耗尽"。
**建议**：`SPDLOG_LOGGER_ERROR(log, "... frame_len={}, pool_avail={}", frame_len, rte_mempool_avail_count(pool));`

---

### [m5] DNS 重试日志缺少当前重试次数和剩余超时

**文件**：`eph-dpdk/include/eph/dpdk/dns.hpp:586-589`
**类型**：可观测性
**描述**：DNS 查询发送日志包含 hostname 和 tx_id，但缺少 retry 序号和距 deadline 的剩余时间。
**建议**：添加 `requests_sent` 和 `(deadline - now).count()` 到日志。

---

### [m6] Gateway `dump()` 未包含 monitor 线程状态

**文件**：`eph-net/include/eph/net/gateway.hpp:265-277`
**类型**：可观测性
**描述**：`dump()` 输出所有连接信息但未包含健康检查 monitor 是否在运行。
**建议**：添加 `Monitor: running/stopped` 行。

---

## 💬 Nit

### [n1] `RateLimiter` 使用 `double` 做 token 计量

**文件**：`eph-net/include/eph/net/rate_limiter.hpp:51-60`
**类型**：精度
**描述**：对于 crypto exchange API 速率限制（通常 < 1200 req/min），`double` 精度足够。但极端场景（百万级 tokens/sec + 长时间运行）可能出现浮点累积误差。
**建议**：当前场景无需修改，若未来扩展到高频场景可考虑定点数。

---

### [n2] `tcp.hpp` 的 `!seq_after(peer_ack, snd_nxt_)` 语义等价但可读性弱

**文件**：`eph-dpdk/include/eph/dpdk/tcp.hpp:657-658`
**类型**：可读性
**描述**：`!seq_after(a, b)` 等价于 `seq_before_or_equal(a, b)`（均为 `(int32_t)(a-b) <= 0`）。使用正面语义的函数名可读性更好。
**建议**：添加 `seq_before_or_equal()` 别名。

---

## 亮点

- `eph-dpdk/include/eph/dpdk/net_header.hpp:517-534`：`parse_packet` 的三层整数溢出防御设计得当，正确处理了 NIC padding、恶意 ip_total、TCP doff 过大等所有边界场景。
- `eph-dpdk/include/eph/dpdk/reactor.hpp:270-276`：RX dispatch 的 hash pre-filter + linear scan 设计对 2-16 连接的 HFT 场景是正确的优化选择，注释中清晰说明了 N>8 时的备选方案。
- `eph-net/include/eph/net/kill_switch.hpp:130-136`：`shutdown()` 在持锁期间复制 handle snapshot 再释放锁后迭代停止，正确避免了 stop 回调中 unregister 导致的迭代器失效。
- `eph-dpdk/include/eph/dpdk/dns.hpp:323-352`：DNS 响应解析的 bounds checking 链路完整（`skip_dns_name` → `offset + 10 > dns_len` → `offset + rdlength > dns_len`）。
- 全局可观测性覆盖率高：几乎所有错误分支都有 spdlog 日志，且日志包含上下文变量值（IP、端口、状态码），符合项目约定。

---

## Diff 统计
```
 eph-dpdk/code_summary.md                     | 307 ---------
 eph-dpdk/dpdk_setup.sh                       | 472 +++++++++++++
 eph-dpdk/dpdk_teardown.sh                    | 373 ++++++++++
 eph-dpdk/include/eph/dpdk.hpp                |  12 +
 eph-dpdk/include/eph/dpdk/arp.hpp            |  51 +-
 eph-dpdk/include/eph/dpdk/connector.hpp      | 712 +++++++++++++++++++
 eph-dpdk/include/eph/dpdk/dns.hpp            | 626 +++++++++++++++++
 eph-dpdk/include/eph/dpdk/eal.hpp            |  96 ++-
 eph-dpdk/include/eph/dpdk/flow_steering.hpp  | 360 ++++++++++
 eph-dpdk/include/eph/dpdk/multicast.hpp      | 787 +++++++++++++++++++++
 eph-dpdk/include/eph/dpdk/net_header.hpp     |  94 ++-
 eph-dpdk/include/eph/dpdk/platform.hpp       | 231 +++++--
 eph-dpdk/include/eph/dpdk/reactor.hpp        | 342 ++++++++++
 eph-dpdk/include/eph/dpdk/tcp.hpp            | 671 ++++++++++++++++--
 eph-dpdk/include/eph/dpdk/types.hpp          |  42 +-
 eph-dpdk/summary.md                          | 323 +++++++++
 eph-net/include/eph/net.hpp                  |  18 +
 eph-net/include/eph/net/circuit_breaker.hpp  | 263 +++++++
 eph-net/include/eph/net/gateway.hpp          | 299 ++++++++
 eph-net/include/eph/net/hmac.hpp             | 145 ++++
 eph-net/include/eph/net/http.hpp             | 266 --------
 eph-net/include/eph/net/http_client.hpp      | 849 +++++++++++++++++++++++
 eph-net/include/eph/net/kill_switch.hpp      | 243 +++++++
 eph-net/include/eph/net/proxy.hpp            | 601 ++++++++++++++++
 eph-net/include/eph/net/rate_limiter.hpp     | 139 ++++
 eph-net/include/eph/net/socket_config.hpp    | 251 +++++++
 eph-net/include/eph/net/socket_connect.hpp   | 216 ++++++
 eph-net/include/eph/net/socket_transport.hpp | 835 +++++++++++++++++++++++
 eph-net/include/eph/net/tcp_concept.hpp      |  80 ---
 eph-net/include/eph/net/tls_record.hpp       | 352 ----------
 eph-net/include/eph/net/tls_session.hpp      | 658 ------------------
 eph-net/include/eph/net/transport.hpp        | 985 ---------------------------
 eph-net/include/eph/net/websocket.hpp        | 453 ------------
 eph-net/summary.md                           | 376 ++++++++++
 34 files changed, 9253 insertions(+), 3275 deletions(-)
```

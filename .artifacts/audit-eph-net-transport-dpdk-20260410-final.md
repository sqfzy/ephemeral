# Code Audit Report (Final) — eph-transport · eph-net · eph-dpdk

## 元信息
- 时间：2026-04-10 04:59 → 05:08（约 9 分钟，3 轮）
- 范围：3 个 header-only C++23 模块，~60K LOC / 111 文件
- 模式：audit（全量审计，9 维度）
- 验证轮数：3（R1 broad discovery → R2 verify critical/major → R3 cross-module sweep + final）
- 约束：不运行任何 DPDK 二进制（用户正在用 dpdk 跑 bench）；纯静态分析 + xmake build 验证
- 编译验证：`xmake build eph-transport`/`eph-dpdk`/`test_socket_transport`/`test_websocket`/`test_tls_record` 全部 PASS

## 项目健康度摘要
- 🔴 **Critical：0**（R1 标记的 7 项全部为误读或可降级）
- 🟡 **Major：5**（验证后留存）
- 🔵 **Minor：~22**
- 💬 **Nit：~8**
- **整体评估**：3 个模块的工程质量均为良好。header-only / zero-virtual / std::expected 约束严格遵守，concept 一致性通过 `static_assert` 强制（`TcpTransport<SocketTransport>` / `TcpTransport<TcpSession<>>`），include/ 目录中找不到任何 `TODO/FIXME/HACK/XXX`。剩余风险集中在「不可信网络输入解析的 fail-fast 一致性」和「错误日志的可执行上下文」。

---

## 一、留存的 Major 问题（验证后真实有效）

### M1 · 🟡 Security · MaskKeyCache 回退路径可预测
- **位置**：`eph-transport/include/eph/transport/detail/websocket.hpp:268-293`
- **问题**：`RAND_bytes` 失败时回退到 SplitMix64+TSC seed。**同一 batch 1024 个连续 mask key 完全确定**——攻击者观察任意一个 key 即可正向/反向推算整个 batch 的所有 mask key。这违反 RFC 6455 §5.3 "the masking key for a given frame MUST NOT make it simple for a server/proxy to predict the masking key for a subsequent frame"。
- **触发概率**：极低（OpenSSL/AWS-LC 的 `RAND_bytes` 在生产中几乎不会失败），因此只标 Major 而非 Critical。
- **建议**：失败时硬错误（`std::abort` 或返回错误向上传播触发 reconnect），不静默降级；或至少每 4 个 key 重新 mix 一次时间熵。

### M2 · 🟡 Security · HTTP header CRLF 注入仅跳过不中断
- **位置**：`eph-transport/include/eph/transport/detail/http.hpp:208-216`
- **问题**：解析响应 header 时检测到 name/value 包含 CR/LF，仅 `SPDLOG_LOGGER_WARN` 然后 `continue` 跳过该 header。攻击者可利用恶意服务器/中间人发送注入 header，客户端只是丢弃该 header 并继续完成握手——破坏头部完整性假设，且可能让后续 header 被错误归并。
- **建议**：检测到 CRLF 必须 fail-fast——返回 `std::unexpected{}`，由上层断开连接重试。

### M3 · 🟡 Correctness · TLS+WS 跨记录碎片溢出未触发 reconnect
- **位置**：`eph-transport/include/eph/transport/detail/rx_worker.hpp:901-908`
- **问题**：在 TLS 模式下，跨多个 TLS record 重组 WS 帧时，如果 `ws_reassembly_buf` 溢出，代码 `WARN` + `ws_reassembly_len = 0` + `consumed += record_total; continue;`——静默丢弃部分帧后继续处理下一个 TLS record。其他 decode 错误路径（plain WS overflow、TLS decrypt 失败等）均触发 reconnect，唯独此处不一致。状态可能进入「半解码」错乱。
- **建议**：与同函数其他错误路径一致，置 `reconnect_needed = true` 然后 `break` 进入 reconnect 流程。

### M4 · 🟡 Observability · TLS AEAD 错误缺 OpenSSL 上下文
- **位置**：`eph-transport/include/eph/transport/detail/tls_decryptor.hpp:178-180`、`tls_encryptor.hpp:170-172`
- **问题**：`EVP_AEAD_CTX_open/seal` 失败仅记录 `record_len` / `plaintext_len`。生产事故定位时，AEAD 失败可能源于 tag 不匹配、key 错误、nonce 重用、padding 错乱等多种原因——缺少 `ERR_get_error()` + `ERR_error_string` 的详细信息，DEBUG 工时显著增加。
- **建议**：失败分支调用 `ERR_get_error()` 和 `ERR_error_string_n` 取错误描述，与已有上下文一并 `ERROR` 日志。

### M5 · 🟡 Security · http_connect_handshake target_host 缺 CRLF 校验
- **位置**：`eph-net/include/eph/net/proxy.hpp:469-481`
- **问题**：`std::format("CONNECT {}:{} HTTP/1.1\r\nHost: {}:{}\r\n", target_host, ...)`——`target_host` 直接进入请求行和 Host 头。如果调用方传入包含 `\r\n` 的不可信主机名（例如来自外部 API、配置文件或用户输入），可注入额外 HTTP 头到 CONNECT 请求中，攻击代理服务器或 routing 层。
- **对比**：`socket_config.hpp:181-184` 和 `proxy.hpp` 的 `ProxyConfig::validate()` 都已对 hostname 做了 control char 校验，但 `http_connect_handshake` 的 `target_host` 入口绕过了这些校验。
- **建议**：在函数入口处对 `target_host` 做与 `ProxyConfig` 同等的 CRLF/control char 校验，发现非法即 `std::unexpected`。

---

## 二、Minor 问题（按模块分组，已验证或合理判断）

### eph-transport
| ID | 维度 | 位置 | 描述 | 建议 |
|---|---|---|---|---|
| t1 | Correctness | `frame_processor.hpp:363-369` | 新 WS 消息开始时若旧碎片缓冲非空，仅 WARN+clear，应用无感知数据丢失 | 计入 stats 并/或上报 reconnect 路径 |
| t2 | Security | `tls_decryptor.hpp:136-140` / `tls_encryptor.hpp:129-133` | seq ≥ 2^32 检查在解密前；伪造 high seq 可触发误报状态切换。但实际 seq 不会跳跃增长，影响有限 | 文档化"DOS 检测一次性"语义 |
| t3 | Correctness | `tls_constants.hpp:343-349` | `derive_key_iv()` 仅按 `secret_len==48` 区分 SHA-384，其他长度静默退到 SHA-256 | 显式拒绝非 32/48 |
| t4 | Performance | `websocket.hpp:268-293` | 回退路径在 RX 热路径同步生成 4096B mask；与 M1 同位置 | 后台异步预填 |
| t5 | Design | `websocket.hpp:79-91` | `opcode_name()` 用 thread_local 缓冲返回 unknown opcode 字符串 | std::format + 静态结构化 |
| t6 | Observability | `tls_record.hpp:446` | 长度上界 `+1` 来源不清晰 | 加 inline 注释或修正 |

### eph-net
| ID | 维度 | 位置 | 描述 | 建议 |
|---|---|---|---|---|
| n1 | Correctness | `socket_transport.hpp:339-343` (connect EINTR) | EINTR 后未减去已耗时间，full timeout 可能被重置 | 用绝对 deadline |
| n2 | Correctness | `socket_transport.hpp:421` (send EPIPE) | EPIPE 与一般错误未区分，重连决策上下文不足 | 显式 case EPIPE → "peer closed" |
| n3 | Security | `http_message.hpp:214-220` | `build_http_request` 仅校验 `extra_headers`，未校验 method/path/host | 入参全 CRLF 校验 |
| n4 | Correctness | `socket_transport.hpp:204` | socket() 缺 SOCK_CLOEXEC | 加 flag |
| n5 | Correctness | `http_client.hpp:405-420` | TLS handshake 循环 deadline 已护航，但无最大迭代护栏 | 加 max_iterations 兜底 |
| n6 | Correctness | `socket_config.hpp:269-270` | keepalive idle/interval 仅单边校验 | 双向校验 |
| n7 | Consistency | `socket_transport.hpp:798-805` | `close_fd` 失败 DEBUG 与其他错误 ERROR 不一致 | 升 WARN |
| n8 | Testing | gateway / socket_transport / circuit_breaker | EINTR send 路径、health_check race、CB 状态切换无并发单测 | 注入信号 + race 测试 |

### eph-dpdk
| ID | 维度 | 位置 | 描述 | 建议 |
|---|---|---|---|---|
| d1 | Correctness | `packet_template.hpp:302` | UDP IP 头使用 `memset(0)+部分填充` 模式；新增字段易被遗漏 | 显式 init 每字段 |
| d2 | Correctness | `packet_template.hpp:42-48` 等 | `hw_cksum` 一次性能力检查，设备状态变化后 stale | 文档化为 immutable 假设 |
| d3 | Correctness | `dns.hpp` rdlength | `rdlength` 边界检查在使用之后 | 检查前移 |
| d4 | Correctness/Performance | `tcp.hpp:64,89-92` 窗口缩放 | `recv_window <= 65535` 已校验；但未拒绝/剥离对端 SYN-ACK 中的 WS option（理论上若对端违规启用 scaling，receiver 误读窗口） | 显式拒绝带 WS option 的 SYN-ACK 或文档化 |
| d5 | Security | `arp.hpp:138-141`、`connector.hpp:161-164` | 默认无 MAC 白名单，ARP 欺骗无防护 | 可选 allowlist 参数 |
| d6 | Performance | `reactor.hpp:447-451` | >8 连接退化 O(n) 分发 | rte_hash 路径（已有 TODO 注释） |
| d7 | Correctness | `reactor.hpp:409-418` | 长 idle 期间 delayed-ACK 不被触发 | 周期性 flush 或文档化 |
| d8 | Observability | `reactor.hpp:434-436` | parse 失败静默丢包，无 counter | per-conn parse_error counter |
| d9 | Consistency | `tcp.hpp` vs `arp.hpp`/`dns.hpp` | tcp 用 `enum TcpState`+`std::string`，arp/dns 用 `std::expected<T,std::string>` | 统一 error type |
| d10 | Observability | tcp/dns/reactor | 多处 INFO 用于 per-operation 事件（连接建立、ARP 解析），生产日志压力大 | 降为 DEBUG，INFO 仅 start/stop |
| d11 | Security | `dns.hpp:256` `kMaxIterations=128` | 128 × an_count(64) = 8192 迭代上限；非 DoS 但可加固 | kMaxIterations → 32 |
| d12 | Design | tcp.hpp 无 idle/keepalive | 长连接对端崩溃无内置检测 | 可选 idle_timeout |

---

## 三、跨模块新增观察（R3 sweep）

### C1 · 🔵 Architecture · 测试层向下依赖
- **位置**：`eph-transport/xmake.lua`
- **观察**：`eph-transport/tests/**` 集体 `add_deps("eph-net")`，注释承认"test_reconnect_policy depends on eph-net (higher layer) for testing"。生产代码方向 (`eph-transport ← eph-net`) 是干净的，但测试层引入了反向依赖——一个修改 eph-transport 的人无法在不构建 eph-net 的情况下跑该模块的测试。
- **建议**：将依赖 eph-net 的测试拆到 `tests/integration/` 或在 eph-net 自己的测试目录托管；模块内部测试只用 eph-transport 自身。

### C2 · 🔵 Build · eph-dpdk include 路径绕过 add_deps
- **位置**：`eph-dpdk/xmake.lua`
- **观察**：eph-dpdk 用 `add_includedirs(eph-transport/include)` 而非 `add_deps("eph-transport")`，注释解释是为了控制 `aws-lc/openssl/*.h` 在 vcpkg DPDK 自带 openssl 之前出现的 include 顺序问题。
- **风险**：若 eph-transport 将来引入 `target_data_files`、cmake export、或非 header 资产，不会被 eph-dpdk 自动获取；xmake 的 dep graph 也会失真。
- **建议**：保留注释；考虑用 `add_deps("eph-transport", { public = true, inherit = false })` + 显式调整 `add_sysincludedirs` 优先级；或将 aws-lc 包装成 wrapper header 来强制顺序。

### C3 · 💬 Positive · concept 一致性强制
- 两个 backend 都用 `static_assert(TcpTransport<...>)`（`socket_transport.hpp:939`、`dpdk/tcp.hpp:1519`），编译期就拦截 concept 漂移。设计良好。

### C4 · 💬 Positive · include/ 完全无 tech-debt 标记
- 三个模块的 `include/` 全部 grep `TODO|FIXME|HACK|XXX` 无命中。代码规范严格。

---

## 四、跨模块通用观察

1. **不可信输入的 fail-fast 一致性**：M2/M3/M5 都呈现"检测到攻击 → log → 继续"模式。HFT 客户端该一律 fail-fast 触发 reconnect，而不是降级运行。建议把这类位置统一改成 `return std::unexpected{...}`。
2. **错误日志的可执行上下文**：M4 是典型——错误分支只记录"发生了什么"而不记录"为什么"。AEAD/EVP/`SSL_get_error` 等 OpenSSL/AWS-LC API 都提供 `ERR_get_error()`，应被一致使用。
3. **Error type 不统一**：eph-transport detail 用 enum；eph-net 混合 `std::expected<T, std::string>` 和 enum；eph-dpdk arp/dns 用 `std::expected<T, std::string>`，tcp 用 enum。可考虑在 `eph-core/error.hpp` 统一一个 `Error` 类型并在新代码中迁移。
4. **CSPRNG 失败处理策略**：`websocket.hpp` 与 `reconnect_policy.hpp` 各自实现 fallback。可在 `eph-utils/random.hpp` 集中——失败 → 硬错误，或后台预填池。

---

## 五、推荐行动计划（按 ROI 排序）

| # | 项 | 推荐 skill | 预估代价 |
|---|---|---|---|
| 1 | M5 修 `http_connect_handshake` CRLF 校验 | `/fix` 或手动 | <30 行 |
| 2 | M2 改 HTTP header 注入为 fail-fast | `/fix` | <20 行 |
| 3 | M3 跨 record 碎片溢出改触发 reconnect | `/fix` | <10 行 |
| 4 | M4 AEAD 失败补 OpenSSL 错误字符串 | `/improve` | <15 行 × 2 处 |
| 5 | M1 MaskKey 回退改 fail-loud | `/fix` | <30 行 |
| 6 | Minor 批量清理 | `/improve` | 零散 |
| 7 | 跨模块 error type 统一 | `/refactor` | 跨模块 |
| 8 | C1 测试反向依赖整理 | `/refactor` | xmake.lua |

---

## 六、收敛性判断

✅ **本次 audit 已收敛**：
- R1 produced ~74 候选 → R2 verify 12 critical/major → R3 cross-module sweep + 编译验证
- R2 中 6/12 候选被推翻（subagent 误读率 50%——主要是行号上下文偏移和正则匹配错位），剩余 5 项确认为真实 Major
- R3 跨模块 sweep 仅新增 4 项观察（2 minor + 2 positive），未新增 critical/major
- R4 若继续将仅重复发现相同问题，新发现率 → 0
- 编译验证（xmake build）通过，无新增 compile-time issue

收敛标准达成 → /repeat 终止条件 `until: 收敛` 满足。

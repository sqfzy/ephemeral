# Code Audit Report (Round 1) — eph-transport · eph-net · eph-dpdk

## 元信息
- 时间：2026-04-10 04:59 → 05:02
- 范围：3 个模块（header-only C++23），共 ~60K 行 / 111 文件
- 模式：audit（全量代码审计，9 维度）
- 验证：本轮为初始发现轮，由 3 个并行 subagent 产出原始候选清单；critical/major 行号在后续轮验证
- 约束：未运行 DPDK 二进制（用户正在用 dpdk 跑 bench）

## 项目健康度摘要
- 🔴 Critical：~7（候选）
- 🟡 Major：~27（候选）
- 🔵 Minor：~30（候选）
- 💬 Nit：~10（候选）
- 整体评估：3 个模块代码质量良好，header-only 与 zero-virtual 约束基本满足；主要风险集中在 **不可信网络输入解析**（DNS/TLS/WS）和 **错误路径上下文丢失**。

---

## 一、eph-transport（候选清单）

| # | Sev | 维度 | 位置 | 描述 | 建议 |
|---|---|---|---|---|---|
| T1 | 🔴 | 安全 | websocket.hpp · MaskKeyCache | RAND_bytes 失败时回退到 LCG/TSC 种子，可能产生可预测的 mask key（RFC 6455 缓存中毒攻防） | 失败应硬错误，不静默降级 |
| T2 | 🟡 | 正确性 | tls_decryptor/encryptor.hpp seq 边界 | seq ≥ 2^32 检查在解密前；攻击者可伪造 seq 触发误报状态切换 | 结合实际解密结果判断；或仅作 metric |
| T3 | 🟡 | 安全 | http.hpp 头部解析 | CRLF 注入检测命中后只 `continue` 跳过该 header，不中断握手 | header injection 必须 fail-fast |
| T4 | 🟡 | 正确性 | rx_worker.hpp rx_loop 解码错误 | 解码错误后碎片缓冲未清空，残留状态可破坏后续帧重组 | 非 kIncomplete 错误应丢弃并重置碎片缓冲 |
| T5 | 🟡 | 正确性 | frame_processor.hpp WS 碎片重置 | 新消息开始时若碎片缓冲非空，仅 warn 后清空，应用无感知 | 计入 stats 并触发 reconnect 路径 |
| T6 | 🟡 | 安全 | reconnect_policy.hpp RNG 种子 | 两次调用 `random_device{}()` 实际等价于一次（c++ 临时对象），有效熵下降 | 用 `seed_seq{rd(), rd(), now}` 单 rd 实例 |
| T7 | 🟡 | 性能 | websocket.hpp mask 回退 | RAND_bytes 失败回退路径在 RX 热路径同步生成 4096B mask | 后台异步补充，热路径不阻塞 |
| T8 | 🟡 | 可观测性 | tls_decryptor/encryptor AEAD 失败 | EVP_AEAD 失败仅记录长度，不取 OpenSSL 错误字符串 | `ERR_get_error()` + `ERR_error_string` |
| T9 | 🔵 | 正确性 | tls_constants.hpp derive_key_iv | secret_len 仅靠 `==48` 选 SHA-384，其他值默认 SHA-256 | 显式拒绝非 32/48 |
| T10 | 🔵 | 正确性 | tls_record.hpp parse_record_header | 长度上限 `+1` 来源不清晰 | 加注释或修正为 `+0` |

> Round 2 重点验证：T1 / T3 / T4 / T6（涉及实际函数语义判断）。

---

## 二、eph-net（候选清单）

| # | Sev | 维度 | 位置 | 描述 | 建议 |
|---|---|---|---|---|---|
| N1 | 🟡 | 正确性 | socket_transport.hpp connect EINTR | EINTR 后未减去已耗时间，full timeout 重置 | 用 deadline 而非 budget |
| N2 | 🟡 | 正确性 | socket_transport.hpp send EPIPE | EPIPE 与一般错误未区分，影响重连决策 | 显式 case EPIPE → peer-closed |
| N3 | 🟡 | 正确性 | http_client.hpp send_all 死等 | poll 返回 0 (timeout) 时未 re-check deadline | 每轮 poll 后 break-on-deadline |
| N4 | 🟡 | 正确性 | http_client.hpp 响应大小检查 | max_response_size 在 append 之后才检查 | 检查 → append |
| N5 | 🟡 | 安全 | hmac.hpp verify_hex/verify_base64 | 未匹配密钥/消息有时间侧信道（早返回 vs 全计算） | 总是执行完整 HMAC，再 constant-time compare |
| N6 | 🟡 | 安全 | proxy.hpp http_connect_handshake | target_host 未做 CRLF 校验，可注入 CONNECT 请求 | 与 ProxyConfig 一致校验 |
| N7 | 🔵 | 安全 | http_message.hpp build_http_request | 仅校验 extra_headers，未校验 method/path/host | 全部入参 CRLF 校验 |
| N8 | 🟡 | 测试 | socket_transport / gateway | EINTR send 路径、health_check 与 add/remove race 无单测 | 注入信号 + 并发 race 测试 |
| N9 | 🔵 | 正确性 | socket_transport.hpp socket() | 缺 SOCK_CLOEXEC | 最佳实践 |
| N10 | 🔵 | 正确性 | http_client.hpp TLS handshake loop | 无最大迭代防护，poll 假事件可无限循环 | max_iterations + log |
| N11 | 🔵 | 正确性 | socket_config.hpp keepalive 校验 | idle/interval 仅单边校验 | 双向 |
| N12 | 🔵 | 一致性 | socket_transport close_fd | close 失败 DEBUG，其他错误 ERROR | 升 WARN |

> Round 2 重点验证：N3 / N4 / N5 / N6（直接影响生产）。

---

## 三、eph-dpdk（候选清单）

| # | Sev | 维度 | 位置 | 描述 | 建议 |
|---|---|---|---|---|---|
| D1 | 🔴 | 正确性/安全 | tcp.hpp RST 校验 `rcv_nxt + rcv_wnd` | 加法可溢出 uint32，RFC 5961 校验弱化 | 用 seq_lte/seq_after 处理 wraparound |
| D2 | 🔴 | 安全 | dns.hpp skip_dns_name + an_count | 128×64 = 8192 标签解析迭代上限，CPU DoS | 收紧 kMaxIterations、an_count 上限 |
| D3 | 🔴 | 安全 | dns.hpp try_parse_dns_packet IHL 未校验 | IHL 直接用于偏移，IHL=255 可越界 | 加 `ihl ∈ [20, 60]` 校验 |
| D4 | 🟡 | 正确性 | packet_template.hpp UDP IP 头 memset+部分填充 | 新增字段时易被遗漏 | 显式 init 每字段 |
| D5 | 🟡 | 正确性 | packet_template.hpp hw_cksum 一次性 | 设备状态变化后 stale 标志可致校验损坏 | 重新校验或文档化 immutable |
| D6 | 🟡 | 正确性 | dns.hpp rdlength 边界 | 校验前已被使用 | 校验前移 |
| D7 | 🟡 | 正确性 | tcp.hpp 窗口缩放 | TcpConfig 限 65535，但未拒绝/剥离对端 WS 选项 | 实现或拒绝 |
| D8 | 🟡 | 安全 | arp.hpp + connector.hpp | 默认无 MAC 白名单，ARP 欺骗无防护 | 可选 allowlist |
| D9 | 🟡 | 性能 | reactor.hpp 连接分发 | >8 连接时退化为 O(n) | rte_hash 路径 |
| D10 | 🟡 | 正确性 | reactor.hpp poll 空 RX | 长 idle 期间 delayed-ACK 不被触发 | 周期性 flush |
| D11 | 🔵 | 可观测性 | reactor.hpp parse 失败 | 静默丢包，无 counter | per-conn parse_error counter |
| D12 | 🔵 | 一致性 | tcp.hpp 错误类型 | mix `enum TcpState` 与 `string`，与 ARP/DNS 不一致 | 统一 DpdkError |
| D13 | 🔵 | 设计 | tcp.hpp idle timeout | 无内置 idle/keepalive | 可选 |
| D14 | 🔵 | 设计 | flow_steering.hpp RETA | RSS hash 与 RETA 非原子 | 文档化或合并 |
| D15 | 💬 | 一致性 | tcp.hpp format_mac | 标 @deprecated 但无 `[[deprecated]]` 强制 | 加 attribute |

> Round 2 重点验证：D1 / D2 / D3 / D7（critical/major correctness）。

---

## 跨模块观察

1. **错误处理风格不统一**：eph-net 用 `std::expected<T, ErrorEnum>`，eph-dpdk 部分用 `std::expected<T, std::string>`，部分用 enum；eph-transport detail/ 大量直接返回 enum。建议在 eph-core 引入跨模块的 `Error` 概念或文档化各自的边界。
2. **不可信输入解析普遍缺少 fail-fast**：WS / HTTP / TLS / DNS / IP 解析路径多处「记录 + 继续」而非「记录 + 中断」。HFT 场景下应优先 fail-fast 触发 reconnect。
3. **CSPRNG 失败处理**：`websocket.hpp`、`reconnect_policy.hpp` 均有 RNG 弱化路径，建议统一策略（fail-loud + 背景刷新）。
4. **并发/race 测试覆盖不足**：Gateway / CircuitBreaker / Reactor 等状态机缺少 race 单测；当前主要靠 happy-path 覆盖。

---

## Round 1 → Round 2 计划

Round 1 是 broad discovery；subagent 报告的 file:line 与具体语义需要验证。Round 2 将：
1. 直接 Read 上述 critical/major 项的源码，确认或推翻每条判断
2. 标记「✅ 确认」「❌ 误读」「⚠️ 部分」三种状态
3. 对确认项写入 final report；对推翻项删除
4. 探索 Round 1 未覆盖的：跨文件 race、xmake.lua 一致性、tests/integration/ 与本三模块的交互

收敛标准（until: 收敛）：连续 1 轮无新发现且所有 critical/major 验证状态稳定。

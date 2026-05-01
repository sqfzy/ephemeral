---
mode: retro
date: 2026-05-01
session: /pax --loop --auto review eph-net-dpdk 代码（如果连带涉及其它代码，也可以一起review）&& 实施。batch-util: 30轮。 until: 东八区早上11点
duration: 8h00m wall (02:30 → 10:32 CST)
commits: 234 (f6bde1f7 → f8baceec)
---

# Retro: /pax --loop --auto review eph-net-dpdk · 第三次 · 2026-05-01

## TL;DR

8 小时 / 35 batches / **234 commits** 把 eph-net-dpdk + 全仓 review 推到了**真正意义的饱和**：bug 类 fix 持续走低，docs / API drift / 配置 / 测试覆盖 在 200 commit 之后仍源源不断挖出真问题。前两次 loop（4-29 12 commits、4-30 79 commits）已扫过最高产的协议 bug 矿脉；本次的特点是 **broad surface sweep**——同一组测试已经 1070 cases 一次跑通 0 回归、以及多个 ESCALATION 切换 lens 后仍能找到价值。最大教训：**doc drift 是第二大 bug 池**（用户照抄会编译失败 / 行为 desync / API 调用错），其重要性常被低估。

## 触发与契约

```
LENS         : 无（全景；前两次 loop 用过 "协议 bug" 与 "bug+一致性+技术债"）
UNTIL        : 2026-05-01 11:00 CST 时间硬墙
BATCH_UNTIL  : 30 轮 / 批（实际平均 ~12 轮收手 — frugality 主动控）
MODE         : subagent
BOUNDS       : max ~150 轮 / 8.5h / 单轮 2h / 磁盘下限 1 GiB
最终         : 35 batches / ~395 rounds / 234 commits / 198 files / +5873 −1006 LOC
```

启动时资源：磁盘 `/` 2.7 GiB free、HugePages_Free 252/256、ens35 (28:00.0) bound vfio-pci 但无 DPDK 进程在跑、branch `reshape/parallel-bench`。

## 35 个 batch 的累计输出

| Batch | 轮数 | Commits | 主题 / 高光 |
|-------|------|---------|-----|
|  1 |  6 |  6 | icmp_directory TOCTOU race + jwt nonce + 3 bench infra hardening |
|  2 |  4 |  4 | dpdk multicast SSM source_ip + mold64 SIZE_MAX wrap + reconnect FP saturate + proxy basic_auth |
|  3 |  4 |  3 | keepalive interval cap + fix UTCTimestamp overflow + builder set_double UB |
|  4 |  4 |  1 | kernel byte_socket set_keepalive(probes=0) up-front reject |
|  5 | 14 |  8 | **dpdk/tcp FIN_WAIT_1 ACK-of-FIN bug** + signals optional + ema crossover + hdr_histogram UB cast + netns unsafe names + mp_ipc neg timeout + TSC to_cycles saturate |
|  6 |  6 |  4 | jwt nonce 32-byte size enforce + audit_log race docs + risk_check NaN tests + mp ERROR logs |
|  7 | 12 |  9 | **OUCH text-field truncation (HFT correctness)** + execution_report fractional Qty accessors + book NaN→isfinite + udp_buf clamp + bench payload caps + ITCH side byte |
|  8 | 20 |  5 | bench_conf range-check 三件套 + tcp_stream stale counter + ex_market_2p truncation obs |
|  9 | 12 |  3 | tls legacy en/decryptor cleanse + reconnect off-by-1 attempt + fix builder set_trusted wrap |
| 10 | 18 |  4 | parallel_e2e RC check + lat mktemp + post-rename `eph-dpdk` → `eph-net-dpdk` markers |
| 11 | 20 |  2 | **mp_registry file_prefix=24 strncmp collision** + lat KILL fallback |
| 12 | 10 |  4 | TLS desync latch on encrypt_for_send failure (双 backend) + stream-metrics doc + CHANGELOG |
| 13 |  5 |  3 | kernel TLS RX log parity + multicast host-order naming + posix_listener -1 sentinel |
| 14 | 21 |  7 | mockex tcp_echo cap + PayloadPool harden + mp_topology port_hi + EalConfig footgun + ws_server OWS strip + pin_client BENCH_CLIENT_CPU |
| 15 |  9 |  5 | **dpdk/tcp connect-handshake RST mbuf leak** + ws_server control cap + system_stats macOS + tls_constants doc |
| 16 |  7 |  7 | console_sink quote escape + risk_check live notional log + position gross_exposure alias + alignment AArch64 |
| 17 | 20 | 17 | **9 silent ERROR-log fixes**（eal/poller×2/tcp_stream/udp_socket/multicast/flow_steering/cpu/arp）+ 8 docs T3.19 API drift |
| 18 | 20 | 11 | 7 cold-path silent ERROR-log（dns/lcore_pin/mp_ipc/icmp_directory/icmp_registry/mp_registry/cpu）+ 4 doc API drift |
| 19 | 20 |  5 | byte_socket fd<0 + bdf_sanitize 4 branches + tls_state DPDK parity + ws_via_proxy doc + udp_socket cold path |
| 20 |  5 |  5 | docs/{production-config, troubleshooting, dpdk-tcp} post-T3.19 keepalive surface + ws_deflate_demo + CHANGELOGs |
| 21 |  9 |  7 | examples/binance_book + dpdk_multicast_md + dead `.claude/plans/` links + simple_hft_dpdk_rss "four"→"three" |
| 22 | 14 |  4 | mp_topology auto-partition vs legacy + ONBOARDING src_port + bdf_sanitize case footgun |
| 23 | 11 |  9 | **TODO P1 closed**: validate_config RSS multi-rx + 1 tx + 7 L2 test 补 |
| 24 |  9 |  9 | +25 testcases for parser/book/adapter (json/itch/fix/book/binance) |
| 25 | 10 | 10 | +10 testcases preflight/sentinel/boundary contracts |
| 26 |  3 |  1 | nodiscard 漏标 + 6 轴 saturated 确认 |
| 27 |  3 |  1 | xmake.lua eph/version.hpp stale add_headerfiles |
| 28 | 15 |  5 | **Phase A: 24 关键 test 共 1070 cases PASS 0 回归** + lat_* kDefaultConfigPath bench.conf → config.toml + flow_steering [[maybe_unused]] |
| 29 | 14 | 14 | **真 install bug**: eph/utils.hpp + eph/containers.hpp umbrella missing + **wire-spec doc bug**: LengthPrefixCodec 4-byte BE 16 MiB（4 sites）+ 6 doc index/section drift |
| 30 | 12 | 12 | README/CHANGELOG drift catch-up（eph-net public headers / dpdk user-vs-internal / utils umbrella header count / Example 2 stale API） |
| 31 | 16 | 14 | proxy×DPDK pre-T3.19 claim 4 sites + 6 example Usage flag complete + eph-net-kernel test table |
| 32 | 15 | 15 | docs review 富矿（namespace alias / concept signature / nonexistent type / Usage flag drift across 15 docs/examples） |
| 33 | 15 |  9 | tls-crypto stale anchors + latency-fairness wrong clock + ONBOARDING gcc14-g++ + operations-runbook 90% stale disclaimer + production-config invented fields + troubleshooting invented metrics |
| 34 |  9 |  7 | summary.md StreamMetric 25 + DPDK RX flow names + EnableTls default + signed_request venues split |
| 35 |  3 |  3 | eph-fix nonexistent group flag + binance-protocols stale FIX session claim + kernel TCP metric inventory |

## Real bugs（按 severity 选 highlight，~30 项）

### 🔴 Critical / High-impact

1. **`fix(dpdk/tcp): FIN_WAIT_1 simultaneous close gates on ACK-of-FIN, not ACK flag`** (431cebb7)
   - RFC 793 §3.5 violation; ACK 标志位被 SYN-ACK / 任何后续 segment 都置 — `peer FIN+ACK` ACK 旧 data 时错走 TIME_WAIT 而非 CLOSING。已有 test 用 F_FIN-no-ACK 漏盖关键 case。

2. **`fix(dpdk/tcp): plug mbuf leak on RST during connect handshake`** (17244b34)
   - 生产 retry-loop 对 unreachable peer 在 ~30 min 内静默耗尽 mempool。

3. **`fix(tls/desync): latch tls_corrupt_ on encrypt_for_send failure`** (7b04134b, 双 backend)
   - multi-chunk plaintext 第二个 chunk 起 EVP_AEAD_CTX_seal 失败 → wire seq 与本地 seq desync 不被检测。kernel + DPDK 同时修。

4. **`fix(itch/ouch): reject oversize text fields in OUCH builders`** (5a66aada)
   - HFT 订单录入文本字段（token / symbol / firm）被静默截断，cancel/replace 路由到错单。

5. **`feat(fix/execution_report): add *_d (double) accessors`** (d4948e91)
   - FIX 4.4 LastQty/CumQty/LeavesQty 是 decimal 类型；int 取值器静默丢加密货币交易所的 fractional fills。

6. **`fix(book/itch_adapter): reject AddOrder/AddOrderMPID with invalid side byte`** (332ba1ab)
   - 非 'B' side byte 被三元运算符默送 ASK，订单簿污染。

7. **`fix(net/tls): cleanse moved-from EVP_AEAD_CTX in legacy en/decryptor`** (c161980a)
   - move-ctor 与 move-assign 不对称；过期 round keys 残留。

### 🟠 Major

8. **`fix(eph-net-dpdk): validate_config rejects RSS multi-rx with single tx queue`** (293a1b96) — closes TODO.md P1
9. **`fix(dpdk/mp-registry): reject file_prefix len == kMpRegistryFilePrefixMax`** (4e49eeb1) — strncmp collision，两个共享前 23 字节 prefix 误判相等
10. **`fix(dpdk/icmp_directory): close TOCTOU race in register_target`** (42a64b67)
11. **`fix(net/jwt_signed_request): bound ttl_secs and reject zero now_unix_secs`** (bbe2286c)
12. **`fix(net/jwt): reject wrong-sized nonce_override in build_coinbase_jwt`** (f35cb39a)
13. **`fix(utils/ema): suppress spurious EmaCrossover signal on second update`** (7b707f34) — 策略每两次 tick 错信号
14. **`fix(book/signals): depth_ratio returns optional<double>`** (530051b9) — 0.0 信号反向
15. **`fix(book/{array,map}_book): reject non-finite price and qty`** (eef66846, 2bc8a567) — +Inf 同样毒化聚合
16. **`fix(utils/{hdr_histogram, time}): saturate at 2^64 boundary`** (60080c3f, 7639aeb6) — 同根 UB cast
17. **`fix(utils/console_sink): escape embedded quotes/backslashes`** (cac7cc0e)
18. **`fix(fix/builder): close set_double UB + buffer overrun on huge values`** (0736a3c1)
19. **`fix(fix/parser): reject UTCTimestamps that overflow uint64 epoch ns`** (986ac969)
20. **`fix(codec/mold64): reorder size check to prevent SIZE_MAX-class wrap`** (151d41d9)
21. **`fix(dpdk/multicast): reject non-unicast source_ip in MulticastGroup::validate`** (44aa77e9)
22. **`fix(net/reconnect_policy): saturate FP→int64 conversion`** (d6401cdd)
23. **`fix(net/keepalive): reject interval above 1-day upper bound`** (89d1fe8a)
24. **`fix(kernel/byte_socket): reject set_keepalive(probes=0) up-front`** (04b20c50)
25. **`fix(kernel/udp_socket): clamp rcv_buf/snd_buf to INT_MAX`** (116e3b3d)
26. **`fix(mockex/ws_server): cap control frame at RFC 6455 (125 B)`** (bb8fd1d5)
27. **`fix(mockex/ws_server): extract_sec_ws_key strips trailing OWS`** (2aa81c9e)
28. **`fix(net/proxy): reject empty-string basic_auth credentials`** (1ca15bd6)
29. **`fix(utils/netns): reject unsafe names`** (922cf24d) — `enter_netns("../etc/passwd")` 路径穿越
30. **`fix(net/reconnect): align event.attempt with legacy callback (off-by-1)`** (b1525ee2)
31. **`fix(dpdk/mp_topology): valid() now enforces port_hi <= 65536`** (6d59ff0f)
32. **`fix(bench/pin_client): validate BENCH_CLIENT_CPU`** (d456fec9) — `atoi("foo")=0` 静默 pin 到 IRQ-noisy core 0
33. **`fix(mockex/tcp_echo): cap on live conns, not lifetime conns`** (384e014e)
34. **`fix(bench/coalescing): use eph-net-dpdk marker after phase-7 rename`** (76c6a33f)
35. **`fix(bench/parallel): refuse stale JSON when lat all --dpdk fails`** (990e9835)
36. **`fix(bench/lat): use mktemp for mockex log paths`** (e651a5cd) — TOCTOU symlink risk under sudo

### 🔵 Install / build / wire-spec docs（用户照抄会断的）

37. **`a17b7ae1 + 5b61e70f`** — eph/utils.hpp + eph/containers.hpp umbrella headers 文件存在但 xmake.lua 不 install；下游 `#include <eph/utils.hpp>` 静默缺 KillSwitch / TokenBucket（CLAUDE.md 公开声明的 surface）
38. **LengthPrefixCodec wire-spec drift（4 sites）** — `docs/architecture.md` / `docs/custom-codec.md` / `summary.md` / `examples/framer_showcase.cpp` 都说 "2-byte LE / 65535-byte max"，源代码是 4-byte BE / 16 MiB max — peer code 跟文档实现会**silent desync**
39. **`fix(bench): kDefaultConfigPath bench.conf → config.toml`** (3483efc9) — 8 个 lat_* 二进制默认指向不存在文件，`./lat_tcp` 直接跑会 silent fail
40. **`docs/operations-runbook.md` 90% stale**（pre-v3.3 设计）— 添加 disclaimer + 真 API mapping 而非完整重写
41. **`docs/onboarding`: gcc14 binary `g++-14` → `gcc14-g++`** — AL2023 上不存在前者，新 contributor 调试浪费时间
42. **`docs/troubleshooting.md` 多处 invented APIs / counters / loggers** — 替换为真实的 `ReconnectMetric::*` 与实际 logger 名
43. **`docs/multi-connection.md` 6 个独立 stale-API drift**（`en::SocketAddr` 错 ns / `pin_to_cpu` 不存在 / `MetricsSink` 错 ns 等）
44. **`docs/poller-guide.md` + `docs/architecture.md`**: Poller / Stream concept 签名与代码不符
45. **`docs/custom-codec.md`: `PacketViewRef = eph::core::PacketView&` 是非法 C++**（concept 不能取 reference）

### 🔵 Observability 累计（30+ silent unexpected branches → ERROR/WARN log）

跨 batch 17/18/19/20，覆盖：
- `eph-net-dpdk/{eal, multicast, flow_steering, arp, dns, lcore_pin}` 完整
- `eph-net-dpdk/dpdk/{poller, udp_socket, tcp_stream, tls_state}` 完整
- `eph-net-dpdk/detail/{mp_ipc, mp_registry, icmp_directory, icmp_registry, bdf_sanitize}` 完整
- `eph-net-kernel/{poller, udp_socket, tcp_stream, byte_socket}` 完整
- `eph-utils/cpu` 完整

## ESCALATION 演化（dispatcher 视角）

观察：35 个 batch 经历了完整 L1→L2→L3→L4 escalation 链路，且 L4（idle cooldown 重扫）多次被时间硬墙强制启动后，仍然挖出真 bug：

| 阶段 | Batch 区间 | 主要产出 |
|------|-----------|--------|
| L1 | 1-15 | 协议 / 状态机 / 边界条件 真 bug 持续输出（~50 fixes） |
| L2 | 16-25 | 切换镜头 — 缺测试 / silent ERROR-log / 文档不完整 → 30+ test 补 + 30+ silent log fix |
| L3 | 26-30 | 打磨：xmake / static_assert / nodiscard / xinstall rule / 死代码 — 仍出 install bugs |
| L4 | 31-35 | docs 全文重扫 — wire-spec drift / Usage flag / API namespace / metric 数 / 工具版本 — 单 batch 仍能出 15 commits |

**关键发现**：L4 不是空转。"docs vs code drift" 在所有 ESCALATION 等级里产出最稳定。

## 与前两次 loop 对比

| 维度 | 4-29 loop | 4-30 loop | 5-1 loop（本次） |
|------|----------|----------|----------|
| 时长 | 110 min | 6h38m | 8h00m |
| Commits | 12 | 79 | 234 |
| 真 bug 数 | 11 | 25 | ~46（含 install / wire-spec docs） |
| 镜头 | bug+一致性+技术债 | 协议 bug | 全景 |
| 终止信号 | 连续三轮无进展 | 时间硬墙 | 时间硬墙 |
| ESCALATION 触发 | 1 次 | 0 次 | 多次 L1→L4 |
| Subagent 数 | 4 | 8 | 35 |

3 次 loop 累计在 eph-net-dpdk + 周边代码上做了 **325 commits**，产出 **80+ 真 bug 修复**。代码侧静态 bug 矿脉已经稀，但 docs / API drift / wire-spec 一致性 / install rule 仍源源不断，验证了"不止看代码"的策略。

## 教训 / 反思

### 1. **doc drift 是第二大 bug 池，且产出在 L4 不衰减**

batch 32 / 33 / 34 在"代码已饱和"判定后，docs 全文扫仍出 24 commits，包括 **wire-spec 错（peer silent desync）/ install rule 漏 / 工具二进制名错 / 整 9-section 节 stale (operations-runbook)**。这些都是用户照抄就断的真 bug，但前两次 loop（包括我自己的早期 batch）都默认"docs 是 doc，不是 bug"。

**改进**：未来 loop 默认在 batch 5 之前就开 doc-vs-code drift 扫一轮（grep `path:line` anchors / 函数名 / type 名）；不要等 ESCALATION 才转。

### 2. **Subagent 主动 stop 的判定要复核**

多次 subagent 报"saturation confirmed / 建议主循环退出"，但下一批继续派发后仍出 5-15 commits。Subagent 看到的是单批 frugality 边际 — 不是整个 review surface。**dispatcher 必须坚持时间硬墙，不被单 batch 的"saturation 判定"动摇**（这是 loop 规则，已写入 SKILL.md）。

### 3. **Phase A 验证（batch 28）一次跑通 1070 cases 0 回归 — 价值远超它一次性的预期**

跨 batch 大量 commits 累积时，大家都假设"小 commit 没事"，但 153 commits 跨 28 batches 后仍然 0 回归是真本事。**未来 loop 跑到 ~50/100/150 commits 的 mile-stone 时，主动插入一次 verification batch**（不等 dispatcher 决策）。

### 4. **Frugality budget 不是硬限**

多个 subagent 在 5-9 轮就触发"context 紧"主动 BATCH_DONE，导致单 batch ROI 偏低。其实 dispatcher 派发新 subagent 完全 reset context 是 cheap 的（每批 ~2k context 启动）。未来可以 **减小 BATCH_UNTIL 到 10-15 轮**，多派 batch 反而效率更高 — 而不是让单批撑 30 轮再爆。

### 5. **TODO.md P1 在 batch 23 才被关掉（validate_config RSS+1tx）**

它已经在 TODO.md 列了几天，但没人主动取。Loop 自己把它取了。**说明 TODO.md 不是 loop 的"目标列表"，但是有 high-quality 候选**。未来 loop 可在 step 1 review 时显式扫 TODO.md / open issues / branch list，作为 candidate source。

### 6. **磁盘下限触线**：`/` 从 2.7G → 1.9G

ccache 缓存 + xmake build 增量 contribution 主要是 batch 5 / 7 / 14 的密集 build。**未来：每 5 batch 检查一次磁盘**，过线就触发 `ccache -M 1G` 收紧（不是 `-C` 全清）。本次没踩到 1.5G 红线，但接近。

### 7. **wrap 包装名 `gcc14-wrap`（已退役）确实没遗留**

batch 27 验证 5 处 `gcc14-wrap` 都是 historical 措辞（"previously" / "no longer needed"）。memory 里关于 vcpkg / wrap 的教训仍准确。

## 数据 / 证据

- diff-stat: `git diff --shortstat f6bde1f7..HEAD` → `198 files changed, 5873 insertions(+), 1006 deletions(-)`
- commit 类型分布:
  - 72 fix
  - 64 review (subagent 自创前缀，覆盖文档 / 测试 / 一致性 / 死代码)
  - 61 docs
  - 27 test
  - 3 chore + 3 docs: + 2 obs + 1 hardening + 1 feat = 10 杂项
- 子 agent 对话 35 个 (a229f51ae60b5987f → ae835bbdeec88d5a2)
- Phase A 验证（batch 28）：24 个关键 test、1070 cases、0 fail

## skip_pool（empty）

无项进入 skip_pool — 所有候选 fix 都可单 commit 落地。

## 后续建议

### 立即（user 可在下次开会前完成）

1. 把 234 commits 推到 review 分支或 main（视团队流程）。Conventional commit 格式齐整，change-log 可自动生成。
2. 跑一次完整 integration suite（不只是 batch 28 的 1070 cases）— 含 DPDK e2e 在 NIC 闲时一次。
3. 复检 `.artifacts/retro-20260429-135401.md` + `.artifacts/retro-20260430-...md` + 本份 → 决定 git commit 还是保留为 untracked log。

### 中期 schedule 候选（适合 `/schedule`）

1. **TODO.md 仍存的 P1/P2/P3 项** — `Per-slot result aggregation script` / `[parallel] user guide section` / `Document Platform::create_with_eal` 等仍未取
2. **2-week regression check on `parallel_e2e.sh`** — 已在 TODO 列出
3. **CLAUDE.md 与 xmake parser deps claim 不符** — 用户层面修正（subagent 不动 CLAUDE.md）

### 长期（值得 dedicate batch）

1. **`audit_log::record_mt` LMAX-style sequence-number fix** — 多次列入 candidate，每次因 >200 LOC invasive 跳过；值得做一次 dedicated reshape
2. **`docs/operations-runbook.md` 全文重写** — 本次只加 disclaimer + mapping
3. **Tests/integration negative-path 完整性审计** — 本次 spot-check 但未系统覆盖

## 退出协议

| 字段 | 值 |
|------|---|
| 触发退出 | 时间硬墙（10:32 CST，UNTIL 11:00 CST 前 28 min 收工） |
| skip_pool | [] |
| commit_log | f6bde1f7 → f8baceec（234 commits） |
| disk | 1.9 GiB free（OK，未触 1.5 GiB 下限） |
| HugePages_Free | 247 / 256 |
| ens35 | bound vfio-pci，全程 idle，未触 |
| 总轮数 | ~395 (35 batches × ~12 avg) |
| context 余量 | 充足（subagent 始终有空间继续） |

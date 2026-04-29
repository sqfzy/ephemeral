# Artifacts Index

<!--
Convention: rows are appended in commit order, not strict time order.
Rebase merges (e.g. blueprint follow-ups landing after the upstream RSS
branch) can therefore show earlier timestamps lower in the file — read
the Commit column as authoritative for ordering. The 时间 column is a
session-local clock sample, kept for human context only.
-->

| 时间 | Skill | 摘要 | Commit | 文件 |
|------|-------|------|--------|------|
| 2026-03-28 03:50 | /discuss HFT 库缺失分析 | 5 角色 7 轮讨论：最缺订单簿、JSON parsing、连接管理、io_uring；四阶段路线图 | 50ca07c | discuss-20260328-035012.md |
| 2026-03-28 04:04 | /design Phase 1 v1.1 | eph-core 提取 + framer decode 实例方法 + ErrorEnum concept + type alias 体系化 | — | design-20260328-040452.md |
| 2026-03-28 05:32 | /ship auto | 5 Gate 全过：修复 throughput delta bug，统一 error_name 返回类型，3 commits pushed | c24d365 | ship-20260328-053255.md |
| 2026-03-28 05:51 | bench Socket vs DPDK | 6 benchmarks: DPDK 11.6× faster RX(单symbol), 5.5× faster TX, 7.1× faster RX(多symbol) | c24d365 | bench-20260328-053255.md |
| 2026-03-28 06:05 | /discuss TCP 重传 | 不做完整重传；三层渐进：reorder buffer 64 slots + SYN 重传 + 丢包遥测 | c24d365 | discuss-20260328-060512.md |
| 2026-03-28 06:11 | /design TCP 鲁棒性 | 三层实现完成：ReorderSlots 64 + SYN retransmit 200ms + gap_histogram telemetry | 5beaa0f | design-20260328-061113.md |
| 2026-03-28 06:26 | bench 6-round Socket vs DPDK | 36 runs, 2.5h: DPDK 8.0× RX(单), 6.7× RX(多), 7.1× TX; 零断线 | 5beaa0f | bench-20260328-062602.md |
| 2026-03-28 09:32 | /review all auto | 52 files全量审查: 5 Critical, 16 Major, 18 Minor, 11 Nit | 5beaa0f | review-20260328-093256.md |
| 2026-03-28 09:45 | /fix+/refactor+/improve | 修复 5 Critical + 11 Major: 数据竞争、栈溢出、DNS泄漏、协议安全 | 948fd9c | fix-20260328-094524.md |
| 2026-03-28 10:07 | /improve remaining 6 Major | get_mean溢出、throughput语义、crypto_竞争、recorder thread_local、POSIX guard | 6c717a1 | — |
| 2026-03-28 10:16 | /fix minor+nit | 18 Minor + 部分 Nit：saturating sub、loss_rate clamp、JSON escape、TLS version log | 50c0004 | — |
| 2026-03-28 11:14 | /evolve implement auto | 6/9 缺口：DnsConfig validate、[[nodiscard]]、config校验、safe move、heartbeat上限 | 61cb6c4 | evolve-20260328-111430.md |
| 2026-03-28 11:28 | /design JSON parsing | 新模块 eph-json：零拷贝 JSON 解析器 + JsonFramer，25 tests | e3ce679 | — |
| 2026-03-28 12:00 | /improve eph-dpdk R1+R2 | 8 fixes: mbuf leak, TSC race, SYN guard, IHL offset, logger perf | b95ca51 | — |
| 2026-03-28 12:46 | /ship | 5 Gate 全过：1,787 tests, bench 无退化, 10 commits pushed | b95ca51 | ship-20260328-124629.md |
| 2026-03-28 15:58 | /discuss 多连接 RSS | 三步优化：hash table→RSS多queue→rte_flow硬件分发；Step 1立即做 | a0fa3ac | discuss-20260328-155821.md |
| 2026-03-28 16:23 | /discuss Reactor 实现细节 | 10个不确定点全解决：epoll模型Reactor替代SharedRxDispatcher | a0fa3ac | discuss-20260328-162334.md |
| 2026-03-30 15:49 | /discuss 自研队列 vs rte_ring | 5角色4轮：自研队列在语义/性能/架构三维度优于rte_ring；EvictingQueue语义不可替代 | 7dd6cbb | discuss-20260330-154959.md |
| 2026-04-01 19:26 | /cleanup eph-dpdk eph-net | 日志100%统一命名logger、TcpSession move优化、Platform bool、ai_addr null guard、spinlock pause | — | cleanup-20260401-192600.md |
| 2026-04-02 08:35 | /discuss 架构重设计评估 | 5角色6轮：架构基础正确，4项重构（TransportMode拆分、模块边界微调、文件拆分、FakeTcpTransport） | abf74e2 | discuss-20260402-083558-architecture-redesign.md |
| 2026-04-02 08:35 | /design auto TransportMode拆分 | 4 commits：3类拆分+4组件提取+组合重写+detail删除，TransportMode完全消除 | 6eda7b0 | design-20260402-083558-transport-split.md |
| 2026-04-02 10:57 | /discuss bench 重设计 | 5角色4轮：tap虚拟网卡+同进程mock server+无TLS，9 bench→3 bench | 49d2b2c | discuss-20260402-105700-bench-redesign.md |
| 2026-04-03 07:27 | /discuss 项目组织重构 | 5角色7轮：模块级xmake.lua+测试跟随模块+transport detail/分层+PCH/ccache，5阶段增量实施 | e0a2beb | discuss-20260403-072707.md |
| 2026-04-03 07:38 | /design 项目组织重构 | 10模块xmake.lua+tests/benchmarks跟随模块+transport detail/分层+rules/ccache/PCH基础设施 | 9f5c271 | design-20260403-073807.md |
| 2026-04-03 07:38 | /plan 项目组织重构 | 已完成归档 | 9f5c271 | project-reorg-20260403.plan.md |
| 2026-04-03 08:37 | /bench transport baseline | build_nonce 0.47ns, twophase 424-661ns, from_url 87-101ns, validate 14-18ns | 5e20ef3 | eph-transport/.artifacts/bench-data-20260403-083736.txt |
| 2026-04-03 16:49 | /repeat improve+test+bench | 20轮（19有效）：159 commits, 101 files, +10683/-397 lines | 03fedff | repeat-20260403-164926.md |
| 2026-04-04 18:51 | /discuss 生产就绪性分析 | 5角色7轮：Gateway降级=#1差距,错误码统一=#1债务,fuzzing+ASan最优起点,soft pin安全折中 | 3cfbb97 | discuss-20260404-185159.md |
| 2026-04-04 19:00 | /plan+/design 生产就绪改造 | 5阶段计划+实施：TLS 2^32, 错误码+6, Gateway降级, soft pin, FakeTcp, 3 fuzzer, HWM | 6cec402 | production-readiness-20260404.plan.md |
| 2026-04-04 14:07 | /design 生产就绪补完 | ARP refresh+send_batch+ThresholdConfig+E2E bench, enqueue 13-57ns | 1b2461f | design-20260404-140700.md |
| 2026-04-05 00:23 | /discuss 生产级差距(utils+core) | 4轮5角色：测试缺口为主(LPF/json/base64/string_checks),AuditLog assert,5c简化 | ed61cd2 | discuss-20260405-002336.md |
| 2026-04-05 00:23 | /repeat discuss+plan+design | 4轮：92+新测试, json_escape/timestamp溢出修复, AuditLog assert, 翻译 | 7152e4f | repeat-20260405-002336.md |
| 2026-04-05 13:12 | /bench compare memcpy | rte_memcpy <128B 快30-45%, >128B glibc 胜出21-48%; 顺手修复eph-dpdk SSE编译 | 9b5f365 | bench-memcpy-compare-20260405.md |
| 2026-04-05 13:20 | /plan rte_ring vs BQ | 5场景SPSC对比(pushpop/throughput/pingpong/batch/full), EAL --no-huge | dfbc756 | plan-rte-ring-vs-bq-20260405-132000.md |
| 2026-04-07 10:27 | /bench rte_ring vs BQ | 交叉64B: BQ小payload快37-2.3×, rte_ring大payload快1.5-4.1× | dfbc756 | bench-rte-ring-vs-bq-20260407.md |
| 2026-04-04 14:15 | /discuss 生产级别差距分析 | 5角色6轮：12个CRITICAL/HIGH，P0=5项(stop竞态/TLS RX seq/pin/assert/RST)，三阶段修复 | — | discuss-20260404-production-level.md |
| 2026-04-04 14:50 | /plan+/design 生产加固 | 10项修复(5P0+5P1)：stop竞态guard/RX seq reconnect/hard pin/timestamp/RST/Gateway/AuditLog/HttpClient/RateLimiter/Histogram | 6d68627 | plan-production-hardening-20260404.md |
| 2026-04-09 17:38 | /discuss | 确认 TransportCore::do_connect 在 do_ws_upgrade 前 snapshot TLS seq 是 P0 真 bug，给出 4-callsite 修复方案 | afaceba | discuss-20260409-173817.md |
| 2026-04-09 17:53 | /plan | TLS hot-path AEAD ordering fix — 5 阶段计划：症状记录→fix 4 callsites→e2e 测试→文档→PR | afaceba | plan-tls-aead-ordering-fix-20260409-175353.md |
| 2026-04-09 18:13 | /repeat (plan execution) | TLS AEAD ordering hotfix shipped — fix + 5 e2e tests, regression-validated, plan complete | 4751ad7 | fix-tls-aead-ordering-20260409-175353.md |
| 2026-04-09 18:14 | /repeat (plan execution) | 5 阶段全部完成：fix + tests + docs；870 测试全绿；待用户授权 push/PR | 81c766d | repeat-20260409-175500.md |
| 2026-04-10 03:40 | /cleanup baseline | benchmarks/latency baseline — 12 scenarios (6 × kernel/dpdk), aggregated to bench-lat-baseline-20260409.md | c88e1dc | bench-lat-baseline-20260409.md |
| 2026-04-10 04:40 | /cleanup bench/latency | benchmarks/latency cleanup — 5 commits, robustness fixes + tests + docs, perf parity verified | 57e90b2 | cleanup-bench-latency-20260409.md |
| 2026-04-10 03:42 | /cleanup audit pass 1 | structural audit (mostly false positives, see deep audit corrections) | c88e1dc | audit-bench-latency-20260409.md |
| 2026-04-10 03:50 | /cleanup audit pass 2 | deep audit — 3 HIGH bugs in lat wrapper + 4 doc/test gaps | c88e1dc | audit-bench-latency-deep-20260409.md |
| 2026-04-10 03:55 | /cleanup discussion | 5-role redesign discussion — converged on temperate cleanup scope | c88e1dc | discuss-cleanup-bench-latency-20260409.md |
| 2026-04-10 04:40 | /cleanup verify | postclean baseline rerun + comparison | 57e90b2 | bench-lat-postclean-compare-20260410.md |
| 2026-04-10 05:00 | /review last 7 | bench/latency cleanup commits — APPROVE, 0 critical, 4 minor, 4 nit | 2d37eff | review-cleanup-bench-latency-20260410.md |
| 2026-04-10 11:16 | /discuss eph-dpdk Reactor/Transport 收敛 | 5 角色 4 轮：删 DirectTxTransport + preset 11→4 + 新增 Reactor example；保留三个核心类 | 54b056f | discuss-20260410-111612.md |
| 2026-04-10 11:46 | /discuss eph 新架构设计 | 5 角色 12 轮：3 concept (Endpoint/Codec/Reactor) + Channel<E,C,Tls> + PacketView 零拷贝；模块切 eph-net/dpdk/codec/channel；13 名字 + 4 alias | 54b056f | discuss-20260410-114659.md |
| 2026-04-10 12:43 | /design eph v3.3 架构 final spec | FROZEN：11 模块切法 B（eph-net + eph-net-kernel + eph-net-dpdk + eph-codec + eph-core 瘦身），TcpStream/UdpSocket/Poller，Tokio 命名对齐，Codec 双概念，PacketView 零拷贝，9 phase 重构计划 | 54b056f | design-eph-v3.3-architecture-20260410.md |
| 2026-04-10 17:10 | /repeat v3.3 refactor 9-phase autonomous | 9/9 phase APPROVE, 10 commits, 368 files, +19946/-56831 net -36885 lines. Phase 0-8: eph-core 瘦身→eph-codec→eph-net concepts→eph-net-kernel→eph-net-dpdk→PacketView+TLS→examples→delete legacy→docs regen | a50f2c0 | repeat-20260410-125550.md |
| 2026-04-10 17:43 | /discuss Phase 9 scope debate | 5 角色 4 轮：grep 证据驱动收敛。从"全量 1332 cases"降到"HFT-pragmatic 770 cases" (51% 迁移率)。关键发现：baseline 自己 example 不用 Gateway/CB/RL/KS（architecture astronaut 作品）；proxy 有真实用户。最终 Level 2：保留 HTTP/WS handshake/HMAC/Proxy + KillSwitch/RateLimiter minimal；放弃 Gateway/CircuitBreaker | a50f2c0 | discuss-20260410-174332.md |
| 2026-04-10 18:03 | /plan Phase 9 HFT-pragmatic recovery | 9 sub-phase 完整 plan.md：6 功能组件接口（Tokio 对齐 typed wrappers + 增量 parser + thread-safe RateLimiter）+ ~770 测试迁移（parser copy / stream rewrite 分层）+ /repeat 执行 + 6 层 verification gate + 6 个关键决策记录 | a50f2c0 | plan-phase-9-recovery-20260410-180306.md |
| 2026-04-10 20:15 | /repeat phase-9 9 sub-phases | 9/9 APPROVE: KillSwitch+TokenBucket / HMAC-SHA256 / HTTP parser subset / P0 security / WS handshake / HTTP CONNECT proxy / parser regression / P2 stream behavioral / closing. 3630 tests PASSED 0 FAIL across 124 binaries; ~914 cases migrated vs 770 planned; 10 commits from 74a73c9 → 9.9 | (9.9) | phase-9-scope-decision.md |
| 2026-04-10 20:15 | Phase 9 scope decision archive | Out-of-scope list + recovery guidance: Gateway / CircuitBreaker / chunked / TE / cookies / SOCKS5 / old alias-system tests. `.temp/baseline-pre-v3.3/` retention policy: keep until v3.4 release | (9.9) | phase-9-scope-decision.md |
| 2026-04-11 04:05 | /plan Phase 10 benchmarks/latency 全面迁移 | 6 sub-phase plan：core 清理 + Python stdlib mock (统一语言) + 6 scenario rewrite (kernel+dpdk) + bench.conf INI sections + lat dumb dispatcher。关键决策：统一 clock_gettime(MONOTONIC_RAW) + Recorder 复用 + 无 sweep/无 4-leg/无 eph-json。性能阈值 demo+50ns | a50f2c0 | plan-phase-10-latency-bench-20260411-040540.md |
| 2026-04-11 06:15 | Phase 10 scope decision archive | Out-of-scope list + recovery guidance: payload sweep / 4-leg decomposition / C mocks >200kHz / Mold64Codec / eph-json / BenchRunner framework / DPDK real-run / proxy support. 7 commits (10.0→10.6). `.temp/baseline-pre-v3.3/` retention through v3.4 | (10.6) | phase-10-scope-decision.md |
| 2026-04-11 06:15 | Phase 10 perf verification (kernel, loopback) | 6 kernel scenarios measured on aarch64 loopback + 1000-sample warmup + 10-15s duration. lat_tcp p50=8811 ns (floor 9032, delta −221). lat_ws delta +26219 ns interpreted as Python WS mock overhead (lat_tcp at-floor proves client clean). All 6 DPDK variants build clean. APPROVE with note | (10.6) | phase-10-perf-results-20260411.md |
| 2026-04-21 04:43 | /design auto | RSS support 5-stage rollout (stage 5 partial) | ed6c94a | design-rss-eph-net-dpdk-20260421-044326.md |
| 2026-04-21 05:02 | /design auto stage 5 bench | RSS plumbing end-to-end verified; default config zero regression; multi-stream lat refactor as follow-up | (pending commit) | bench-report-rss-multi-queue-20260421-050200.md |
| 2026-04-21 05:19 | /design auto stage 3 retest | Platform RSS integration test now real-NIC verified — 1/1 PASS (consolidated 6 atomic assertions into single TEST to dodge DPDK port slot detach after dev_close) | (pending) | (no new artifact) |
| 2026-04-21 05:25 | /review main..HEAD | 10-commit RSS branch review — 0 critical, 3 major (perf O(N) NIC syscalls, dispatch_mode/rss_active inconsistency, no e2e create_and_attach test), 5 minor, 3 nit; APPROVE with M1+m4 fix recommended pre-merge | 8314757 | review-rss-eph-net-dpdk-20260421-052500.md |
| 2026-04-21 06:28 | /repeat follow-ups | 8 commits landed: PR-0..PR-4 + PR-2.5 RETA collapse real fix; PR-6/7/8 deferred to feature sessions | 094f8f3 | repeat-rss-followups-20260421-062800.md |
| 2026-04-21 06:40 | /repeat PR-8 final | Multi-queue single-stream tail latency catastrophe on ENA (-32% throughput, +200% p99); RETA collapse preserves correctness but ENA RSS hash_update unsupported → real multi-stream demo deferred to RSS-capable NIC | 001b840 | bench-report-rss-multi-queue-pr8-20260421-064020.md |
| 2026-04-21 06:50 | /review audit | RSS-rollout 23-commit audit: 0 critical / 3 major (RETA collapse fallback, mockex unbounded threads, PR-8 N=1 unconfirmed) / 5 minor / 3 nit; APPROVE with Major fixes recommended | 0c531df | audit-rss-rollout-20260421-065000.md |
| 2026-04-21 08:25 | /repeat audit fixes | 6-round action plan: M1+M2 fixes + 4 minor cleanups + Round 6 N=10 校准 推翻 PR-8 +202% perf trap (Δ +1.65%, Welch t=0.98, CI 重叠) | 175dd22+1 | bench-report-rss-multi-queue-pr8-20260421-064020.md |
| 2026-04-21 07:33 | /repeat summary | 6-round audit-action-plan execution report (6 commits 1306a74..8675806 全部 push origin/main) | 8675806 | repeat-20260421-073324-audit-action-plan.md |
| 2026-04-21 04:08 | /design auto Phase A-G DPDK TCP P0 hardening | 7 commits (rebased onto RSS branch): MSS negotiation, auto src_port, destructor RST, keepalive (default off), ICMP PMTU feedback, 7 new StreamMetrics, 66-row state-machine conformance table. Unit tests only (shared DPDK NIC not exercised) | dd0c5b2 | design-20260421-040844.md |
| 2026-04-21 07:49 | /review audit rebase | Post-rebase review of 8 P0-hardening commits: 0 critical / 2 major (ICMP callback not wired into create_and_attach; keepalive tick missing in DpdkPoller-driven loop) / 4 minor / 1 nit. NEEDS_FOLLOWUP | 35b6de0 | review-20260421-074945.md |
| 2026-04-21 08:28 | /blueprint 生产级统一对齐 | 7-phase plan 执行：Phase 1-3+5+7 LANDED (7 commits 971a494..this), Phase 4 REVISED (保留 std::string boundary), Phase 6 DEFERRED (需 NIC_B). 10 new tests, 0 regression. | (this commit) | blueprint-exec-20260421-082851.md |
| 2026-04-21 09:11 | /review blueprint | 7-commit blueprint review：0 critical / 2 major (Platform ICMP 零直接测试；lifecycle 裸指针无文档) / 7 minor / 2 nit. REQUEST_CHANGES | 1c3f85a | review-20260421-091100.md |
| 2026-04-21 10:55 | /review 新代码 | Major 1+2 root fix review: 0 critical / 2 major (set_icmp_callback 线程安全边界未覆盖；dispatch callback 在锁下有 latent 死锁) / 4 minor / 1 nit. NEEDS_DISCUSSION | 0385fc8 | review-20260421-105534.md |
| 2026-04-22 02:54 | /pax --loop --auto review eph-net-dpdk | 15 轮 loop (非 RSS 面审查与实施)：6 fix + 1 security + 4 test + 2 script + 2 doc；135/135 unit tests 通过；4+ Explore 候选经核验 REFUTED；未 push | 14acd46 | loop-eph-net-dpdk-review-20260422-025435.md |
| 2026-04-22 06:51 | /pax --auto bench lat 全扫 + /report experiment | 7 scenarios × (kernel,dpdk) = 14 runs；DPDK p50 RTT 比 kernel 低 11–16%；中途发现 ex_market_2p fixture 与 BookTicker::from 协议不匹配 → 100% malformed，会话内合成 bookTicker fixture 修复（未 commit），两后端修复后 samples 21万/25万；ex_market DPDK p99=36ms 为测量 artifact 待查 | eb7bb64 | experiment-20260422-065113.md |
| 2026-04-22 10:34 | lat 全扫 rerun (client+mock 都绑核) + /report experiment | Post-reshape 14 runs；client cpu 4 / mockex cpu 6，log 确认；DPDK p50 RTT −12..−17% vs kernel；DPDK RX p99 tcp −25% / ws −18% 相比未绑核 baseline，p50 漂移 ±5% 内；3 处 anomaly 标注（DPDK ex_order TX p50=254µs stall、UDP p99=262µs、ex_market p99=16ms MMPP 稀疏 artifact）不作 DPDK 劣势引用 | 7869ea4 | experiment-20260422-103422.md |
| 2026-04-22 11:59 | experiment 5min 复现验证 DPDK ex_order / UDP 异常 | 4 次 5min 重跑（ex_order ×3 + udp ×1）全部正常：ex_order TX p50 12.4µs throughput 46k/s（基线 254µs / 3.8k）；UDP p99 29µs（基线 262µs）；跨 run 极差 <1% mean → H1 成立，确认两处 anomaly 为单次 transient；10:34 对应两行不作稳态引用 | 7869ea4 | experiment-20260422-115953.md |
| 2026-04-22 15:50 | /pax --loop --auto round 2 父决策记录 | eph-net-dpdk round 2 sweep 2 batches × 15 + follow-up pax 的 scope / outcomes / 9 个 latent bug 清单 / 关键决策 / 仍开的 follow-up 候选 | d4dc95a | decision-20260422-pax-loop-dpdk-review.md |
| 2026-04-22 16:17 | /report decision 四个 TODO | Round 2 sweep 收尾的 4 个 deferred item 决策：TLS API record-by-record 推迟 / Proxy 不做 / WS timeout 可做 / TLS cert 推迟；每项含候选/理由/代价/重评估条件 | d4dc95a | decision-20260422-161707.md |
| 2026-04-23 04:58 | /report decision --auto | lucky-giggling-kahan review roadmap: 9/11 done (T1×2, T2×4, T3×3), 2 skipped with TD-1..TD-5 ledger + tomlplusplus test fix | 754d734 | decision-20260423-045825.md |
| 2026-04-23 06:15 | /report decision --auto | TD ledger closeout: TD-3+TD-5+TD-1+TD-2+TD-6 closed (5/6), TD-4 env-gated; 6 commits 1491691..3bbd1fb; 39/39 targets green | 3bbd1fb | decision-20260423-061527.md |
| 2026-04-27 10:47 | /report retro --auto | /pax --loop --auto eph-net-dpdk: 3 完整 batch + batch-4-partial 9 轮 = 54 rounds / 42 commits / 0 失败 0 回滚；ESCALATION L1→L2 在 batch 3-4 边界精准触发；5 教训 6 action items；待用户决定 batch-4-partial 9 commits 命运 | cc52895 | retro-20260427-104752.md |
| 2026-04-28 09:32 | /report decision | eph-net-dpdk crypto HFT review + T1/T2/T3 全链：22 项 ADR (Tier 边界 / Orchestrator 设计 / WS deflate / async DNS / 多 NIC / JWT / aws-lc TLS resumption gap 等)；54 commit / 14 merge | 4337750e | decision-20260428-093206.md |
| 2026-04-29 10:57 | /report decision | eph-venue 抽取 7 决策 ADR + 同日下午重审章节（barter-rs 对照触发）：D1 wire-layer-only / D2 book 留 eph-book / D3 public-only（hook 命名 `data_channel_auth`）/ D4 unit→eph-venue/tests + e2e 留 integration / **D5 改为 trait + 薄 generic facade（VenueTraits concept + 4 hooks + v2 预留 VenueStreamSelector/VenueBook）** / **D6 改为 bool 谓词 + 预留 SubResponse=void 关联类型** / D7 顺补 eph-json/coinbase.hpp（独立 feat commit）；M1 保行为 + 局部 feat / M2 12-phase checkpoint（plan 阶段 1-3 待重写） | 8790f123 | decision-20260429-105720-eph-venue-extraction.md |
| 2026-04-29 13:54 | /report retro --auto | /pax --loop --auto eph-net-dpdk：4 batches / 17 rounds / 12 commits（2🔴 8🟡 3🔵）/ 0 skip；UNTIL 健康触发；3 教训 7 action items；187/187 cases pass，与 market_infra h2h 全程并存零抢占 | 9c7a21c6 | retro-20260429-135401.md |
| 2026-04-29 14:50 | /pax --loop --auto eph-net-dpdk batch 1 | 7 rounds / 7 commits（4 fix + 3 docs）：poller 空 cb dispatch crash guard / EAL `--lcores` extra_args 互斥 / UdpConfig self-send warning + 修两个 silently-passing test / arp 注释同步 / changelog / stale `cfg.legacy.tuple` 引用清理 / examples README；49 + 32 + 23 = 104 unit tests pass | 9cdd7197 | （此 INDEX 行） |

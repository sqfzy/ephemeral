# Artifacts Index

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

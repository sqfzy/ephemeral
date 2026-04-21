# Repeat Report — audit 行动计划执行

## 概况

- **命令**：按推荐行动 [until: 完成行动计划]
- **来源 audit**：`.artifacts/audit-rss-rollout-20260421-065000.md`
- **开始**：2026-04-21 ~07:00（含会话续接前轮）
- **结束**：2026-04-21 07:33
- **总轮数**：6（全部有效）
- **终止原因**：6 项 audit 推荐行动全部完成 → `until: 完成行动计划` 满足
- **commit 数**：6（全部 push 到 `origin/main` `0c531df..8675806`）

## 各轮详情

── 第 1 轮 ✅ 1306a74 ──
fix(mockex): cap mockex_max_connections to prevent OOM (audit Major #2)
 mockex/include/mockex/scenarios/tcp_echo.hpp | 12 +++++++++++

── 第 2 轮 ✅ aaa9827 ──
fix(eph-net-dpdk): RETA collapse failure now refuses Platform start (audit Major #1)
 eph-net-dpdk/include/eph/dpdk/platform.hpp | 14 +++++++++-----

── 第 3 轮 ✅ c956e90 ──
fix: minor edge-case hardening (audit Minor #5+#6+#7)
 eph-net-dpdk/include/eph/net/dpdk/flow_steering.hpp |  4 +++
 eph-net-dpdk/tests/integration/test_dpdk_rss_platform.cpp | 18 +++++++++++++--

── 第 4 轮 ✅ f24e1fb ──
refactor(bench): extract attach_and_run helper in lat_ex_market{,_2p} (audit Minor #4)
 benchmarks/latency/exchange/lat_ex_market.cpp    | 18 +++++++++---------
 benchmarks/latency/exchange/lat_ex_market_2p.cpp | 18 +++++++++---------

── 第 5 轮 ✅ 175dd22 ──
refactor(mockex): tcp_echo graceful shutdown via shutdown(fd) + join (audit Minor #8)
 benchmarks/mockex/include/mockex/scenarios/tcp_echo.hpp | 38 ++++++++++++++++++++++++++++++++--

── 第 6 轮 ✅ 8675806 ──
docs(artifacts): N=10 重复实验推翻 PR-8 +202% p99 退化结论 (audit Major #3)
 .artifacts/INDEX.md                                          |  1 +
 .artifacts/bench-report-rss-multi-queue-pr8-20260421-064020.md | 60 ++++++++++++

## 汇总表格

| 轮次 | 状态 | commit | audit 项 | 摘要 |
|------|------|--------|---------|------|
| 1 | ✅ | 1306a74 | Major #2 | mockex_max_connections 加 hard cap (64) 防止 thread/OOM 爆炸 |
| 2 | ✅ | aaa9827 | Major #1 | RETA collapse 失败时改 ERROR + return unexpected, 拒绝带病 multi-queue 启动 |
| 3 | ✅ | c956e90 | Minor #5+#6+#7 | ChildReaper RAII + sample 数 GT/LE 范围断言 + RETA partial-fill 注释 |
| 4 | ✅ | f24e1fb | Minor #4 | lat_ex_market{,_2p} 提取 attach_and_run lambda 避免 add+run 重复 |
| 5 | ✅ | 175dd22 | Minor #8 | mockex tcp_echo: active_fds + shutdown(SHUT_RDWR) + join, 替代 detach() |
| 6 | ✅ | 8675806 | Major #3 | N=10 校准 推翻 PR-8 +202% p99 perf trap (Δ +1.65%, Welch t=0.98, CI 重叠) |

## Round 6 关键统计结果

```
Run A — NB_RX_QUEUES=1 (10 runs, p99 ns):
  21175 23143 24343 23127 23079 24855 22871 22743 22935 23143
  mean=23141.4  stddev=973.3  95% CI=[22445, 23838]

Run B — NB_RX_QUEUES=4 + RSS (10 runs, p99 ns):
  24759 23191 22503 22903 23031 24119 23095 23527 24695 23415
  mean=23523.8  stddev=762.2  95% CI=[22979, 24069]

Δ = +382 ns (+1.65%)
Welch t = 0.978 << 临界值 2.10 → 不显著
CI 重叠 = YES
```

→ 原 PR-8 N=1 报告的 +202% / +315% / -32% 全部为 outlier，已在
`bench-report-rss-multi-queue-pr8-20260421-064020.md` 添加 "Round 6
重复实验" 章节正式撤回。

## 累计变更 (origin/main 0c531df..8675806)

```
.artifacts/INDEX.md                                          |  1 +
.artifacts/bench-report-rss-multi-queue-pr8-20260421-064020.md | 62 +++++++++-
benchmarks/latency/exchange/lat_ex_market.cpp                | 18 +--
benchmarks/latency/exchange/lat_ex_market_2p.cpp             | 18 +--
benchmarks/mockex/include/mockex/scenarios/tcp_echo.hpp      | 50 ++++++++-
eph-net-dpdk/include/eph/dpdk/platform.hpp                   | 14 ++-
eph-net-dpdk/include/eph/net/dpdk/flow_steering.hpp          |  4 +
eph-net-dpdk/tests/integration/test_dpdk_rss_platform.cpp    | 18 +++
8 files changed, 165 insertions(+), 20 deletions(-)
```

## 后续建议

- audit 全部行动闭环；无遗留 Critical/Major。
- 下一次涉及 perf 数据的 PR 默认 N≥10 + 95% CI 检验。 单次 cloud bench
  的 p99 不再视作可下结论的信号（本次教训直接来自 PR-8 的 +202% 假阳性）。
- 真 multi-stream throughput 验证仍需 RSS-controllable NIC（Mellanox
  ConnectX / Intel E810）。 ENA 上软件兜底正确性已闭环 + 性能无显著代价。

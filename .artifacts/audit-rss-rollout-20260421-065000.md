# Code Audit Report — RSS rollout (PR-0..PR-8 + cleanup)

## 概况
- 时间：2026-04-21 06:50
- 耗时：≈4 min
- 审计范围：23 commits, `447cc34..0c531df` (本 session 全部 RSS rollout + cleanup + softening)
- 代码规模：41 files, +2664 / -1608 (净 +1056)
- 构建状态：✅ 通过 (1.2s rebuild)
- 测试状态：✅ test_flow_steering 42/42

## 项目健康度摘要
- 🔴 Critical：0
- 🟡 Major：3
- 🔵 Minor：5
- 💬 Nit：3
- 整体评估：feature-as-a-unit 设计连贯、向后兼容、测试覆盖良好；几处 NIC 兜底逻辑和 unbounded resource 假设值得收紧

## 技术债清单

| # | 严重度 | 维度 | 描述 | 位置 | 影响 | 推荐 Skill |
|---|---|---|---|---|---|---|
| 1 | 🟡 Major | 正确性 | RETA collapse 失败后仍允许 N queue 启动 → 原 bug 复现 | `platform.hpp:823-845` | NIC-edge | `/fix` |
| 2 | 🟡 Major | 安全 | `mockex_max_connections` 未校验上界 → 用户配 1e6 → spawn 1M 线程 OOM | `tcp_echo.hpp:113-114` | mockex 进程 | `/fix` |
| 3 | 🟡 Major | 测试 | PR-8 perf 信号已 N=1 软化，但仍未跑 N≥10 重复实验 → 结论未确认 | `.artifacts/bench-report-pr8-*.md` | 文档 | `/bench compare` |
| 4 | 🔵 Minor | 设计 | lat_ex_market / lat_ex_market_2p 内 add() 4 处重复 | `lat_ex_market{,_2p}.cpp` | 代码重复 | `/refactor` |
| 5 | 🔵 Minor | 正确性 | test SetUp fork→EAL init 间若 fork 失败可能 leak 子进程 | `test_dpdk_rss_platform.cpp:144-155` | test infra | `/fix` |
| 6 | 🔵 Minor | 正确性 | 测试 `nb_rx_queues=4` hardcoded，未与 NIC `max_rx_queues` 对齐 | `test_dpdk_rss_platform.cpp:178` | 移植性 | `/fix` |
| 7 | 🔵 Minor | 正确性 | `find_src_port_for_queue` 假设 RETA query 完全填充 reta[]，partial failure → 读垃圾 | `flow_steering.hpp:559-571` | NIC-edge | `/fix` |
| 8 | 🔵 Minor | 设计 | mockex tcp_echo 关停用 detach()，非 graceful join → 进程退出才回收 | `tcp_echo.hpp:177-179` | mockex 进程 | `/refactor` |
| 9 | 💬 Nit | 性能 | toeplitz_hash 96×32 内层循环可 precompute 但 connect-path-only 无收益 | `flow_steering.hpp:341-365` | n/a | — |
| 10 | 💬 Nit | 设计 | RssState ~2 KB 栈结构，short-lived，无需堆但可考虑 | `flow_steering.hpp:474-491` | n/a | — |
| 11 | 💬 Nit | 文档 | PR-8 报告标题仍含 "tail latency on ENA"，已软化但标题未更新 | `bench-report-pr8-*.md:1` | 文档 | — |

## 架构评估

### ✅ 设计方向
- flow_steering：纯算法 + NIC syscall helper，clean 分层
- Platform：从 single-queue 扩展到 multi-queue 控制平面
- Stream::create_and_attach：turnkey factory，依赖方向正确
- RETA collapse：放在 Platform::create 而非更高层是正确选择

### ⚠️ 可关注点
- flow_steering 错误类型 `expected<T, std::string>` vs Stream 层的 `ErrorInfo` 双轨
- kMaxRssQueues = 64 硬编码（PR-4 已加 doc 解释）

### 🚧 已知 NIC 假设
- ENA-specific：`rte_eth_dev_rss_hash_update` ENOTSUP → RETA collapse 兜底
- 假设 RETA update 在所有 PMD 上可用 — 见 Major #1
- 假设 NIC 至少有 4 RX queue（test 用） — 见 Minor #6

## 推荐行动计划

1. **🟡 Major #2 mockex_max_connections cap** — `/fix` ~5 行
2. **🟡 Major #1 RETA collapse 失败兜底** — `/fix` ~15 行
3. **🟡 Major #3 PR-8 N≥10 重复实验** — `/bench compare`，~30 min
4. **🔵 Minor #5,#6,#7** — 一次小 `/fix` 可一起处理
5. **🔵 Minor #4** — `/refactor` 抽 helper
6. **🔵 Minor #8** — `/refactor` graceful shutdown
7. **💬 Nit** — 跳过

## 后续建议

- 架构和一致性已经很好，没有大的技术债。Major 都是 "edge case 兜底" 类，不阻塞 production。
- 优先：Major #1 + #2 应该 merge 前修；不大但都涉及"被忽略的 edge"。
- PR-8 数据：值得独立 `/bench compare` session 把"+202%"信号 confirm 或 deny。

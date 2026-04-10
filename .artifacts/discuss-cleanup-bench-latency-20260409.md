# Discussion Record — /cleanup benchmarks/latency 重设计

## Context
- 时间：2026-04-09 ~03:42
- 议题：simplify benchmarks/latency 代码
- 复杂度：中
- 讨论轮数：3 轮
- 参与角色：R8 激进创新者、R14 架构师、R6 维护性倡导者、R2 极简主义者、R1 风险卫士

## 摘要

5 角色对抗讨论收敛于一个温和的清理方案：删 264 LOC 死代码 + inline 单调用 ws_handshake.hpp + 缩减 config.hpp 移除 exchange-only 字段 + 收敛 runner 的 3 个 sweep 变体为 1 个 templated API + 砍 lat 脚本 defensive 分支 + 加 lint test 防止未来积出死代码。R8 提议的"删 runner / 删 config / 重写 lat 脚本"被 R6+R14+R1 联合反对（理由：把工程债从 1 处分散到 6 处不是简化）。R2 否决了 R14 提议的 socket_io / bench_main / retry_connect 提取（理由：dedup 短重复反而增加理解负担）。R1 设定 commit 拓扑：6 个独立 commit，每个有明确的 perf parity 验证策略。

## 方案细节

### 架构变更清单

| # | 变更 | LOC Δ | 风险 | 验证 |
|---|---|---|---|---|
| 1 | rm 3 dead headers (scenario_concept, stream_scheduler, udp_client) | −264 | 0 | 0 |
| 2 | inline ws_handshake.hpp (208) into lat_ws | ~−100 | 低 | lat_ws kernel+dpdk |
| 3 | shrink config.hpp + split exchange/exchange_config.hpp | ~−180 | 中 | lat_tcp_dpdk + 3 exchange |
| 4 | collapse runner.hpp 3 sweep variants → 1 templated `run_sweep<Mode>` + rename inflight param | ~−60 | 中 | lat_tcp + lat_tcp_dpdk |
| 5 | trim lat script defensive branches | ~−85 | 低 | sanity lat tcp + dpdk |
| 6 | add lint test for 0-caller core/*.hpp | +30 | 0 | test build |

**净 LOC**: ~−629 (4770 → ~4141, ~13% reduction)

### 拒绝列表

- ❌ 删 core/runner.hpp (R6 R14 R1 反对：把单点修改变成 6 点修改)
- ❌ 删 core/config.hpp (R8 收回：6 处 getenv() validation 重复)
- ❌ 提取 core/socket_io.hpp (R2 反对：mock vs client 的 send_all 语义边界不同)
- ❌ 提取 core/bench_main.hpp (R2 + R1 反对：隐藏 fork/signal/TSC 顺序、潜在 perf 风险)
- ❌ 提取 core/retry_connect.hpp (R2 反对：8 行 × 3 重复不值得 extract)
- ❌ 删 core/dpdk_env.hpp (R2 假设错误：6 callers)
- ❌ 改 core/tsc_protocol.hpp / sample.hpp (R1 红线：hot path)

### Commit 拓扑

```
1. rm dead headers
2. inline ws_handshake
3. split exchange config
4. collapse runner sweep API
5. trim lat script
6. add lint test
```

每 commit 独立可回滚。perf parity 验证：每个 commit 重跑该列出的代表性场景；任意 p50 漂移 > 5% 或 p99 漂移 > 10% 触发 git revert。

## 关键决策

| 决策 | 选项 | 选了 | 理由 |
|---|---|---|---|
| 是否删 runner.hpp | A) 删 222 LOC, 6 处复制 sweep loop  B) 收敛到 1 templated API | B | 复制 6 次的 bug risk > 222 LOC 收益 |
| 是否删 config.hpp | A) 删, 6 处 getenv  B) 缩减到 ~150 LOC | B | 配置 schema 必须有 single source of truth |
| 是否提取 socket_io.hpp | A) 提取  B) 接受 39 LOC 重复 | B | mock 和 client 的 send_all 语义边界不同 |
| 是否提取 bench_main.hpp | A) 提取  B) 接受 boilerplate | B | 隐藏 fork/signal/TSC 顺序，且有 perf 风险 |
| 验证策略 | A) 只跑 lint+compile  B) 抽样 baseline  C) 全 baseline | B+full at end | commit 间抽样，cleanup 末做全 baseline |

## 全部 5 角色第 3 轮最终立场

| 角色 | 修正 | 最终立场 |
|---|---|---|
| R8 | 收回"删 runner/config" | 接受温和方案 |
| R14 | 收回 socket_io 提取 | 接受温和方案 |
| R6 | 加"留下回归保护" lint test | 接受方案 |
| R2 | 维持立场 | 接受方案 |
| R1 | 设定 commit 拓扑 | 接受方案 |

收敛 = 5/5 同意。

# Design Report: rte_memcpy 条件替换

## 概况
- 时间：2026-04-07
- 模式：默认 (auto)
- 需求：编译时开关替换 DPDK tcp.hpp reorder path 的 std::memcpy → rte_memcpy
- 讨论轮数：2 轮
- 参与角色：R3 性能狂热者, R2 极简主义者, R1 风险卫士
- 提交：⏭️ 未提交（等用户确认）

## 需求边界

**In scope**：
- `EPH_USE_RTE_MEMCPY` 编译开关（xmake option）
- tcp.hpp:959 reorder buffer memcpy 条件替换
- `EPH_USE_RTE_RING` xmake option 预留（仅定义，无代码生效）

**Out of scope（评审后排除）**：
- RteRingQueue wrapper（实验 B）— bench 用 DirectTransport 无队列，不可测量
- types.hpp 修改 — 依赖实验 B
- eph-transport/eph-net 中的 memcpy 替换 — 无 DPDK 依赖

## 设计方案

### 修改文件

| 文件 | 改动 |
|------|------|
| `xmake.lua:44-54` | 新增 `use_rte_memcpy` 和 `use_rte_ring` option |
| `eph-dpdk/include/eph/dpdk/tcp.hpp:34-37` | 条件 `#include <rte_memcpy.h>` |
| `eph-dpdk/include/eph/dpdk/tcp.hpp:956-960` | 条件 `rte_memcpy()` 替换 |

### 关键设计决策

| 决策 | 选择 | 否决项 | 理由 |
|------|------|--------|------|
| 实验 B 推迟 | 仅实验 A | A+B 同时实现 | bench 用 DirectTransport（无队列），rte_ring 不可测量 |
| ifdef 位置 | 仅 reorder path | 全文件所有 memcpy | 唯一 std::memcpy 调用点在 reorder buffer |
| use_rte_ring 预留 | 仅 option 定义 | 完整实现 | 零成本预留，为后续扩展保留入口 |

### 评审摘要

- **R3 性能狂热者**：支持实验 A（~0 成本），反对实验 B（copy-via-temp 可能抵消 rte_ring 优势）
- **R2 极简主义者**：实验 A 是 1 行 ifdef，ROI 极高；实验 B 投入 ~150 行不可测量代码
- **R1 风险卫士**：实验 B 有接口缺口（try_consume_n）和 EAL 析构顺序风险
- **共识**：仅实验 A，实验 B 推迟

## 实现概况
- 新增文件：0 个
- 修改文件：2 个 (xmake.lua, tcp.hpp)
- 测试用例：编译验证（eph-dpdk headeronly build OK）

## 注意事项

1. **reorder path 触发率**：memcpy 仅在 out-of-order segment 到达时触发。有序流量不走此路径。bench_latency.sh 的 mock server 通过物理网络发送，偶尔乱序取决于网络条件。
2. **ARM64 编译**：DPDK bench 目标在 ARM64 上因 `-mssse3`（x86 SSE 指令）无法编译——这是 pre-existing issue，与本次改动无关。bench_latency.sh 运行在 x86 EC2 上。
3. **构建命令**：
   ```bash
   # 基线
   xmake f -m release && xmake build bench_market_dpdk bench_order_rtt_dpdk
   # 替换版本
   xmake f -m release --use_rte_memcpy=y && xmake build bench_market_dpdk bench_order_rtt_dpdk
   ```

## 后续建议
- 在 x86 双 NIC 环境运行 bench_latency.sh 对比基线 vs `--use_rte_memcpy=y`
- 若 reorder path 触发率过低导致无统计差异，考虑构造强制乱序的 bench 场景
- 实验 B (rte_ring) 应先用已有 micro-bench (`bench_rte_ring_vs_bq.cpp`) 数据评估，确认 >20% 改善后再实现 wrapper

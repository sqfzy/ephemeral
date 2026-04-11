# Discussion Record

## Context
- 时间：2026-04-10 05:36:00 → 06:29:31（约 53 分钟）
- 用户原始需求：

  > 我在想现在的架构合理吗？是不是把mock代码独立出来更好。例如根目录新建 mock/ 作为一个xmake子项目，这样mock的实现可以更丰富，更完整，而仅仅是为了bench latency而存在。benchmarks/latency/就不作为xmake子项目了，把latency扁平化，简单化。特别是core中的代码。一些普遍有用的，可以考虑放到eph-**库里。另一些可以考虑合并，例如用一个latency_common.hpp。谈谈你的看法

- 复杂度评估：中（多个合理方案 + 非显然权衡）
- 讨论轮数：4
- 参与角色：R14 架构师 / R2 极简主义者 / R8 激进创新者 / R6 维护性倡导者 / R11 怀疑论者

## 内容摘要

用户提议把 mock 抽出独立子项目、扁平化 benchmarks/latency、合并 core/ 为 latency_common.hpp。讨论否决了"独立 mock/ 子项目"和"大合并"两个具体方案，但确认了用户的核心直觉（mock 散落 + core/ 反向依赖）是真问题。最终共识是「局部促进」路线：5 项通用 helpers 推到 eph-* 库，echo_mocks.hpp 复用 ws_framing/ws_handshake（cleanup C3 遗漏修复），latency/ 保留为 xmake 子项目。

---

## 上下文事实（讨论前确认）

### benchmarks/latency/core/ 14 个文件实际清单
- config.hpp (415 lines) — bench.conf parser
- dpdk_env.hpp (179) — DpdkBenchEnv (EAL + Platform + ARP)
- netns.hpp (50) — generic Linux setns
- runner.hpp (221) — BenchRunner sweep loop with pre-warmup
- sample.hpp (37) — RttSample / OneWaySample
- scenario_concept.hpp (34) — RttScenario / OneWayScenario concepts
- signal.hpp (30) — install_signal_handlers + g_running
- socket_bind.hpp (134) — kernel TCP/UDP bind helpers
- socket_io.hpp (54) — send_all_fd / recv_exact_fd (just added in C1)
- stream_scheduler.hpp (149) — exchange-specific scheduling
- tsc_protocol.hpp (114) — bench TSC wire format
- ws_client.hpp (204) — bench kernel WS client (TSC-stamped)
- ws_framing.hpp (177) — server-side WS frame helpers (potentially reusable)
- ws_handshake.hpp (208) — server-side HTTP upgrade with inline SHA-1

### benchmarks/latency/exchange/
- mock_ws.hpp (268) — Binance-style multi-stream WS server (shared by lat_ex_market + lat_ex_order)
- mock_md_udp.hpp (74) — exchange UDP MD push

### 反向依赖现状
core/ 被 4 个外部消费者使用：
- benchmarks/latency/{tcp,udp,ws,exchange}/lat_*.cpp（其自身 binary）
- tests/unit/bench/test_*.cpp（一年多前已经存在）
- eph-dpdk/tests/integration/{dpdk_e2e_env,echo_mocks}.hpp（cleanup 阶段新引入）

### 关键发现
echo_mocks.hpp 内联了 80 行 SHA-1 + 40 行 base64 + 40 行 WS framing —— 全部与 `core/ws_framing.hpp` + `core/ws_handshake.hpp` 重复。这是 cleanup C3 的遗漏。

---

## 完整讨论

### 第 1 轮 — 各方亮出立场

**R11 怀疑论者**（反对动手）：
现在的架构哪里坏了？14 个 core/ 头文件已按职责拆分，bench 能跑、tests 能跑、用户刚验证 7/7 PASS。要求所有支持重构的角色给出**可量化指标**——不能只说"看起来不漂亮"。

**R14 架构师**（部分支持）：
真问题是**依赖方向反转**——`tests/` 在 include `benchmarks/latency/core/`，违反"目录名 = 内容契约"的隐含承诺。`benchmarks/` 应该只放 performance binaries，不应该藏 14 文件的支持库。支持 R14+R6 联合方案 C：通用部分推到 eph-utils/eph-net，测试/bench 共享但非通用的部分放 test support，**不**新建顶层 mock/ 子项目。

**R8 激进创新者**（强烈支持用户提案）：
项目里至少 6 份 mock 实现，应该统一到 `mock/` 顶层子项目，泛型化 `TcpEchoMock<WireProtocol>`，TSC stamping 作为 protocol layer 差异。三个量化指标：120 行可消除、修改范围 2→1、可加单元测试。

**R2 极简主义者**（反对 R8，部分支持 R14）：
R8 用一个真问题（重复）证明了一个伪解决方案（独立子项目）。bench 三个 mock_fn 的 wire format 完全不同，不能共享代码；exchange mock 是另一种生物。echo_mocks 与 core/ws_handshake 的重复是真问题，但用 1 个 commit 修就行，不需要顶层重组。最小动作：通用 helpers 推 eph-utils/eph-net，echo_mocks 复用 ws_framing/handshake。

**R6 维护性倡导者**（中立偏向重组）：
三年后接手项目的人问"加新 DPDK 集成测试要看哪些文件"——当前答案要穿透 3 层 + 一个反向依赖 + commit log 考古，令人沮丧。但反对新增顶层 `mock/`：每个顶层目录都是独立概念，新增需要充分理由。支持 R14 的"流向已存在的概念"路线。

### 第 2 轮 — 反驳与对线

**R8 反驳 R2**：TSC stamping 是协议层差异，模板化可以容纳。**反驳 R11**：3 个量化指标具体存在。**反驳 R6**：项目已有 10+ 库目录，新人对找东西有心智模型。

**R11 反驳 R8**：3 个指标全部可以用更小的动作满足——120 行用 1 commit 修；"未来加新协议"是 YAGNI；mock 的 happy-path 不需要 unit test，需要 integration test 而我们刚加了 7 个。**反驳 R14**：反向依赖是审美约束不是技术约束，header-only 零运行时成本。提议最小方案：承认 core/ 是事实上的"测试支持库"，更新文档，零代码改动。

**R14 反驳 R11**：契约约束是真技术问题——具体场景：开发者删 bench 死代码意外破坏 tests，因为 tests 在反向 include。**联盟 R6**，提出**联合方案 v1**：5 项 helpers 局部迁移到 eph-* 库，core/ 从 14 → 8 文件，依赖反转完全消除，无需新建顶层目录。**不支持**flatten latency（有 cxflags + linkgroups 需求），**不支持**latency_common.hpp 大合并。

**R2 反驳 R8**：bench 的 TSC stamping 与 server_work_ns spin 交织，不是简单的"send 前覆写 8 字节"；exchange mock 是 4 个 stream 调度器 + Poisson + 多 symbol 状态机，不能模板化。**联盟 R14+R6**，接受联合方案 v1，要求每项独立 commit + 全量 build 验证。

**R6 反驳 R8**：顶层目录的认知成本和深层目录不一样——`mock/` 在顶层意味着新人 ls 项目根时要问"这是干嘛的？和 tests 有什么区别"。R14 的"流向已存在概念"是 onboarding 的最优策略。**补充**：联合方案 v1 漏了一项——echo_mocks.hpp 应改用 ws_framing/ws_handshake，cleanup C3 遗漏。

### 第 3 轮 — 收敛与剩余分歧

**R8 部分让步**：放弃顶层 mock/ 子项目，接受联合方案 v1，但坚持必须包含 echo_mocks.hpp WS 复用。**承认**：YAGNI 反驳和"行为差异不可框架化"反驳击中要害。

**R11 部分让步**：承认"开发者删 bench 死代码破坏 tests"是真实风险，反向依赖不只是审美问题。接受联合方案 v1，**条件**是每项独立 commit + 全量 build 验证。坚持否决 latency_common.hpp 大合并 和 mock/ 顶层子项目。

**R14+R6+R2 联盟共识**：联合方案 v1 + R6 补充的 echo_mocks WS 复用 = 最终方案。

### 第 4 轮 — 终止条件检验

5 个角色全部已收敛，无新论点。终止。

---

## 最终方案

### 核心决策

不做用户提案的"独立 mock/ 子项目 + flatten latency + 大合并"。改为**「局部促进」路线**：把通用 helpers 推到 eph-* 库，把 test/bench 共享但非通用的部分放正确的 namespace，latency/ 保持作为 xmake 子项目。

### 6 项变更（按建议执行顺序）

| # | 动作 | 当前 | 目标 | 收益 |
|---|---|---|---|---|
| 1 | echo_mocks.hpp 复用 WS helpers（C3 遗漏修复） | 内联 80 行 SHA-1 + 40 行 base64 + 40 行 WS framing | `#include core/ws_framing.hpp` + `core/ws_handshake.hpp` | -160 行重复代码 |
| 2 | signal.hpp 推到 eph-utils | bench-only path | `eph-utils/include/eph/utils/signal.hpp` | 消除反向依赖 1/5 |
| 3 | socket_bind.hpp 推到 eph-net | bench-only path | `eph-net/include/eph/net/posix_listener.hpp` | 消除反向依赖 2/5 |
| 4 | socket_io.hpp 推到 eph-net | bench-only path | `eph-net/include/eph/net/posix_io.hpp` | 消除反向依赖 3/5 |
| 5 | netns.hpp 推到 eph-utils | bench-only path | `eph-utils/include/eph/utils/linux/netns.hpp` | 消除反向依赖 4/5 |
| 6 | dpdk_env.hpp 推到 eph-dpdk test namespace | bench-only path | `eph-dpdk/include/eph/dpdk/test/bench_env.hpp` | 消除反向依赖 5/5 |

每项独立 commit，每项 commit 后**全量 build**所有 lat_*_dpdk + test_dpdk_e2e + tests/unit/bench 测试。

### 留在 latency/core/ 的 8 个文件
config.hpp, runner.hpp, sample.hpp, scenario_concept.hpp, tsc_protocol.hpp, ws_client.hpp, ws_framing.hpp, ws_handshake.hpp（最后两个未来 consumer 数 ≥ 3 时再迁移）

### 明确否决的事

| 否决项 | 否决理由 |
|---|---|
| 新增顶层 mock/ 子项目 | YAGNI；mock 之间差异在行为而非 framing；现有 6 个 mock 中只有 echo_mocks 和 ws_framing/handshake 真正可共享 |
| flatten benchmarks/latency | latency 有 bench-specific cxflags + DPDK PMD linkgroups，需要独立 xmake target |
| latency_common.hpp 大合并 | 14 个职责清晰的文件就是好的；合并 = 反模式 |
| 模板化 TcpEchoMock<WireProtocol> | bench TSC stamping 与 work spin 交织；exchange mock 是另一种生物 |

### 已解决的分歧

| 分歧点 | 解决方式 | 关键论据 |
|---|---|---|
| 是否新建 mock/ 顶层子项目 | 否 | YAGNI + 行为差异不可框架化 + 顶层目录认知成本 |
| 是否做 latency_common.hpp 大合并 | 否 | 按职责拆分的小文件优于大杂烩 |
| 反向依赖是技术问题还是审美问题 | 是技术问题（契约违背） | 开发者删 bench 死代码意外破坏 tests 的真实场景 |
| 是否保留 latency/ 作为 xmake 子项目 | 保留 | bench 自有 cxflags + linkgroups 需求 |
| echo_mocks.hpp 80 行 SHA-1 内联 | 删除，复用 ws_handshake.hpp | C3 遗漏 |
| 通用 helpers 推到 eph-* | 是，分 5 个独立 commit | R14+R6+R2+R11 一致 |

### 未解决的权衡

| 冲突 | 选项 A | 选项 B | 建议 |
|---|---|---|---|
| ws_framing.hpp / ws_handshake.hpp 是否一并推到 eph-net | R14: 推 | R2/R6: 留 latency | 当前留 latency——只有 2 consumers，未达迁移阈值。consumer ≥ 3 时再做 |
| socket helpers 推 eph-net 时命名 | posix_* | kernel_* | 用 posix_listener.hpp / posix_io.hpp，与 eph-net 现有 socket_transport 命名一致 |

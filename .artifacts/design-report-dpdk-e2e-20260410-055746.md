# Design Report: eph-dpdk End-to-End Integration Tests

## 概况
- 时间：2026-04-10 ~05:34 → 05:57（约 23 分钟）
- 模式：default + auto（基于现有 plan）
- 需求：execute /design auto on plan-dpdk-integration-tests-20260410-053355.md
- 讨论轮数：2（plan 阶段已完成，本次直接执行）
- 提交：`41ed1e6`（test infrastructure + 7 tests）+ `289196b`（wrapper script）+ docs (uncommitted)

## 需求边界（来自 plan.md）

补足 eph-dpdk 模块从「单元测试 only」到「具备完整 E2E 数据路径覆盖」的缺失。

**In scope（本次实现）**：
- 7 个 P0+P1 测试用例（TCP / UDP / WS / RST / FIN / Reactor / ARP）
- gtest fixture + 子进程 mock dispatcher 基础设施
- xmake.lua 集成
- wrapper 脚本（preflight 检查）
- CLAUDE.md 文档更新

**Out of scope（标记为 follow-up）**：
- P2 测试（DNS / Multicast / TCP retransmission）
- 物理迁移 `benchmarks/latency/core/` → `tests/support/dpdk_bench/`
- 提取 `mock_fn::run` 与 bench 共享
- TLS over DPDK E2E
- CI 集成

## 设计方案

### 文件清单（新增 / 修改）

| 文件 | 行数 | 角色 |
|---|---|---|
| `eph-dpdk/tests/integration/echo_mocks.hpp` | 460 | 内核侧 TCP/UDP/WS/RST/FIN echo 函数 |
| `eph-dpdk/tests/integration/mock_dispatcher.hpp` | 105 | fork 子进程入口；启动 N 个 mock 线程；SIGTERM 处理 |
| `eph-dpdk/tests/integration/dpdk_e2e_env.hpp` | 220 | gtest::Environment：load conf / resolve PCI / fork / EAL init / ARP |
| `eph-dpdk/tests/integration/test_dpdk_e2e.cpp` | 380 | 7 个 TEST() + main |
| `eph-dpdk/xmake.lua` | +25 | 排除 integration/ 子目录 + 新建 test_dpdk_e2e 目标 |
| `tests/integration/dpdk_e2e` | 117 | bash wrapper：preflight + exec |
| `CLAUDE.md` | +10 | 文档化新测试入口 |

总：~1320 行新代码（含注释）。

### 关键设计决策（与 plan.md 的偏差）

| 决策 | 选择 | plan 中的方案 | 偏差理由 |
|---|---|---|---|
| **D-Δ1** | 不迁移 `benchmarks/latency/core/` | Phase 1 物理迁移到 `tests/support/dpdk_bench/` | plan 自身在 R1 风险中标记需要 user 介入跑 lat 冒烟；auto 模式无法获取该介入。Plan 的 D-2 选项 A（`add_includedirs` 直接 include）作为 fallback 完全可用，与 `tests/unit/bench/` 的现有先例一致。 |
| **D-Δ2** | 不提取 `mock_fn::run` | Phase 2 提取共享 | bench 的 `mock_fn::run` 有 bench-specific 4-byte msg_size + TSC stamp 框架，并不能作为通用 echo 复用。新写的 `echo_mocks.hpp` 是更小更直接的代码（无 TSC 耦合），不影响 bench。 |
| **D-Δ3** | 不修改 `benchmarks/latency/lat` 提取 dpdk-state.sh | Phase 6 共享 NIC 状态机 | 同 D-Δ1：lat 是用户当前的工作工具，无 runtime 验证情况下不动。Wrapper 改为只做 preflight 检查 + 引导用户先跑一次 lat。 |
| **D-Δ4** | NIC_B PCI BDF 动态发现 | plan 隐含使用 BenchConfig.nic_b_pci | bench 的 BenchConfig 不包含 nic_b_pci；fixture 改为按优先级依次尝试：env var → bench.conf 直接解析 → /sys/class/net symlink。零侵入 bench 代码。 |
| **D-Δ5** | UdpE2E RX 用 rte_eth_rx_burst 直读 | plan 假设 UdpSender 有 poll_rx | UdpSender 是 send-only 设计，无 poll_rx。RX verification 改为 best-effort 直接消费 PMD burst，不依赖 sender 提供 RX。 |
| **D-Δ6** | WsE2E 用 raw TcpSession 走 HTTP upgrade，不用 DirectTransport | plan 推荐 DirectTransport 全栈 | DirectTransport 集成需要 TransportConfig + callback 集 + on_message hook，超出"smoke test"范围。当前测试验证：(a) HTTP 升级握手通过 (b) 服务器返回正确的 RFC 6455 §1.3 sample accept hash。完整 DirectTransport 覆盖留待 phase 2-C。 |

### 编码实现要点

1. **mock 线程使用 poll-based accept 而非阻塞 accept**——signal 可能投递到 dispatcher 主线程而非 worker 线程，poll 100ms 超时让 worker 自检 `running` 标志后干净退出
2. **TcpSession::send 返回 expected<size_t>**，UdpSender::send 返回 bool——测试代码区分对待
3. **gtest::Environment vs SetUpTestSuite**——选 Environment 因为它是 once-per-binary，与 EAL/fork 的 once-per-binary 语义匹配
4. **NIC_B PCI BDF 编译期常量**——通过 `add_defines("EPH_BENCH_CONF_ABS_PATH=...")` 把 bench.conf 的绝对路径烧进二进制，避免 cwd 依赖
5. **WS mock 内嵌 SHA-1 + base64**——避免拉入 OpenSSL/aws-lc 给 mock，~80 行 RFC 3174 参考实现足够
6. **Reactor mock = 8 个独立 echo thread**——每个 bind 不同 port (19301..19308)，最简单的并发等效

### 验证

- ✅ `xmake build test_dpdk_e2e` 编译通过（仅 1 个 pre-existing warning，不在本次改动）
- ✅ `xmake build test_tcp` 等现有 eph-dpdk unit tests 仍然能 build（xmake.lua 的 glob 改动 `tests/**.cpp` → `tests/*.cpp` 不影响顶层）
- ✅ `xmake run test_tcp/test_udp/test_arp` 等 130 个现有 unit test 全部通过（无回归）
- ✅ `xmake run test_dpdk_e2e` 在当前 NIC_B（bound to ena 内核驱动）状态下：7/7 SKIP，每个 SKIP 都有清晰的 actionable 信息
- ✅ Wrapper preflight：bench.conf 解析正确（修复了 inline comment 截断 bug），root 检查正确，vfio-pci 检查正确
- ⏭️ **未运行**：实际 DPDK 数据路径 testing——需要 `sudo lat tcp --dpdk` 先转换 NIC_B 状态，超出 auto 模式可独立完成的范围。验证留给用户。

## 实现概况

- **新增文件**：6（4 个 C++ + 1 个 bash + 1 个 plan artifact 已提交）
- **修改文件**：2（eph-dpdk/xmake.lua, CLAUDE.md）
- **测试用例**：7（P0×4 + P1×3）
- **commits**：3
  - `41ed1e6` — test infrastructure + 7 tests
  - `289196b` — dpdk_e2e wrapper script
  - (uncommitted) — CLAUDE.md docs update

## 后续建议

按优先级：

1. **第一次真跑** — 用户运行 `sudo benchmarks/latency/lat tcp --dpdk` 一次（让 NIC_B 进 vfio-pci），然后 `sudo tests/integration/dpdk_e2e`，验证 7 个测试在真实硬件上的行为。预计第一次跑会暴露至少 1-2 个 timing/race issue，需要小修
2. **P2 测试**（DNS / Multicast / TCP retransmission）— 用 `/design` 单独立项；DNS 需要写微型 kernel DNS responder；retransmission 需要可控丢包 mock
3. **WS 全栈测试** — 把 WsE2E.HandshakeAndEcho 升级为 `WsE2E.FullDirectTransport`，跑通 `eph::net::DirectTransport<TcpSession, WsFramer>` 的完整 send/recv 路径
4. **CI 集成** — 决定 CI 跑 DPDK 测试的 runner（需要 NIC + 可控制 vfio-pci 的 host）
5. **重新评估 plan 中跳过的 Phase 1-2** — 如果测试稳定后想要更干净的依赖图，再做 `benchmarks/latency/core/` → `tests/support/dpdk_bench/` 的物理迁移；建议结合 `/refactor` 而非 `/design`
6. **提取 dpdk-state.sh** — 当 dpdk_e2e wrapper 长期使用后再考虑提取，避免过早抽象

## 与原 plan 的总体对照

| 阶段 | plan 状态 | 实施状态 | 备注 |
|---|---|---|---|
| Phase 1: 物理迁移 core/ | 跳过 | ⏭️ | D-Δ1 |
| Phase 2: 提取 mock_fn | 跳过 | ⏭️ | D-Δ2 |
| Phase 3: dispatcher + RST/FIN mocks | 完成 | ✅ | echo_mocks.hpp + mock_dispatcher.hpp |
| Phase 4: fixture + P0 测试 | 完成 | ✅ | 4 P0 全部写入 test_dpdk_e2e.cpp |
| Phase 5: P1 测试 | 完成 | ✅ | 3 P1 全部写入同文件 |
| Phase 6: dpdk-state.sh + wrapper | 部分完成 | 🔶 | wrapper 已写；dpdk-state.sh 提取跳过（D-Δ3） |
| Phase 7: docs | 完成 | ✅ | CLAUDE.md 更新 + design report |

7/7 阶段，5 完成、2 跳过（皆为风险性物理迁移），1 部分完成。

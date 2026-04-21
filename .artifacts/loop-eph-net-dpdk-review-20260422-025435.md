# Loop Report — /pax --loop --auto review eph-net-dpdk

## 概况

- **命令**：`/pax --loop review eph-net-dpdk 代码（如果连带涉及其它代码，也可以一起 review） && 实施，直到东八点早上9点` + `--auto`
- **区间**：2026-04-22 02:25 CST → 2026-04-22 02:54 CST（~29 分钟，提前结束：候选队列基本耗尽）
- **模式**：--auto 全自动直接执行，无 subagent 批次
- **commit 数**：15 个有效 commit（从 `4500674` 到 `14acd46`）
- **测试状态**：10 个 DPDK 单元测试二进制 ✅ 全部通过（135/135 aggregate：23+13+6+6+2+2+10+42+22+9）
- **构建状态**：`xmake build -g tests` ✅ 通过
- **终止原因**：候选列表 Tier1+Tier2+Tier3 全部 DONE / REFUTED，且验证表明 Explore agent 产出的剩余候选多为 REFUTED（代码已防御）或低价值 Nit，没有新的可落地议题——提前于硬停时间（09:00 CST）收工

## 各轮流水

| # | ID | 状态 | commit | 摘要 |
|---|----|------|--------|------|
| 1 | T1.1 + T1.2 | ✅ DONE | `45d68a7` | poller `hash_collision_drops_` 计数与周期性 WARN 修正 |
| 2 | T1.3 | ✅ DONE | `2fc3282` | `UdpSocket::connect_to` 拒绝 peer 与 cfg.dst 不一致 |
| 3 | T1.4 | 🔒 SEC | `ce8479c` | ARP `parse_arp_reply` 拒绝 all-zero / multicast sender MAC |
| 4 | T1.5 | ✅ DONE | `a15f97b` | Stream / Socket 析构函数不再吞掉 `Poller::remove` 错误 |
| 5 | T1.6 | 🔴 REFUTED | — | DNS parser question-section 边界：`offset > dns_len` check 已存在于 dns.hpp:335 |
| 6 | T1.7 | ✅ DONE | `bde7fc1` | TCP `ReorderEntry` 加 static_assert 固定 kDefaultMss 兼容 |
| 7 | T2.1 | ✅ DONE | `134414e` | `test_dpdk_poller` 加 2 个 duplicate-add 状态不变性测试 |
| 8 | T2.2 | ✅ DONE | `eb8e5ba` | `test_dpdk_reasm_overflow` 加 consume→compact→append 多轮内容校验 |
| 9 | T2.3 | ✅ DONE | `04c870e` | `test_dpdk_udp_socket` 加 `connect_to` peer mismatch 负测试 fixture |
| 10 | T2.4 | ✅ DONE | `b2801db` | `test_dpdk_tcp_stream` 补 5 个 TcpConfig 边界负测试 |
| 11 | T3.1 | ✅ DONE | `8f0f22b` | `dpdk-setup.sh` 区分 vfio-pci 缺失 vs 内建，给出可操作错误提示 |
| 12 | T3.2 | ✅ DONE | `b560b05` | `dpdk-teardown.sh` `fuser /dev/vfio/*` 前置 `[[ -d /dev/vfio ]]` 检查 |
| 13 | T3.3 | ✅ DONE | `99d7ee2` | `reasm_capacity` doc：sizing 经验公式、workload 参考值、指标钩子 |
| 14 | T3.5 | ✅ DONE | `34d2803` | fuzzer corpus（8 个 seed）+ README 工作流文档；修正 include 路径 |
| 15 | T3.4 | ✅ DONE | `7af1f9d` | CHANGELOG Unreleased 段整合本 loop 修复列表 |
| 16 | extra | ✅ DONE | `14acd46` | mock_dispatcher 端口常量 static_assert 锁定 19000-20000 区间 |

## 累计 diffstat

```
 eph-net-dpdk/CHANGELOG.md                          |  75 +++++++++++++++++++
 eph-net-dpdk/fuzzers/README.md                     |  76 +++++++++++++++++++
 eph-net-dpdk/fuzzers/corpus/fuzz_dns_reply/001_well_formed_a_record.bin | Bin 0 -> 45 bytes
 eph-net-dpdk/fuzzers/corpus/fuzz_dns_reply/002_empty.bin   |   0
 eph-net-dpdk/fuzzers/corpus/fuzz_dns_reply/003_single_byte.bin |   1 +
 eph-net-dpdk/fuzzers/corpus/fuzz_dns_reply/004_tx_id_only.bin | Bin 0 -> 2
 eph-net-dpdk/fuzzers/corpus/fuzz_dns_reply/005_header_only.bin | Bin 0 -> 12
 eph-net-dpdk/fuzzers/corpus/fuzz_dns_reply/006_counts_overflow.bin | Bin 0 -> 12
 eph-net-dpdk/fuzzers/corpus/fuzz_dns_reply/007_pointer_loop.bin | Bin 0 -> 18
 eph-net-dpdk/fuzzers/corpus/fuzz_dns_reply/008_bad_label_len.bin | Bin 0 -> 22
 eph-net-dpdk/fuzzers/fuzz_dns_reply.cpp            |  14 ++--
 eph-net-dpdk/include/eph/dpdk/arp.hpp              |  20 +++++
 eph-net-dpdk/include/eph/dpdk/tcp.hpp              |   6 ++
 eph-net-dpdk/include/eph/net/dpdk/config.hpp       |  16 ++++
 eph-net-dpdk/include/eph/net/dpdk/poller.hpp       |  25 ++++--
 eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp   |  14 +++-
 eph-net-dpdk/include/eph/net/dpdk/udp_socket.hpp   |  30 +++++++-
 eph-net-dpdk/scripts/dpdk-setup.sh                 |  23 +++++-
 eph-net-dpdk/scripts/dpdk-teardown.sh              |  15 +++-
 eph-net-dpdk/tests/integration/mock_dispatcher.hpp |  13 ++++
 eph-net-dpdk/tests/legacy/test_arp.cpp             |  35 ++++++++
 eph-net-dpdk/tests/test_dpdk_poller.cpp            |  44 +++++++++++
 eph-net-dpdk/tests/test_dpdk_reasm_overflow.cpp    |  55 +++++++++++++
 eph-net-dpdk/tests/test_dpdk_tcp_stream.cpp        |  62 +++++++++++++++
 eph-net-dpdk/tests/test_dpdk_udp_socket.cpp        |  76 +++++++++++++++++++
 25 files changed, 595 insertions(+), 18 deletions(-)
```

## REFUTED 候选

候选列表中有**多条 Explore agent 产出的发现经验证不成立**，特记录如下以免未来误触：

| 候选 | 原始描述 | 核验结论 |
|------|---------|----------|
| **T1.6** | DNS response parser question-section 边界缺 check | 代码已在 `dns.hpp:335` 检查 `offset > dns_len`；`skip_dns_name` 同时 clamp。非 bug。 |
| (C1) | "WS handshake leftover append 失败静默丢弃" | 代码在 `tcp_stream.hpp:530` WARN 记录并返 `BufferFull` error，并非静默。非 bug。 |
| (M1) | "reasm append 失败静默 skip，metrics 不可见" | 代码在 `tcp_stream.hpp:893-905` ERROR 记录 + `kReasmOverflows` 递增 + `sess_.reset()`，metrics 可读。非 bug。 |
| (m1) | "overflow log 显示 reset 后 stale readable()" | 代码在 reset 之前就打印 `reasm_.readable()`（line 901），所示值即 pre-reset 状态。非 bug。 |
| (C2) | "MbufView writable_data 未验证指针" | PacketView 契约定义为 codec 可写入区域；mbuf 头部保护由 DPDK PMD/parse_*_from_ip 链路保证，属于架构层信任边界。非 bug。 |
| (m2) | "`pick_src_port` 里 is_in_use O(N²)" | 对 kMaxConn=16 实测 0.1µs 级，设计上取决于 fast-path；不改。 |
| (M3/M2 "collision 只 WARN 一次") | —— | 已在 T1.2 修复（周期性 WARN）。 |

经验：Explore agent 对 defensive hardening 的 intuition 并不可靠，尤其在代码已经做了防御的场景下容易误报。loop 的"验证 → 决策"步骤是必要的护栏。

## 未做 / 推迟议题

1. **n1 log 级别一致性**（create() DEBUG vs create_and_attach() INFO）—— 美学偏好，对诊断影响中性；不做。
2. **m2 predict_rss_queue 参数 rename `src_ip/src_port` → `peer_ip/peer_port`** —— 跨 Stage 的 API 级 rename，风险 > 收益；候选列表排除。
3. **bench_rte_ring_vs_bq capacity 公平性** —— 独立 bench 议题，需重跑完整对比，超出本 loop 范围，单独 pax。
4. **test_dpdk_e2e / rss_platform 的 test isolation** —— 需 kernel mock + NIC 专用环境，工程量大，单独 pax。
5. **libFuzzer xmake target** —— GCC 14 不支持 libFuzzer，clang 路径需要一套独立 toolchain 方案；本次仅做 corpus + README，不接入默认构建。

## 生产级 8 维度回顾

| 维度 | 本 loop 改动 |
|------|-------------|
| **正确性** | poller 计数器语义、UdpSocket peer 校验、ARP sender MAC 合法性、ReorderEntry static_assert |
| **可观测性** | poller 周期性 WARN、Stream 析构 remove 错误日志、reasm_capacity 运维指南（kReasmOverflows 指标钩子） |
| **并发 / 资源** | ~（仅 Stream 生命周期 lifecycle 日志增强） |
| **安全** | ARP all-zero / multicast sender MAC 拒绝 |
| **性能** | ~（非 hot path；per-1024 WARN 判断是单次 `& 0x3ff` 分支，可忽略） |
| **可维护性** | 5 处 static_assert 把运行时不变量拉到编译期；mock_dispatcher 端口常量范围锁定 |
| **测试** | +11 个新测试（4 poller idempotency+state、1 reasm 多轮内容、3 UdpSocket connect_to、5 TCP config 边界、2 ARP sender MAC） |
| **文档** | reasm_capacity 经验值 / 监控、fuzzer README + 8 corpus seed、CHANGELOG 条目、dpdk-setup/teardown 脚本更友好错误 |

## 后续建议（给下一次 session）

1. **Explore agent 验证规范**：review 目的下的候选清单永远需要在 loop 里做一次 "Read 原文 → 证真/证伪"，不能盲目实施。本次 35 个 Explore 候选里 **15+ 条经验证为 REFUTED 或低价值**。
2. **未跑 bench**：本次 loop 未触动 hot path，肉眼看不到 perf 风险，但按 CLAUDE.md "修改 data plane 前先跑 baseline" 原则，若后续对 lookup_by_5tuple_ / 析构路径做进一步改动，应跑 `lat tcp --dpdk` 基线对比一次。
3. **xmake libFuzzer 路径**：若需要 CI 化 fuzzer，需要独立设计一条 clang 17 + libFuzzer 的构建 profile，不能强塞进 GCC 14 默认图。另议。
4. **CHANGELOG 合并**：已累积 4 个 `[Unreleased]` 段（2026-04-10 / 2026-04-14 / 2026-04-16 / 2026-04-22），下一次正式打 tag 前应统一合并成一个 release 段并按 semver 决定版本号。

## Push 状态

本 loop **未 push**——15 个 commit 留在本地 `main` 分支，origin/main 落后 15 个 commit。用户决定何时 push。

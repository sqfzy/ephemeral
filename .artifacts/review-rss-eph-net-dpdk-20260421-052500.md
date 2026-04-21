# Code Review Report

## 元信息
- 时间：2026-04-21 05:25
- 耗时：≈4 min
- Diff 来源：`main..worktree-rss-dpdk` (10 commits)
- 审查范围：30 files, +1567 / -1552
- 审查维度：all
- 构建状态：✅ 通过（`xmake build -g tests -g benchmarks`，27.8s, 0 errors）
- 测试状态：✅ 38/38 unit (`test_flow_steering`) + 1/1 NIC integration (`test_dpdk_rss_platform`)

---

## Review 摘要

### 变更概况
- 文件数：30
- 增删：+1567 / -1552（净 +15）
- 主要变更：RSS / multi-queue 支持端到端 — `eph/dpdk/flow_steering.hpp` 搬到
  `eph/net/dpdk/`，加 Toeplitz 预测器；`Platform` 集成 RSS 配置 + Poller
  注册表；`DpdkTcpStream/DpdkUdpSocket` 新增 turnkey `create_and_attach +
  pin_to_queue`；`bench.conf` schema + plumbing；退役 `RxDispatcher`（619L 删除）

### 总体评价
设计方向正确：API 分层清晰（hot path 不变 + opt-in turnkey），向后兼容
（默认 `enable_rss=false` 行为 = main HEAD），ENA-specific fallback graceful。
3 个值得重视的缺陷：(1) `find_src_port_for_queue` 性能 O(N) NIC 系统调用、
(2) `Platform` 的 `dispatch_mode` 与 `rss_active` 可能不一致让用户混淆、
(3) `create_and_attach` 端到端 runtime 行为没有测试覆盖。其余 polish 级别。

### 问题统计
- 🔴 Critical：0
- 🟡 Major：3
- 🔵 Minor：5
- 💬 Nit：3

### 结论
**APPROVE with recommended fixes** — 没有 Critical 阻塞 merge；Major 项中
M1 perf 与 M3 test gap 建议 merge 前修复，M2 是设计澄清可 follow-up。

---

## 🟡 Major

### M1. `find_src_port_for_queue` 在每次循环都重新查询 NIC RSS 配置

**文件**：`eph-net-dpdk/include/eph/net/dpdk/flow_steering.hpp:596-616`
**类型**：性能
**描述**：`find_src_port_for_queue` 在 RSS+pin 模式下被
`Stream::create_and_attach` 调用，最坏情况扫描 32768..60999 共 28232 个端口。
每次循环调 `predict_rss_queue` (line 533+)，后者每次都执行：
```
rte_eth_dev_rss_hash_conf_get(port, ...)     // syscall #1
rte_eth_dev_info_get(port, ...)              // syscall #2
rte_eth_dev_rss_reta_query(port, ...)        // syscall #3
```
RSS key + RETA 在循环内**完全不变**。28k iterations × 3 syscalls = ~85k
冗余 NIC 调用。在 connect 路径上这可能引入秒级延迟，而 HFT 客户端的
connect path 应该是 ms 级。

**建议**：把 RSS key + RETA 查询提取到 `find_src_port_for_queue` 顶部一次性
完成，循环内只做 `toeplitz_hash_ipv4` + `queue_for_hash`。这把 N 次 NIC
syscall 降到 2 次。

### M2. `Platform::dispatch_mode()` 与 `rss_active` 可能矛盾，无方法分辨

**文件**：`eph-net-dpdk/include/eph/dpdk/platform.hpp:737-758`
**类型**：设计 / 可观测性
**描述**：当 ENA 这类 PMD 拒绝 `rte_eth_dev_rss_hash_update` 时，
`Platform::create()` 流程：
1. `configure_rss()` 失败 → 日志 WARN，`impl_->rss_active = false`
2. 但 port 仍然以 `nb_rx_queues=N` 启动（NIC 自带 default RSS 还在工作）
3. `detect_rx_dispatch_mode()` 探测时 NIC 报 `RTE_ETH_RSS_NONFRAG_IPV4_TCP`
   支持 → 返回 `RssPartitioned`

结果：`platform.dispatch_mode() == RssPartitioned` 但 `rss_active == false`。
bench 日志实测：
```
configure_rss(...) failed: rte_eth_dev_rss_hash_update failed: -95 --
  continuing in single-queue Software fallback
Platform ready (port=0, nb_rx_queues=2, rss_active=false,
                dispatch_mode=RSS Partitioned)
```
而 `create_and_attach` 完全靠 `dispatch_mode()` 决策 — 它会走 RSS 分支
（`predict_rss_queue` 等），但用户读注释/日志会困惑："到底是 single-queue
Software 还是 RSS？"

**建议**：二选一：
- (a) `rss_active=false` 时强制 `dispatch_mode = Software`（最严谨）
- (b) 把 `rss_active` 暴露为 `Platform::rss_hash_actively_managed() const
  noexcept`，并在 `dispatch_mode()` 的 doc 里明确"RssPartitioned 不保证我们
  own RSS key"

(a) 更安全，但会改变 ENA 上 `create_and_attach` 行为 — 它会落到 Software
单队列 path，与现状一致 (single stream 实测就走 queue 0)。

### M3. `create_and_attach` 没有任何端到端行为测试

**文件**：`eph-net-dpdk/include/eph/net/dpdk/{tcp_stream,udp_socket}.hpp`
（新增 ~280 行）+ `tests/integration/test_dpdk_rss_platform.cpp`
**类型**：测试覆盖
**描述**：stage 4 加的两个 turnkey factory 是核心新 API，但目前只有：
- 编译时 verify（build pass = signature 对）
- `test_dpdk_rss_platform.cpp` 测的是 `register_poller` / `poller_for_queue`
  / `dispatch_mode` 接口 — **不**调 `create_and_attach`

实际行为完全没 runtime verify：
- Software 模式 + `pin_to_queue=0` → attach 是否成功？
- RSS 模式 + `pin_to_queue=nullopt` → predict_rss_queue 算出的 qid 与 NIC
  真实 hash 是否一致？
- RSS 模式 + `pin_to_queue=value` → src_port 反搜后 attach 是否成功？
- FlowDirector 模式 → install_flow_rule 后 SYN-ACK 是否真到目标 queue？

**建议**：扩展 `test_dpdk_rss_platform.cpp` 加 3 个用例（fork 一个 kernel
TCP echo mock 在 NIC_A，client 用 `create_and_attach` 在 NIC_B vfio 跑）。
最后一个最有价值 — 验证 `find_src_port_for_queue` 反搜的 src_port 真能让
流量落到目标 queue。

---

## 🔵 Minor

### m1. `predict_rss_queue` 参数命名 `src_ip/src_port` 容易让用户误用

**文件**：`flow_steering.hpp:521-534`
**类型**：设计 / 文档
**描述**：参数 `src_ip / src_port` 实际意指 RSS 输入端的 "source"，对 RX
方向的客户端 = REMOTE peer。文档注释里说明了，但 `create_and_attach` 内
调用时需要 `(remote_ip=dst_ip, remote_port=dst_port)` 的反映射。

**建议**：rename 到 `peer_ip/peer_port + local_ip/local_port`（语义清晰），
或保留 `src/dst` 但加 doxygen 提醒 "do NOT pass your local IP"。

### m2. `kMaxRssQueues = 64` 写死，没有 static_assert 与
`RTE_MAX_QUEUES_PER_PORT` 同步

**文件**：`platform.hpp:131`
**类型**：可维护性
**描述**：`kMaxRssQueues = 64` 是 Poller 注册表大小上限。ConnectX-6 等高端
NIC 支持 128+ queues，未来可能需要扩容。值与 DPDK 自身常量无关联。

**建议**：加一个 comment 明示限制 + 可调整路径。

### m3. `register_poller` 的 `void*` 类型擦除没有运行时类型校验

**文件**：`platform.hpp:329-345`
**类型**：设计 / 安全
**描述**：`Platform::register_poller(uint16_t qid, void* poller)` +
`Platform::poller_for_queue(qid)` 返 `void*`，调用方在 `create_and_attach`
内 `static_cast<DpdkPoller<void>*>(p_void)`。如果调用方注册的是不同模板
实例或完全错误的指针，编译期通过，运行时 UB。

**建议**：把 `void*` 换成 `DpdkPoller<void>*` — `create_and_attach` 已经
cast 到这个类型了，没必要再隐藏。

### m4. FlowDirector 路径上 install_flow_rule 与 add 之间存在短暂时间窗口

**文件**：`tcp_stream.hpp:678-700` (新版)
**类型**：正确性 / 并发
**描述**：`create_and_attach` FlowDirector 路径顺序：
```
1. Stream::create(cfg) → TCP handshake done, 5-tuple final
2. install_flow_rule → flow rule active in NIC
3. emplace flow_rule_ into stream
4. poller->add(stream)
```
步骤 2 完成后，匹配该 5-tuple 的入站 packet 立即 hash 到目标 queue；步骤
4 之前 Poller 还没注册这个 stream — 此时如果 peer 已发数据，先到的 packet
在目标 queue burst loop 里找不到匹配的 entry → 被静默 drop。

**建议**：reorder — 先 `poller->add(stream)`，**再** `install_flow_rule`。
这样 NIC 切流量到目标 queue 之前 stream 已可被找到。

### m5. `bench-report` markdown 的 RTT_ns 表格里 RSS-on 比 baseline 慢，
但 commit message 标 "zero regression"

**文件**：`.artifacts/bench-report-rss-multi-queue-20260421-050200.md` +
commit `822540b` message
**类型**：文档准确性
**描述**：报告自己的对比表格显示 RSS-on 2q 的 p99/p99.9/throughput 全部
退化 7-22%；commit message 在标题写 "default config zero regression"——
这两件事不冲突（baseline = default config 的 zero regression，这是对的；
RSS-on 的退化是因为 single stream 不利用多 queue 资源），但读者只看
commit message 容易误以为 RSS-on 也无回归。

---

## 💬 Nit

### n1. `LCORE_PER_QUEUE` 已声明 reserved，但 `bench.conf` 默认值 "4,5,6,7"
与 `NB_RX_QUEUES=1` 不一致
**文件**：`benchmarks/latency/bench.conf:42`

### n2. `tcp_stream.hpp:710` `poller->add()` 失败时 stream 已构造完且
flow_rule_ 已 emplace — 但 RAII 自动 unwind。代码读者会担心 leak；可加
comment 说明。

### n3. `bench_udp.cpp:3` 修正后的注释与文件实际内容覆盖范围一致；可加一句
解释为什么这个 bench 比早期更简短。

---

## 亮点

- **`platform.hpp:711-731`**：`configure_rss` 失败时 graceful degrade
  （log WARN + 继续启动）而不是返 error，让 ENA 这类 PMD 自然工作 — 设计
  判断正确。
- **`flow_steering.hpp` 5 个 Microsoft Toeplitz 验证向量**：known-answer
  test 是金标准，比 self-consistent property test 强得多。Bench 时一次跑
  过且全 5/5 命中，证明算法实现完全正确。
- **`test_dpdk_rss_platform.cpp` 折叠成 1 个 TEST**：DPDK port slot detach
  是个深坑，commit message 把根本原因写清楚了 — 后人调试会感谢。
- **commit 粒度**：10 个 commit 各代表一个原子改动，每个独立可 build &
  test，git bisect 友好。

---

## 推荐后续动作

按优先级：

1. **Merge 前修复 M1 perf**（`find_src_port_for_queue` 提 RSS conf 出循环外）
   — 30 行改动，单元测试 trivial 加。
2. **Merge 前修复 m4 race**（reorder add → install_flow_rule）— 5 行改动，
   FlowDirector path 不变更测试。
3. **Merge 后 follow-up**: M2 dispatch_mode 一致性澄清、M3 端到端测试。
4. 其余 minor / nit 见 lat 重构 / `/doc summary` 时一并清理。

---

## Diff 统计

```
30 files changed, 1567 insertions(+), 1552 deletions(-)

  .artifacts/bench-report-rss-multi-queue-20260421-050200.md     | 175 +++
  .artifacts/design-rss-eph-net-dpdk-20260421-044326.md          | 147 +++
  benchmarks/latency/bench.conf                                  |  16 +
  benchmarks/latency/core/config.hpp                             |  20 +
  benchmarks/latency/core/dpdk_env.hpp                           |  41 +-
  eph-net-dpdk/README.md                                         |   5 +-
  eph-net-dpdk/benchmarks/bench_tcp_header.cpp                   |  94 +-
  eph-net-dpdk/benchmarks/bench_udp.cpp                          |  67 +-
  eph-net-dpdk/docs/ONBOARDING.md                                |   3 +-
  eph-net-dpdk/include/eph/dpdk/packet_parse.hpp                 |  18 +-
  eph-net-dpdk/include/eph/dpdk/platform.hpp                     | 152 ++-
  eph-net-dpdk/include/eph/dpdk/rx_dispatcher.hpp                | 619 ---
  eph-net-dpdk/include/eph/dpdk/tcp.hpp                          |  18 +-
  eph-net-dpdk/include/eph/dpdk/test/dpdk_env.hpp                |  29 +-
  eph-net-dpdk/include/eph/net/dpdk/config.hpp                   |  27 +-
  eph-net-dpdk/include/eph/{=>net}/dpdk/flow_steering.hpp        | 237 +++-
  eph-net-dpdk/include/eph/net/dpdk/poller.hpp                   |  24 +-
  eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp               | 169 +++
  eph-net-dpdk/include/eph/net/dpdk/udp_socket.hpp               | 123 +++
  eph-net-dpdk/tests/integration/mock_dispatcher.hpp             |  12 +-
  eph-net-dpdk/tests/integration/test_dpdk_e2e.cpp               | 109 +-
  eph-net-dpdk/tests/integration/test_dpdk_rss_platform.cpp      | 239 +++
  eph-net-dpdk/tests/legacy/test_flow_protocol_and_multicast_boundary.cpp | 4 +-
  eph-net-dpdk/tests/legacy/test_rx_dispatcher.cpp               | 427 ---
  eph-net-dpdk/tests/legacy/test_udp.cpp                         |  11 +-
  eph-net-dpdk/tests/{legacy=>}/test_flow_steering.cpp           | 150 +-
  eph-net-dpdk/xmake.lua                                         |  21 +-
  examples/simple_hft_dpdk_rx_dispatcher.cpp                     | 151 ---
  xmake.lua                                                      |   8 -
```

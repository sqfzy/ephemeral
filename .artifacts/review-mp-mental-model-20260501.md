# Review: MP Mental Model — User Error → Silent Run Vulnerabilities

> Date: 2026-05-01
> Branch: main (post-MP-teardown-fix)
> Mode: --review --auto
> Scope: benchmarks/latency + eph-net-dpdk MP path

---

## Executive Summary

**5 known vulnerabilities verified, 1 false alarm, 3 new vulnerabilities found.**

Of the 5 originally identified:
- 4 confirmed real (Vuln 1, 2, 3, 5)
- 1 FALSE ALARM (Vuln 4 — mockex per-connection isolation IS correct)

3 new vulnerabilities surfaced during the silent-failure pattern scan:
- Vuln A: `EPH_LAT_AUTOJOIN_MAX_PROCS` silent atoi() coercion
- Vuln B: lat bash wrapper does not check mockex exit status
- Vuln C: mockex bind failure compounded with Vuln B (same root cause)

**Updated fix list: 6 actionable items (dropped Vuln 4, merged Vuln C into Vuln B).**

---

## Vulnerabilities

### Vuln 1: lcore cross-process conflict not detected
- **现象**: 两个 MP 进程指定同一 EAL lcore 完全不报错
- **触发条件**: `EPH_LAT_AUTOJOIN_LCORES="0,1"` 设给两个进程 / 或 `MpTopology` 里两个 ProcSpec 的 lcore 重叠
- **静默原因**:
  - `mp_registry.hpp:106-121` ProcSlot 只有 `claimed/tag/queue_lo/queue_hi/port_lo/port_hi` — **没有 lcore 字段**
  - `mp_registry.hpp:542-560` attach_secondary 只 cross-validate queue/port range，**不检查 lcore**
  - `dpdk_env.hpp:258-270` `EPH_LAT_AUTOJOIN_LCORES` 解析后直接传给 `create_via_autojoin`，无去重
- **当前行为**: OS 调度让两进程争同 CPU → p99 暴涨 5-10x，但 binary 不崩，JSON 输出"正常"
- **影响严重度**: 🔴 **H** — 用户以为在并行 bench，实际数据是 CPU 抢占噪声
- **修复成本**: 1 天（中复杂度，需要 ABI bump v2）
- **修复复杂度**: 中
- **建议方向**: ProcSlot += `uint64_t lcore_mask`；attach 时与已 claim slot 做集合交集检测；冲突→`Error::InvalidConfig`+列出冲突 lcore IDs
- **ROI**: 9/10

### Vuln 2: kill-9 stale slot 永久占位
- **现象**: secondary 异常退出后 ProcSlot.claimed 永远 1，primary 永远不 stop port
- **触发条件**: secondary `kill -9` / OOM / SIGSEGV in unrelated code
- **静默原因**:
  - `mp_registry.hpp:759-768` `release_()` 是纯 RAII，无 heartbeat
  - `mp_registry.hpp:707-749` `try_claim_free_slot` 不做 pid liveness check
  - `mp_registry.hpp:312-337` `count_alive_procs` / `is_last_alive_proc` 只读 claimed flag，不验进程存活
- **当前行为**: 下次 session 启动时如果 stale slot 还在，新 primary 看到"还有人活着" → 永远 defer teardown；`dpdk-teardown.sh` 兜底
- **影响严重度**: 🟠 **M** — 资源泄漏，需要外部脚本兜底；用户没跑 teardown 会撞 weird 现象
- **修复成本**: 半天（小复杂度，attach-time pid probe 而非 background reaper）
- **修复复杂度**: 低
- **建议方向**: ProcSlot += `pid_t pid`；attach_secondary 看到 stale slot 用 `kill(stored_pid, 0)` 探活，dead → CAS preempt + WARN log
- **ROI**: 7/10

### Vuln 3: pin_to_queue 实际不静默回退（部分误判）
- **现象**: 之前以为 brute-force 找不到 src_port 会回退到任意 port，**实际不会**
- **当前实际行为**:
  - `flow_steering.hpp:1411-1414` `find_src_port_for_queue` 找不到 → return `std::unexpected("no src port hashes to target queue")`
  - `tcp_stream.hpp:781-793` 上层捕获 → surface 为 `Error::InvalidConfig`，hard error，**不回退**
- **真正的静默风险**：`flow_steering.hpp:1399-1404` 注释承认存在历史 RSS key mismatch bug。如果 NIC 实际 RSS key 与软件预测不一致（ENA quirk 历史上有），predicted_queue ≠ actual_queue → reply 落到错误 queue，secondary 收 0 包
- **影响严重度**: 🟠 **M** — 实际硬错误已是正确行为；剩余风险是 RSS key probe 失败时的"软件认为对硬件不对"silent mismatch
- **修复成本**: 1 天（需要发探测包 + 观察实际落到哪个 queue 的运行时基础设施）
- **修复复杂度**: 中-高
- **建议方向**: post-handshake 发 1-2 个测试包，观察实际落 queue；不一致 → abort + 报告 NIC RSS key mismatch
- **ROI**: 4/10 — 修复复杂度高 vs 残留风险中等
- **建议**: **deferred**，记入 TODO，不在本批 fix

### ~~Vuln 4: mockex 多客户端 RTT 串扰~~ — **FALSE ALARM**
- **重新核查后**: code 实际是**完全 per-connection 隔离**的
  - `tcp_echo.hpp:65-91` `echo_client_loop<IoStream>` 每个客户端连接 spawn 独立线程 + 独立 scratch buffer
  - `tcp_echo.hpp:78-82` timestamp stamping 用 `monotonic_raw_ns()` per recv 调用，**没有共享 ts_buf**
  - `tcp_echo.hpp:122-132` `mockex_max_connections` 默认 1，硬上限 64
- **结论**: 不需要 fix；mental model 是对的（每客户端独立线程独立 buffer）
- **建议**: 从 fix 列表删除

### Vuln 5: 同 scenario 多进程 JSON 文件名互相覆盖
- **现象**: 两个 lat_tcp_dpdk 进程同时跑，JSON 输出文件名相同 → 后写的覆盖先写的
- **触发条件**: 7-process 并行 acceptance 跑同一 scenario 多份 / 用户主动 fan-out 测试
- **静默原因**:
  - `lat_tcp_loop.hpp:193-199` 输出 `lat_tcp_<backend>_rtt/tx/rx<suffix>` 其中 `suffix` 永远是空字符串（path A 删除后）
  - 没有 `_proc<N>` / `_pid<N>` 区分
- **当前行为**: 两份测量数据，最后写盘的赢；前者数据 silently 丢失
- **影响严重度**: 🟠 **M** — 数据丢失但不污染（被覆盖的那份没了，剩下的是真的）
- **修复成本**: 半天（小改动）
- **修复复杂度**: 低
- **建议方向**: lat_*_loop.hpp 输出文件名 append `_pid<getpid()>` 后缀（最简单），或 `_proc<self_index>` 当 MP 模式
- **ROI**: 8/10

### NEW Vuln A: `EPH_LAT_AUTOJOIN_MAX_PROCS` 静默 atoi 强转
- **现象**: 拼写错的 envvar 静默变成 max_procs=0
- **触发条件**: `EPH_LAT_AUTOJOIN_MAX_PROCS=bad`（typo）或 `MAX_PROCS=-1` 或省略前缀
- **静默原因**: `dpdk_env.hpp:258-260` `std::atoi(s)` 解析失败返回 0；负数 cast to uint32_t 变成 4294967295
- **当前行为**: `max_procs=0` → autojoin 报"no free slot"或拓扑失败；用户看到的错误信息不指向 envvar typo
- **影响严重度**: 🟡 **L** — 仅诊断；最终会失败但错误信息误导
- **修复成本**: 1 小时
- **修复复杂度**: 低
- **建议方向**: `strtoul` + range check `[1, 64]`；reject → `SPDLOG_ERROR("EPH_LAT_AUTOJOIN_MAX_PROCS='{}' invalid (expect 1..64)")` + 立即退出
- **ROI**: 6/10 — 成本极低，价值中等

### NEW Vuln B: lat bash wrapper 不检查 mockex exit status
- **现象**: mockex 启动失败（端口被占 / config 错 / scenario 没找到），lat wrapper 仍然 fork 客户端 → 客户端 hang 或 connection refused
- **触发条件**: 上次 mockex crash 留 socket TIME_WAIT；用户连续两次 `lat tcp` 没间隔 ≥120s；config 文件错
- **静默原因**:
  - `benchmarks/latency/lat:556-557` `mockex --scenario X &` 后台 fork，**不 capture exit code**
  - mockex 自己 ERROR + exit 1 是对的（`main.cpp:134-138`，`tcp_echo.hpp:133-139`）
  - 但 wrapper 不知道 mock 死了，继续起 client
- **当前行为**: client 跑 connect → hang 直到 timeout（30s），然后 "connect timeout" 错误 — 用户以为是网络问题
- **影响严重度**: 🟠 **M** — 不污染数据但浪费时间 + 错误诊断方向
- **修复成本**: 1 小时（bash 改动）
- **修复复杂度**: 低
- **建议方向**: 启动 mockex 后 sleep 0.5s 然后 `kill -0 $MOCK_PID` 探活；死了 → 立即报错退出 + 显示 mockex 日志 tail
- **ROI**: 8/10

### NEW Vuln C: mockex 端口冲突静默被 wrapper 吞 — 与 Vuln B 同根
- **机制**: 同 Vuln B；mockex 自己正确报错，但 wrapper 不传播
- **建议**: 与 Vuln B 合并修复

---

## ROI Sorted Fix List

| Rank | Vuln | Severity | Cost | ROI | 推荐 fix 顺序 |
|------|------|----------|------|-----|--------------|
| 1 | Vuln 1 (lcore conflict) | 🔴 H | 1 day | 9/10 | **fix 1** |
| 2 | Vuln 5 (JSON filename) | 🟠 M | 0.5 day | 8/10 | **fix 2** |
| 3 | Vuln B (mockex exit check) | 🟠 M | 1h | 8/10 | **fix 3**（也修 Vuln C） |
| 4 | Vuln 2 (stale slot) | 🟠 M | 0.5 day | 7/10 | **fix 4** |
| 5 | Vuln A (envvar parsing) | 🟡 L | 1h | 6/10 | **fix 5** |
| 6 | Vuln 3 (pin_to_queue probe) | 🟠 M | 1 day | 4/10 | **deferred (TODO)** |
| - | Vuln 4 (mockex mixing) | — | — | — | **DROPPED (false alarm)** |

## 推荐 fix 顺序（auto 接管将按此执行）

```
fix 1: Vuln 1 (lcore conflict) — ABI bump v2 + ProcSlot.lcore_mask
fix 2: Vuln 5 (JSON filename collision) — _pid suffix
fix 3: Vuln B+C (mockex exit status check) — bash wrapper
fix 4: Vuln 2 (stale slot pid probe) — same v2 ABI bump as fix 1
fix 5: Vuln A (envvar parsing strict) — strtoul + range check
doc 6: same-scenario fan-out semantics — dpdk-mp-teardown-protocol.md
```

## ABI 协调

fix 1 和 fix 4 都改 ProcSlot。**自动合并**：
- fix 1 commit 引入 v2 schema（添加 lcore_mask）
- fix 4 commit 在已 v2 schema 上添加 pid 字段（不再 bump version，因为已经 v2）
- ✅ 单次 ABI bump，两个 fix 共享

## Dropped / Deferred

### Dropped
- **Vuln 4 (mockex multi-client mixing)** — 代码 per-connection 完全隔离，mental model 错误。无需 fix。

### Deferred (TODO 留底)
- **Vuln 3 (pin_to_queue runtime probe)** — 修复成本高（需要运行时探测 NIC 实际 RSS 行为），ROI 低。建议未来若用户报告 RSS hash 不匹配的实际故障再做。当前 hard-error 路径已经覆盖最常见情况。

---

## 与现有 fix list 对比

原计划:
1. ~~lcore/cpu 跨进程冲突~~ → **保留**（ROI 第 1）
2. ~~pin_to_queue 静默回退~~ → **降级到 deferred**（实际已 hard error）
3. ~~kill-9 stale slot~~ → **保留**（ROI 第 4）
4. ~~mockex 多客户端串扰~~ → **DROPPED**（false alarm）
5. ~~同 scenario 语义文档化~~ → **升级为 fix**（实际是数据丢失，不只是 doc）

新增:
- **Vuln A**：envvar 解析硬化
- **Vuln B+C**：bash wrapper 检查 mockex 存活

总数：原 4 fix + 1 doc → 现 5 fix + 1 doc（doc 仍单独写，作为 fix 2 的 follow-up 文档）

---

## 推荐入口

```
开始 fix 1: lcore conflict detection (ABI bump v2 + ProcSlot.lcore_mask)
```

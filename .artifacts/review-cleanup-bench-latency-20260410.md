# Code Review Report — bench/latency cleanup commits

## 元信息
- 时间: 2026-04-10 ~05:00
- 范围: 7 commits `bcd6e71..2d37eff` (the bench/latency cleanup series authored in this session)
- 审查维度: all
- 构建状态: ✅ 通过
- 测试状态: ✅ 13/13 通过 (12 `test_bench_runner` + 1 `test_bench_no_dead_headers`)

---

## Review 摘要

### 变更概况
- 文件数: 11 (excluding .artifacts/)
- 增删: +857 / -168
- 主要变更:
  1. Delete dead `udp_client.hpp` (the only true positive from the structural audit)
  2. lat wrapper robustness: sysfs-driven NIC state detection (H1), idempotent `host_to_bench_ns` (H2), wedged state auto-recovery (H3), post-condition retry (M4), explicit `NIC_B_PCI` config key
  3. Doc updates: outlier semantics (M1), reproducibility checklist (M2), fix false "no formal unit tests" claim (M3)
  4. New tests: `BenchRunner` unit tests (12 cases) + dead-header lint test (1 case, regression-validated by canary)
  5. Move `setup_coalescing.sh` under `benchmarks/latency/scripts/` + update role-table comments in 3 scripts + add reproducibility checklist step 0

### 总体评价
高质量的清理工作。所有变更都有具体的失败模式作为驱动（不是凭空"重构"）。lat 包装器改动经过本次会话的真实失败 case 验证；新加的测试 12 + 1 都跑过了；regression-validate 通过 canary header 注入。**没有发现 Critical 或 Major 缺陷**。少数 Minor / Nit 都属于可改可不改的改善空间。

### 问题统计
- 🔴 Critical: 0
- 🟡 Major: 0
- 🔵 Minor: 4
- 💬 Nit: 4

### 结论
**APPROVE**. 本次清理已完成 + 已 push + 已经过 build & test 验证。Minor 项可作为未来的小幅改善，不阻塞。

---

## Minor 级别问题

### 🔵 Minor 1: `pci_driver()` 在 readlink 部分失败时可能返回 "."

**文件**: `benchmarks/latency/lat:153-157`
**类型**: 正确性

```bash
pci_driver() {
    local pci="$1"
    [[ -n "$pci" && -e "/sys/bus/pci/devices/$pci" ]] || { echo ""; return; }
    basename "$(readlink -f "/sys/bus/pci/devices/$pci/driver" 2>/dev/null)" 2>/dev/null || echo ""
}
```

**描述**: 如果 PCI 设备存在但没有任何驱动绑定（罕见状态：例如在 unbind 和 bind 之间的瞬间），`readlink -f /sys/.../driver` 在符号链接不存在的情况下会输出空字符串 + 非零退出。`basename ""` 会输出空 — 但 `basename` 本身也可能在某些 corner case 下输出 `.`。最终的 `|| echo ""` 只在 `basename` 失败时触发，而不是在 `basename` 输出为空时触发。

**影响**: 当前没有可观察的 bug — 调用方依赖返回值精确为 "vfio-pci" 或 "ena"，返回 `.` 也会判 false 走 fallback 路径。但函数语义和文档描述（"或 '' 如果没有"）有微小不一致。

**建议**:
```bash
pci_driver() {
    local pci="$1"
    [[ -n "$pci" && -L "/sys/bus/pci/devices/$pci/driver" ]] || { echo ""; return; }
    basename "$(readlink -f "/sys/bus/pci/devices/$pci/driver")"
}
```

### 🔵 Minor 2: `bench.conf` 的 `NIC_B_PCI` 值是 host 特定的，没有 placeholder 注释

**文件**: `benchmarks/latency/bench.conf:22-28`
**类型**: 设计 / 文档

提交了具体的 PCI 地址 `0000:28:00.0` — 这是当前 EC2 host 的值。其他人 clone 这个分支换 host 跑 bench 时要换。虽然 SERVER_IP/LOCAL_IP/GATEWAY_IP 也都是这样提交的（既有惯例 ✓），但**注释里没说明它是 host 特定的**。

**建议**: 注释加一句强调："Like SERVER_IP/LOCAL_IP, this value is specific to the host that committed it. Replace before running on a different host."

### 🔵 Minor 3: `test_no_dead_headers` 依赖 CWD 而非源码位置

**文件**: `tests/unit/bench/test_no_dead_headers.cpp:38-46` (`find_project_root`)
**类型**: 测试鲁棒性

`find_project_root` 从 `fs::current_path()` 开始向上 8 层找 `benchmarks/latency/core`。`xmake run` 通常 cwd = project root 所以 OK，但如果开发者从其它位置直接运行二进制就会失败。

**建议**: 用 `__FILE__` 宏作为锚点（编译期硬编码源文件路径），从那里向上找：
```cpp
fs::path find_project_root() {
    fs::path p = fs::path(__FILE__).parent_path();
    while (p.has_parent_path()) {
        if (fs::exists(p / "benchmarks" / "latency" / "core")) return p;
        p = p.parent_path();
    }
    return {};
}
```

权衡：binary 与编译时源码路径绑定，cross-machine 不可移植。但 unit test 二进制本来就不是 distribute 用的。

### 🔵 Minor 4: `unwedge_nic` 的 sysfs 写入静默失败

**文件**: `benchmarks/latency/lat:185-187`
**类型**: 可观测性

```bash
echo "$pci" > "/sys/bus/pci/drivers/$drv/unbind" 2>/dev/null || true
sleep 1
echo "$pci" > "/sys/bus/pci/drivers/$drv/bind" 2>/dev/null || true
```

**影响**: 如果 unbind/bind 失败（进程持有设备、驱动模块已卸载），用户只能看到 6 秒后的通用 "wedge auto-recovery did not produce a netdev within 3s" 错误。

**建议**: 把错误流式输出到 stderr 而非 dev null：
```bash
if ! echo "$pci" > "/sys/bus/pci/drivers/$drv/unbind" 2>&1; then
    log_warn "unbind write failed (continuing): $?"
fi
```

---

## Nit 级别建议

### 💬 Nit 1: `test_runner.cpp` 共享 `g_running` 全局
**文件**: `tests/unit/bench/test_runner.cpp:191-205`

每个 test 显式 `g_running.store(true)` 是 OK，但用 gtest fixture 的 `SetUp()` 更干净，并能删掉文件底部的 `ResetGRunning` 死代码（其只在进程退出时跑，不在测试间跑）。

### 💬 Nit 2: lat wedge 错误信息的命令替换在 die 时机求值
**文件**: `benchmarks/latency/lat:364-365`

`die "..." "Manual fix: sudo bash -c 'echo $(nic_b_pci) > .../$(pci_driver "$(nic_b_pci)")/unbind ...'"` — 在错误发生之前缓存到局部变量更稳。

### 💬 Nit 3: `pci_has_netdev` 用 `ls` 而非纯 shell glob
**文件**: `benchmarks/latency/lat:166`

`compgen -G` 或 `nullglob + array` 避免 fork 子进程。性能上无所谓（一次调用），可忽略。

### 💬 Nit 4: README 复现 checklist 步骤 0 用 markdown 编号 `0.`
**文件**: `benchmarks/latency/README.md:198-211`

GitHub 某些版本的 markdown 渲染可能把 `0.` 重新编号成 `1.`。建议改成"**Step 0 (do this first)**: Disable adaptive..." 或重新编号 1-8。

---

## 亮点

✅ **`benchmarks/latency/lat:135-198`** — `nic_b_pci()` + `pci_driver()` + `pci_has_netdev()` + `unwedge_nic()` 4 个 helper 是教科书式的"小函数+清晰责任"分解。每个函数有明确的输入、输出、错误条件 + 注释解释为什么需要它。

✅ **`tests/unit/bench/test_no_dead_headers.cpp`** — lint test 的 regression-validation 流程（注入 canary 死 header → 测试失败 → 删 canary → 测试通过）是这次清理里最干净的"我自己证明我有用"的例子。测试用 `<filesystem>` 静态分析 `#include` 图，无 build system 依赖。

✅ **`tests/unit/bench/test_runner.cpp:172-187`** — `PrepareFailureSkipsWindow` test 的注释明确写出 scenario contract（"prepare owns its own resources until it succeeds, so cleanup is NOT called"），这条不变量本来只藏在 `runner.hpp` 里没有显式文档化，测试既验证了它又文档化了它。

✅ **`benchmarks/latency/README.md:142-190`** ("Why max is sometimes huge") — 用真实的 baseline 数据作为示例（tcp/kernel/64B max=1.97 ms，ex_md_udp/kernel/256B max=15.3 ms），而不是空泛地说"max 不可靠"。

---

## Diff 统计

```
 benchmarks/latency/README.md                       | 103 ++++++-
 benchmarks/latency/bench.conf                      |   7 +
 benchmarks/latency/core/udp_client.hpp             |  80 -----
 benchmarks/latency/lat                             | 229 ++++++++++++---
 .../latency/scripts}/setup_coalescing.sh           |  32 +-
 benchmarks/latency/summary.md                      |  43 ++-
 eph-dpdk/scripts/dpdk-setup.sh                     |  15 +-
 eph-dpdk/scripts/dpdk-teardown.sh                  |  18 +-
 tests/unit/bench/test_no_dead_headers.cpp          | 159 ++++++++++
 tests/unit/bench/test_runner.cpp                   | 327 +++++++++++++++++++++
 tests/unit/bench/xmake.lua                         |  12 +
 11 files changed, 857 insertions(+), 168 deletions(-)
```

## 后续建议

如需自动修复 review 中的问题:
- Minor 1 (`pci_driver` simplification) → `/refactor` (~5 min)
- Minor 2 (bench.conf comment) → 直接 Edit (~1 min)
- Minor 3 (`__FILE__` lookup) → `/refactor` (~5 min)
- Minor 4 (sysfs write logging) → 直接 Edit (~3 min)
- Nit 1-4 → batch via `/improve` 或忽略

总建议: Minor 项不阻塞，可累积到下次清理；Nit 项仅作记录。

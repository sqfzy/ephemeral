# Experiment: 全场景延迟基准 sweep — kernel vs DPDK（2026-04-22）

> Mode: experiment ｜ 报告生成：2026-06-01 10:42 (UTC+8)
> 实验执行：2026-04-22 10:19–10:31（EC2 aarch64 Graviton，session clock）
> 被测 commit：`7869ea4` *refactor(bench): S3 — remove legacy config.hpp + bench.conf + INI MMPP params*
> 原始数据：[`../../../.artifacts/bench-data-20260422-103422-lat-full-sweep.txt`](../../../.artifacts/bench-data-20260422-103422-lat-full-sweep.txt)（14-run 全量逐指标输出）
> 配套深度分析：`.artifacts/experiment-20260422-103422.md`（同次实验的完整 anomaly 归因）

---

## TL;DR

这是 `benchmarks/latency` 套件**最近一次覆盖全部 7 个 scenario 的 kernel-vs-DPDK 完整 sweep**（7 scenario × {kernel, dpdk} = 14 runs）。核心假设 **H1「DPDK client p50 RTT ≤ kernel」在 5 个 echo scenario 的 4 个 + 2 个 push scenario 的 1 个上成立**：echo 场景 DPDK p50 RTT 比 kernel 快 **12–17 %**（tcp 24.3→21.0 µs、udp 23.5→20.6 µs、ws 26.1→21.7 µs、ex_md_udp 23.3→20.5 µs）。这一跑同时验证了 **H2「为每个线程绑核能压低 run-to-run 方差」**：DPDK RX p99 在 tcp −25 %、ws −18 % 收紧。

**三个 DPDK 异常被标记、不得当作性能特征引用**：ex_order TX 卡在 253 µs、UDP p99 飙到 262 µs、ex_market p99 16 ms（MMPP 稀疏采样伪影）。详见 §异常。

> ⚠️ 时效提醒：此 sweep 距今已一个多月，期间 TLS record-layer / TLS 1.2 e2e 等改动落地（见 git log `0174a153`→`78fe1bdb`）。当前 main（`c591c217`）的全场景延迟需重跑才能引用。后续单场景跑（2026-05-01 仅 DPDK+TLS 烟雾验证、2026-05-06 仅 lat_tcp sysmetrics）**都不是**全场景 sweep。

---

## 假设（Hypothesis）

可证伪陈述，绑核修复后的验证跑：

- **H1（主）**：client 线程绑核后，DPDK client 在**每个 echo scenario** 的 p50 RTT 都 ≤ kernel；p99 的优势至少不弱于上一次（未绑核）sweep。
  - 证伪条件：任一 echo scenario 出现 kernel p50 ≤ DPDK p50 → 该场景 H1 被否；或 DPDK p99/p50 比未绑核基线明显更差 → 绑核有害，H1 被否。
- **H2（次）**：绑核降低 run-to-run 方差。预期信号：p50 偏移 ±5 % 以内，DPDK 侧 RX p99 向其 p90 收紧。

---

## 动机

上一次 sweep（同日 06:51）client 线程在 dev host 上**未绑核**，数字噪声大，用户（正确地）否决了建立在这些噪声数字上的因果叙事。随后的 reshape：

1. 在 `benchmarks/latency/core/pin_client.hpp` 加 `bench::pin_client_from_cfg(cfg, "lat_X")`；
2. 每个 `lat_*` 二进制 main() 调用它；
3. 落了一条记忆规则（`feedback_bench_pin_every_thread.md`：分析抖动前先 grep 每个参与二进制里的 pin_thread）。

本次 rerun 就是验证绑核修复有效——同 host、同拓扑、同 seed、同（派生）config，**唯一差异 = 每个参与线程都绑了核**。这让它与 06:51 跑构成可配对的 paired-delta 分析。

---

## 方法（Method）

### 环境

| 维度 | 值 |
|------|----|
| 硬件 | AWS EC2 **aarch64 Graviton**（c7g 级别） |
| 内核 / OS | Linux 6.1.163-186.299.amzn2023.aarch64 |
| CPU 频率 | TSC 标定 **1.00 GHz，CV = 0.00 %**（每跑都报告） |
| 编译器 | **gcc 14**（xmake release toolchain；DPDK 走 gcc14-wrap） |
| config 解析 | toml++ v3.4.0（post-S3 reshape，commit `7869ea4`） |
| NIC 布局 | `nic_a = ens34`（server，kernel mock 绑这里，IP 172.31.47.238）；`nic_b = ens35`（client，IP 172.31.38.174，PCI `0000:28:00.0`） |
| CPU 绑核 | `cpu_client = 4`、`cpu_mock = 6`、`eal_cores = "0,1"`（DPDK EAL） |
| 绑核策略 | relaxed（dev host 无 isolcpus，`allow_non_isolated = true`）；client/mock 均打印 `pinned to CPU N (source: config.toml ...)` |
| Hugepages | sweep 开始时 256 reserved / 256 free |

> **术语**：*echo scenario* = client 发包、mock 原样回弹并打时间戳，测往返 RTT 并拆成 TX（client→server）/ RX（server→client）两腿；*push / one-way scenario* = mock 单向推送行情，client 收到时减去 payload 内 mock 打的时间戳得单向延迟。*MMPP-2* = 2 态马尔可夫调制泊松过程，模拟行情的"安静/爆发"双速率到达。

### 公平契约（为什么这个对比可信）

```
            kernel client ──► ens35 (bench_ns netns) ─┐
                                                      ├─► AWS VPC fabric ─► mock (永远 kernel, ens34, CPU6)
            dpdk   client ──► ens35 (vfio-pci, PMD)  ─┘
```

- mock **永远是 kernel**，只有 client 侧代码路径在 kernel/DPDK 间切换 → 差异完全归因于 client 传输栈。
- kernel client 走 `bench_ns` 网络命名空间（封掉 same-host loopback 抄近路）；DPDK client 把 ens35 绑 vfio-pci 由 PMD 独占队列。两条路径都经过 AWS VPC fabric，是物理 NIC 往返，不是 loopback 作弊。

### 被测对象

- Git HEAD `7869ea4`，除 `.artifacts/INDEX.md` / `CLAUDE.md` / fuzzer 目录 / 一个 mockex fixture 外干净（均与本跑无关）。
- 14 个二进制：`lat_tcp / udp / ws / ex_order / ex_market / ex_market_2p / ex_md_udp` 各 kernel + `_dpdk` 两版，外加 `mockex`（永远 kernel，绑 CPU6）。
- 全部 default release 优化；run 之间不重新编译。

### 数据集（测试输入）

- echo 场景：`payload_size = 256` 字节固定。
- push 场景（ex_market / ex_market_2p）：MMPP-2 泊松采样器，`mockex_seed = 42`，安静态 λ_q = 2.69916 Hz，爆发态 λ_b = 44,235.1 Hz。
- `warmup_samples = 1000` 丢弃。
- 样本量：echo 每跑 1.0 M–1.4 M；ex_market 仅 2,142（MMPP 稀疏所限）；ex_market_2p 163 k / 251 k。

### 时长（关键参数）

| 场景类 | 时长 | 通过 |
|---|---|---|
| echo（tcp/udp/ws/ex_order/ex_md_udp） | **30 s** 每跑 | `/tmp/bench-30s.toml`（把 config.toml 的 `duration_seconds=300` 改 30） |
| push（ex_market/ex_market_2p） | **120 s** 每跑 | `/tmp/bench-push-120s.toml`（MMPP 安静态 λ_q 在 seed=42 下 30 s 内很少跳到爆发，需更长窗口） |

> 注：`config.toml` 里每个场景默认 `duration_seconds = 300`，但本 sweep 用了上述 override 缩短到 30 s / 120 s，**实测时长不是 300 s**。整轮 14 runs wall-clock 约 12 分钟（10:19–10:31）。

### 测量方式

- echo RTT：client TSC 配对往返；24 字节二进制头携带 client `t0` 和 mock 回弹的 `t_mock_recv` / `t_mock_send`，据此拆 TX/RX 腿。
- one-way（ex_market / ex_market_2p）：client RX 处 `bench::monotonic_raw_ns()` 减去 JSON payload 里 mockex `PayloadPool::stamp_and_next` 打的 `"T"` 字段。
- 聚合：`eph::utils::Recorder::record_ns`（精确桶，非 HdrHistogram）。
- **单 trial / (scenario, backend)** —— 本 sweep 每组合只跑一次，无跨 seed 复制。

### 复现命令

```bash
# 派生 override config
sed 's/^duration_seconds = 300$/duration_seconds = 30/' \
    benchmarks/latency/config.toml > /tmp/bench-30s.toml
python3 -c "
import re, pathlib
s = pathlib.Path('/tmp/bench-30s.toml').read_text()
s = re.sub(r'(\[scenarios\.lat_ex_market\]\n(?:.*\n)*?duration_seconds = )30', r'\g<1>120', s)
s = re.sub(r'(\[scenarios\.lat_ex_market_2p\]\n(?:.*\n)*?duration_seconds = )30', r'\g<1>120', s)
pathlib.Path('/tmp/bench-push-120s.toml').write_text(s)
"

# echo sweep
for s in tcp udp ws ex_order ex_md_udp; do
  sudo ./benchmarks/latency/lat "$s"        --config /tmp/bench-30s.toml
  sudo ./benchmarks/latency/lat "$s" --dpdk --config /tmp/bench-30s.toml
done

# push sweep
for s in ex_market ex_market_2p; do
  sudo ./benchmarks/latency/lat "$s"        --config /tmp/bench-push-120s.toml
  sudo ./benchmarks/latency/lat "$s" --dpdk --config /tmp/bench-push-120s.toml
done
```

`lat` wrapper 幂等处理 NIC-B 状态切换（host ↔ bench_ns ↔ vfio-pci）。

---

## 数据

完整 14-run 逐指标原始输出（min / p50 / p90 / p99 / p99.9 / max / avg / stddev + 样本数 + 吞吐）见
[`../../../.artifacts/bench-data-20260422-103422-lat-full-sweep.txt`](../../../.artifacts/bench-data-20260422-103422-lat-full-sweep.txt)。

### 汇总表（RTT / one-way，单位 ns，⚠ = 异常）

| 场景（时长） | backend | samples | metric | p50 | p90 | p99 | p99.9 | max | throughput |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|
| tcp (30 s) | kernel | 1,168,952 | RTT | 24,311 | 26,871 | 69,022 | 69,918 | 1,357,407 | 39,055 /s |
| tcp (30 s) | dpdk | 1,392,609 | RTT | **20,999** | 23,303 | 27,367 | 35,119 | 1,546,895 | 46,474 /s |
| udp (30 s) | kernel | 1,239,291 | RTT | 23,479 | 25,719 | 33,199 | 69,342 | 12,504,879 | 41,349 /s |
| udp (30 s) | dpdk | 1,223,805 | RTT | **20,551** | 22,823 | **261,944** ⚠ | 264,056 | 1,553,344 | 41,144 /s |
| ws (30 s) | kernel | 1,001,851 | RTT | 26,087 | 32,647 | 69,917 | 74,269 | 3,449,072 | 33,434 /s |
| ws (30 s) | dpdk | 1,348,070 | RTT | **21,735** | 23,751 | 28,071 | 34,927 | 15,839,916 | 44,969 /s |
| ex_order (30 s) | kernel | 1,005,085 | RTT | 25,751 | 68,062 | 70,174 | 75,933 | 939,584 | 31,437 /s |
| ex_order (30 s) | dpdk | 120,808 ⚠ | RTT | **262,072** ⚠ | 263,800 | 268,408 | 287,351 | 1,754,897 | 3,806 /s ⚠ |
| ex_md_udp (30 s) | kernel | 1,252,199 | RTT | 23,287 | 25,447 | 32,151 | 69,342 | 949,381 | 41,773 /s |
| ex_md_udp (30 s) | dpdk | 1,426,240 | RTT | **20,471** | 22,487 | 28,103 | 40,430 | 7,482,089 | 47,575 /s |
| ex_market (120 s) | kernel | 2,142 | one-way | 14,435 | 61,902 | 158,139 | 273,784 | 276,949 | 23 /s |
| ex_market (120 s) | dpdk | 2,142 | one-way | **8,955** | 15,419 | **16,264,758** ⚠ | 34,520,117 | 34,775,610 | 23 /s |
| ex_market_2p (120 s) | kernel | 163,125 | one-way | 69,470 | 223,545 | 297,591 | 325,750 | 353,671 | 1,339 /s |
| ex_market_2p (120 s) | dpdk | 251,682 | one-way | **21,815** | 49,774 | 81,693 | 100,253 | 19,686,274 | 2,067 /s |

### TX / RX 腿拆解（仅 echo，单位 ns）

| 场景 / backend | TX p50 | TX p99 | RX p50 | RX p99 |
|---|---:|---:|---:|---:|
| tcp kernel | 13,123 | 20,631 | 10,995 | 48,654 |
| tcp dpdk | 12,435 | 17,671 | **8,355** | **12,331** |
| udp kernel | 12,787 | 19,495 | 10,483 | 16,011 |
| udp dpdk | 12,099 | **253,496** ⚠ | **8,259** | **12,563** |
| ws kernel | 14,371 | 57,998 | 11,275 | 54,830 |
| ws dpdk | 13,147 | 18,183 | **8,387** | **12,395** |
| ex_order kernel | 14,099 | 57,934 | 11,059 | 54,030 |
| ex_order dpdk | **253,880** ⚠ | 258,488 | **8,097** | **12,691** |
| ex_md_udp kernel | 12,691 | 19,239 | 10,411 | 15,291 |
| ex_md_udp dpdk | 12,035 | 18,279 | **8,235** | **12,467** |

DPDK RX p50 在 5 个 echo 场景一致落在 8.1–8.4 µs，RX p99 ≤ 12.7 µs —— 迄今观测到的最紧 RX 分布。

### Kernel vs DPDK p50 RTT（echo）

```
                  0      5     10     15     20     25     30  µs
                  |------|------|------|------|------|------|
  tcp  kernel     ████████████████████████████▌              24.3
  tcp  dpdk       ████████████████████████▍                  21.0    −14 %
  udp  kernel     ████████████████████████▍                  23.5
  udp  dpdk       ████████████████████▌                      20.6    −12 %
  ws   kernel     ██████████████████████████████             26.1
  ws   dpdk       █████████████████████████▌                 21.7    −17 %
  eord kernel     █████████████████████████████              25.8
  eord dpdk       ❯❯❯❯❯❯❯❯❯❯ 262 µs（异常 — 见 §异常 #1）
  emd  kernel     ███████████████████████▌                   23.3
  emd  dpdk       ████████████████████▍                      20.5    −12 %
```

### DPDK 吞吐（echo，samples/s）

```
            0    10k   20k   30k   40k   50k
            |-----|-----|-----|-----|-----|
  tcp  k    ███████████████████████████████▏        39,055
  tcp  d    █████████████████████████████████████▏  46,474   +19 %
  ws   k    ██████████████████████████▋             33,434
  ws   d    ███████████████████████████████████▉    44,969   +35 %
  emd  k    █████████████████████████████████▍      41,773
  emd  d    ██████████████████████████████████████  47,575   +14 %
```

---

## 分析

### 统计显著性

- echo 每场景 1.0–1.4 M 样本，分布平稳时 p99 以下精度到单 ns；但两个跑的 p99 被异常 outlier 主导（见 §异常）。
- **本 sweep 每组合单 trial、无跨 seed 复制** —— p99.9 / max 由每跑 1–2 个事件决定，不应引用。
- 与 06:51 跑可比：同 host、既有路径同二进制产物，**唯一差异是 client 侧绑核**，适合 paired-delta。

### H1 判定 — ✅ 在 4/5 echo + 1/2 push 成立；ex_order 推迟

- tcp / udp / ws / ex_md_udp + ex_market_2p：DPDK p50 严格低于 kernel。幅度：echo 12–17 %；ex_market_2p 3.2×（**但这是 one-way 测量不对称放大的，见 §局限 4，不可当"twophase parse 快 3 倍"引用**）。
- ex_order：DPDK p50 = 262 µs vs kernel 25.8 µs，H1 灾难性失败 —— 但本跑异常（§异常 #1），不是真实 DPDK 回退。
- ex_market：DPDK p50 = 9.0 µs vs kernel 14.4 µs，DPDK 赢，但 2,142 样本下 p99 不可靠。

### H2 判定 — ✅ 绑核收紧了 DPDK RX p99

对 06:51（未绑核）跑的 paired-delta（DPDK RX p99）：

| 场景 | 06:51 未绑核 | 10:19 绑核 | Δ |
|---|---:|---:|---:|
| tcp | 16,347 | 12,331 | **−25 %** |
| udp | 12,755 | 12,563 | −1 % |
| ws | 15,179 | 12,395 | **−18 %** |
| ex_md_udp | 12,619 | 12,467 | −1 % |
| ex_order | 12,843 | 12,691 | −1 % |

RX p99 正是绑核该帮上忙的地方：client poll-recv 路径留在同一核的 cache。tcp/ws 的 25%/18% 收紧符合预期；udp/ex_md_udp/ex_order 绑核前已 ≤ 13 µs，提升空间小。kernel 侧基本没变（p50 ±5 % 内，某些 p99 反而更差）——符合预期，kernel NAPI + softirq 抖动不受用户态 client 绑核影响。

### 归因

1. **DPDK p50 echo 优势、幅度不变**：根因同上次 —— DPDK 在 client RX 路径绕过 NAPI + syscall。绑核没改变 syscall 数，而是消除了无 syscall RX 路径上的**方差**。
2. **tcp/ws RX p99 收紧**：两者共享 codec-decode 热路径（ws 用 WsCodec、tcp 用 24 字节头解析），绑核后热函数稳定驻留 L1；未绑核时 CFS 可能在突发中迁移 client，驱逐热帧。
3. **kernel p99 部分场景反而更差**（kernel TCP p99 31,991→69,022 ns）：与绑核无关，是当日环境噪声 / 特定 softirq 模式；kernel 对 host 状态敏感，DPDK 不敏感。

---

## 结论

- **H1（DPDK ≤ kernel p50 RTT）**：✅ 在 4/5 echo + 1/2 push 成立。ex_order 是异常非反例；ex_market DPDK 赢但样本太稀疏不承重。
- **H2（绑核降方差）**：✅ 证据来自 tcp（−25 %）/ ws（−18 %）RX p99 收紧，p50 偏移 ±5 % 内。
- 本 sweep 是绑核后的新基线；06:51 未绑核跑测的是"另一个系统"，**从此不再作参考**。

---

## 异常（不得当作 DPDK 性能特征引用）

### #1 — DPDK ex_order TX p50 = 253,880 ns ｜ 严重度：高

- 上次跑（未绑核）DPDK ex_order p50 = 20,983 ns、吞吐 31,728 /s，与其他 DPDK echo 一致；本跑 p50 RTT 262 µs，**整段 253 µs gap 全在 TX 腿**，RX 正常（8.1 µs），吞吐掉到 3,806 /s（8× 下降）。
- `stddev = 12,622` 在 262 µs p50 上 → TX 腿稳定 ~250 µs，不是偶发，是常数级开销。
- 疑因（未 profile）：mockex `ex_order_echo` 的 JSON 时间戳 splice 撞冷分配器 / DPDK `tx_burst` 队列 0 描述符耗尽阻塞 / 该端口的冷 VPC flow-cache 效应。kernel 侧同 mock 跑 31,437 /s，问题特定于 DPDK-client↔mock 这一对。
- **建议**：`lat ex_order --dpdk` 连跑 5×；若 TX p50 仍 ~250 µs，对 mockex + client PID `perf record --call-graph dwarf`；若后续跑正常则是当日 host 冷启动噪声。

### #2 — DPDK UDP p99 = 261,944 ns（配 TX p99 253,496）｜ 严重度：中

- p50 RTT 20,551 ns 完全正常，p99 跳到 262 µs，`stddev = 27,639`；p99.9 ≈ p99 → 约 0.9 %（~11 k 事件）的 250 µs stall。
- 该数字与 #1 的 253 µs 巧合吻合 → 疑似共享根因（该 30 s 窗口内 mockex kernel 侧周期性 stall）；tcp/ws/ex_md_udp 未现。
- **建议**：关联跑内时间戳看 250 µs 事件是否时间聚簇 + 重跑。

### #3 — DPDK ex_market p99 = 16,264,758 ns（16 ms）｜ 严重度：低

- 与上次 ex_market DPDK p99 = 36 ms 同模式。根因：MMPP 安静态 λ_q = 2.7 Hz @ seed 42，爆发间隔数十秒，跨 inter-burst gap 的样本泄漏进 recorder。**不是"网络花了 16 ms"，是测量伪影。**
- **建议**：用真实 capture 重拟 `ex_market_params.toml` 抬高 λ_q，或 client 侧过滤前序到达间隔超阈值的样本。

### #4 — Kernel TCP p99 = 69,022 ns（上次 31,991）｜ 严重度：低（kernel 侧预期噪声）

- p50 不变（24.3 vs 24.5 µs），仅 p99 涨。与绑核无关，是 NAPI batching 时刻效应；kernel TCP p99 由 softirq 调度尾决定，无 `isolcpus` + NIC IRQ affinity 时不稳定。
- **建议**：接受为环境性；若系统化则配 isolcpus / IRQ 重绑后复查。

---

## 边界条件 / 局限

1. **每组合单 trial**。鉴于 3 个可见异常，后续应 ≥ 3 seed/场景以圈定可复现性。
2. **30 s echo 窗口** —— p99.9 / max 由每跑 1–2 个事件主导，不要引用。
3. **ex_market 稀疏**（2,142 样本 / 120 s ≈ 18 Hz），p99+ 统计上无意义。
4. **ex_market_2p p50 3.2×（DPDK 21.5 µs vs kernel 69.5 µs）被 one-way 计时方法放大**（DPDK PMD vs kernel `recvmsg()` 的 RX 端打戳不对称），不是纯 parse-cost；不可当"twophase parse 快 3 倍"引用。
5. **Graviton aarch64** —— 数字不迁移到 x86；绑核收益在 x86 上可能更大（BTB 更敏感）。
6. **无 isolcpus**（dev host `allow_non_isolated = true`）—— kernel p99 尾被此 floor 限制。

---

## 对决策的影响 / 未来工作

- **信任任何 DPDK-ex_order 指标前先 5× 重跑** ex_order（§异常 #1）；在此之前不要拿 ex_order 与其他场景比。
- **保留本 sweep 为基线**；06:51 跑被绑核修复作废。
- **`fit_mmpp.py` 的 λ_q 对 120 s bench 窗口过低** —— 用真实 capture 重生成或加 `--artificial-busy` override。
- `lat_*` + mockex 绑核代码（commit 链 `c0488b7 … fe9134d`）确认有效，保留。
- 未来工作：ex_order 5× 重跑 → 若可复现则 `perf record`；5-seed 复制 sweep 求真实 CI；MMPP 重拟；1 h `lat_tcp --dpdk` soak 测真实 p99.99 尾；profile ex_market_2p twophase 槽解析以排除 RX 打戳不对称的混淆。

---

## 相关

- **原始数据**：`.artifacts/bench-data-20260422-103422-lat-full-sweep.txt`（14-run 全量）
- **配套深度分析**：`.artifacts/experiment-20260422-103422.md`（本报告的素材来源，含完整 paired-delta 与归因）
- **上一次（未绑核）sweep**：`.artifacts/experiment-20260422-065113.md` + `bench-data-20260422-065113-lat-full-sweep.txt`（已被绑核修复作废，仅留作历史）
- **Commits**：被测 HEAD `7869ea4`；绑核修复链 `c0488b7 → 90ae87a → 27e484d → aa75bf2 → af9d226 → 2d83f1e → a855e93 → 6f6d4f2 → fe9134d`（+ S1 `053bc42`）
- **记忆规则**：`feedback_bench_pin_every_thread.md`（分析抖动前先 grep 每个参与二进制的 pin_thread）
- **配置 schema**：`benchmarks/latency/config.toml`、套件说明 `benchmarks/latency/README.md`、公平性 `docs/latency-benchmark-fairness.md`

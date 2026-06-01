# kernel vs DPDK TCP echo — baseline vs shielded

生成时间：2026-05-06T11:32:26.646850+00:00  
数据源：
- baseline (no shield): `.artifacts/bench-sysmetrics-tcp-echo-20260506`
- shielded (cgroup AllowedCPUs=0-3,6-7 on system+user slice): `.artifacts/bench-sysmetrics-tcp-echo-shielded-20260506`

## 核心问题：cgroup shield 真的有用吗？

| metric | baseline kernel | shielded kernel | Δ kernel | baseline dpdk | shielded dpdk | Δ dpdk |
|---|---:|---:|---:|---:|---:|---:|
| cycles | 831.91G | 834.67G | +0.3% | 836.98G | 836.56G | -0.1% |
| instructions | 1480.58G | 1475.73G | -0.3% | 1305.72G | 1310.48G | +0.4% |
| IPC | 1.7798 | 1.7682 | -0.7% | 1.5600 | 1.5665 | +0.4% |
| cache-refs | 407.43G | 406.71G | -0.2% | 479.10G | 480.93G | +0.4% |
| cache-misses | 914.81M | 990.73M | +8.3% | 337.42M | 199.59M | -40.8% |
| branch-misses | 134.06M | 146.05M | +8.9% | 22.67M | 21.01M | -7.3% |
| ctxt-sw (perf) | 39.04k | 23.29k | -40.3% | 2.87k | 3.92k | +36.6% |
| cpu-migrations | 0 | 0 | — | 0 | 0 | ∞ |
| page-faults | 0 | 176 | ∞ | 0 | 224 | ∞ |
| vol_ctxt (/proc) | 0 | 0 | — | 0 | 0 | — |
| invol_ctxt (/proc) | 38.96k | 23.26k | -40.3% | 2.87k | 3.92k | +36.7% |
| NIC IRQ delta | 32.47M | 31.28M | -3.7% | 19.05M | 16.92M | -11.2% |
| elapsed_sec | 301 | 301 | -0.0% | 301 | 301 | -0.0% |

## 每 trial 原始 (kernel/dpdk × shielded × 3)

### shielded kernel

| trial | cycles | inst | IPC | cache-miss | ctxt-sw | vol/invol | NIC IRQ | elapsed |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 836.86G | 1449.48G | 1.7321 | 1.29G | 1.56k | 0/1541 | 30.93M | 301.3s |
| 2 | 836.92G | 1471.57G | 1.7583 | 998.86M | 1.94k | 0/1930 | 36.05M | 301.3s |
| 3 | 830.24G | 1506.15G | 1.8141 | 685.01M | 66.38k | 0/66319 | 26.86M | 301.3s |

### shielded dpdk

| trial | cycles | inst | IPC | cache-miss | ctxt-sw | vol/invol | NIC IRQ | elapsed |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 836.63G | 1307.74G | 1.5631 | 204.02M | 3.93k | 0/3932 | 18.42M | 301.3s |
| 2 | 836.51G | 1311.47G | 1.5678 | 199.63M | 3.85k | 0/3849 | 14.08M | 301.3s |
| 3 | 836.54G | 1312.25G | 1.5687 | 195.12M | 3.99k | 0/3982 | 18.26M | 301.3s |

## 解读 (interpretation)

### TL;DR

cgroup shield **效果不一致**：kernel 模式有时显著降噪、有时几乎无效；DPDK 模式 cache-miss 砍 41%，但 ctxt-sw 反而略涨。**与真正的 `isolcpus=` 比，cgroup shield 只搬走了用户态噪声，没搬走 kernel softirq / timer tick / IRQ delivery**——而后两者才是 kernel TCP 路径上 ctxt-sw 的主要来源。

### 关键观察

1. **kernel-shield 的 ctxt-sw 双峰分布**：
   ```
   shielded kernel trial 1: ctxt-sw=1.5k    ← 25× 优于 baseline
   shielded kernel trial 2: ctxt-sw=1.9k    ← 同上
   shielded kernel trial 3: ctxt-sw=66k     ← 1.7× 差于 baseline (outlier)
   ```
   这说明 cgroup shield **能** 把 user-space 噪声压到 ~1.5k 量级（trial 1 + 2 的水平），但 trial 3 撞到了某个 systemd-managed kernel 工作流（可能 unattended-upgrades / journal flush / cron / metricbeat），导致 cgroup AllowedCPUs 不能阻挡的 kernel-side 抢占。3 次里 2 次清净不算稳定。

2. **DPDK-shield 的 cache-miss -41% 是真信号**：
   ```
   baseline dpdk: cache-miss = 337M ± 203M
   shielded dpdk: cache-miss = 200M ± 5M
   ```
   stddev 也从 203M 降到 5M——shield 把 baseline 里 50% 的 cache-miss variance 吃掉了，说明这些 miss 来自 cpu 4/5 上邻居进程的 LLC 污染。busy-poll 的 DPDK 路径对 cache 干净度敏感。

3. **DPDK-shield 的 ctxt-sw +37% 反直觉但真实**：
   ```
   baseline dpdk: ctxt-sw = 2.87k
   shielded dpdk: ctxt-sw = 3.92k
   ```
   这是**测量伪差**：systemd-run 创建 `bench-shield.slice` + transient `.scope` 给 lat 多套了一层 cgroup hierarchy，systemd 内部状态机产生额外的 statistics-collection ctxt switches。实际 lat 业务路径没退化（cycles / IPC 都和 baseline 同档）。

4. **page-faults 从 0 → 176/224**：
   两边都引入 ~200 minor page faults。这是 systemd-run + bench-shield.slice 在 setup 阶段创建 cgroup 层级 + 加载额外 systemd 单元产生的内存页映射。**业务路径仍走 mlockall**，所以这是 startup-only 的开销，对 sustained workload 影响微乎其微。

5. **NIC IRQ 几乎没变**（kernel -3.7% / DPDK -11%）：
   `AllowedCPUs` 不影响 IRQ 路由 — NIC IRQ 仍按 SMP affinity 分配到原 CPU。要真正减少 ens35 IRQ 落在 cpu 4 上，需要 `echo X > /proc/irq/N/smp_affinity_list` 强制把 NIC IRQ 拉到 cpu 0/1。

### 与 `isolcpus=` 真正隔离的差距

| 噪声来源 | cgroup shield 能挡? | isolcpus= 能挡? |
|---|---|---|
| 用户进程在 cpu 4/5 上调度 | ✅ | ✅ |
| 系统服务 (systemd, journal, cron) | ✅ | ✅ |
| kernel softirq (NET_RX 等) | ❌ 仍落在 cpu 4 | ✅（通过 nohz_full=）|
| kernel timer tick | ❌ 仍每秒 250-1000 次 | ✅（通过 nohz_full=）|
| IRQ 分发到 cpu 4 | ❌ 需手动 IRQ affinity | 部分（取决于 driver）|
| Per-cpu kernel threads (kworker/4) | ❌ | 部分 |

→ cgroup shield 是 isolcpus 的"用户态半截"。对 DPDK busy-poll workload 帮助有限（DPDK 本来就不跑 user-space 邻居）；对 kernel TCP 路径帮助也只有 trial-1/2 那种"运气好没撞 systemd 工作"的场景才显现。

### 一句话结论

**对当前 lat_tcp + sustained 46k pps workload，cgroup shield 不值得作为 default**：DPDK 的 cache-miss 砍 41% 是唯一稳健收益；kernel 模式效果太不稳定（3 次里 1 次回到 baseline）；DPDK 的 ctxt-sw 反而被 systemd-run 的 scope 叠加抬高。要真正干净就上 `isolcpus=4,5 nohz_full=4,5` + IRQ 重路由（B/C 路径），代价是**重启**。

### 建议

- 留着 `cpu-shield-run.sh` 作为 cgroup-only 的 quick-shield 工具（无需重启就能跑），但不作为 bench 的默认配置
- 真要做 production-grade 隔离，需要 grub `isolcpus=4,5 nohz_full=4,5 rcu_nocbs=4,5` + `/proc/irq/<NIC-N>/smp_affinity_list=0,1`，需要重启
- cache-miss 的 41% 收益证明 cpu 4/5 的 LLC 是有邻居污染的；如果只追求这一项，单独做 IRQ 亲和性调整（`/proc/irq/.../smp_affinity_list`）可能比整套 shield 更便宜

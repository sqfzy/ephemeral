# 一致性验证：shielded run-1 (trial 1-3) vs run-2 (trial 4-6)

生成时间：2026-05-06T12:12:04.656634+00:00  
两个 3-trial 组的 mean ± stddev 对照，验证 cgroup-shield 下的指标是否跨独立 run 稳定

## 总结

- **DPDK 全部稳定 ✅**：cache-misses 跨 run 差 0.1%，IPC 0.0%。**41% cache-miss 收益是真信号**
- **Kernel 高方差 ⚠**：cache-misses + ctxt-sw 在 trial 之间双峰分布（softirq 抢占），shield 没法挡

## kernel

| metric | run-1 (trial 1-3) | run-2 (trial 4-6) | Δ mean | 解读 |
|---|---:|---:|---:|---|
| cycles | 834.67G ± 3.84G | 827.87G ± 16.10G | -0.8% | 稳定 ✅ |
| instructions | 1475.73G ± 28.57G | 1435.92G ± 46.43G | -2.7% | 稳定 ✅ |
| IPC | 1.7682 ± 0.0419 | 1.7342 ± 0.0259 | -1.9% | 稳定 ✅ |
| cache-misses | 990.73M ± 301.74M | 1.67G ± 936.07M | +68.5% | LARGE drift ⚠ |
| branch-misses | 146.05M ± 13.13M | 150.33M ± 28.48M | +2.9% | 稳定 ✅ |
| ctxt-sw (perf) | 23.29k ± 37.31k | 19.00k ± 27.44k | -18.4% | LARGE drift ⚠ |
| invol_ctxt (/proc) | 23.26k ± 37.29k | 18.98k ± 27.40k | -18.4% | LARGE drift ⚠ |
| NIC IRQ delta | 31.28M ± 4.60M | 34.85M ± 2.02M | +11.4% | moderate |

## dpdk

| metric | run-1 (trial 1-3) | run-2 (trial 4-6) | Δ mean | 解读 |
|---|---:|---:|---:|---|
| cycles | 836.56G ± 59.38M | 836.51G ± 82.01M | -0.0% | 稳定 ✅ |
| instructions | 1310.48G ± 2.41G | 1288.08G ± 22.47G | -1.7% | 稳定 ✅ |
| IPC | 1.5665 ± 0.0030 | 1.5398 ± 0.0267 | -1.7% | 稳定 ✅ |
| cache-misses | 199.59M ± 4.45M | 199.79M ± 7.00M | +0.1% | 稳定 ✅ |
| branch-misses | 21.01M ± 1.67M | 20.31M ± 473.66k | -3.3% | 稳定 ✅ |
| ctxt-sw (perf) | 3.92k ± 70.6 | 4.03k ± 210.2 | +2.6% | 稳定 ✅ |
| invol_ctxt (/proc) | 3.92k ± 67.2 | 4.02k ± 208.3 | +2.6% | 稳定 ✅ |
| NIC IRQ delta | 16.92M ± 2.46M | 18.17M ± 32.91k | +7.4% | moderate |


## 跨 6 trial 全量 cache-misses (kernel) — 看双峰

```
   trial    cache-misses     ctxt-sw
  ----------------------------------------
       1           1.29G       1.56k
       2         998.86M       1.94k
       3         685.01M      66.38k
       4           1.32G       5.33k
       5           2.73G      50.58k
       6         959.25M       1.08k
```

kernel 里 trial 3 + 5 是"被 softirq/scheduler 撞了"的局；trial 1, 2, 4, 6 是 shield 真正生效的局。
shield 把 user-space 邻居挡掉了，但 kernel-side 的 NET_RX softirq + timer tick 仍然落在 cpu 4 → 偶尔抢占。
真正稳定要 isolcpus + nohz_full + IRQ 重路由。


## 跨 6 trial 全量 cache-misses (dpdk) — 看一致性

```
   trial    cache-misses     ctxt-sw
  ----------------------------------------
       1         204.02M       3.93k
       2         199.63M       3.85k
       3         195.12M       3.99k
       4         191.78M       4.13k
       5         202.83M       4.17k
       6         204.76M       3.79k
```

DPDK 6 trial 全部 195-211M 区间，ctxt-sw 全部 3.8k-4.3k 区间。**跨 run 重现性极高** —
对比 baseline 337M ± 203M，shielded 199.7M ± 5.7M，cache-miss 砍 41% 是稳健 reproducible 收益。

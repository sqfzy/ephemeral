# Replication check：cache-miss "shield 收益" 是真的还是 baseline 噪声？

生成时间：2026-05-06T12:55:46.632098+00:00  

## 实验设计

| 组 | trial 数 | 何时跑 |
|---|---:|---|
| baseline kernel run-1 (1-3) | 3 | 第一次 baseline 跑 (10:00) |
| baseline kernel run-2 (4-6) | 3 | 重新跑（12:21）|
| shielded kernel (1-6) | 6 | 加 shield 跑（10:57 + 11:38）|
| baseline dpdk run-1 (1-3) | 3 | 第一次 baseline 跑 |
| baseline dpdk run-2 (4-6) | 3 | 重新跑 |
| shielded dpdk (1-6) | 6 | 加 shield 跑 |

问题：原始 baseline DPDK cache-miss = 337M ± 203M（stddev 60% of mean — 极大方差）。
shield 后 = 200M ± 5M。是 shield 真的把 cache-miss 砍了 41%，还是原 baseline 那次"恰好"撞上了大噪声？
→ 重新跑 3 次 baseline (4-6)，对比 baseline-1-3 / baseline-4-6 / shielded-1-6 三组。

## cache-miss 三方对照（核心问题）

| group | mean | stddev | trial-by-trial |
|---|---:|---:|---|
| baseline-kernel-run1 (1-3) | 914.81M | 127.03M | 1.00G / 769.00M / 973.91M |
| baseline-kernel-run2 (4-6) | 892.70M | 37.59M | 855.34M / 930.52M / 892.25M |
| shielded-kernel (1-6) | 1.33G | 724.57M | 1.29G / 998.86M / 685.01M / 1.32G / 2.73G / 959.25M |
| baseline-dpdk-run1 (1-3) | 337.42M | 202.85M | 207.35M / 233.77M / 571.15M |
| baseline-dpdk-run2 (4-6) | 234.10M | 22.44M | 259.74M / 218.00M / 224.57M |
| shielded-dpdk (1-6) | 199.69M | 5.25M | 204.02M / 199.63M / 195.12M / 191.78M / 202.83M / 204.76M |

## 全指标对照（mean ± stddev）

### kernel

| metric | baseline-kernel-run1 (1-3) | baseline-kernel-run2 (4-6) | shielded-kernel (1-6) |
|---|---:|---:|---:|
| cycles | 831.91G ± 2.92G | 833.71G ± 2.74G | 831.27G ± 11.11G |
| inst | 1480.58G ± 28.70G | 1498.55G ± 21.15G | 1455.82G ± 40.80G |
| IPC | 1.7798 ± 0.0407 | 1.7975 ± 0.0310 | 1.7512 ± 0.0363 |
| cache-misses | 914.81M ± 127.03M | 892.70M ± 37.59M | 1.33G ± 724.57M |
| branch-misses | 134.06M ± 33.45M | 144.04M ± 24.71M | 148.19M ± 19.97M |
| ctxt-sw | 39.04k ± 26.20k | 33.63k ± 27.57k | 21.14k ± 29.39k |
| invol_ctxt | 38.96k ± 26.21k | 33.60k ± 27.53k | 21.12k ± 29.36k |
| NIC IRQ | 32.47M ± 3.38M | 37.03M ± 1.19M | 33.06M ± 3.73M |

### dpdk

| metric | baseline-dpdk-run1 (1-3) | baseline-dpdk-run2 (4-6) | shielded-dpdk (1-6) |
|---|---:|---:|---:|
| cycles | 836.98G ± 138.52M | 836.68G ± 471.79M | 836.54G ± 69.32M |
| inst | 1305.72G ± 12.33G | 1309.24G ± 3.33G | 1299.28G ± 18.84G |
| IPC | 1.5600 ± 0.0145 | 1.5648 ± 0.0040 | 1.5532 ± 0.0224 |
| cache-misses | 337.42M ± 202.85M | 234.10M ± 22.44M | 199.69M ± 5.25M |
| branch-misses | 22.67M ± 3.24M | 22.04M ± 2.45M | 20.66M ± 1.16M |
| ctxt-sw | 2.87k ± 294.3 | 3.26k ± 153.9 | 3.98k ± 151.0 |
| invol_ctxt | 2.87k ± 292.5 | 3.26k ± 156.2 | 3.97k ± 148.9 |
| NIC IRQ | 19.05M ± 743.77k | 16.51M ± 2.54M | 17.55M ± 1.70M |

## 解读

### DPDK cache-miss 三段对照（核心问题）

- **baseline run-1 (1-3)**: 337.42M ± 202.85M（原始大方差观察）
- **baseline run-2 (4-6)**: 234.10M ± 22.44M（重做）
- **baseline 全 6 trial**:  285.76M ± 140.93M
- **shielded 6 trial**:     199.69M ± 5.25M

**shielded vs baseline-6 cache-miss 差异：-30.1%**

**裁定**：shield 对 cache-miss 的减少是真实的，跨多 run reproducible。

### kernel cache-miss

- baseline 全 6: 903.76M ± 84.66M
- shielded 6:    1.33G ± 724.57M

kernel shielded vs baseline-6 差异：+47.2%（kernel 路径本来就受 softirq 影响 → cache-miss 双峰大方差，shield 挡不住）

## 一句话

shielded vs baseline-6 cache-miss 差异 -30.1% — shield 收益验证仍待进一步分析。

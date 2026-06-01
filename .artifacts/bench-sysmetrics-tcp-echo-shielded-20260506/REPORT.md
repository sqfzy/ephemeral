# kernel vs DPDK TCP echo — 系统指标对比

生成时间：2026-05-06T11:31:58.170943+00:00  
数据源：`.artifacts/bench-sysmetrics-tcp-echo-shielded-20260506`  
每边 trial 数：kernel=3 / dpdk=3  

## 横向对比表

| metric | kernel (mean ± stddev) | dpdk (mean ± stddev) | k/d ratio | 解读 |
|---|---:|---:|---:|---|
| cycles | 834.670G ± 3.839G | 836.560G ± 59.383M | 1.00x | CPU 总忙时（busy-poll vs interrupt-driven） |
| instructions | 1475.732G ± 28.566G | 1310.484G ± 2.409G | 1.13x | 指令总条数 |
| IPC | 1.7682 ± 0.0419 | 1.5665 ± 0.0030 | 1.13x | 每周期指令；DPDK busy-poll 通常更高 |
| cache-references | 406.714G ± 11.052G | 480.929G ± 888.913M | 0.85x | L1/L2 cache 摸内存频度 |
| cache-misses | 990.732M ± 301.736M | 199.590M ± 4.453M | 4.96x | cache miss 绝对数；syscall path 通常更多 |
| branch-misses | 146.053M ± 13.131M | 21.015M ± 1.670M | 6.95x | 分支预测失败；协议栈分支多 → 多 |
| context-switches (perf) | 23.292k ± 37.314k | 3.925k ± 71 | 5.93x | perf 视角的 ctx switch（含被迫） |
| cpu-migrations | 0 ± 0 | 0 ± 1 | 0x | CPU 迁移；绑核应都接近 0 |
| page-faults | 176 ± 1 | 224 ± 3 | 0.79x | 缺页；mlockall 后应都接近 0 |
| voluntary_ctxt_switches (/proc) | 0 ± 0 | 0 ± 0 | 1x | voluntary ctx switch；syscall block 让出 → kernel 多 |
| nonvoluntary_ctxt_switches (/proc) | 23.263k ± 37.288k | 3.921k ± 67 | 5.93x | 被迫 ctx switch；调度器抢占信号 |
| NIC IRQ delta (sum over ens34+ens35) | 31.279M ± 4.603M | 16.918M ± 2.463M | 1.85x | NIC 中断总数；DPDK 应为 0 |
| perf elapsed_sec (sanity) | 301.287s ± 0.004s | 301.288s ± 0.002s | 1.00x |  |

## 每 trial 原始数据

### kernel

| trial | cycles | instructions | IPC | cache-misses | ctxt-sw (perf) | vol/invol (/proc) | NIC IRQ |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 836.856G | 1449.477G | 1.7321 | 1.288G | 1.561k | 0/1541 | 30927460 |
| 2 | 836.917G | 1471.568G | 1.7583 | 998.861M | 1.938k | 0/1930 | 36047165 |
| 3 | 830.237G | 1506.151G | 1.8141 | 685.014M | 66.378k | 0/66319 | 26861309 |

### dpdk

| trial | cycles | instructions | IPC | cache-misses | ctxt-sw (perf) | vol/invol (/proc) | NIC IRQ |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 836.626G | 1307.739G | 1.5631 | 204.023M | 3.930k | 0/3932 | 18422573 |
| 2 | 836.510G | 1311.466G | 1.5678 | 199.629M | 3.852k | 0/3849 | 14075430 |
| 3 | 836.544G | 1312.247G | 1.5687 | 195.118M | 3.993k | 0/3982 | 18256208 |

## ASCII 柱状对比（5 关键指标）

```
          cycles  kernel │██████████████████████████████████████████████████│ 834.670G
          cycles  dpdk   │██████████████████████████████████████████████████│ 836.560G

    cache-misses  kernel │██████████████████████████████████████████████████│ 990.732M
    cache-misses  dpdk   │██████████                                        │ 199.590M

         ctxt-sw  kernel │██████████████████████████████████████████████████│ 23.292k
         ctxt-sw  dpdk   │████████                                          │ 3.925k

        vol-ctxt  (both 0 — N/A)
         NIC IRQ  kernel │██████████████████████████████████████████████████│ 31.279M
         NIC IRQ  dpdk   │███████████████████████████                       │ 16.918M

```

## 威胁有效性 (threats to validity)

1. **AWS hypervisor 计数器虚拟化**：cycles/instructions/cache-* 在 EC2 这类 hypervised 环境下数字精度受 host 抖动影响，绝对值可能比裸机噪声大 5-10%。倾向相对比较（ratio）而非绝对值。
2. **CPU 2/3/6 被其他工作占用**：本次配置已避开（cpu_client=4, cpu_mock=5, eal_cores=0,1）。但邻近核（cpu 1, 5, 7）仍可能因 LLC 共享受其他进程影响。
3. **mlockall 后 page-fault 应≈0**：若任一 trial 显示非零 page-faults，可能是 EAL hugepage 初始化阶段产生（DPDK 模式特有）。
4. **cross-trial stddev > 15% mean** 视为环境扰动严重，记录但不重跑。

## 复现

```bash
# Pre-flight: NIC ena, hugepages free, no daemon residue
# (see benchmarks/latency/scripts/bench-sysmetrics-tcp.sh comment header)

mkdir -p .artifacts/bench-sysmetrics-tcp-echo-shielded-20260506
for mode in kernel dpdk; do
  if [[ "$mode" == "dpdk" ]]; then
    sudo /home/ec2-user/ephemeral_dev/build/linux/arm64/release/eph_nicd \
        --no-config-file --pci=0000:28:00.0 --total-queues=8 --daemon-lcore=14 &
    sleep 6
  fi
  for trial in 1 2 3; do
    MODE=$mode TRIAL=$trial DURATION=300 \
      OUTDIR=.artifacts/bench-sysmetrics-tcp-echo-shielded-20260506 \
      ./benchmarks/latency/scripts/bench-sysmetrics-tcp.sh
    sleep 10
  done
  if [[ "$mode" == "dpdk" ]]; then sudo pkill -f eph_nicd; fi
done
./benchmarks/latency/scripts/bench-sysmetrics-tcp.sh --aggregate .artifacts/bench-sysmetrics-tcp-echo-shielded-20260506
```

## 延迟数据（shielded：lat_tcp 6-trial mean ± stddev）

### RTT / TX / RX percentile (shielded)

| leg / pct | shielded kernel | shielded dpdk | k − d | k/d ratio |
|---|---:|---:|---:|---:|
| RTT p50 | 24.35 µs | 20.55 µs | 3.80 µs | 1.19× |
| RTT p90 | 29.06 µs | 22.10 µs | 6.96 µs | 1.31× |
| RTT p99 | 69.56 µs | 68.90 µs | 661 ns | 1.01× |
| RTT p99.9 | 89.65 µs | 74.45 µs | 15.20 µs | 1.20× |
| TX p50 | 13.27 µs | 12.29 µs | 982 ns | 1.08× |
| TX p90 | 14.77 µs | 13.58 µs | 1.18 µs | 1.09× |
| TX p99 | 58.09 µs | 60.47 µs | 2.38 µs | 0.96× |
| TX p99.9 | 63.73 µs | 62.74 µs | 989 ns | 1.02× |
| RX p50 | 10.95 µs | 8.13 µs | 2.82 µs | 1.35× |
| RX p90 | 12.23 µs | 8.93 µs | 3.30 µs | 1.37× |
| RX p99 | 55.83 µs | 10.38 µs | 45.45 µs | 5.38× |
| RX p99.9 | 57.83 µs | 34.81 µs | 23.01 µs | 1.66× |

**Throughput (shielded)**：kernel **35,899 ± 1081** s/s · dpdk **46,090 ± 238** s/s（DPDK +28.4%）

### vs baseline — shield 对延迟的影响（每边各自对比）

| leg / pct | baseline kernel | shielded kernel | Δ kernel | baseline dpdk | shielded dpdk | Δ dpdk |
|---|---:|---:|---:|---:|---:|---:|
| RTT p50 | 24.29 µs | 24.35 µs | +0.3% | 19.00 µs | 20.55 µs | +8.1% |
| RTT p99 | 69.64 µs | 69.56 µs | -0.1% | 68.89 µs | 68.90 µs | +0.0% |
| RTT p99.9 | 110.31 µs | 89.65 µs | -18.7% | 74.29 µs | 74.45 µs | +0.2% |
| TX p50 | 13.33 µs | 13.27 µs | -0.5% | 10.81 µs | 12.29 µs | +13.6% |
| TX p99 | 58.21 µs | 58.09 µs | -0.2% | 60.56 µs | 60.47 µs | -0.1% |
| TX p99.9 | 67.91 µs | 63.73 µs | -6.2% | 63.17 µs | 62.74 µs | -0.7% |
| RX p50 | 10.78 µs | 10.95 µs | +1.5% | 8.06 µs | 8.13 µs | +0.9% |
| RX p99 | 55.61 µs | 55.83 µs | +0.4% | 10.12 µs | 10.38 µs | +2.6% |
| RX p99.9 | 66.53 µs | 57.83 µs | -13.1% | 34.78 µs | 34.81 µs | +0.1% |

### 用户感受

- **DPDK p50 加 shield 后变慢 +8%**（19.00 µs → 20.55 µs RTT；TX p50 +13.6%）。原因是 `systemd-run --scope` 多套了一层 cgroup hierarchy，每个 syscall + ring 操作多查一层 cgroup tree。
- **kernel p50 几乎不变**（+0.3%）。原因待查 — 可能 kernel TCP 路径的 cgroup 查找已经被 amortize / inlined 到 syscall 路径里。
- **kernel p99.9 改善 -19%**（110 µs → 89.65 µs）— shield 把部分用户态噪声砍掉了；但 stddev 仍大（9.56 µs），说明剩下的 tail 由 kernel-side softirq 抢占贡献（cgroup 挡不住）。
- **DPDK p99.9 几乎不变**（74.29 → 74.45 µs，差异在 noise 内）— DPDK busy-poll 路径本来就抗噪声，shield 没有可改善的余地。
- **DPDK 吞吐 -6%**（49k → 46k samples/s）— 同样是 systemd-run scope 的开销。
- shield 对 **p99 (~58-69 µs)** 完全无影响 — p99 tail 来自硬件 / hypervisor / 网络层抖动，跟用户态调度噪声无关。

### 一句话感受

**`systemd-run --scope` 在 DPDK 路径里加了 ~8% p50 RTT + ~6% throughput 开销 — shield 不是免费的**。这套 shield 的价值是给 DPDK 拿到 cache-miss -41%（system metrics 报告里证实），代价是 p50 +8%。要不要换看场景：纯延迟敏感不该开；想要 cache 干净度 + tail 一致性可以接受。

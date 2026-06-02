# CPU 不跨核 (no-cross-core) — 概念、机制、工具与工作流

> Mode: freeform（综合指南）｜ 生成 2026-06-01 16:45 (UTC+8)｜ 锚定 HEAD `6d97ac4a`
> 适用：在本项目(eph)上让业务流的收包路径不跨核的工程实践。Host 实证基线：
> AWS EC2 Graviton aarch64 + ENA NIC。

## TL;DR

"不跨核" = **业务流的收包处理和 app 消费在同一个物理 CPU 上**,避免 socket
队列/skb/缓存行在核间弹跳。本项目两条腿的达成路径不同:

- **kernel 路径**:让"软中断协议栈核 == app recv 核"。靠 `RSS → 队列 → IRQ
  affinity → 核` 三段对齐 + app 绑同核。
- **DPDK 路径**:run-to-completion,**没有软中断 → 没有 kernel 式跨核**。但
  RSS 仍决定流落哪条队列 → 哪个 lcore → 哪个 CPU(影响"被 poll 到"和"摆放")。

**关键约束(本机 ENA 实测)**:AWS ENA 的 RSS key 是 **Nitro 托管、guest 读到
的是占位**,离线 Toeplitz 预测 `src_port→queue` **不可靠**(命中率≈随机)。所以
src_port 选择必须**经验实测**,不能算。工具链与库已据此重构(见 §5/§6)。

完整工具链:`nic_lowlat_setup.sh`(地基) → `rss_queue_probe.py`(kernel)/`dpdk_rsskey_probe --finder`(DPDK)选 src_port
→ app 绑核 → `run_bpf_checks.sh` / `cross_core_check.bt`(验证)。

---

## 1. 什么是"跨核",为什么伤延迟

收包从 NIC 到 app 经过若干阶段,每阶段在某个 CPU 上执行。若**生产数据的核**和
**消费数据的核**不同,socket 接收队列 + socket lock + skb/payload 的缓存行要跨
核(尤其跨 NUMA/CCX)弹跳 → 延迟与抖动。HFT 热路径上这是要消除的。

**注意落点**:`recvmsg` 的 copy_to_user **永远在 app 核**(系统调用在进程上下文
跑)。所以"copy 用的核 == app 核"是恒真废话,**不是**判据。真正决定跨核的是
**"哪个核把数据塞进 socket 接收队列(生产)" vs "app 在哪个核消费"**。

---

## 2. kernel 路径:对齐软中断核与 app 核

```
  包到达
    │ ① RSS：NIC 硬件 Toeplitz 哈希 4-tuple → 选 RX 队列 Q   （硬件,不可改 on ENA）
    ▼
  队列 Q
    │ ② IRQ affinity：Q 的 MSI-X 中断 → smp_affinity → 核 C    （/proc/irq）
    ▼
  核 C 跑 NET_RX_SOFTIRQ → ip_rcv → tcp_v4_rcv → 持 socket lock 入 sk_receive_queue  ← 生产
    │
  ③ app 在核 C recvmsg → copy_to_user 出队                                          ← 消费
```

**不跨核 ⟺ ② 的核 == ③ 的 app 核**(① 正常情况下与 ② 同核同次软中断,RPS 是唯一
能拆开它们的东西)。要做到:
- (a) 让业务流的 4-tuple 经 RSS 落到队列 Q —— **凑 src_port**(ENA 无 ntuple,见 §5)。
- (b) 把 Q 的 IRQ 绑到目标核 C(`smp_affinity_list`)。
- (c) app 线程绑核 C。
- 关掉会动态搬核的:RPS / RFS / aRFS / irqbalance;关中断合并降单包延迟。

> 验证用 `tools/bpftrace/cross_core_check.bt`:`@softirq_cpu`(tcp_v4_rcv,按
> ifindex+sport 过滤)`== @recv_cpu`(tcp_recvmsg,按 comm 过滤)`== 业务核` → PASS。
> (注:NAPI poll 落核不是承重判据——它不 copy payload、不碰 socket 队列,正常
> 又恒等于 ②;只在诊断 RPS 时单挂 napi tracepoint。)

---

## 3. DPDK 路径:没有软中断,但 RSS 仍要管

DPDK 是 kernel-bypass:PMD 在某 lcore 上 **poll 队列**,eph 的 `DpdkPoller` 是
**run-to-completion**——poll 到包就在**同一 lcore** 上跑 `on_message`。**poll 核 =
处理核,无交接 → kernel 式软中断跨核不存在。**

但 RSS 在 DPDK 里管两件事:

| | 作用 | 不对齐的后果 |
|---|---|---|
| **投递** | 多队列(尤其 daemon-led 多进程):流只有落在**某 lcore 真在 poll 的队列**才被处理 | 落到没人 poll 的队列 = **静默丢** |
| **摆放** | 队列 → lcore → 物理 CPU | 流落到非预期 lcore = NUMA/cache 错位 |

**重要**:**单 lcore** 的 DPDK(一条关键连接一个 poll 核)**不需要 RSS 工程**——
流落哪条队列都被那个 lcore poll 到、就地处理,没有"选哪个核"。只有**多 lcore
各绑不同 CPU、在意流落哪个核**时,才需要把流的队列 steer 到目标 lcore。

DPDK 的 `queue → cpu` = **lcore 绑定 = app 配置**(不是 IRQ,探测不出来),由
operator 提供(见 §5 的 `--queue-cpu-map`)。

---

## 4. RSS key 的硬约束(AWS ENA 实测,这是全局前提)

实证(`.artifacts/experiment-20260601-142315.md`,commit `23305023`):

> **AWS ENA 上 guest 能读到的 RSS hash key 是占位,不是硬件真正 steering 用的
> key——真 key 被 Nitro 藏死。**

| | kernel(ethtool) | DPDK(rte_eth probe) |
|---|---|---|
| guest 读到的 key | 固定占位 `55:14:…`(实为 per-boot 随机 netdev_rss_key) | per-attach 随机占位 |
| set key | 接受但只是驱动缓存,硬件不认 | `rss_hash_update` 直接拒(-95) |
| `Toeplitz(读到的 key)` 预测落核 | **零相关**(均匀分布) | **命中 3/16 = 随机** |

⟹ **离线 Toeplitz 计算 `src_port→queue` 在 ENA 上根本不成立**(两后端皆然)。
⟹ 唯一可靠手段是**经验实测**:实际发包、看回包落哪个队列/napi。
⟹ 不可跨 attach/重启复用:ENA reset 可能换 key,每次现测。

(能设 key 的 NIC 如 mlx5/i40e 不受此限——可装已知 key、离线算可行。本约束特定于 ENA。)

---

## 5. 怎么做:三步工作流

### 第 1 步 — 铺地基:`tools/nic_lowlat_setup.sh <nic> <business_cpu>`
建立确定的 队列↔核 映射 + 去掉动态搬核:停 irqbalance、队列 IRQ 1:1 绑核(业务
队列→业务核且该核独占)、关 RPS/RFS/aRFS、XPS 对齐 TX、关中断合并(ENA 需
`adaptive-rx off`)。幂等、`--dry-run`、`--restore`。NIC reset 会丢 affinity → 可重放。

### 第 2 步 — 选 src_port(纯经验,不算 Toeplitz)
RSS key 在 ENA 是占位 → 预测不可信,只能实测。两后端各有专用经验探测:
- **kernel:`tools/rss_queue_probe.py`** —— 无状态 raw SYN 勾包 + eBPF 在
  `tcp_v4_rcv` 读**真实 `skb->queue_mapping`**(= RX 队列;实测与 softirq 落核
  100% 自洽、跨次确定)。不建连接、不需 key、限速抗批处理。输出 `by_queue`/
  `by_cpu` 的 src_port,挑落"业务队列(IRQ 已钉 app 核)"的端口。
- **DPDK:`examples/dpdk_rsskey_probe --finder`** —— DPDK 自己发探针实测
  `src_port→queue`(`FINDERMAP <src_port> <queue>`),按 operator 的 lcore→queue
  绑定挑端口。同样纯经验、对占位 key 免疫。

```
# kernel(与 app 用同一 dst IP)
$ sudo tools/rss_queue_probe.py --nic ens6 --dst <venue ip> --dst-port 443 --count 16
  → JSON: by_cpu:{ "1":[33072,33073,…], … } / by_queue:{ "1":[…] }
```
> 旧的 predict-then-verify `rss_srcport_finder.py` 已退役(commit `885212e1`):
> ENA 占位 key 下它 verify 戳穿后拒绝出文件,产不出 src_port。

#### 本机 ENA RSS 实测结构(2026-06-02,可省扫描)
实测本机 ENA 的 `src_port → 队列` **不是逐端口散列,而是规整块状**:
- **低 4 位无关**:同一 16-对齐块(`src_port & ~0xF`)的 16 个端口落同一队列
  (256 端口扫描:16-对齐块内队列不一致的块数 = 0)。
- **周期 128**:`队列 = T[(src_port >> 4) & 7]`,T 是 8 项查找表(本次实测
  queue_mapping `[4,2,1,3,3,1,2,4]`,回文);8 槽对 4 队列**恰好均分**(64/64/64/64)。
- 只用了 src_port 第 4–6 位 → 有效哈希位极少(与 `RETA=128` 一致);读到的占位
  Toeplitz key 算不出这张表(只能实测)。确定性:同 dst+同 boot 100% 复现。
- **优化**:既然低 4 位无关,**挑一个落在 app-核队列的 16-对齐块,块内 16 个端口
  直接够 N≤16 条 conn**——`rss_queue_probe` 只需扫一个块判定其落核,不必收集分散端口。
- **边界**:T 的具体值是 per-(dst IP, 本次 boot key)(key 占位、per-attach 随机)——
  换 dst / NIC reset / 重启须重测;块结构本身只在单 dst 上验过(大概率通用,因是
  src_port 取位的性质)。

### 第 3 步 — 喂进连接 + 绑 app
- kernel:`cfg.kernel.local_bind.port = <选中的 src_port>`;app 线程绑业务核。
- DPDK:`cfg.dpdk.wire.tuple.src_port = <选中>` + `cfg.dpdk.pin_to_queue = <队列>`;
  lcore 绑业务核。

### 验证:`sudo tools/run_bpf_checks.sh <ifindex> <sport> <comm> <cpu>`
并发跑 4 个 bpftrace 检查(`cross_core_check`/`clean_nic`/`nic_check`/`sched_switch`)。
不跨核判据:`@softirq_cpu == @recv_cpu == 业务核`。

---

## 6. 库侧:eph-net-dpdk 退役了运行时 RSS 预测(2026-06-01 reshape)

因为 §4 的占位 key,eph 库**曾在 ENA 上静默把 src_port 选到错队列**(=悄悄跨核,
正是要消除的东西)。两轮 reshape(均 BREAKING)后,DPDK RSS 收敛成**单一经验模型**:

- **开 RSS**:`Platform::create` 在 `configure_port` 里设 `mq_mode=RTE_ETH_MQ_RX_RSS`
  + `rss_hf`(与 NIC 能力取交集)。`nb_rx_queues>1` 且 `rss_hf!=0` → `rss_active`;
  `rss_hf==0` → 硬失败(不静默塌缩到队列 0)。eph **不装也不读 RSS key**。
- **probe**:用 `examples/dpdk_rsskey_probe --finder` 实测 `src_port→queue`。
- **pin**:`DpdkTcpStream/UdpSocket::create_and_attach` 的 `RssPartitioned` 模式
  要求 `pin_to_queue` + 显式 `cfg.dpdk.wire[.tuple].src_port`(finder 实测得来),
  缺失 → 明确 error;**从不预测/改写 src_port**。
- DNS:恒用随机 ephemeral src_port(单队列语义)。`rss_prediction_trusted` 字段已删。

整条 trusted-key 预测面(`rss_using_probed_key`/`rss_key_trusted`/`predict_rss_queue`/
`queue_for_tuple`/`find_src_port_for_queue`/`configure_rss`/`query_rss_state`/`RssState`)
**已删除**。详见 `eph-net-dpdk/CHANGELOG.md` 的 2026-06-02 BREAKING 条目。

---

## 7. 工具参考

| 工具 | 路径 | 作用 | commit |
|---|---|---|---|
| NIC 地基 setup | `tools/nic_lowlat_setup.sh` | IRQ 绑核 + 关 RPS/RFS/aRFS/irqbalance/coalescing + XPS | `d2de749f` |
| src_port 探测(kernel) | `tools/rss_queue_probe.py` | raw SYN + eBPF 读 queue_mapping,纯经验选 src_port(取代退役的 rss_srcport_finder) | `885212e1` |
| 不跨核验证套件 | `tools/run_bpf_checks.sh` + `tools/bpftrace/*.bt` | cross_core / clean_nic / nic_check / sched_switch | `6c4d949c`/`c61b8460` |
| DPDK RSS-key 真伪 / finder 后端 | `examples/dpdk_rsskey_probe.cpp` | 实测 `src_port→queue`(VPC-DNS 反射器绕同实例阻断) | `e6784167`/`6d97ac4a` |
| ENA key 实证报告 | `.artifacts/experiment-20260601-142315.md` | key 占位的完整证据链 | `23305023` |

---

## 8. 陷阱 / caveat

- **ENA RSS key 不可信**:别在 ENA 上信 `ethtool -x` / `rss_hash_conf_get` 的 key 去
  算 src_port。一律经验实测。
- **不可跨 attach/重启复用**:finder 产物只对当前 NIC RSS 状态有效;reset/重启/
  重跑 nic_lowlat_setup 后须**重跑 finder**。
- **同实例 ENI 互通被 AWS 屏蔽**:本机造流量验证 DPDK 收包时,同实例 ENI→ENI
  不通(bench 用 `bench_ns` netns 绕开;DPDK 实测用 VPC DNS 当反射器)。
- **DPDK 单 lcore 无需 RSS 工程**:别给单 poll 核的部署套 src_port 选择——多余。
- **DPDK queue→cpu 探测不出来**:它是 lcore 绑定(app 配置),finder dpdk 后端必须
  靠 `--queue-cpu-map` 提供。
- **NAPI 落核不是判据**:验不跨核看 `@softirq_cpu == @recv_cpu`,不是 NAPI 核。
- **vfio on Graviton 需 noiommu**:`/sys/kernel/iommu_groups` 有项但 guest
  passthrough 不可用 → `enable_unsafe_noiommu_mode=1`。

---

## 9. 两条腿一致吗:统一心智模型与漏点(kernel vs DPDK)

**高层一致**(reshape 刻意做的):两边都能收敛成一句——

> 不跨核 = 让"我这条流被 RSS 分到的队列" ↔ "我的业务核" 对齐。三步:① 队列绑到核;
> ② 经验实测挑一个 src_port 让流落到那个队列;③ 把 src_port 显式喂给 eph。

eph 旋钮也是**镜像**的:探测 `rss_queue_probe` ↔ `dpdk_rsskey_probe`;喂 src_port
`cfg.kernel.local_bind.port` ↔ `cfg.dpdk.wire.tuple.src_port`(都必须显式、都退役了自动预测)。

**但有 4 处 intrinsic 漏点,拿 kernel 直觉套 DPDK 会踩坑:**

1. **单核不对称**:DPDK **单 lcore 啥都不用做**(run-to-completion,poll 核=处理核);
   kernel 单连接仍要把队列 IRQ 钉到 app 核(或 busy-poll)。别给单 lcore DPDK 套 src_port 工程。
2. **做错的后果不同(correctness 级,最危险)**:kernel 没对齐 = **慢**(跨核弹跳,数据还在);
   DPDK 没对齐 = 流落到没人 poll 的队列 = **静默丢包**(broken)。拿 kernel 的"最多慢点"
   直觉套 DPDK 会丢数据还查不出原因。
3. **"队列→核"这端不对称**:kernel 是 IRQ 亲和,**可探测**(`/proc/irq`);DPDK 是你的
   lcore 绑定 = app 配置,**探不出来**,要你用 `--queue-cpu-map` 告诉探测工具。
4. **探测工具天然两套**:kernel 内核看得到包 → eBPF 探(`rss_queue_probe`);DPDK 内核
   bypass、看不到包 → 只能 DPDK 内部自探(`dpdk_rsskey_probe`)。

**记法**:心智模型与 eph 旋钮已统一;但**"DPDK 单核免做"**和**"DPDK 错=丢包而非变慢"**
是 intrinsic 差异,统一不掉,必须分清。

---

## 10. 相关文档

- `eph-net-dpdk/CHANGELOG.md` — RSS-unification reshape 的 BREAKING + 迁移
- `.artifacts/experiment-20260601-142315.md` — ENA RSS key 占位实证(假设/方法/数据/结论)
- `docs/dpdk-setup.md` — hugepages / vfio-pci 绑定环境
- `CLAUDE.md` — `create_and_attach` / DPDK RSS 单一经验模型的库侧描述

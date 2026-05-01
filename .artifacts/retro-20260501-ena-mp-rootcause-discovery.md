---
mode: retro
date: 2026-05-01
scope: ENA MP secondary RX SIGSEGV — methodology retro：根因如何从 "ENA PMD 限制" 翻转到 "eph `~Platform()` 违反 DPDK MP teardown 协议"
worktree: /home/ec2-user/worktrees/ena-mp-root-cause
branch: diag/ena-mp-root-cause
supersedes_framing: eph-net-dpdk/docs/ena-mp-limitation.md（标题与定性即将随 fix 一并被改写）
---

# Retro：ENA MP 根因定位的方法论复盘

## TL;DR

过去约两周，我们一直把这条 SIGSEGV 当作 **"AWS ENA PMD 不支持 secondary RX"** 的硬限制来对待。文档、commit message、TODO、retro 全都按这个 frame 在记账。今天这次 GDB + 反汇编 + ENA 源码三路证据汇合后，认知发生了一次完整的翻转：

> ENA PMD 的行为完全符合 DPDK 协议——primary 调 `rte_eth_dev_stop` 就该拆所有 queue（port 是"system-owned"资源，primary 是 lifecycle owner）。**真正的 bug 是 eph 的 `~Platform()` 不区分 primary 是不是"最后一个活进程"，无条件 `rte_eth_dev_stop` / `rte_eth_dev_close` / `rte_mempool_free`，把 secondary 还在用的共享 hugepage 状态写成 NULL。**

本篇不是 "what" 复盘——根因结论已落在 `eph-net-dpdk/docs/ena-mp-limitation.md` 与 commit `aa625b4d`。本篇是 "how" 复盘：**为什么过去两周一直没看见自己 code 才是协议违规者？哪些 methodological habit 起了作用，哪些把我们困在错的 frame 里？** 这条经验对未来 eph 多进程 / 多用户协议设计的指导意义比根因本身还大。

---

## 1. 时间线与认知弧

### Phase A — "ENA PMD broken" 的过度声明（2026-04-19 → 2026-04-29）

最早的观察：
- `bench-parallel` v1 测出 secondary `rx_burst` 在 primary 持续打流时 SIGSEGV。
- 把 autojoin 路径换成 declarative `Platform::create_secondary` 也照样崩，**用同一个 stack frame**。

那时的产出（被本次 retro 视为"过度声明"）：

| Commit | 标题 | 当时的 frame |
|---|---|---|
| `4c2b08ee` | docs(dpdk): ena-mp-limitation — post-isolation diagnosis + repro | "ENA limitation"；写下 "this is a real ENA limitation, not eph autojoin specific" |
| `f6bde1f7` | fix(ena-mp): precise two-condition root cause + simplified sentinel | 进一步精确化为"两条件"，但**仍把 ENA 当违规方** |
| TODO.md / parallel-bench v1 retro | "ENA PMD MP secondary `rx_burst` is fundamentally broken" | 项目级别记录：广而告之"以后别再撞这堵墙" |

这一阶段做对了什么：
- 记录得很细；把"autojoin 路径 vs declarative 路径"做成 A/B 对照排除了 eph 接入路径的嫌疑。

这一阶段做错了什么：
- **看见 vendor PMD 在 stack 顶端就推断 vendor 错**——是经典的归因偏误。我们没问"primary 这一刻的 lifecycle 在做什么？"
- **写文档动作太早**。`ena-mp-limitation.md` 这个标题一旦落盘，后续每次 review 它都强化"这是 vendor 限制"的心智模型。**Doc 标题有重力**：取错名字会在后续每个读它的人心里再灌一次错的 frame。

### Phase B — 两条件 A/B 隔离（2026-04-30）

实验设计：用 `diag/ena-mp-isolation-2` 分支跑 2×2 正交：

| Primary | Secondary | 结果 |
|---|---|---|
| `lat_tcp_dpdk` 高频 DpdkPoller I/O | `lat_udp_dpdk` 全套 DpdkPoller + DpdkUdpSocket | **CRASH** |
| `lat_tcp_dpdk` 高频 DpdkPoller I/O | minimal raw `rte_eth_rx_burst` 循环 | NO CRASH |
| benign primary（Platform up，无 I/O） | `lat_udp_dpdk` 全套 | NO CRASH（753k 样本 / 50k QPS） |
| benign primary | minimal raw rx_burst | NO CRASH |

得出的"两条件"结论**事后看是对的，但只是机制的一半**：

- ✅ "primary 高频 I/O 是必要条件"——的确：但**真正起作用的不是"高频"，而是"15s benchmark 跑完→`~Platform()`触发"**。我们把 *correlation* 当成了 *mechanism*。
- ✅ "secondary 跑全套 DpdkPoller / DpdkUdpSocket 是必要条件"——的确：因为 raw `rx_burst` sentinel 进程生命周期短，**先于** primary 的 `~Platform()` 退出，race window 不开。我们把 *machinery 复杂度* 当成了 *机制*。

这一阶段做对了什么：
- **A/B 隔离纪律**：每次只动一根杆，不允许同时换 bring-up 路径 + 流量 + 进程 lifetime。这一条是后续能看清的关键。
- 把每根杆的实验脚本（`ena_mp_isolation.sh` / `_step2.sh` / `ena_mp_rootcause.sh`）单独留下，复现 0 摩擦。

这一阶段做错了什么：
- 仍然在"vendor 边界外面"找答案。如果当时问一句**"primary `lat_tcp_dpdk` 退出时，它的 `~Platform()` 对端口做了什么？我们的 secondary 在那一刻是不是还在 poll？"**，至少能省 24 小时。

### Phase C — GDB 抓到 `x2 = 0x0` 的那一刻（2026-04-30 18:42）

在两条件 reproducer 上挂 `gdb -batch` 抓 SIGSEGV，命中 `ena_com_get_next_rx_cdesc + 36`，捕获寄存器：

```
x0 = 0x100393800   ; io_cq* (in 0x100200000–0x100e00000 = 共享 hugepage map 0–6)
x1 = 0x8b0 (2224)  ; byte_offset = ((head-1) & mask) * entry_size
x2 = 0x0           ; io_cq->cdesc_addr.virt_addr  ← 这是关键
x3 = 0x10 (16)     ; entry_size，残留在寄存器
pc = ena_com_get_next_rx_cdesc + 36  ; ldr w3, [x2, w1, sxtw]
```

机器级证据先把 frame 拆掉一半：

1. **`x0` 落在 `0x100200000–0x100e00000`**——`info proc mappings` 显示这个段是 `/dev/hugepages/eph_0000_28_00_0map_*`，secondary 进程地址空间里 **`rw-s`** 共享映射。这就证明 io_cq 不是 primary heap 内的私有结构，而是双进程都看见的共享内存。
- 为什么这个观察重要：它**首次把"primary 写、secondary 读"的因果链落到了同一个物理页**。在此之前我们一直默认 primary heap 与 secondary heap 是隔离的，所以"primary 析构动了 secondary 的内存"这个假设根本没浮出来过。

2. **`x1 = 2224 = 16 * 139`**——逆推：`head ≈ 140`，意味着 RSS 已经把 ~140 个 completion descriptor 投递到 secondary 的 queue 1，secondary 在崩溃那一刻**确实在处理真实流量**。
- 这一步排除了"queue 还没初始化"或"head/tail 错位"的猜测。queue 状态是健康的，但是 ring metadata 指针被人改成了 NULL。

3. **`x2` 之外还能立刻看到的反向证据**：dump `0x100393800` 后 0x40 字节全 0、`0x1003a3800` 也全 0——*if* 我们直接相信这块内存"始终是 0"，就会得出"secondary 根本没拿到正确的 io_cq"。**我们差点就走上这条歧路。**关键纠偏：post-crash dump 看到的是**寄存器命中 fault 之后的瞬时快照**，不能用作"过去这块内存的状态"。`x1 = 2224` 才是反证——bytes 已经出现过非零（head 被推到 140），只是 *现在* 被清零了。

### Phase D — ENA 源码 + librte_net_ena.so 反汇编合龙（2026-04-30 ～ 2026-05-01 凌晨）

寄存器证据指向 `cdesc_addr.virt_addr` 被外力清零。需要回答**"谁清的，什么时候清的"**。

1. 在 DPDK 源码树定位 `ena_com_io_queue_free`（`drivers/net/ena/base/ena_com.c:960-969`）：

```c
static void ena_com_io_queue_free(..., struct ena_com_io_cq *io_cq) {
    if (io_cq->cdesc_addr.virt_addr) {
        ENA_MEM_FREE_COHERENT(..., io_cq->cdesc_addr.virt_addr, ...);
        io_cq->cdesc_addr.virt_addr = NULL;   // ← 写 NULL 进共享 hugepage
    }
    ...
}
```

2. 反汇编 `librte_net_ena.so.25.0` 找谁调它。`ena_stop` 的 prologue（`0x174e0`）：

```asm
17504:  bl   rte_eal_process_type
17508:  cbnz w0, 17694      ; secondary caller? early return -1
1750c:  ...                  ; PRIMARY 继续
17530:  loop over ALL rx_ring[]:  ena_queue_stop(ring[q])
1757c:  loop over ALL tx_ring[]:  ena_queue_stop(ring[q])
```

ENA 这段的语义就此清楚：**ENA 的"per-process 守卫"只防"secondary 来调 stop"，不防"primary 来调 stop 时把 secondary 的 queue 一起拆了"**——因为按 DPDK 模型，`rte_eth_dev_stop` 是 system resource teardown，primary 的语义就是"我替整个系统拆了"。

3. 三路证据交叉：
   - **GDB 寄存器**（runtime）：x2 = NULL，x0 在共享 hugepage。
   - **ENA C 源码**（intent）：函数确实写 `virt_addr = NULL`。
   - **librte_net_ena.so 反汇编**（actual binary）：primary 路径无脑遍历所有 queue。
   - 三者必须互相印证。**任何一个独立来源都不够**：
     - 只有 GDB → "可能是 secondary 自己的 race"。
     - 只有源码 → "也许 distro patch 改过"。
     - 只有反汇编 → "也许那段代码不可达"。

### Phase E — 翻转：ENA 守 contract，eph 才是违规方（2026-05-01 凌晨）

在 Phase D 末尾，我们把 secondary 的栈与 primary 的栈对齐看：

```
primary 进程：                       secondary 进程：
  bench 测 15s 完成                    DpdkPoller::poll()
  return 0                              ↓ rte_eth_rx_burst(port=0, queue=1)
  ↓                                     ↓ ena_com_rx_pkt
  ~Platform() → Impl::cleanup()         ↓ ena_com_get_next_rx_cdesc
  port_started == true                  ↓ ldr w3, [NULL + 2224]
  ↓ rte_eth_dev_stop(port=0)            ↓ SIGSEGV
       → ena_stop primary 路径
            → ena_queue_stop(ring[1])  ← queue 1 是 secondary 的
                 → ena_com_destroy_io_queue
                      → io_cq->cdesc_addr.virt_addr = NULL（共享 hugepage）
```

frame 翻转：
- 旧 frame：**"ENA 不支持 secondary RX"** → 解决路径是"换 NIC / 换 PMD / 单进程 N-lcore"。
- 新 frame：**"eph `~Platform()` 不区分 primary 是不是最后一个活进程"** → 解决路径是 **`MpRegistry::is_last_alive_proc()` gate**：primary 析构时先看一眼 registry，还有别的 proc 活着就跳过 `rte_eth_dev_stop` / `close` / `rte_mempool_free`，把 device-wide teardown 让给最后一个退出的进程。

这才是真根因。ENA 没什么可指责的。

---

## 2. 起作用的 methodology pattern

### A. 严苛的 A/B 隔离

每个实验只动一根杆。先单独证明：
- "autojoin 还是 declarative 不影响"（Phase 1 双脚本）
- "raw rx_burst 还是 DpdkPoller machinery 不影响"（Phase 2 矩阵）

如果 Phase 1 没跑透就上 GDB，被复合变量糊住的概率 100%——会反复在"是 join_dynamic 哪一行"和"是 epoll 哪个 wakeup"之间打转。

### B. GDB 批处理：寄存器先于 process map

捕获顺序是 `info registers` → `bt 30` → `info proc mappings` → `x/40xg $x0`。**寄存器必须最先抓**：post-crash 的内存还会被 `gdb` 自身、内核 / glibc / signal handler 改写，而寄存器在 `received signal` 那一刻已经冻结。这条习惯是 `ena_mp_rootcause.sh` 写得对的关键：先抓 register，再 dump map，再 dump 内存。

### C. 三路证据三角验证

在 Phase D 总结里已说过：**source 代码（`ena_com.c`）+ 二进制反汇编（`librte_net_ena.so` `0x174e0` / `0x17508`）+ runtime 寄存器（GDB x0/x1/x2）必须各自独立指向同一个机制**。少一路就放过——不要在两路证据上发布根因结论。

### D. 从寄存器反推业务量

`x1 = 2224` → `entry_size = 16` → `head ≈ 140`：这一步把"内存看着像被 zero 过"和"系统正在跑真实流量"两个观察连起来，把"secondary 根本没跑起来"的歧路当场堵死。**别只看寄存器是什么，要算它意味着什么。**

### E. "我自己的代码这一刻在做什么" 的提问纪律

最大的方法论教训是这个反向提问：当 stack 顶在 vendor lib 里时，**不要立刻去钻 vendor**，先问 *自己代码* 在 fault 那一刻的 lifecycle 状态。  
具体到这次：primary 跑完 15s benchmark → 进入 `main()` 退出 → 触发 `~Platform()` → 调 `rte_eth_dev_stop`。在我们抓到寄存器后再花 5 分钟读自己的 `Platform::Impl::cleanup()`（`platform.hpp:1677-1681`）就能看到 `rte_eth_dev_stop` 这一行。**这件事本来在 Phase A 就应该问。**

---

## 3. 拖慢我们的 anti-pattern

### A. Premature naming：标题有重力

`ena-mp-limitation.md` 这个文件名一旦写下，后续每次 review 都会强化"vendor 限制"的认知。**Doc 标题应当跟随根因 flip 滞后，不应当领先**。本次的纠偏措施：根因 flip 的下一步紧跟一个 doc 改写 plan（见第 5 节），把这份 doc 从 "ENA limitation" 改写成 "DPDK MP teardown 协议指南"。

### B. 把 post-crash 内存当 live state

Phase C 一度让我们差点说"`io_cq` 的内容自始至终都是 0，secondary 没拿到 queue"。差点就抛掉 `x1 = 2224` 这条反证。**post-mortem 内存 dump = 快照，不等于 history**——只有寄存器和已落盘的 hugepage 文件 mtime 才能讲过去。

### C. 把 *correlation* 当 *mechanism*

两条件 A/B 找到的"primary 高频 I/O"+"secondary 全套 machinery"是**充分条件的相关性**，不是机制。机制是"primary 退出 → `~Platform()` → `rte_eth_dev_stop` 跨进程拆 queue"。  
- "高频 I/O" 让 primary 15s 跑得正常，按时退出 → 真正起作用的是 *按时退出*。
- "全套 machinery" 让 secondary 也按 15s 跑、活到 race window 打开 → 真正起作用的是 *secondary 在那一刻还活着*。

把"两条件"写到 docs 里时，没有把"它们各自为什么必要"算清楚。这一类**条件归纳但不归因**的疏漏，在分布式 race condition 里非常容易反复出现。教训：每个 A/B 列出的"必要条件"都得问一句"它实际上控制的是哪个时序变量"。

### D. vendor first, self last 的归因顺序

文化上根深蒂固的"vendor 库不会错，先怀疑 vendor"——尤其在 stack 顶端就是 vendor symbol 的时候。这次教训反过来：**当 vendor 的 contract 文档允许它做某件事（DPDK 允许 PMD 不支持 secondary I/O burst），而 vendor 实际行为又确实落在该 contract 内，应该先怀疑自己有没有违反 contract**。

---

## 4. 最终机制（一段话总结）

ENA PMD 严格遵守 DPDK 协议：`rte_eth_dev_stop` 是 device-level lifecycle 操作，primary 调用时它会遍历 `rx_ring[]` / `tx_ring[]` 拆所有 queue；`ena_com_io_queue_free`（`ena_com.c:960-969`）把 `io_cq->cdesc_addr.virt_addr = NULL` 写进共享 hugepage 段。eph 的 `Platform::Impl::cleanup()`（`platform.hpp:1677-1681`）在 primary 析构时**无条件**调 `rte_eth_dev_stop` + `rte_eth_dev_close` + `rte_mempool_free`，从未问过"是否还有 secondary 在 attach 这个 port"。当 primary 先于 secondary 退出（15s bench 在两端时长一致时几乎必然），primary 的 NULL 写入立刻通过共享 hugepage 映射变成 secondary 下一次 `rte_eth_rx_burst(0, 1, …)` 的 NULL load → SIGSEGV。`MpRegistry`（`mp_registry.hpp:630-654`）已经维护 per-slot `claimed` 原子位，用它实现 `is_last_alive_proc()` 即可终结 race window，根本不需要 IPC heartbeat。

---

## 5. 对未来 eph 协议设计的教训

### A. RAII 析构必须显式建模 multi-process / multi-user 假设

C++ 默认惯性是 "destructor = 我自己创建的资源全释放"。在 multi-process 场景下，**"creator 的析构 ≠ 全局 cleanup"**——共享资源的 lifecycle 由"最后一个引用者"决定，不是由创建者决定。

操作化建议：
- 对每一个 RAII 析构里的 system-wide 调用（`rte_eth_dev_stop` / `rte_eth_dev_close` / `rte_mempool_free` / 任何写共享 hugepage 的调用），都要列一行 invariant：**"此调用什么时候是安全的？"**
- 如果"此调用安全的前提"包含"我是最后一个 user"，则必须在析构里**显式查证**这个前提。本例中查证手段已就在 `MpRegistry`，零额外基础设施。

### B. attach 时加 refcount 比 fix 时加 refcount 便宜得多

我们其实早就有 `MpRegistry::try_claim_free_slot`（`mp_registry.hpp:588-619`），它在 secondary attach 时已经 CAS 把 slot 从 0 翻成 1。**只差一个对称的 release 顺序约束** + `is_last_alive_proc()` 读取——大约 30 行代码。

如果当初 attach 时就把 "is_last_alive_proc / refcount" 写在 `~Platform` 里，本次 race 永远不会发生。**"先做最小可工作版本，refcount 再说"留下的债是 ~两周 misframing + 多次 GDB session + 一份要重写的 doc。**

### C. Doc framing 必须跟随根因 flip，不能滞后

当机理认知翻转时，第一份要改的产物**就是相关 doc 的标题与 TL;DR**——而不是先去改代码再回头改 doc。理由：
- 后续读者会把 doc 当 mental model 来背，错的标题会持续向下游灌输错的 frame。
- Pull request review、TODO grooming、跨人 onboarding 都会引用 doc——doc 错一段时间，整个团队（即便只有自己）的认知都会被持续污染。

### D. 协议守约方与协议违规方：先看自己再看 vendor

把这个判据写进 `docs/troubleshooting.md` 的 "vendor lib SIGSEGV" 章节会很有用：
1. fault 在 vendor symbol 内部？查 vendor 的 contract 文档：本调用是否被允许做 X？
2. 如果"允许"，再查 *自己代码* 是否在 fault 之前的某个 lifecycle event 触发过 contract 允许它做的事。
3. 只有 1 + 2 都不解释，才进入"vendor bug"假设。

---

## 6. Follow-up（实际在飞的工作）

| 类型 | 对象 | 状态 |
|---|---|---|
| 代码 fix | `Platform::Impl::cleanup()` 的 primary 分支前置 `MpRegistry::is_last_alive_proc()` gate；release 顺序：先 release slot → 检查 alive count → 仅当 0 时执行 `rte_eth_dev_stop` / `close` / `rte_mempool_free` | plan：`/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`（"修复 `~Platform()` 多进程 teardown 协议违规"） |
| Doc 改写 | `eph-net-dpdk/docs/ena-mp-limitation.md` → 重命名 + 重写为 `dpdk-mp-teardown-protocol.md`：从 "ENA 限制" 改成 "DPDK MP teardown 协议指南"，附 do/don't 与 `~Platform()` 实现规范 | 待 fix 落地后立刻改写（紧跟 fix 的同一批 commit） |
| v2 候选 | IPC heartbeat + reaper（处理 primary crash 不优雅退出的情形） | 当前选 A（refcount-based）够用；B 候选保留在 plan 的 "Phase 2.5" 节，按用户决定 deferred |
| Sentinel 保留 | `tests/integration/repro_ena_mp_secondary_rxburst.cpp` 退出 9（idle-ring sentinel） | 维持作为 idle-ring 路径的 regression 哨兵；不删 |
| 重跑验证 | fix 落地后，必须把 `ena_mp_rootcause.sh` 在同一台机器再跑 3 次连续 PASS：primary 先退、secondary 仍能跑满 15s、退出码 0、无 SIGSEGV | 在 fix plan 的 Phase 4 acceptance gate 里 |

---

## 7. 一句话留给未来的我

**当一个 frame 让你两周里不断写新文档去补它的漏洞时，先怀疑这个 frame 本身——而不是去补文档。doc 标题领先于根因，意味着每写一行都是在加固错认知。**

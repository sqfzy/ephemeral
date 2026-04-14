# Discussion Record

## Context
- 时间：2026-04-14 02:50:14
- 耗时：约 4 分 52 秒
- 用户原始需求：dpdk TcpStream create时，是作为一个client，应该在合理的范围随机选一个合适的端口，而不是手动指定（但也要支持这种功能），不是吗？
- 复杂度评估：中
- 讨论轮数：4 轮
- 参与角色：R4 实用主义者、R14 架构师、R3 性能狂热者、R2 极简主义者、R1 风险卫士（全部来自预定义角色库）

## 内容摘要
围绕 `DpdkTcpStream::create` 是否应自动分配 src_port 展开。R4 初始主张 Tokio-style 默认自动分配；R3 举出 flow-director 预注册、交易所端口白名单、bench 可复现三个 HFT 真约束打回 "默认自动"；R14 提出独立 `SourcePortPool` 值类型作为状态归属方案；R1 用 `SourcePortLease` RAII 堵住 reserve→create 之间的泄漏时间窗；R2 基于 "库 API 应让错误不可表达" 的论点有条件让步。最终共识是保持 `TcpConfig::validate()` src_port != 0 约束不变，新增独立 `SourcePortPool`/`SourcePortLease` 值类型 + `DpdkTcpStream::create(cfg, lease)` 新重载作为 opt-in 便利路径，并把 `DpdkPoller::add` 缺失的 4-tuple 唯一性检查作为前置修复。

---

## 事实基础（讨论前代码 review）

- `eph-net-dpdk/include/eph/dpdk/tcp.hpp:81-82` —— `TcpConfig::validate()` 明确拒绝 `src_port == 0`，错误消息："src_port must be explicit (DPDK has no ephemeral port allocator)"
- `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp` —— kernel 后端不需要用户指定 src_port，隐式依赖内核 bind(0)
- `eph-net-dpdk/include/eph/net/dpdk/poller.hpp:192-200` —— `DpdkPoller::add()` 只检查重复的 obj 指针，**不检查重复的 4-tuple**。这是独立的 bug，但被本讨论作为前置条件纳入
- `eph-net-dpdk/include/eph/net/dpdk/poller.hpp:216-222` —— 路由表 entry 的 key 是 `(src_ip, dst_ip, src_port, dst_port)`，所以重复 src_port 会导致路由歧义
- 现有 DPDK 调用点（lat_tcp_dpdk、lat_ex_market_dpdk、lat_ex_order_dpdk、E2E 测试）均为单连接或小规模多连接，每处硬编码 src_port 常量

---

## 第 1 轮 — 各自亮出核心立场

### R4 实用主义者 | 支持自动分配
- KernelTcpStream 从不指定 src_port（POSIX bind(0)）；DpdkTcpStream 作为同一 Stream concept 的另一个实现，违反最小惊讶原则
- Memory 记录了设计偏好："default to Tokio-equivalent patterns over C-style minimal"
- "DPDK 没有 ephemeral port allocator" 是实现缺席不是设计选择
- 方案：StreamConfig 新增 `std::optional<uint16_t> src_port{}`；为空时自动分配

### R14 架构师 | 原则上支持，质疑状态归属
- 默认自动在 concept 对称性上正确，但 DPDK 无内核中介，必须有人持有 "哪些端口已用" 的权威视图
- 候选位置：Eal 单例（粒度不对）/ DpdkPoller（依赖倒置）/ 独立 SourcePortPool（最干净）
- 立场：支持独立 `SourcePortPool`；反对 R4 的 optional 形态（暗藏全局态或隐式依赖）

### R3 性能狂热者 | 反对改变默认，接受可选
- Flow Director 预注册：生产部署把 (src_port, dst_port) 硬编码进 RTE_FLOW 规则
- 交易所端口白名单：部分交易所只放通源端口范围
- 重连 2MSL 陷阱：立即重连撞到刚释放的同一端口 → 远端 TIME_WAIT 拒绝
- Bench 可复现：lat_* 场景需要 src_port 稳定以复现 flow rule
- 立场：默认必须保持显式；可以加 opt-in helper

### R2 极简主义者 | 反对
- 当前 "5 行手滚 counter" 足够：`static std::atomic<uint16_t> next{40000}; cfg.tuple.src_port = next.fetch_add(1);`
- 现有错误消息 "src_port must be explicit (DPDK has no ephemeral port allocator)" 是 self-documenting 约束，不是缺陷
- DPDK 用户已经签署 "我负责底层细节" 契约，再发 POSIX 的糖是把两个模型混在一起

### R1 风险卫士 | 反对静默默认
- Poller 路由冲突静默化风险：DpdkPoller::add 不 reject 重复 4-tuple，偶然碰撞 → 静默数据破坏
- 线程安全：多线程并发 create 从未测试
- 端口耗尽 + 2MSL 回避窗口：长期 reconnect 循环逐步耗尽池子
- 错误消息退步：静默分配后故障在远端握手超时出现，调试路径急剧变长
- 立场：可接受 opt-in 分配器，但必须带 (1) 4-tuple 冲突拒绝、(2) 显式 2MSL 回避、(3) 池耗尽明确错误

---

## 第 2 轮 — 直接反驳，形成联盟

### R4 实用主义者 | 调整立场：接受 opt-in 形态
- 撤回 "create() 默认自动" 的激进版本；R3 的三点真约束被接受
- 改良方案：StreamConfig::src_port 保持 mandatory；新增独立工具类 SourcePortPool，用户在 main 里显式构造
- 反驳 R2：5 行手滚 counter 绕过 2MSL/池耗尽/碰撞检测，"你能写但你会写错"
- 反驳 R3：默认显式和提供对象式 helper 正交
- 联盟 R14

### R14 架构师 | 加强：池对象的边界
- 所有权粒度：每个 (src_ip, port_id, queue_id) 一个池
- 不做状态回收：池只管 "还没交出去的端口"；release 端做 2MSL 等待
- 线程语义：默认 std::mutex（冷路径）
- 反驳 R2：错误消息是教育新手的最差媒介；正确的自文档 API 是类型让错误不可表达
- 反驳 R3：bench deterministic 需求可以通过固定起始点支持

### R3 性能狂热者 | 部分接受，保留红线
- 接受 R4 修正版，红线：池对象不碰 validate()、不碰 StreamConfig 默认值、不进 create() 隐式路径
- 追加硬需求：reserve(preferred) 语义 —— 支持启动时预留端口给 flow-director 规则预下发
- 反驳 R2："DPDK 用户已签 C-style 契约" 是倒因为果 —— 他们签的是 "我控制硬件资源"，不是 "我手动 bookkeeping"

### R2 极简主义者 | 转为条件反对
- 反对力度减弱；精简到一条：凭什么认为这是 v3.3 项目此刻需要的抽象
- 翻仓库：`lat_tcp_dpdk`/`lat_ex_market_dpdk` 等共 <10 个调用点，全部单连接/硬编码常量
- 条件让步：真出现动态多连接用户再加
- 反驳 R14：对 DPDK 用户而言编译期 assert 和 runtime ErrorInfo 成本没区别

### R1 风险卫士 | 对 R3 的 reserve 语义喊停
- R3 的 reserve(preferred) 引入新时间窗风险：reserve → create 失败 → 端口泄漏
- 折中方案：reserve 返回 `[[nodiscard]] SourcePortLease`，move-only RAII，create() 消耗它
- 成功时 lease 生命周期转入 stream，失败时 lease 析构自动还池
- 联盟 R14；反驳 R2：调用点个数 ≠ 需求强度（HFT 部署实践视角）

---

## 第 3 轮 — 聚焦 reserve/lease 方案的边角

### R3 性能狂热者 | 接受 lease 形态，提醒 hot path
- 接受 SourcePortLease 方案
- 约束：lease 只作为 cold-path member 存在（析构时 release），绝不出现在 send/recv hot path
- 同时 `DpdkTcpStream::create(cfg)` 老接口继续存在（不需要 lease），是 overload 而不是替换
- 反驳 R2 "需求密度 <10"：phase-9 scope decision 文档里 Gateway 被砍但多 venue 连接的核心用例没消失

### R4 实用主义者 | 推动 API 最终形状
- 提出具体 API 草案（SourcePortPoolConfig、SourcePortPool、SourcePortLease、DpdkTcpStream::create 双重载）
- TcpConfig::validate() 规则保持不变，新 overload 负责把 lease.port() 写入 tuple

### R14 架构师 | 对 API 签字，反驳 R2
- 微调：SourcePortPoolConfig 不该有 src_ip（暗示跨 IP 校验，误导用户）
- 微调：`acquire(preferred, strict=false)` 的 bool 参数是代码异味，拆成 acquire_preferred / acquire_exact
- 正式拒绝 R2 的 "需求密度 <10"：库 API 不以当前调用点计数，而以 "让未来调用点写错的概率降到零"
- 5 行手滚版每个调用点 20% 写错概率 → 5 处调用点就是 67% 仓库有某处错的

### R1 风险卫士 | 给最终形态上锁
- 签字支持，但追加三条硬约束：
  1. DpdkPoller::add() 必须新增 4-tuple 唯一性检查（独立但前置）
  2. lease → stream 所有权移交失败路径必须有集成测试（故意让 TLS 握手失败，验证池 free count 恢复）
  3. SourcePortPool 内部用位图：range 默认 32768-60999 = 约 28k 位 = 3.5KB 固定分配

### R2 极简主义者 | 最后一次抵抗
- 反驳 R14 "20% 写错概率" 是靠推测得来：现有调用点都是单连接一次性，不触发 2MSL/碰撞/线程安全
- 但承认 R14 "库 API 让错误不可表达" 是更根本的论点
- 准备最后一轮让步，但要求实现总行数 ≤ 200 行作为 governor

---

## 第 4 轮 — 最终立场修正

### R4 实用主义者 | 立场修正
- 最终立场 = 默认显式 + 独立 SourcePortPool + lease RAII
- 相比第 1 轮 "create() 默认自动" 已大幅后退
- 被改变：R3 的 flow-director 论点让我放弃 "默认自动"；R1 的 lease RAII 让我放弃 "optional<uint16_t> 在 cfg"；R14 的 "状态归属必须显式" 让我接受独立池对象
- 未改变：Tokio-style 的期望在 DPDK 也应该被满足，只是路径是 pool+lease 而不是隐式 bind(0)

### R14 架构师 | 立场修正
- 最终签字 API：SourcePortPool + SourcePortLease + DpdkTcpStream::create 双重载
- 被改变：从 "原则上支持但不知道状态放哪" 到完整 API 规格
- 未改变：反对 Eal 全局态、反对 create() 里隐式分配

### R3 性能狂热者 | 立场修正
- 从 "默认必须显式" 走到 "默认显式 + 独立池"
- 关键让步：R1 的 lease RAII + R14 的 acquire_exact 硬约束让我确信 flow-director 工作流在池化形态下完全成立
- 未改变：TcpConfig::validate() 的 src_port != 0 硬约束必须保留

### R1 风险卫士 | 立场修正
- 从 "静默默认不可接受" 走到 "签字支持显式 opt-in 池化"
- 让步基于 R4/R14 的独立池 + R3 的冷路径约束 + 自加的 lease RAII
- 未让步的三条硬条件：(a) Poller 4-tuple 检查 (b) lease 失败路径集成测试 (c) 位图 + 2MSL grace

### R2 极简主义者 | 立场修正
- 让步到 "勉强同意"；被说服于 R14 "让错误不可表达" / R3 "Gateway 砍但多连接需求没消失" / R1 "静默数据破坏 severity" 三论点合力
- 未改变：实现总行数 ≤ 200 行作为软目标，超了触发 PR review 逐行论证
- 明确反对 kernel backend 对称改造

---

## 最终方案

### 核心决策
在 `eph-net-dpdk` 引入独立的 `SourcePortPool` + `SourcePortLease` 值类型，默认保持 `TcpConfig::validate()` 的 `src_port != 0` 硬约束不变，通过 `DpdkTcpStream::create(cfg, lease)` 新重载提供池化路径。Kernel backend 不做任何改动。

### 方案细节

**新增类型**（`eph-net-dpdk/include/eph/net/dpdk/source_port_pool.hpp`，header-only）

```cpp
struct SourcePortPoolConfig {
    uint16_t range_begin{32768};
    uint16_t range_end{60999};              // = Linux ip_local_port_range
    std::chrono::seconds grace{60};         // 2MSL 回避窗口
};

class SourcePortPool {  // 位图实现，~3.5KB 固定分配
public:
    static std::expected<std::unique_ptr<SourcePortPool>, core::ErrorInfo>
        create(SourcePortPoolConfig) noexcept;

    [[nodiscard]] std::expected<SourcePortLease, core::ErrorInfo>
        acquire() noexcept;
    [[nodiscard]] std::expected<SourcePortLease, core::ErrorInfo>
        acquire_preferred(uint16_t p) noexcept;     // 软约束
    [[nodiscard]] std::expected<SourcePortLease, core::ErrorInfo>
        acquire_exact(uint16_t p) noexcept;         // 硬约束
};

class SourcePortLease {     // move-only RAII，析构时 pool->release(port, with_grace=true)
};
```

**DpdkTcpStream 新重载**

```cpp
// 既有签名保留不变
static std::expected<std::unique_ptr<DpdkTcpStream>, core::ErrorInfo>
    create(StreamConfig cfg) noexcept;

// 新增：接受 lease，失败时 lease 析构自动还池
static std::expected<std::unique_ptr<DpdkTcpStream>, core::ErrorInfo>
    create(StreamConfig cfg, SourcePortLease lease) noexcept;
```

- `TcpConfig::validate()` 的 `src_port != 0` 规则不改
- 新重载把 `lease.port()` 写入 `cfg.legacy.tuple.src_port` 再走老 validate 路径
- 若用户在 cfg 里已设 src_port 且与 lease 不一致 → `InvalidConfig`

**前置修复**（独立 PR，本方案前提）

`DpdkPoller::add()`（`eph-net-dpdk/include/eph/net/dpdk/poller.hpp:192-200`）除了现有的 obj 重复检查外，必须新增 4-tuple 唯一性检查 —— 与池无关，是现有 bug。

**范围外**

- Kernel backend 不引入池（kernel 自带 bind(0)）
- Eal/Poller 不持有全局分配状态
- bench 客户端不迁移到池化（保持 src_port 硬编码以维持可复现）

### 已解决的分歧

| 分歧点 | 解决方式 | 关键论据 |
|--------|----------|----------|
| 默认自动 vs 默认显式 | 默认显式，池为 opt-in | R3 flow-director / 白名单 / bench 三个真约束 |
| 分配器状态归属 | 独立值类型 SourcePortPool | R14 "端口所有权必须显式建模" |
| optional<src_port> vs lease 参数 | lease RAII move-only | R1 指出 optional 形态有泄漏时间窗 |
| 软/硬 preferred 约束 | 拆成 acquire_preferred / acquire_exact | R14 反对布尔参数代码异味 |
| 是否支持 bench 路径 | 不支持，bench 保持硬编码 | R3 / R2 一致：bench 需确定性 |
| 抽象必要性 | 接受引入 | R14 "让错误不可表达" 胜过 R2 "调用点密度" |

### 未解决的权衡

| 冲突 | 角色 A | 角色 B | 建议 |
|------|--------|--------|------|
| 实现规模红线 | R2：≤200 行上限 | R1：行数应服从正确性 | ≤200 行为软目标；超 250 行触发额外 review |
| 首批引入时机 | R2：等真需求 | R14/R1：先加避免后续错误 | 若短期有多 venue 接入：立即加；否则记 TODO 推迟到首个多连接 PR |
| Kernel backend 对称性 | R2：反对 | 未表态 | 本 PR 只触 DPDK |

### 会议摘要
- 参与角色：R4 / R14 / R3 / R2 / R1（全部预定义）
- 讨论轮数：4 轮
- 主要争议：(1) 默认是否隐式分配 (2) 分配器状态归属 (3) HFT 约束是否否决便利性 (4) 抽象是否过早
- 收敛路径：R4 "默认自动" 被 R3 HFT 约束打回 → R14 独立池对象 → R1 lease RAII → R4 综合 API → R2 哲学论点下有条件让步
- 最终共识：默认显式 + 独立 SourcePortPool/SourcePortLease + create 双重载 + 前置修 Poller 4-tuple 检查

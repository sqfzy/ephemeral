# Decision Record — eph-venue 模块抽取的 7 个关键决策

> 时间：2026-04-29 10:57 CST · 类型：decision · 状态：已决（待施工）
> 关联：~/.claude/plans/snazzy-swimming-cascade.md（已 ExitPlanMode 通过）
> 起点 HEAD：8790f123

---

## TL;DR

把 4 venue (Binance/OKX/Bybit/Coinbase) 的 subscribe / auth / config wire-layer 抽取为新的 header-only 模块 `eph-venue`，**仅 wire layer 范围**：stateless helpers（不暴露 facade）、不依赖 eph-json、不动 book reconstruction、不含私有频道。顺带补 `eph-json/include/eph/json/adapters/coinbase.hpp`（feat，独立 commit）。测试搬到 `eph-venue/tests/`。预计 12 commits，分阶段分批落地。

---

## 背景

### 触发

今日 (2026-04-29) 完成的 `/pax --auto --loop` 在 main 累积 87 commits 后，TODO.md 的 P2 第一项「`eph-venue` 模块完整抽取」成为 ROI 最高的下一步动作：

- **venue 数量已超 review 元计划设定的 N=3 抽取阈值**：现 4 venue（Binance/OKX/Bybit/Coinbase），再多就强行抄
- **现状重复**：4 个 `tests/integration/test_*_adapter.cpp` 共 ~1900 LOC，~80% 是测试侧 wire 脚手架的重复
- **首次切片已落**：batch 7 commit `771486e6/a881793d/1451e0b2` 已经把测试公用部分抽到 `tests/support/venue_adapter_test_kit.hpp`，但 venue 协议层（subscribe 编码、auth、config builder、ack predicate）仍内联在 4 个 test 文件里
- **核心痛点**：用户接 venue 没有 module 可 import，要复制 ~400 LOC 测试样板

### 约束

| 约束 | 来源 |
|------|------|
| Header-only C++23（不引入 .cpp 到 module include/）| `CLAUDE.md` |
| std::expected<T, ErrorInfo> 错误模型 | `CLAUDE.md` |
| 模块依赖拓扑：sibling backends 永远不互相依赖 | `CLAUDE.md` |
| 每 commit build + 直接相关 test 必须通过（无 broken commit）| 项目惯例 |
| 设计偏 Tokio-style typed wrappers > C-style minimal | memory `feedback_tokio_style` |
| /pax 关键决策必须用户拍板（不能 AI 代决）| memory `feedback_blueprint_interactive` |

### 假设

- 4 venue 现有外部行为（subscribe payload byte 序列、Upgrade headers、JWT byte-equal）必须保持
- `eph-net::build_coinbase_jwt` / `signed_request` / `hmac` 已经存在且稳定，复用不重写
- `eph-book::BinanceBookAdapter` 公共 API 不动
- `tests/support/tls_ws_echo_server.hpp` 不动

---

## 决策矩阵（7 项）

每项决策都跟用户对齐（Phase 0.3 + Phase 2.5 共 7 题），格式：候选 → 决定 → 理由 → 代价。

---

### D1 · 模块覆盖范围

**候选**：

| 选项 | 摘要 | 优势 | 劣势 |
|------|------|------|------|
| **A** 4 venue · 仅 wire layer（subscribe + auth + config）| 抽测试里的协议层 | 与"重复抽取"问题精准对位；最小风险 | 不解决"OKX/Bybit/Coinbase 没 book reconstruction" 缺失 |
| B 4 venue + book 重建 | 顺带为 OKX/Bybit/Coinbase 各加 BookAdapter | 完整 venue 接入栈 | 混合 reshape + feat（3 个 BookAdapter 是新代码不是抽取）；PR 不可审 |
| C 4 venue + 全栈（含 REST snapshot + reconnect resume）| 一次到位 | 最完整 | ~2500 LOC；REST snapshot 各家差异大易 bikeshed |
| D 仅 Binance · 作为模板 | 留 75% 重复在仓里 | 风险最小 | Binance 是 4 venue 里**最不典型**（无 in-band auth、bookTicker 不是标准 orderbook），做模板反而误导 |

**决定**：**A**（4 venue · 仅 wire layer）

**理由**：
- "完整抽取"在现状下真实含义就是 venue protocol 层；book / REST snapshot / reconnect resume 是**正交**的另外几个 axis，混在一起会让 PR 不可审
- OKX/Bybit/Coinbase 没有 book reconstruction 是**功能缺失**不是抽取问题（test 里手工 update 是 placeholder），应该单独 PR 补
- D 否决：Binance 用 bookTicker 是流式 BBO 而非标准 orderbook，"以 Binance 为模板"会让其他 venue 形似神不似

**代价**：
- 用户接 OKX/Bybit/Coinbase market data 仍需自己实现 book reconstruction（或等独立 PR）
- 私有频道（订单流）继续没有 framework 支持

---

### D2 · Book 归属

**候选**：

| 选项 | 摘要 | 优势 | 劣势 |
|------|------|------|------|
| **A** eph-book 继续负责 book 重建 | binance_adapter 留 eph-book；OKX/Bybit/Coinbase 后续在 eph-book 加 | 职责正交清晰：eph-book = 数据结构，eph-venue = 协议接入 | eph-venue 不直接持有 book，用户多写 1 行胶水 |
| B eph-venue 自持 book 引用 + 内置 update 逻辑 | venue::BinanceAdapter 内部构造 ArrayBook | 用户更省事 | eph-venue 同时依赖 eph-book + eph-json，耦合更深；与 D1 wire layer 边界冲突 |
| C 本次只抽 wire 层，book 推后 | 等需求触发 | 最小当前改动 | OKX/Bybit/Coinbase 一直没 book，未来再分 |

**决定**：**A**（eph-book 继续负责）

**理由**：
- **职责正交**：eph-book = 数据结构（`ArrayBook<MaxLevels>` / `MapBook`），eph-venue = 协议接入。两个 module 解决两类问题
- 与 D1 wire layer 的边界一致：venue 抽 protocol，book 抽 reconstruction
- 未来给 OKX/Bybit/Coinbase 补 book 时，独立 PR 加 `eph-book/include/eph/book/{okx,bybit,coinbase}_adapter.hpp`，不与 venue 抽取捆绑

**代价**：
- 用户接 venue 时需要"额外 import + new book adapter + 在 on_message 里手挂"（多 3 行胶水）
- 跨 module 耦合的胶水代码留给 caller 写

---

### D3 · 私有频道范围

**候选**：

| 选项 | 摘要 | 优势 | 劣势 |
|------|------|------|------|
| **A** Public market data only（与现状一致）| 4 venue 都只覆盖 public WS | 与现状 4 个 test 全 public 对齐；无新代码 | 接 venue 私有数据时仍需用户自己实现 |
| B Public + private 全覆盖 | OKX/Bybit op:auth + op:login，Coinbase user channel JWT，Binance listenKey REST | 一次到位 | 工作量翻倍；每家 auth 流程不同；订单状态机本身是大模块 |
| C Public + private auth（不含订单流）| auth 抽取，但下单 / cancel / fills 留给 caller | 中间方案 | 仍是新代码不是抽取；scope creep |

**决定**：**A**（public only）

**理由**：
- 现 4 test 全部只覆盖 public 频道 — 抽取**已经测过的代码**是低风险
- 私有频道是另一个**完整 axis**：每家 auth 流程不同、错误恢复语义不同、订单状态机本身是大模块
- 混合"抽取 + private 新功能"= 不可审 PR

**代价**：
- 当真要接 venue 下单时还要专门做一次（非小动作）
- private 频道相关 follow-up 永远比 public 多

---

### D4 · 测试迁移位置

**候选**：

| 选项 | 摘要 | 优势 | 劣势 |
|------|------|------|------|
| A in-place（test_*_adapter.cpp 留 tests/integration/）| 文件位置不变，内容改用 venue helper | 改动最少 | 违反 per-module tests 惯例（CLAUDE.md）|
| **B** 进 eph-venue/tests/（unit 部分）+ tests/integration/ 留 e2e | 双层覆盖：每 venue unit + e2e | per-module 自带 tests 是项目惯例；layered coverage | 4 个 test 文件要拆分 |
| C 全部搬 eph-venue/tests/，删 tests/integration/ 中的对应 | 集中 | 跨模块 e2e 测试归类不清晰 |

**决定**：**B**（unit 进 eph-venue/tests/，e2e 留 tests/integration/）

**理由**：
- per-module tests 是项目惯例（CLAUDE.md: "Each module owns its tests under `<module>/tests/**.cpp`"）
- venue protocol 测试和 eph-venue 模块**强绑定**，搬过去后归类清晰
- e2e 涉及 TlsWsEchoServer + 实际网络 I/O，本质上跨模块（venue + net + codec），留 tests/integration/ 合理

**代价**：
- 单个 venue 的测试覆盖分两处（要看 happy path 在 eph-venue/tests/，要看 e2e 在 tests/integration/）
- 搬迁工作量略大于 in-place 方案

---

### D5 · API 形态（核心设计岔路）

**候选**：

| 选项 | 摘要 | 优势 | 劣势 |
|------|------|------|------|
| **A** Stateless helpers + VenueId enum | 每 venue 自由函数：make_config / make_subscribe / is_ack / make_subscribe_with_jwt | 最小 LOC（~600）；与 wire layer 定位一致；零状态零内存 | 用户要自己拼 ReconnectOrchestrator 三步（手动 wire on_connect/on_message）|
| B `<Stream>` facade（Tokio-style）| 每 venue 暴露 OkxVenue<Stream> 类，封装 lifecycle | 用户一行 `orch.attach(venue)` | 隐含状态机；超出 wire layer 边界；LOC ~900 |
| C 两者都提供（底层 helpers + 顶层 facade）| 两套 API | 灵活 | LOC 翻倍 (~1200)；测试也翻倍；常常是 over-engineering 的征兆 |

**决定**：**A**（Stateless helpers + VenueId enum）

**理由**：
- **与 D1 wire layer 定位精确对齐**：facade 含轻量状态机（reconnect 时 subscribe 重发逻辑、ack 等待），实质上超出"协议层"边界
- Tokio-style 偏好（memory `feedback_tokio_style`）确实推荐 typed wrappers，但**这里的 wrapper 是 helper 函数 + 类型化参数**（`std::span<std::string_view const>` 而非 `char const*`），不是非要 class 才算 typed
- C 否决：双 API 维护成本翻倍，且会出现"facade 怎么调用 helper" 的内部一致性测试

**代价**：
- 用户接 venue 多写 3-5 行胶水（`orch.on_connect = ...`）— 但这本就是 ReconnectOrchestrator 的标准用法
- 若以后真要 facade，再加一层是 backwards-compat 改动（不会破坏 helpers）

**否决但保留语义的关键点**（防止以后 reset）：
- 推荐函数命名：`make_config(host, port, streams)` / `make_subscribe(streams, req_id=1)` / `is_subscribe_ack(payload, req_id=1)`
- Coinbase 多一个 `make_subscribe_with_jwt(channels, jwt)`
- 每函数 `[[nodiscard]] inline noexcept`（where applicable），SPDLOG_DEBUG 一行

---

### D6 · 入站消息处理（eph-json 依赖）

**候选**：

| 选项 | 摘要 | 优势 | 劣势 |
|------|------|------|------|
| **A** eph-venue 不依赖 eph-json，caller 自己 parse | eph-venue 只做出站（subscribe encode + auth）+ ack predicate（裸字符串匹配）| 模块依赖最小；与 wire layer 一致 | caller 多写一步：parse JSON 再 dispatch |
| B 依赖 eph-json，eph-venue 暴露 typed events | 用户拿到 OkxBookSnapshot/OkxBookDelta 等 typed objects | 用户体验更好 | eph-venue → eph-json → eph-core 链路加深；模块边界模糊 |

**决定**：**A**（不依赖）

**理由**：
- 保持 eph-venue 模块依赖最小（eph-core + eph-net）
- 入站消息的 parse 在 eph-json 已经独立成熟（`binance::BookTicker::from(view)` 等），caller 写 1 行 parse 调用而非"等 eph-venue 帮忙"
- ack predicate 用 `string_view::find` 字符串匹配，不需要 JSON parse

**代价**：
- 用户必须显式 import `<eph/json/adapters/<venue>.hpp>` 自己 parse（多一个 include + 多一行 parse 调用）

---

### D7 · Coinbase parser 是否顺便补

**候选**：

| 选项 | 摘要 | 优势 | 劣势 |
|------|------|------|------|
| A 不补，本次只做 Coinbase 出站（subscribe + JWT）| 范围最小 | 用户接 Coinbase 入站时 inbound 仍裸字符串匹配 | 与 binance/okx/bybit 不对齐 |
| **B** 顺便补 eph-json/include/eph/json/adapters/coinbase.hpp | 4 venue parser 全齐 | 与现有 parser 风格对齐 | **混合 reshape + feat**（reshape 反模式之一）；~150 LOC 新代码 |

**决定**：**B**（顺便补，但**独立 commit 标为 feat**）

**理由**：
- 用户偏好"完整"覆盖（4 venue parser 一致）
- 工作量很有限（~150 LOC + 配套 test）
- 风险隔离：单独 commit 标 `feat(eph-json): coinbase typed parser`，不与 reshape commits 混

**代价**：
- 违反 reshape 反模式 "夹带 fix / feat 改动" — 但通过独立 commit 显式分离已经把风险压到最低
- 计划复杂度 + 1 个阶段（阶段 5 专门做这件事）
- 测试覆盖额外要求：SubscribeAck / Level2Snapshot / Level2Update 各一个 happy + 至少一个 poison-pill

**何时重审**：若 reshape 主体出现 byte-mismatch 或测试退化，**先 revert 阶段 5 这个独立 commit**（最便于隔离），再处理主线问题。

---

## 跨切决策

### M1 · 改造幅度

```
[x] 保行为重构（主体）— 4 venue 现有外部行为不变
[x] 允许行为变化（局部，feat 部分）— 新增 eph-json::coinbase 解析器（无旧实现）
```

**主体**保行为：4 个 integration test 必须以**相同的网络字节流**与 TlsWsEchoServer 交互（subscribe payload byte-equal、ack predicate 行为一致、JWT byte-equal）。

**feat 部分**（D7 决定的 eph-json/coinbase.hpp）是纯新增，无既有行为可破坏，独立 commit 标 `feat(eph-json)`。

### M2 · 迁移策略

**选定**：分阶段分批（checkpoint）

**否决**：
- 一次性：跨 7+ commits 大量改动，一次性提交风险高
- 渐进共存：新模块没有"旧接口"，无需共存期；旧 test 内联代码是 dead code 不是 API

**结构**：12 阶段（基线 + 11 commit + 最终验证），每阶段独立可编译可测试可回滚。详见 `~/.claude/plans/snazzy-swimming-cascade.md` 实施计划章节。

---

## 后续影响

### 对架构

```
                       ┌──────────┐
                       │ eph-core │
                       └─────▲────┘
                             │
         ┌───────────────────┼─────────────────┐
         │                   │                 │
   ┌─────┴────┐       ┌──────┴────┐     ┌──────┴────┐
   │ eph-net  │       │  eph-json │     │ eph-book  │
   └─────▲────┘       └─────▲─────┘     └─────▲─────┘
         │                  │                 │
         └──────────┬───────┘                 │
                    │                         │
              ┌─────┴────┐                    │
              │ eph-venue│  ◄── 新增          │
              └─────▲────┘                    │
                    │ (用户代码)              │
                    └──────────►──────────────┘
                                            (eph-book 由 caller 自己挂)
```

eph-venue 模块新增 = 第 12 个 module；公开依赖 eph-core + eph-net；不依赖 eph-json/eph-book/eph-net-kernel/eph-net-dpdk。

### 对其他模块

- **eph-book**：不动（D2 决策）
- **eph-net**：不动（复用 jwt_signed_request / signed_request / hmac）
- **eph-net-kernel / eph-net-dpdk**：不动（与 venue protocol 解耦）
- **eph-json**：D7 顺带补 coinbase.hpp（额外 ~150 LOC + 配套 test）

### 对测试

- 4 个 `tests/integration/test_*_adapter.cpp` 各 -200~-300 LOC
- 新增 `eph-venue/tests/test_{binance,okx,bybit,coinbase,venue_id}.cpp`（5 个文件）
- 新增 `eph-json/tests/test_coinbase_json.cpp`
- 删除 `tests/support/venue_adapter_test_kit.hpp` 中 venue 特异 helper（保留 `drive_until` / `IncomingSink` / `make_attach` / `make_detach`）

### 对团队

- 新接 venue：模块级决策（新建一个 `.hpp`），不再粘贴测试样板
- private 频道接入仍需独立 PR（D3 限定）
- OKX/Bybit/Coinbase 的 book reconstruction 仍需独立 PR（D2 限定）

---

## 何时重新评估

**触发**：
- 接入第 5 个 venue 时，发现 stateless helpers 用法显著不便（频繁手动拼 ReconnectOrchestrator） → 重审 D5（考虑追加 facade 层，不破坏 helpers backwards-compat）
- 接入私有频道时 → 重审 D3（重新设计 auth + 订单流的边界）
- 用户接 venue 时反复抱怨"手挂 book adapter 麻烦" → 重审 D2（考虑 eph-venue 提供 book hook 接口）
- 出现第 5+ venue 而 venue helpers 模式高度同构 → 考虑加 VenueTraits concept 让代码更 generic

**不会触发的**：
- venue 协议变更（OKX 升级 v6）：在 eph-venue 内部更新，不影响决策
- aws-lc / openssl 切换：不动 venue 模块的 auth 调用（仍 build_coinbase_jwt）

---

## 相关

- **Plan**：`~/.claude/plans/snazzy-swimming-cascade.md`（已 ExitPlanMode 通过，12 阶段实施契约）
- **起点 commit**：`8790f123` (fix(mockex): self-arm PR_SET_PDEATHSIG)
- **TODO.md 关联条目**：`.artifacts/TODO.md` 的 P2 第一项（"抽 eph-venue 模块"）
- **前置 batch**：今日 `/pax --auto --loop` 87 commits 中的 `771486e6 / a881793d / 1451e0b2 / 7f949785`（venue_adapter_test_kit 测试侧第一切片）
- **review 元计划**：`~/.claude/plans/wiggly-napping-duckling.md`（venue 抽取门槛 N≥3，现 N=4 已超）
- **共识基础**：`.artifacts/decision-20260428-093206.md`（22 项 ADR）和 `.artifacts/phase-9-scope-decision.md`（Gateway/CircuitBreaker/SOCKS5 排除决策）

---

> 由 `/report decision` 草拟 · HEAD=8790f123 · 待 confirm 落盘
> Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>

---

## 重审记录 — 2026-04-29 下午

> 时间：2026-04-29 下午 · 触发：用户 "重审并讨论决策" · HEAD=8790f123（未变，未施工）
> 方法：引入 **barter-rs**（https://github.com/barter-rs/barter-rs）作为现代 Rust 量化框架的实物对照，逐项压力测试 7 项决策
> 原文未改，本节为追加修订

### 关键变更

#### D5 → D5-D 修订版（**替换**原 D5-A）

把"API 形态"原本单一维度拆成两个独立子维度（原 D5 矩阵的盲点）：

- **维度 A · 编码 API 形状**：自由函数 → **trait struct（`VenueTraits` concept）**
- **维度 B · 生命周期装配 API 形状**：用户自拼 → **可选薄 generic facade `attach_venue<V>(...)`**

**修订后的 trait 形状**：

```cpp
// eph-venue/include/eph/venue/concepts.hpp
template <class T>
concept VenueTraits = requires {
    { T::name }         -> std::convertible_to<std::string_view>;
    { T::ws_path }      -> std::convertible_to<std::string_view>;
    { T::default_host } -> std::convertible_to<std::string_view>;
    { T::default_port } -> std::convertible_to<std::uint16_t>;
    typename T::Channel;        // 类型安全：barter Connector 同款
    typename T::Market;
    typename T::Auth;           // 默认 NoAuth
    typename T::SubResponse;    // 默认 void（D6 修订预留位）
    // 方法签名（伪）：
    //   T::emit_subscribe(streams, req_id, OutputBuffer&) -> expected<void>
    //   T::is_subscribe_ack(payload, req_id) -> bool
    //   T::ping_interval() -> std::optional<Duration>  默认 nullopt
    //   T::data_channel_auth(OutputBuffer&) -> expected<size_t>  默认 noop
};
```

**对 D5-D 初版草稿的 4 处修订**（基于 barter-rs 实物对照得出）：

1. `make_subscribe -> string` 改为 `emit_subscribe(buffer&)` —— Coinbase 实际是"每 (channel, market) 一帧"非 batch；barter 用 `requests() -> Vec<WsMessage>` 解决
2. 新增 `ping_interval()` hook —— Bybit/OKX 应用层 ping 必需，原草稿漏；barter `Connector::ping_interval()` 默认 None
3. 新增 `Auth` 关联类型 + `data_channel_auth()` —— barter 也未解决，我们要自己设计；分阶段于 WS handshake 后、第一帧 SUBSCRIBE 前执行
4. 新增 `Channel` / `Market` 关联类型 —— 类型安全，barter 同款

**v2 预留扩展点**（v1 设计保证向前兼容、不实现）：

- `VenueStreamSelector<V, Kind>` 二层 concept：trade vs depth vs book 分流（barter `StreamSelector<Kind>` 对应物，文件 `barter-data/src/exchange/mod.rs:53-61`）
- `VenueBook<V, BookT>` 二层 concept：book reconstruction 挂载
- v1 约束：`VenueTraits` 本体不长 Kind / Book 字段，未来走独立 concept 注入

**移除原 D5-A**（stateless free functions）的关键理由：

- 与本仓库既有 `Stream` / `Codec` / `Poller` concept-driven 风格不一致
- barter-rs 实物证明：现代静态语言的同类项目无人选 free functions（barter README 没解释为什么用 trait + 泛型 —— 因为对 Rust 是显然默认）
- 痛点消化率从 ~20%（D5-A）提到 ~70%（D5-D）：原 D5-A 只消化 ~20-30 LOC/venue，D5-D 通过 attach helper 多消化 ~25 LOC/venue 的 orchestrator 装配样板

#### D6 → A2（**替换**原 A）

**修订后的决定**：`is_subscribe_ack(payload, req_id) -> bool` 谓词 + 预留 `using SubResponse = void;` 关联类型

理由：

- **保留 codebase "parser 与 protocol 拆开" 惯例**：eph-venue 仍只依赖 `eph-core` + `eph-net`，不强加 `eph-json` 给只用 wire 层的用户
- **HFT 性质对齐**：substring 谓词（数十 ns，SIMD 加速 `find`）比 JSON parse（微秒级）快 1-2 个数量级；ack 虽是冷路径，但保持简单更好
- **向前兼容**：未来若需要升 typed，加默认实现走 `SubResponse` 路径，旧 venue impl 仍编译通过
- **D7 解耦**：coinbase.hpp 不再是 eph-venue 硬依赖，回归独立 feat commit 性质
- 与 D5-D v2 扩展点逻辑一致：v1 都用"占位关联类型 + 默认 void/NoAuth"，v2 才填实

**否决 B1**（barter 同款强制 typed `SubResponse`）的关键理由：barter 选 B1 是因为 Rust 里 serde 是事实标准、所有 crate 都吃；C++ 这边 `eph-json` 是可选 parser 模块，不应盲目镜像。

#### D3 微调：hook 重命名

`connect_time_auth` → `data_channel_auth`，**明确只服务 market data 频道**。

防止未来 private 频道接入时误把 venue-data 的 auth hook 当订单流签名用 —— 它们生命周期阶段（WS upgrade headers vs post-handshake login frame vs HTTP REST signature）、签名材料（API key vs JWT vs HMAC）、错误恢复语义（重连重签 vs 单次失败终止）都不同。命名上钉死边界更安全。

### 未变更的决策（重审后仍维持）

| ID | 决定 | 重审后理由强化 |
|---|---|---|
| D1 | A · wire layer only | 已被 D5-D 修订版的 trait 形状自动定型；barter `Connector` 同样严格 wire 层 |
| D2 | A · book 留 eph-book | barter 把 book 排除在 `Connector` 外；与刚否决 v1 加二层 concept 的逻辑自动一致 |
| D3 | A · public only（含 hook 重命名） | barter 把 public/private 拆成**两个独立 crate**（`barter-data` vs `barter-execution`），强力背书未来路径：sibling 模块 `eph-venue-exec` |
| D4 | B · unit 进 `eph-venue/tests/`, e2e 留 `tests/integration/` | 新 trait 形状让 unit 测试更值钱：每 venue 可写 4-5 个 < 10ms 的纯 unit 测试 |
| D7 | B · coinbase.hpp 独立 feat commit | D6=A2 让 coinbase.hpp 不再是 eph-venue 硬阻塞，纯 commit hygiene 性质 |
| M1 | 保行为重构 + 局部 feat | 不变 |
| M2 | 12 阶段分批 | 总阶段数不变；阶段 1-3 内容需按 D5-D 修订版重写（plan 后续修订项） |

### barter-rs 对照速查

| eph-venue 要素 | barter-rs 对应 | 关键文件 |
|---|---|---|
| `VenueTraits` concept | `Connector` trait | `barter-data/src/exchange/mod.rs:68-136` |
| Binance impl 形态 | `impl<Server> Connector for Binance<Server>` (PhantomData 标签型，可平移到 C++ 标签型模板参数) | `barter-data/src/exchange/binance/mod.rs:60-103` |
| `attach_venue<V>` 装配 | `MarketStream::init` blanket impl | `barter-data/src/lib.rs:177-280` |
| `ReconnectOrchestrator` 正交 | `ReconnectingStream` 独立 trait + 组合子 | `barter-data/src/streams/reconnect/stream.rs:12-54` |
| 未来 `eph-venue-exec` 路径 | `ExecutionClient` 独立 trait family | `barter-execution/src/client/mod.rs:24-99` |
| `ping_interval()` hook | `fn ping_interval() -> Option<PingInterval>` 默认 None | `barter-data/src/exchange/mod.rs` |
| v2 `VenueStreamSelector` 预留位 | `StreamSelector<Instrument, Kind>` 二层 trait | `barter-data/src/exchange/mod.rs:53-61` |
| Coinbase N-frame-per-sub 模式 | `requests() -> Vec<WsMessage>` 多帧 | `barter-data/src/exchange/coinbase/mod.rs:74-88` |

barter-rs **未解决我们要解决的问题**：connect-time auth（barter 的 execution 层 Binance/Coinbase client 是空 stub `barter-execution/src/client/binance/mod.rs`，仅 mock 实现）。我们的 D3 hook + D5-D `Auth` 关联类型设计在这一点上**比 barter 更完整**。

### 7 项决策最终状态

| ID | 最终决定 |
|---|---|
| D1 | A · wire layer only |
| D2 | A · book 留 eph-book |
| **D3** | **A · public only（hook 命名 `data_channel_auth`）** |
| D4 | B · unit 进 `eph-venue/tests/`，e2e 留 `tests/integration/` |
| **D5** | **D-修订版 · trait + 薄 generic facade + 4 hooks + v2 预留 `VenueStreamSelector` / `VenueBook`** |
| **D6** | **A2 · bool 谓词 + 预留 `SubResponse = void`** |
| D7 | B · coinbase.hpp 独立 feat commit |

### 后续动作（不在本文件范围）

- [ ] plan `~/.claude/plans/snazzy-swimming-cascade.md` 按 D5-D 修订版重写（阶段 1-3 内容变化最大）
- [ ] 施工时按 trait 形状先写 `eph-venue/include/eph/venue/concepts.hpp` + `noauth.hpp`
- [ ] D3 重命名 `data_channel_auth` 写进 trait 注释与文档

---

> 重审由 Opus 4.7 (1M context) 主导 · 用户逐项拍板（D5 / D6 单独展开 / 重命名 + 其余 6 项批量 confirm 经选项 a 隐含）· HEAD 仍为 8790f123 未施工
> Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>

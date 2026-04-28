# eph TODO

> 最后更新：2026-04-28（review + T1/T2/T3 全链 ship 后快照）
> 当前 main：`71e72903`（origin/main 同步）
> **核心遗留 = 0**。所有 review meta-plan "必做"项已 ship 到 main。本表是"做了更好 / 有特定触发条件再做"的清单。
>
> 相关产物：
> - `.artifacts/decision-20260428-093206.md` — 22 项 ADR（review + T1/T2/T3 决策链）
> - `.artifacts/phase-9-scope-decision.md` — Gateway / CircuitBreaker / chunked / SOCKS5 排除决策
> - `~/.claude/plans/wiggly-napping-duckling.md` — review 元计划 + T1.3 plan

---

## A. review meta-plan 内 — 2 项未做

| # | 任务 | 工作量 | 风险 / 拦路 |
|---|------|--------|------------|
| **T3.16** | 硬件 RX 时间戳（`PKT_RX_TIMESTAMP`，DPDK only；NIC 不支持时降级 TSC） | 中 | 与 T2.8 / T2.9 / T2.12 在 poller / platform / tcp_stream 层有潜在 merge-conflict（原本因此延后） |
| **T3.19** | 配置入口统一（StreamConfig / TlsConfig / ProxyConfig 收敛） | 小-中 | 跨 eph-net / eph-net-kernel / eph-net-dpdk 多 config 表面，易引连锁改动 |

---

## B. T1/T2/T3 documented gaps — 3 项

| 来源 | 缺口 | 触发条件 / 工作量 |
|------|------|-----------------|
| T2.4 + T2.4-FU | TLS resumption 2 个测试 SKIP | aws-lc-specific 架构性限制 — 需 aws-lc 行为变化或项目切 OpenSSL（见 ADR D22） |
| ~~T2.10~~ | ~~真 JWT 接 venue mock~~ | **已 ship**（2026-04-28）— `tests/integration/test_coinbase_adapter.cpp` 现使用 `build_coinbase_jwt` + 服务端 `EVP_DigestVerify` 端到端校验签名 |
| T2.10 | OKX/Bybit 私有频道 `op:login` 集成测试 | scope-narrow 决议（D24）— 不写 helper class，但可补一个测试演示 in-band login pattern |

---

## C. review 没列、被点名为"现在 unblocked"的后续工作 — 1 项核心

| 任务 | 触发条件 | 现状 |
|------|---------|------|
| **抽 `eph-venue` 模块** | review 决议"等 N≥3 venue 模板沉淀" | **门槛已过**（现 N=4：Binance + OKX + Bybit + Coinbase）；4 个 venue adapter test 文件结构 ~80% 重叠，可下沉为 `eph-venue::BinanceAdapter` / `eph-venue::OkxAdapter` 等 |

---

## D. `/pax --ship` 闸需要的事项 — 5 项（如果想正式发版）

| Gate | 内容 |
|------|------|
| 1 review | 全项目 diff（不是只针对 eph-net-dpdk） |
| 2 test | `xmake run -g tests` 全量一次（之前只逐个跑） |
| 3 **bench** | DPDK 真机 bench baseline 对比（一直 SKIP；本仓 host 实有 vfio-pci NIC，per CLAUDE.md） |
| 4 doc | CHANGELOG entry / release notes |
| 5 tag | `git tag vX.Y.Z` + push tag |

---

## E. CLAUDE.md 显式 deferred 的更长期项

| 类别 | 候选 |
|------|------|
| 可观测性 | OpenTelemetry SDK adapter / tracing context / Histogram metrics（如 `reconnect.duration_ns` 升级为 distribution） |
| 测试 | libFuzzer 扩展（现仅 ARP/DNS/ICMP/UDP，缺 TLS record / WS frame） |
| 测试 | TSan / ASan CI gate 规律化 |
| 性能 | `bench_reconnect_orchestrator` / `bench_http_client` 量化 hot path |
| 运维 | 生产部署 cookbook（colo + AWS 双形态） |
| CI | GitHub Actions 跑 `xmake -g tests` |

---

## F. 已显式搁置（review 决议明确不做） — **不算遗留**

| 项 | 理由 | 引用 |
|----|------|------|
| Gateway / CircuitBreaker | venue 异构性大；框架化 ROI 负 | phase-9 archive |
| chunked HTTP / SOCKS5 / Expect:100-continue | HFT 不用 + 攻击面 | phase-9 archive |
| Lossless multicast (PGM/Aeron) | crypto 无多播 MD | review |
| IPv6 | crypto venue 全 IPv4 | review |
| NUMA 感知（除 mempool 外） | colo 单 socket 居多，ROI 低 | review |

---

## 推荐排序（按 ROI / 解锁价值）

| 优先级 | 项 | 理由 |
|-------|-----|------|
| **🔴 P1** | 接第一个真 venue 验证 reconnect 24h cycle | 唯一能验证 D22 的方式（看 OpenSSL 后端 resume 是否真工作）+ 暴露真实 venue 边角 |
| **🟡 P2** | 抽 `eph-venue` 模块（C 类） | 4 venue 重复样板下沉；review 等待门槛已过 |
| **🟡 P2** | T2.10 收尾 — 真 JWT wire 进 coinbase adapter test（B 类） | 30 行改动；闭合 documented gap |
| **🔵 P3** | T3.19 配置入口统一（A 类） | 清债，让 onboarding 路径更顺 |
| **🔵 P3** | T3.16 hw timestamp（A 类） | 工作量中；ROI 看是否有跨主机时间戳对比需求 |
| **🟢 P4** | `/pax --ship` 走完 5 Gate（D 类）正式发版 | 当且仅当真需要 vX.Y.Z tag |
| **⚪ P5** | E 类长期项（OTel / Histogram / Fuzzer 扩展 / CI） | 项目稳了再看 |

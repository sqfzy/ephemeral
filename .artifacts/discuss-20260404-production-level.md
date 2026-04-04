# Discussion Record

## Context
- 时间：2026-04-04 14:15
- 耗时：约 8 分钟
- 用户原始需求：对于 eph-net, eph-transport, eph-dpdk, eph-utils，我觉得似乎还没有达到生产级别
- 复杂度评估：高
- 讨论轮数：6 轮
- 参与角色：R1 风险卫士, R3 性能狂热者, R10 安全专家, R13 测试驱动者, R14 架构师

## 内容摘要

4 个模块经代码审计发现 12 个 CRITICAL/HIGH 级问题，主要集中在并发状态机竞态（transport stop/reconnect）、TLS 安全缺陷（RX 序列号耗尽无自动重连、cert soft pin 默认放行）、输入验证缺失（timestamp assert 在 release 消失、RST 无序列号验证）和生命周期管理（Gateway void* 悬垂指针、AuditLog 多线程竞争）。核心争议在于 R3 认为"控制面 bug 不阻塞上线"，被 R10 证明 TLS 序列号耗尽是数据面问题后让步。最终达成三阶段方案：Phase A 修 5 个 P0（阻塞上线），Phase B 修 5 个 P1（上线后 1 周），Phase C 架构重构（下个 milestone）。

---

## P0 修复清单（阻塞上线）

| # | 修复项 | 模块 | 文件 | 实现要点 |
|---|--------|------|------|----------|
| 1 | stop/reconnect guard | transport | transport.hpp | spin-wait on reconnecting + 5s timeout + forced reset |
| 2 | TLS RX seq reconnect | transport | rx_worker.hpp | 95% 阈值触发 do_reconnect() |
| 3 | cert pin 默认拒绝 | transport | tls_session.hpp | 有 pin 无回调 = reject |
| 4 | timestamp assert→check | utils | timestamp.hpp | `if (ms < 0) return 0` |
| 5 | RST 序列号验证 | dpdk | tcp.hpp | RFC 5961 window check |

依赖顺序：1→2→{3,4,5}
每项修复附带 1 个回归测试。

## P1 修复清单（上线后 1 周）

| # | 修复项 | 模块 | 文件 |
|---|--------|------|------|
| 6 | Gateway void*→shared_ptr | net | gateway.hpp |
| 7 | AuditLog per-slot committed flag | utils | audit_log.hpp |
| 8 | HttpClient check-before-append | net | http_client.hpp |
| 9 | RateLimiter clock rewind guard | net | rate_limiter.hpp |
| 10 | Histogram establish_size overflow | utils | hdr_histogram.hpp |

## Phase C（下个 milestone）

- Transport reconnect 互斥协议统一
- Gateway 所有权模型重设计（weak_ptr + RAII）
- AuditLog 无锁 MPSC 重写

## 已解决的分歧

| 分歧点 | 解决方式 | 关键论据 |
|--------|----------|----------|
| 控制面 bug 是否阻塞上线 | P0 中 3 项是数据面必须修 | R10 证明 TLS 序列号是数据面问题 |
| RST 验证优先级 | P0（从 P1 提升）| RFC 5961 合规 + 成本仅 1ns |
| 修复 vs 测试顺序 | 并行执行 | 某些修复是显然正确的 1 行改动 |
| DNSSEC 优先级 | P2（降级）| 专线网络场景风险缓解 |
| Gateway 修复阶段 | Phase B | void* UB 不是可延期的技术债 |
| spin-wait vs mutex | spin-wait + 超时 | non-blocking + 超时兜底 |

## 未解决的权衡

| 冲突 | 选项 A | 选项 B | 建议 |
|------|--------|--------|------|
| stop() 超时后行为 | log ERROR + 继续 | log ERROR + abort() | 生产选 A，测试选 B |
| AuditLog 重设计 | per-slot committed flag | SeqLock | Phase A/B 用 flag，Phase C 评估 SeqLock |

## 模块评分

| 模块 | 架构 | 实现 | 测试 | P0 后 | P1 后 |
|------|------|------|------|-------|-------|
| eph-transport | A | B+ | B- | B+ | A- |
| eph-dpdk | A- | B+ | C+ | B | B+ |
| eph-net | B+ | B | C | B- | B+ |
| eph-utils | A | B | C+ | B+ | A- |

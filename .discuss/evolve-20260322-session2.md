# Evolve Report

## 概况
- 时间：2026-03-22
- 目标：ephemeral 全项目
- 发现缺口：10 个（Tier 1: 3, Tier 2: 3, Tier 3: 4）
- 已完成：6 个（Tier 1: 3, Tier 2: 3）
- 终止原因：Tier 1 + Tier 2 全部完成

## 项目画像

**定位**：C++23 header-only 超低延迟 WebSocket (WSS) 库，支持 DPDK 内核旁路和 POSIX socket
**核心能力**：
  - 用户态 TCP 状态机 + TLS 1.3 (aws-lc AEAD)
  - RFC 6455 WebSocket 帧编解码（批量缓存 CSPRNG 掩码）
  - 无锁 SPSC 队列（BoundedQueue / EvictingQueue + 字节变体）
  - 自动重连、CPU 亲和性绑定、结构化日志
**架构概要**：5 层模块 eph-base → eph-utils → eph-containers → eph-net → eph-dpdk
**当前成熟度**：生产就绪（285 个测试全部通过，E2E 延迟 164-441ns）
**已有测试覆盖**：容器/协议帧/TLS 记录/HTTP 升级/TCP concept 全面覆盖

## 缺口全景与优先级

### Tier 1 — 必须做
1. **std::formatter 特化** — TcpState/TransportEvent/TransportState/TransportStats 无法用 std::format ✅
2. **连接元数据暴露** — Transport 不暴露 TLS 版本/密码套件 ✅
3. **指数退避重连** — 固定间隔重连是反模式，可能导致雷群效应 ✅

### Tier 2 — 应该做
4. **send_n 热路径堆分配** — 每次 make_unique 违背零分配热路径设计 ✅
5. **HdrHistogram 百分位报告** — 无格式化输出方法 ✅
6. **TransportStats 格式化 dump** — 调试需手动打印每个字段 ✅

### Tier 3 — 可以做（未完成）
7. URI 解析（wss:// → host/port/path）
8. Transport 级集成测试
9. 使用示例代码
10. 非 TLS ws:// 支持

## 推进记录

### 缺口 1: std::formatter 特化
- 方案：为 TcpState/TransportEvent/TransportState/TransportStats 添加 std::formatter 偏特化
- 改动：tcp_concept.hpp, transport.hpp, test_tcp_concept.cpp
- 测试：4 个新增
- 提交：4bba7f5

### 缺口 2: 连接元数据暴露
- 方案：do_connect() 成功后捕获 tls_version_/cipher_name_，通过 string_view 访问器暴露
- 改动：transport.hpp, test_tcp_concept.cpp
- 测试：1 个新增
- 提交：3a5b487

### 缺口 3: 指数退避 + 抖动重连
- 方案：base * 2^(attempt-1) + ±25% 抖动，新增 max_reconnect_backoff 配置（默认 16x base）
- 改动：transport.hpp, test_tcp_concept.cpp
- 测试：2 个新增
- 提交：2a1c21b

### 缺口 4: send_n 零分配优化
- 方案：BoundedQueue::try_produce_n(n, visitor) 就地写入预留槽位，替换 make_unique 临时数组
- 改动：bounded_queue.hpp, transport.hpp, test_bounded_queue.cpp
- 测试：8 个新增（4 case × 2 容量模板）
- 提交：cb691d7

### 缺口 5: HdrHistogram 百分位报告
- 方案：report(title, unit) 输出 p50/p90/p99/p99.9/p99.99 + min/max/mean/stddev
- 改动：record.hpp, test_record.cpp
- 测试：3 个新增
- 提交：b46180e

### 缺口 6: TransportStats 格式化 dump
- 方案：dump() 成员方法按 TX/RX/WebSocket/lifecycle 分类多行输出
- 改动：transport.hpp, test_tcp_concept.cpp
- 测试：1 个新增
- 提交：71563d8

## 代码变化统计
- 修改文件：7 个
- 净行数：+428 / -21
- 新增测试：19 个（总测试数从 ~266 增至 285）

## 未完成缺口
| 缺口 | Tier | 建议跟进 |
|------|------|----------|
| URI 解析 (wss://) | 3 | 下次 /evolve 继续，复杂度低 |
| Transport 集成测试 | 3 | 需要 loopback mock server，单独 /test 更合适 |
| 使用示例代码 | 3 | /doc 或 /feature 推进 |
| 非 TLS ws:// 支持 | 3 | 需架构调整（可选 TLS 层），建议 /design 讨论 |

## 成熟度变化
- 之前：接近生产就绪（API 功能完备但缺乏惯用 C++23 集成和健壮重连）
- 之后：生产就绪（格式化/可观测/健壮性/性能均达标）
- 关键变化：std::format 全面支持 + 零分配热路径 + 工业级重连策略

## 索引标签
标签：C++23 WebSocket TLS DPDK SPSC lock-free low-latency header-only formatter backoff observability

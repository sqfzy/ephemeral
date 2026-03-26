# Evolve Report

## 概况
- 时间：2026-03-22
- 目标：ephemeral (全项目)
- 发现缺口：8 个（Tier 1: 3, Tier 2: 3, Tier 3: 2）
- 已完成：6 个
- 终止原因：Tier 1 + Tier 2 全部完成

## 项目画像

**定位**：高性能 C++23 header-only 库，基于 DPDK 用户态 TCP 栈提供超低延迟 WebSocket 通信
**核心能力**：
  - 无锁 SPSC 有界队列 + wait-free 淘汰队列（含字节变体）
  - 用户态 TCP 状态机（DPDK 旁路内核）
  - TLS 1.3 AEAD 记录加解密（AES-256-GCM via aws-lc）
  - WebSocket 帧编解码（RFC 6455）
  - 通用 WebSocket Transport：自动重连、ping/pong、CPU 亲和性
  - 高精度 TSC 计时、HdrHistogram、大页内存分配
**架构概要**：base → utils → containers → net → dpdk 分层 header-only
**当前成熟度**：功能基本可用 → 接近生产就绪
**已有测试覆盖**：16 个测试文件，覆盖容器、网络协议、加密层

## 缺口全景

| 序号 | 维度 | 缺口描述 | Tier | 状态 |
|------|------|----------|------|------|
| 1 | API 完备性 | Transport recv() 丢失 opcode 信息 | 1 | ✅ 已完成 |
| 2 | API 一致性 | BoundedQueueBytes 缺少 size/empty/full | 1 | ✅ 已有 (发现前已存在) |
| 3 | 可观测性 | EvictingQueueBytes 缺少写入计数 | 1 | ✅ 已完成 |
| 4 | 可观测性 | Transport 缺少 reset_stats() | 2 | ✅ 已完成 |
| 5 | 健壮性 | TransportConfig 无验证方法 | 2 | ✅ 已完成 |
| 6 | 可观测性 | EvictingQueue 主模板无写入计数 | 2 | ✅ 已完成 |
| 7 | API 完备性 | WebSocket 缺少 build_pong_frame() | 3 | ✅ 已有 (发现前已存在) |
| 8 | 测试缺口 | Transport 无单元测试 | 3 | 搁置（需大量 mock） |

## 推进记录

### 缺口 1: Transport recv() 丢失 opcode
- 需求：用户无法区分接收到的 text/binary 帧
- 方案：新增 `recv(callback<data, len, opcode>)` 重载 + `try_recv_msg()` 返回 `ReceivedMessage{data, opcode}`
- 改动：eph-net/include/eph/net/transport.hpp
- 测试：编译验证（Transport 需要 DPDK 端到端测试）
- 提交：30e81eb

### 缺口 3: EvictingQueueBytes 写入计数
- 需求：高频场景下无法监控数据吞吐
- 方案：新增 `total_pushed()` 暴露 writer 端 push_count_
- 改动：eph-containers/include/eph/containers/evicting_queue_bytes.hpp
- 测试：4 个新测试（starts at zero, increments, not on oversized, persists across clear）
- 提交：30e81eb

### 缺口 4: Transport reset_stats()
- 需求：Stats 持续累积，无法做窗口化测量
- 方案：新增 `reset_stats()` 将所有计数器归零
- 改动：eph-net/include/eph/net/transport.hpp
- 测试：编译验证
- 提交：30e81eb

### 缺口 5: TransportConfig validate()
- 需求：无效配置延迟到运行时才暴露
- 方案：新增 `validate()` constexpr 方法，检查 host/port/path/burst/timeout 等，集成到 `create()`
- 改动：eph-net/include/eph/net/transport.hpp
- 测试：7 个新测试（valid default, empty host, zero port, empty path, zero burst, negative reconnect, zero timeouts）
- 提交：30e81eb

### 缺口 6: EvictingQueue write_count()
- 需求：Reader 无法知道 Writer 总共写了多少条数据
- 方案：主模板暴露 `global_index_` relaxed load；Capacity=1 特化暴露 `seq_/2`
- 改动：eph-containers/include/eph/containers/evicting_queue.hpp
- 测试：6 个新测试（starts at zero, increments, unaffected by clear × 2 variants）
- 提交：30e81eb

## 代码变化统计
- 新增文件：0 个
- 修改文件：6 个
- 净行数：+232 / -2
- 新增测试：17 个

## 未完成缺口
| 缺口 | Tier | 建议跟进 |
|------|------|----------|
| Transport 单元测试 | 3 | 需要完整 mock TcpTransport + TlsSession；建议下次 /evolve 或 /test 专项覆盖 |

## 成熟度变化
- 之前：功能基本可用
- 之后：接近生产就绪
- 关键变化：补全了可观测性（write_count, total_pushed, reset_stats）、防御性验证（TransportConfig::validate）、接收端信息完整性（recv opcode）

## 索引标签
标签：C++23 networking DPDK WebSocket TLS SPSC lock-free observability API-completeness

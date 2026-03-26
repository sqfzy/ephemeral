# Evolve Report

## 概况
- 时间：2026-03-22
- 目标：ephemeral 全项目
- 发现缺口：10 个（Tier 1: 2, Tier 2: 4, Tier 3: 4）
- 已完成：5 个（Tier 1: 2, Tier 2: 3）
- 终止原因：Tier 1 + Tier 2 可实施项全部完成

## 项目画像

**定位**：高性能用户态 WebSocket-over-TLS 网络库，基于 DPDK 绕过内核，面向超低延迟场景
**核心能力**：
  - 无锁 SPSC 队列（bounded + evicting），cache line 对齐
  - 用户态 TCP 状态机（DPDK 数据平面）
  - TLS 1.3（aws-lc/BoringSSL），自定义 BIO
  - WebSocket RFC 6455 完整实现
  - 通用 Transport 层（TCP->TLS->WS），自动重连
  - TSC 高精度计时 + HdrHistogram 延迟直方图
  - CPU 拓扑、线程绑核、大页内存、ARP 解析
**架构概要**：header-only 模块链 eph-base -> eph-utils -> eph-containers -> eph-net -> eph-dpdk
**当前成熟度**：功能基本可用
**已有测试覆盖**：16 个测试文件，覆盖队列、加密、HTTP、WebSocket、网络头部、CPU、大页等

## 缺口全景

| 序号 | 维度 | 缺口描述 | Tier | 状态 |
|------|------|----------|------|------|
| 1 | 健壮性 | Logger stdout_color_mt 无防重复注册保护 | 1 | **已修复** |
| 2 | 健壮性 | RAND_bytes 返回值未检查（ISN 生成） | 1 | **已修复** |
| 3 | 测试缺口 | Align<T> 公开工具无测试覆盖 | 2 | **已补充** |
| 4 | API | BoundedQueue 缺少 size() | 2 | 已存在（误报）|
| 5 | API | Transport 缺少 send_binary 便捷方法 | 2 | **已添加** |
| 6 | 健壮性 | ws_echo_client CLI 参数 std::stoi 无异常保护 | 2 | **已修复** |
| 7 | API | Transport::send 返回 int 非 std::expected | 3 | 未处理 |
| 8 | 可观测性 | Stats 无重置能力 | 3 | 未处理 |
| 9 | 功能 | EvictingQueue 缺少 pop_oldest | 3 | 未处理 |
| 10 | 测试缺口 | Transport 编排层零测试覆盖 | 3 | 未处理 |

## 推进记录

### 缺口 1: Logger 防重复注册
- 需求：spdlog::stdout_color_mt() 在同名 logger 已存在时抛异常，导致运行时崩溃
- 方案：改为 get-or-create 模式 — spdlog::get() 优先，不存在时再创建
- 改动：11 个文件（全部 logger 初始化点）
- 测试：101 个已有测试通过
- 提交：d9fd6ca

### 缺口 2: RAND_bytes 返回值检查
- 需求：generate_isn() 和 ws_echo_client 端口选择未检查 RAND_bytes 返回值
- 方案：检查返回值，失败时使用 time-based fallback + ERROR 日志
- 改动：tcp.hpp, ws_echo_client.cpp
- 测试：无新增（DPDK 依赖）
- 提交：854ea86

### 缺口 3: Align<T> 单元测试
- 需求：alignment.hpp 中的 Align<T> 模板变量无测试覆盖
- 方案：新增测试覆盖小类型提升、过对齐保留、精确 cache line、混合成员
- 改动：新增 tests/test_alignment.cpp
- 测试：5 个新测试全部通过
- 提交：6010600

### 缺口 5: send_binary 便捷方法
- 需求：Transport 有 send_text 但无 send_binary，API 不对称
- 方案：新增 send_binary 方法，与 send_text 对称
- 改动：transport.hpp
- 测试：无新增（编译验证通过）
- 提交：1afc4b4

### 缺口 6: CLI 参数校验
- 需求：std::stoi 在非法输入时抛异常导致 crash
- 方案：替换为 std::from_chars + 友好错误信息
- 改动：ws_echo_client.cpp
- 测试：无新增（示例程序）
- 提交：361915e

## 代码变化统计
- 新增文件：1 个 (test_alignment.cpp)
- 修改文件：12 个
- 净行数：+114 / -18
- 新增测试：5 个

## 未完成缺口

| 缺口 | Tier | 建议跟进 |
|------|------|----------|
| Transport::send 返回 int 非 std::expected | 3 | 需评估下游影响，可在下次 API 重构时一并处理 |
| Stats 无重置能力 | 3 | 添加 reset_stats()，低优先级 |
| EvictingQueue 缺少 pop_oldest | 3 | 需谨慎设计，涉及 SeqLock 语义变更 |
| Transport 编排层零测试覆盖 | 3 | 工程量大，需先设计 mock TCP 框架 |

## 成熟度变化
- 之前：功能基本可用
- 之后：功能基本可用（健壮性显著提升）
- 关键变化：消除了 2 个运行时崩溃/安全隐患，补齐了公开 API 测试覆盖和接口对称性

## 索引标签
标签：C++23 networking DPDK WebSocket TLS low-latency lock-free SPSC

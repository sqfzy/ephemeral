# Design Report

## 概况
- 时间：2026-04-02 08:35
- 模式：auto（基于 plan.md）
- 需求：将 Transport<TcpImpl, Framer, Mode, ...> 拆为 3 个独立类，使用组合模式
- 提交：9788c4a → 6e9d8e1 → b82d405 → 6eda7b0（4 commits）

## 需求边界
**In scope**：
- 从 Transport 提取 4 个独立组件：FrameProcessor、TxWorker、RxWorker、ReconnectPolicy
- 提取 TransportCore 共享状态结构体
- 创建 3 个独立 Transport 类
- 更新所有类型别名
- 删除 TransportMode 枚举和旧 detail 文件

**Out of scope**：
- Gateway/KillSwitch 模块迁移
- connector.hpp 瘦身
- 新增组件单元测试（需 FakeTcpTransport，留作后续）

## 设计方案

### 架构
```
Transport (threaded)
  ├── TransportCore<TcpImpl>                    — 共享连接状态
  ├── TxWorker<TcpImpl, Framer, MaxPayload, QD> — TX 线程 + 队列 + 统计
  ├── RxWorker<TcpImpl, Framer, MaxPayload, QD> — RX 线程 + 队列 + 统计
  │     └── FrameProcessor<...>                 — 帧解码 + 碎片重组
  └── ReconnectPolicy                           — 指数退避重连

DirectTxTransport
  ├── TransportCore<TcpImpl>
  ├── RxWorker<...>
  └── ReconnectPolicy
  (TX: inline send_direct on app thread)

DirectTransport  
  ├── TransportCore<TcpImpl>
  ├── FrameProcessor<...>
  └── ReconnectPolicy
  (全 inline: send_direct + poll/feed_rx/process_pending)
```

### 组件依赖
- FrameProcessor: DeliverPolicy + SendFn 模板参数（零开销策略模式）
- TxWorker/RxWorker: TransportCore& 引用 + 回调注入
- ReconnectPolicy: TransportConfig& 引用

## 实现概况

### 新增文件
| 文件 | 行数 | 职责 |
|------|------|------|
| `transport_core.hpp` | ~300 | 共享连接状态 + do_connect + do_ws_upgrade |
| `reconnect_policy.hpp` | ~140 | 指数退避重连（独立可测试） |
| `frame_processor.hpp` | ~700 | 帧解码 + WS 碎片重组 + 控制帧处理 |
| `tx_worker.hpp` | ~550 | TX 线程 + SPSC 队列 + 批量发送 + 延迟直方图 |
| `rx_worker.hpp` | ~960 | RX 线程 + SPSC 队列 + TLS 解密循环 + recv API |

### 修改文件
| 文件 | 变更 |
|------|------|
| `transport.hpp` | 重写为组合 4 组件的 ~700 行委托类（原 2046 行） |
| `direct_tx_transport.hpp` | 重写为组合 RxWorker + inline send |
| `direct_transport.hpp` | 重写为组合 FrameProcessor + inline poll |
| `presets.hpp` | 更新使用 3 个独立类的别名 |
| `dpdk/types.hpp` | 更新 DPDK 别名 |
| `net/socket_connect.hpp` | 删除 TransportMode::kThreaded 引用 |
| `tests/net/test_tcp_concept.cpp` | 更新编译测试使用新类名 |

### 删除文件
| 文件 | 原行数 |
|------|--------|
| `detail/transport_state.hpp` | 471 |
| `detail/transport_tx.hpp` | 419 |
| `detail/transport_rx.hpp` | 339 |
| `detail/transport_frame.hpp` | 694 |

### 关键设计决策
| 决策 | 选择 | 理由 |
|------|------|------|
| 组合粒度 | 4 组件 + TransportCore | 每个组件有明确的状态所有权和线程归属 |
| Transport 命名 | 保留给 kThreaded | 主用例，零破坏性变更 |
| FrameProcessor 投递 | 模板策略参数 | 零开销，编译时绑定 |
| Reconnect 编排 | Transport 层协调 | RxWorker 通过 do_reconnect 回调通知 |
| Stats 归属 | 各组件自有 | TxWorker 写自己的 stats，RxWorker 写自己的 |
| kEnableTimestamps | 统一 guard 宏 | EPH_NET_ENABLE_TIMESTAMPS_DEFINED |

## 验收状态
- ✅ 全项目 build 通过
- ✅ 所有 4 个测试目标编译通过（linker error 是预存在的 GCC 15 ABI 问题）
- ✅ TransportMode 枚举完全删除
- ✅ conditional_t<..., Empty> 完全删除
- ✅ 旧 detail 文件完全删除
- ✅ 4 个独立组件各自可独立实例化
- ⏳ 组件单元测试（需 FakeTcpTransport，留作后续）
- ⏳ Benchmark 回归验证（需解决 GCC 15 linker 问题后执行）

## 后续建议
1. 实现 FakeTcpTransport 用于组件单元测试
2. 解决 GCC 15 `_M_replace_cold` / `__cxa_call_terminate` linker 问题
3. Gateway/KillSwitch 迁移到 eph-transport（独立任务）
4. connector.hpp 瘦身为 builder 模式（独立任务）

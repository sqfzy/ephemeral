# Discussion Record

## Context
- 时间：2026-04-02 10:57:00
- 耗时：约 8 分钟
- 用户原始需求：重新设计bench_market_**和bench_pingpong_**。不依赖真实binance，只使用mock server。去掉冗余的bench，只需要以下bench: 1. 订单发送和接收 2. 单连接多币种接收行情 3. 多连接多币种接收行情。mock server不需要部署到另一个服务器，但需要保持kernel和dpdk公平。指标只看mock server的数据包的打点->App打点的时间。
- 复杂度评估：中
- 讨论轮数：4 轮
- 参与角色：R3 性能狂热者、R2 极简主义者、R14 架构师、R4 实用主义者、R5 第一性原理者

## 内容摘要
核心争议围绕 mock server 应走内存注入还是真实网络路径展开。R3 主张内存注入最公平（测纯 Transport 处理延迟），R14 反驳指出这无法测后端差异（WsMockTcpTransport 替代了真实后端本身）。R4 提出 tap 虚拟网卡方案作为折中——mock server 用 kernel TCP 监听 tap 接口，Socket/DPDK 客户端都通过同一 tap 接口通信。R5 确认 tap 是最佳折中（两种后端经历相同的 tap 延迟，差异来自 Transport 层）。最终全员收敛：tap + 同进程 kernel TCP mock server + 无 TLS + 3 个精简 bench。

---

## 最终方案

### 核心决策
用 tap 虚拟网卡 + 同进程 kernel TCP mock server 实现公平的 Socket vs DPDK benchmark，从 9 个 bench 精简为 3 个。

### Mock Server 设计
```
同进程线程 → kernel TCP server → 监听 tap0:port
  ├── 接受 WS 连接，完成 HTTP Upgrade 握手（无 TLS）
  ├── 按配置频率推送 mock JSON bookTicker 数据
  ├── 每条消息 "T" 字段 = TSC::now() 纳秒时间戳
  └── 支持 echo 模式（回复客户端发送的"订单"帧）
```

### 网络拓扑
```
  tap0 (10.0.0.0/24)
    │
    ├── Mock Server: kernel TCP server on 10.0.0.1:9999
    ├── Socket client: kernel TCP connect to 10.0.0.1:9999
    └── DPDK client: --vdev net_tap0, 10.0.0.2 → 10.0.0.1:9999
```

### 3 个 Benchmark

| Bench | 场景 | Mock 模式 | 指标 |
|-------|------|-----------|------|
| bench_order_rtt | 订单发送和接收 | echo（回复客户端帧） | 发送打点 → 回复接收打点 |
| bench_market_single | 单连接多币种 | N 币种推送同一连接 | mock "T" 打点 → app on_message 打点 |
| bench_market_multi | 多连接多币种 | 每币种独立连接 | mock "T" 打点 → app on_message 打点 |

### 统一指标
- min / p50 / p99 / p99.9 / max（纳秒）
- msg/s throughput
- Socket 和 DPDK 分别报告

### 文件结构
```
benchmarks/
├── mock_ws_server.hpp        ← mock WS server
├── bench_order_rtt.cpp       ← 订单 RTT
├── bench_market_single.cpp   ← 单连接多币种
├── bench_market_multi.cpp    ← 多连接多币种
└── bench_common.hpp          ← 共享工具
```

### 已解决的分歧
- 内存注入 vs 网络 → tap 虚拟网卡（R3 接受 R4 折中）
- 复杂 MockMarketFeed 类 vs 简单函数 → 简单函数 + 线程（R14 接受 R2 简化）
- 复用 WsMockTcpTransport vs 新写 → 新写 mock WS server（R2 接受 R14 论证）
- TLS vs 无 TLS → 无 TLS（R14 提议，全员同意）

### 未解决的权衡（需用户决策）
- tap 接口创建：自动化（需 root）vs 手动（setup 脚本）→ 建议提供 setup 脚本
- DPDK vdev：net_tap vs net_af_packet → 建议先用 net_tap

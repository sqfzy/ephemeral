# Design Report

## 概况
- 时间：2026-04-14 07:05
- 耗时：约 15 分钟
- 模式：默认（auto）
- 需求：删除 KernelTcpStream / DpdkTcpStream 中从未被读取的死字段 `reconnect_policy_` 与 `StreamConfig::reconnect`，将重连责任明确归到调用层
- 讨论轮数：0（设计决策来自前置 /discuss，5/5 共识，无需重复评审）
- 参与角色：继承自 discuss-tcp-create-retry-20260414-070000（R1 风险卫士, R3 性能狂热者, R5 第一性原理者, R7 用户代言人, R14 架构师）
- 提交：4fb76ca（已 push）

## 需求边界

### In scope
- 删除 `KernelTcpStream::reconnect_policy_` 及其初始化器与 include
- 删除 `DpdkTcpStream::reconnect_policy_` 及其初始化器与 include
- 删除 `kernel::StreamConfig::reconnect` + 相关 include
- 删除 `dpdk::StreamConfig::reconnect` + 相关 include
- 同步删除 `test_stream_config_validation.cpp` 的 2 个过时测试
- 重命名 `test_dpdk_tcp_stream.cpp` 中名字误导的 `DefaultReconnectPolicyIsDefault` → `DefaultConnectTimeoutIs3000ms`（该测试实际测的是 connect_timeout）
- 重写 `examples/production_client.cpp` 展示外部 `ReconnectPolicy` 循环
- 新增 `examples/session_reconnect.cpp` 极简 demo + 设计注释
- 两个模块 CHANGELOG 加 BREAKING 小节

### Out of scope
- 保留 `eph::net::ReconnectPolicy` 类本身（纯数学，仍然正确）
- 保留 `test_net_reconnect_policy.cpp` 独立测试
- 不提供 `create_with_retry()` helper（避免再次引入"看起来自动的"API）
- 不改 `test_tls_record.cpp` 预先存在的编译错误（与本次改动无关）

## 设计方案

### 核心数据结构 / 模块结构

```
删除的字段：
  eph::net::kernel::StreamConfig::reconnect    (ReconnectPolicyConfig)
  eph::net::dpdk::StreamConfig::reconnect      (ReconnectPolicyConfig)
  eph::net::kernel::KernelTcpStream::reconnect_policy_
  eph::net::dpdk::DpdkTcpStream::reconnect_policy_

保留的：
  eph::net::ReconnectPolicy (class, 纯 backoff 数学)
  eph::net::ReconnectPolicyConfig (struct, 保留作为独立配置类型)
```

### 接口 — 正确使用模式

```cpp
ReconnectPolicy policy{ReconnectPolicyConfig{
    .initial_backoff = 100ms,
    .max_backoff     = 5s,
    .max_attempts    = 0,  // unlimited
}};

while (running && policy.should_reconnect()) {
    auto stream_r = KernelTcpStream<Codec>::create(cfg);
    if (!stream_r) {
        std::this_thread::sleep_for(policy.next_backoff());
        continue;
    }
    auto stream = std::move(*stream_r);
    poller->add(stream.get());
    policy.reset();
    // 协议层：FIX Logon / seq sync / ITCH snapshot / WS subscribe
    run_session(*stream);
    poller->remove(stream.get());
    // 循环回到 create
}
```

### 关键设计决策

| 决策 | 选择 | 否决项 | 理由 |
|------|------|--------|------|
| create() 是否内部重试 | 否 | (B) 内部吞掉循环 | HFT 会话恢复需协议层知识 (FIX Logon/seq)；create 在 attach Poller 之前无监督者；max_attempts×max_backoff 破坏延迟可预测性 |
| reconnect_policy_ 死字段去留 | 删除 | (C) 保留并暴露 accessor | 全仓库零生产读点；保留会加深"配了即生效"的错觉 |
| ReconnectPolicy 类去留 | 保留 | 一起删 | 纯数学，独立可测，正是 session manager 层需要的 primitive |
| 是否提供 create_with_retry helper | 否 | 提供 | 会再次创造"看起来自动"的 API，违背讨论共识 |
| Example 位置 | 项目根 `examples/` | `eph-net-kernel/examples/` | 项目惯例：所有 example 都在根 `examples/`，可编译、有 xmake target |
| 是否保留旧 production_client | 重写 | 保留不动 | 旧版本展示的是已删字段的用法，会误导用户 |

## 实现概况

### 新增文件
- `examples/session_reconnect.cpp` — 极简 reconnect loop demo（113 行）
- `.artifacts/design-drop-stream-reconnect-field-20260414-070500.md` — 本报告
- `.artifacts/discuss-tcp-create-retry-20260414-070000.md` — 前置讨论记录

### 修改文件
- `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp` — 删 include / 成员 / 初始化
- `eph-net-kernel/include/eph/net/kernel/config.hpp` — 删 include / 字段
- `eph-net-kernel/tests/test_stream_config_validation.cpp` — 删 2 个过时测试
- `eph-net-kernel/CHANGELOG.md` — BREAKING 小节
- `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` — 删 include / 成员 / 初始化，注释更新
- `eph-net-dpdk/include/eph/net/dpdk/config.hpp` — 删 include / 字段
- `eph-net-dpdk/tests/test_dpdk_tcp_stream.cpp` — 重命名误导测试
- `eph-net-dpdk/CHANGELOG.md` — BREAKING 小节
- `examples/production_client.cpp` — 重写为外部 ReconnectPolicy 循环模式
- `xmake.lua` — 新增 `session_reconnect` target

### 测试结果

| 测试 | 数量 | 结果 |
|------|------|------|
| test_stream_config_validation | 65 | ✓ |
| test_net_reconnect_policy | 11 | ✓ |
| test_kernel_tcp_stream | 6 | ✓ |
| test_kernel_tcp_stream_behavioral | 60 | ✓（含所有 Reconnect_* 用例） |
| test_dpdk_tcp_stream | 8 | ✓ |
| test_ws_handshake | 14 | ✓ |
| test_tcp_concept + test_tcp_state_name | 9 | ✓ |
| test_transport_e2e | 3 | ✓ |
| **总计** | **176** | **✓ 0 failures** |

`test_tls_record.cpp` 的编译错误为预先存在（上一次 DPDK port-zero commit 时已观察到），与本次改动无关。

## 后续建议

- **优先级中** - `test_tls_record.cpp:1236,1242,1243,1249,1254` 有一组预先存在的编译错误（把 `std::expected<void, ErrorInfo>` 当 `string_view` 用）。不是本次范围，但已连续两次提交中观察到，建议单独修一次。
- **优先级低** - 将来如果真需要 "框架管重连"，应该在 Session Manager 层（而非 Stream 层）实现，并把 FIX Logon seq、kill switch、primary/backup 路由作为显式参数而非藏在 `ReconnectPolicyConfig` 里。

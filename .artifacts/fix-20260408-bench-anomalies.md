# Fix Report — bench data anomalies

## 概况
- 时间：2026-04-08
- 错误类型：逻辑错误（测量正确性 bug）
- 提交：`7b00bc8`

## 异常现象（来自全量 bench 数据）

| # | 现象 | 严重度 |
|---|------|------|
| 1 | ws_echo RTT p50=50-60µs（预期 ~25µs），TX 样本数仅 RTT 的 30% | 高 |
| 2 | ws_echo RTT p99 硬截断在 100µs（恰好 send interval） | 高 |
| 3 | udp_relay 64B kernel p99=71µs（其他 payload p99 ~30µs） | 中 |
| 4 | udp_echo 1472B DPDK max=2133µs | 低 |
| 5 | kernel tcp_echo 1460B RX p50=55µs | 信息（NIC GRO） |

## 根因分析

### 根因 #1：ws_echo 异步测量 + 在飞包错配

**位置**：`bench_ws_echo.cpp`

**触发条件**：客户端按固定 100µs 间隔持续发送，不等待响应。当 RTT ≥ 100µs 时：
- 第 N 个 send 触发，`last_send_tsc = T_N`
- 第 N+1 个 send 触发，`last_send_tsc = T_(N+1)`（覆盖了 T_N）
- 第 N 个响应到达，但 `last_send_tsc` 已被覆盖
- on_message 错误地用 `recv_tsc - T_(N+1)` 计算 RTT，结果可能是负数（被 guard 过滤）或异常小值

**证据**：
- RTT p99 = 96-99µs（恰好接近 100µs 上限）
- TX 样本数 6k-10k，RTT 样本数 21k-34k（30% 命中率）
- 30% 是"在 send_interval 内回包"的成功率

### 根因 #2：mock 在 echo_mode 仍推送 market ticks

**位置**：`mock/mock_ws_server.hpp:431-433`

**触发条件**：mock server 主循环无条件推送 market data ticks（`if (now - last_tick >= config.tick_interval)`），即使 `echo_mode=true`。客户端 on_message 收到 tick 后视为响应，更新 RTT histogram。

**证据**：
```cpp
{"scenario":"ws_echo","leg":"rtt","samples":21441}  // RTT 样本
{"scenario":"ws_echo","leg":"tx", "samples":6225}   // TX 样本
{"scenario":"ws_echo","leg":"srv","samples":10001}  // server stamp 样本（=真正的 echo 数）
```
RTT 样本远多于 srv 样本，差额就是 spurious ticks。

### 根因 #3：UDP recv 2s 超时阻塞

**位置**：`bench_udp_echo.cpp:73`, `bench_udp_relay.cpp:102`

**触发条件**：单个丢包导致 `recvfrom()` 阻塞 2 秒，整个测量循环停滞。每个被影响的 payload 的 throughput 暴跌，导致样本数和 cold-start 异常。

**证据**：udp_relay 64B kernel 样本数 72k vs 128B 的 130k（约 1.8x 差距），暗示 64B 期间发生了多次 stall。

## 修复

### 改动摘要

| 文件 | 改动 | 行数 |
|------|------|-----|
| `mock/mock_ws_server.hpp` | echo_mode 时跳过 tick 推送 | +5/-1 |
| `bench_ws_echo.cpp` | 重写为同步 send→wait→record→loop；filter 只接受 echo 响应 | +51/-44 |
| `bench_udp_echo.cpp` | recv 超时 2s → 100ms | +3/-1 |
| `bench_udp_relay.cpp` | recv 超时 2s → 100ms | +5/-2 |

### 关键 diff

```cpp
// mock/mock_ws_server.hpp
- if (now - last_tick >= config.tick_interval) {
+ // Skip in pure echo_mode: pushing ticks would pollute the
+ // client-side RTT measurement.
+ if (!config.echo_mode && now - last_tick >= config.tick_interval) {

// bench_ws_echo.cpp - sync rewrite
- while (timer.is_running() && ...) {
-     if (now >= next_send) { /* send */ }
-     transport.poll();  // async: response handled in callback whenever
- }
+ while (timer.is_running() && ...) {
+     pending_send_tsc = TSC::now();
+     transport.send_text(...);
+     got_response = false;
+     while (!got_response && timer.is_running() && ...) {
+         transport.poll();  // sync: poll until THIS response arrives
+     }
+     if (timer.is_warmup()) continue;
+     // Record measurements (no in-flight ambiguity)
+ }

// bench_udp_*.cpp - shorter timeout
- timeval tv{.tv_sec = 2, .tv_usec = 0};
+ timeval tv{.tv_sec = 0, .tv_usec = 100'000};  // 100ms, not 2s
```

### 修复逻辑

1. **同步 ws_echo**：每次只允许 1 个 in-flight 消息，消除 last_send_tsc 错配的可能性。匹配 raw tcp_echo / udp_echo 的设计哲学。
2. **mock 跳过 ticks**：根本上消除 spurious 消息源，不依赖客户端过滤。
3. **客户端 echo filter**：防御性，未来 mock 改动不会重新引入污染。
4. **短 recv 超时**：单包丢失最多 stall 100ms，远短于 2s，对吞吐量影响可忽略。

## 验证结果

### ws_echo 修复前后对比

| Transport | Payload | Metric | Before | After |
|-----------|---------|--------|-------:|------:|
| kernel | 64B | RTT p50 | 33.2µs | **21.7µs** |
| kernel | 64B | RTT p99 | 97.2µs (capped) | **26.7µs** |
| kernel | 64B | TX p50 | 26.7µs | **11.4µs** |
| kernel | 64B | RX p50 | 70.2µs ❌ | **10.1µs** ✓ |
| dpdk | 64B | RTT p50 | 61.2µs | **20.9µs** |
| dpdk | 64B | RTT p99 | 96.3µs (capped) | **30.1µs** |
| dpdk | 64B | TX p50 | 53.3µs | **13.2µs** |

### udp_relay 修复前后对比

| Transport | Payload | Metric | Before | After |
|-----------|---------|--------|-------:|------:|
| kernel | 64B | RTT p99 | 71.0µs ❌ | **28.0µs** ✓ |
| kernel | 64B | RTT p50 | 23.5µs | 22.5µs |

### 健全性检查

修复后 ws_echo RTT (21µs) 与 raw TCP RTT (24µs) 的差仅 ~3µs，与 WS framing 开销预期一致。tail 不再被 send_interval 硬截断，p99/max 反映真实分布。

### 构建验证

- ✅ `bench_ws_echo` (kernel + DPDK)
- ✅ `bench_udp_echo` (kernel + DPDK)
- ✅ `bench_udp_relay` (kernel + DPDK)
- ✅ `bench_mock_server`
- ✅ 端到端 bench 运行：ws_echo + udp_relay 全 5 个 payload 数据合理

## 代码 review 发现的次要问题（未修，已记录）

1. **HdrHistogram 每 payload 重建**：bench loop 中每次创建新 instance 而非 `reset()`，效率略低但无功能问题。
2. **tcp_echo_dpdk src_port 随机化已修**：commit `9900bc9` 已通过 PID + time 随机化解决 TIME_WAIT 冲突。
3. **udp_echo / tcp_echo 第一次 payload 偶现 cold-start tail**：需要全局 pre-warmup phase 才能根治，是测量方法论问题而非代码 bug。

## 后续建议

- `bench_order_rtt` 仍是异步设计（send interval 1000µs ≫ RTT ~25µs），目前无问题，但应在文档中标注：用户调小 order_interval 时需注意 in-flight 风险。
- 考虑给 `bench_loop.hpp` 加一个全局 pre-warmup 函数（运行 N 个 dummy send/recv 以预热路由缓存、cache、调度器），在 payload sweep 之前调用一次。
- 可以用 `/test` 补充回归测试：构造一个 mock 在 echo_mode 仍推 ticks 的版本，验证 bench_ws_echo 的过滤器能正确忽略它们。

# Discussion Record

## Context
- 时间：2026-04-02
- 议题：重构 Transport/Reactor/TcpSession 数据流架构
- 复杂度：高 → 6 轮讨论
- 参与角色：R14 架构师, R2 极简主义者, R3 性能狂热者, R6 维护性倡导者, R5 第一性原理者

## 内容摘要

Reactor 从 NIC burst 后调 session->process_rx() 把 TCP payload 交给 on_data 回调，但 Transport 的 TLS/WS pipeline 只认 tcp_->poll_rx() 产出的数据——两条管道互不连通。

讨论了三个方案：A) 新增 kReactor mode + feed_rx；B) DataSource concept 统一抽象；C) 提取独立 ProtocolPipeline 组件。

最终收敛为**不新增 mode，在 kDirect 模式下新增 feed_rx() + process_pending() 两个公共方法**。feed_rx() 仅做 memcpy 积累到 reassembly buffer，process_pending() 执行 TLS decrypt → WS decode → on_message。poll() 重构为两者的组合。Reactor 在 burst 循环的 on_data 回调中调 feed_rx()，burst 结束后调 process_pending()——与现有 poll() 路径共享完全相同的 TLS/WS 处理代码，零额外 memcpy。

关键论据：
- R2 证明 feed_rx() 解决 100% 问题且不引入新概念
- R3 证明 memcpy 次数与现有路径完全相同
- R14/R5 推动了 feed_rx + process_pending 分离，使 Reactor batch 场景更高效
- R6 确认 poll()/rx_loop()/feed_rx() 三种入口汇入同一条内部路径，满足"无 surprise"要求

## 最终方案

### 公共 API
```cpp
void feed_rx(const uint8_t* data, uint16_t len) noexcept requires (!kHasRxThread);
void process_pending() noexcept requires (!kHasRxThread);
auto poll() noexcept requires (kIsDirect);  // = feed_rx loop + process_pending
```

### Reactor 集成模式
```cpp
reactor.add_connection(session, [&tp](data, len, idx) { tp->feed_rx(data, len); });
// burst 后：tp->process_pending();
```

### 内部实现
- feed_rx = 现有 poll() 内 callback 代码（memcpy to reassembly）
- process_pending = 现有 poll() 内 decrypt loop + frame processing
- poll() = tcp_->poll_rx(feed_rx) + process_pending()
- rx_loop (kThreaded) 不变

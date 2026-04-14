# Discussion Record

## Context
- 时间：2026-04-14 07:00
- 耗时：约 3 分钟
- 用户原始需求：你觉得哪种更好？（上下文：TCP create() 是否应该支持重试策略 —— 方案 A 单次 / B 内部重试 / C 混合）
- 复杂度评估：中
- 讨论轮数：2 轮（第 3 轮确认收敛）
- 参与角色：R1 风险卫士, R3 性能狂热者, R5 第一性原理者, R7 用户代言人, R14 架构师

## 内容摘要

讨论前先核对事实：`StreamConfig.reconnect` 字段已存在于两个后端 (`kernel/config.hpp:46`, `dpdk/config.hpp:71`)，`TcpStream` 里也有 `reconnect_policy_` 成员，但**全仓库除测试外零生产读取点** —— 是 v3.3 架构文档的未实现占位。三个方案：A 单次 create + 上层驱动循环（现状的"应该"版本）、B create 内部吞掉重连循环、C 混合辅助函数。R7 初始倾向 B（最小惊讶原则），被 R5 的"HFT 会话恢复需要协议层 Logon/seq sync，create 看不到"和 R14 的"未 attach Poller 阶段无监督者"论点说服，收敛到 A。R3 补充延迟可预测性论点，R1 补充信号响应性论点。5/5 最终共识：删除死字段，create() 保持单次，`ReconnectPolicy` 类保留给 session manager 层使用。

---

## 核心结论

**方案 A + 删除死字段**：
1. 删除 `KernelTcpStream::reconnect_policy_` 成员和初始化器
2. 删除 `DpdkTcpStream::reconnect_policy_` 成员和初始化器
3. 删除 `StreamConfig::reconnect` 字段
4. 删除 `DpdkStreamConfig::reconnect` 字段
5. 保留 `eph::net::ReconnectPolicy` 类（独立数学对象）
6. 新增 `examples/session_reconnect.cpp` 展示正确用法

## 为什么 B 不可行

1. **正确性**：HFT 会话恢复需要协议层 Logon + seq num sync + kill switch 检查，`create()` 看不到这些，做的重连必然是错的
2. **架构**：Stream 在 create() 阶段尚未 attach 到 Poller，重连循环期间无监督者
3. **延迟可预测性**：`max_attempts × max_backoff` 让 create() 延迟不可预测
4. **可中断性**：`sleep_for` 无法响应信号，SIGTERM 最长延迟 `max_backoff`
5. **多路径**：primary/backup DC 切换是业务逻辑，policy 无法表达

## 为什么现状（字段存在但是死代码）最糟

字段存在制造"配了即生效"的错觉，用户以为配置好了实际什么都没发生。删掉比保留更诚实。

## 正确用法示例

```cpp
ReconnectPolicy policy{ReconnectPolicyConfig{...}};
while (policy.should_reconnect()) {
    auto stream_r = KernelTcpStream<Codec>::create(cfg);
    if (stream_r) {
        policy.reset();
        run_session(*stream_r);  // 协议层 Logon / seq sync / kill switch
        continue;
    }
    std::this_thread::sleep_for(policy.next_backoff());
}
```

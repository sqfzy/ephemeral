# Discussion Record

## Context
- 时间：2026-04-07
- 用户原始需求：现在dpdk的udp支持已经完善了吗？已经满足最佳实践了吗？
- 复杂度评估：中
- 讨论轮数：3 轮
- 参与角色：R1 风险卫士, R3 性能狂热者, R6 维护性倡导者, R13 测试驱动者, R14 架构师

## 内容摘要

对刚实现的 UDP 支持层进行全面审查。R1 发现 `send_batch` 中 stats 统计的确定性 bug（部分 build 失败时索引错位）。R13 发现 `build_udp_packet` 公共 API 零测试覆盖。R6 指出 `UdpConfig` 缺少 `dump()/to_json()` 与 TCP 体系不一致。R14 否决了 MTU payload 限制（jumbo frame 合法），R3 建议 alloc_bulk 优化但接受推迟。最终共识：架构正确（85% 完成度），需修 1 bug + 补充测试和可观测性方法。

---

## 必须修复

1. **`send_batch` 统计 bug**（`udp.hpp:256-259`）：partial-build 时 segs 索引与 mbufs 索引不对齐，tx_bytes 不准确。修复方案：维护 `sent_lens[]` 数组。

## 应该补充

1. `build_udp_packet` 便捷函数测试（公共 API 零覆盖）
2. `send_batch` stats bug 回归测试
3. `UdpConfig::dump()/to_json()`
4. `ip_id` 回绕（65535→0）测试

## 后续优化

1. `send_batch` 改用 `rte_pktmbuf_alloc_bulk`（减少锁争用）
2. `build` 路径跳过冗余 `rte_pktmbuf_reset`

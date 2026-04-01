# Artifacts Index

| 时间 | Skill | 摘要 | Commit | 文件 |
|------|-------|------|--------|------|
| 2026-03-28 03:50 | /discuss HFT 库缺失分析 | 5 角色 7 轮讨论：最缺订单簿、JSON parsing、连接管理、io_uring；四阶段路线图 | 50ca07c | discuss-20260328-035012.md |
| 2026-03-28 04:04 | /design Phase 1 v1.1 | eph-core 提取 + framer decode 实例方法 + ErrorEnum concept + type alias 体系化 | — | design-20260328-040452.md |
| 2026-03-28 05:32 | /ship auto | 5 Gate 全过：修复 throughput delta bug，统一 error_name 返回类型，3 commits pushed | c24d365 | ship-20260328-053255.md |
| 2026-03-28 05:51 | bench Socket vs DPDK | 6 benchmarks: DPDK 11.6× faster RX(单symbol), 5.5× faster TX, 7.1× faster RX(多symbol) | c24d365 | bench-20260328-053255.md |
| 2026-03-28 06:05 | /discuss TCP 重传 | 不做完整重传；三层渐进：reorder buffer 64 slots + SYN 重传 + 丢包遥测 | c24d365 | discuss-20260328-060512.md |
| 2026-03-28 06:11 | /design TCP 鲁棒性 | 三层实现完成：ReorderSlots 64 + SYN retransmit 200ms + gap_histogram telemetry | 5beaa0f | design-20260328-061113.md |
| 2026-03-28 06:26 | bench 6-round Socket vs DPDK | 36 runs, 2.5h: DPDK 8.0× RX(单), 6.7× RX(多), 7.1× TX; 零断线 | 5beaa0f | bench-20260328-062602.md |
| 2026-03-28 09:32 | /review all auto | 52 files全量审查: 5 Critical, 16 Major, 18 Minor, 11 Nit | 5beaa0f | review-20260328-093256.md |
| 2026-03-28 09:45 | /fix+/refactor+/improve | 修复 5 Critical + 11 Major: 数据竞争、栈溢出、DNS泄漏、协议安全 | 948fd9c | fix-20260328-094524.md |
| 2026-03-28 10:07 | /improve remaining 6 Major | get_mean溢出、throughput语义、crypto_竞争、recorder thread_local、POSIX guard | 6c717a1 | — |
| 2026-03-28 10:16 | /fix minor+nit | 18 Minor + 部分 Nit：saturating sub、loss_rate clamp、JSON escape、TLS version log | 50c0004 | — |
| 2026-03-28 11:14 | /evolve implement auto | 6/9 缺口：DnsConfig validate、[[nodiscard]]、config校验、safe move、heartbeat上限 | 61cb6c4 | evolve-20260328-111430.md |
| 2026-03-28 11:28 | /design JSON parsing | 新模块 eph-json：零拷贝 JSON 解析器 + JsonFramer，25 tests | e3ce679 | — |
| 2026-03-28 12:00 | /improve eph-dpdk R1+R2 | 8 fixes: mbuf leak, TSC race, SYN guard, IHL offset, logger perf | b95ca51 | — |
| 2026-03-28 12:46 | /ship | 5 Gate 全过：1,787 tests, bench 无退化, 10 commits pushed | b95ca51 | ship-20260328-124629.md |
| 2026-03-28 15:58 | /discuss 多连接 RSS | 三步优化：hash table→RSS多queue→rte_flow硬件分发；Step 1立即做 | a0fa3ac | discuss-20260328-155821.md |
| 2026-03-28 16:23 | /discuss Reactor 实现细节 | 10个不确定点全解决：epoll模型Reactor替代SharedRxDispatcher | a0fa3ac | discuss-20260328-162334.md |
| 2026-03-30 15:49 | /discuss 自研队列 vs rte_ring | 5角色4轮：自研队列在语义/性能/架构三维度优于rte_ring；EvictingQueue语义不可替代 | 7dd6cbb | discuss-20260330-154959.md |
| 2026-04-01 19:26 | /cleanup eph-dpdk eph-net | 日志100%统一命名logger、TcpSession move优化、Platform bool、ai_addr null guard、spinlock pause | — | cleanup-20260401-192600.md |

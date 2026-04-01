# Code Audit Report

## 概况
- 时间：2026-04-01 10:00:05
- 审计范围：整个项目 (8 modules)
- 代码规模：93 header files, ~80K lines of C++23
- 构建状态：✅ 通过
- 测试状态：✅ 131 tests 全部通过

## 项目健康度摘要
- 🔴 Critical：11 项
- 🟡 Major：26 项
- 🔵 Minor：26 项
- 💬 Nit：16 项
- 整体评估：代码质量高，架构清晰，但存在若干安全/正确性隐患需要修复

## 技术债清单

| 序号 | 严重度 | 维度 | 模块 | 描述 | 位置 | 推荐 Skill |
|------|--------|------|------|------|------|------------|
| 1 | 🔴 | 安全 | core | UTF-8 continuation byte 未验证，可透传无效 UTF-8 | json_escape.hpp:52-57 | /fix |
| 2 | 🔴 | 正确性 | utils | ms_to_ns() 负数输入导致 uint64_t 回绕 | timestamp.hpp:25-26 | /fix |
| 3 | 🔴 | 正确性 | utils | clock_gettime() 返回值未检查 | timestamp.hpp:54 | /fix |
| 4 | 🔴 | 正确性 | containers | EvictingQueueBytes discard 计数无符号下溢 | evicting_queue_bytes.hpp:266 | /fix |
| 5 | 🔴 | 并发 | containers | push_n_wts 中 push_count_ relaxed ordering 与 reader 不同步 | evicting_queue_bytes.hpp:146,163 | /fix |
| 6 | 🔴 | 安全 | net | TLS 序列号检查与解密不原子，nonce 重用风险 | tls_record.hpp:362-367 | /fix |
| 7 | 🔴 | 安全 | net | HTTP CONNECT 代理响应缓冲区无上限（65KB 仍偏大） | proxy.hpp:403-408 | /fix |
| 8 | 🔴 | 正确性 | json | Binance symbol_hash 缺少 null pointer 检查 | binance.hpp:56 | /fix |
| 9 | 🔴 | 正确性 | book | unordered_map<double,double> 作为价格键，hash 不可靠 | itch_adapter.hpp:90-91 | /refactor |
| 10 | 🔴 | 并发 | dpdk | Reactor::mark_reconnected() session 指针交换与 RX loop 竞争 | reactor.hpp:176-187 | /fix |
| 11 | 🔴 | 正确性 | fix | FIX session 序列号达到 UINT32_MAX 后不可恢复 | session.hpp:604 | /fix |
| 12 | 🟡 | 安全 | net | SOCKS5 auth 日志泄漏密码长度 | proxy.hpp:229-230 | /fix |
| 13 | 🟡 | 正确性 | net | WebSocket frame payload 长度减法可能下溢 | websocket.hpp:534-536 | /fix |
| 14 | 🟡 | 正确性 | net | HTTP CONNECT status code 解析无溢出保护 | proxy.hpp:425-432 | /fix |
| 15 | 🟡 | 类型安全 | containers | BoundedQueueBytes span size cast narrowing to uint32_t | bounded_queue_bytes.hpp:多处 | /fix |
| 16 | 🟡 | 并发 | containers | EvictingQueueBytes ID 赋值与 SeqLock 未同步 | evicting_queue_bytes.hpp:115-116 | /fix |
| 17 | 🟡 | 正确性 | containers | BoundedQueue available space unsigned underflow | bounded_queue.hpp:240,244,278,282 | /fix |
| 18 | 🟡 | 性能 | fix | OrderManager 用 std::string 做 map lookup（应用 heterogeneous lookup） | order_manager.hpp:128,286,326 | /refactor |
| 19 | 🟡 | 正确性 | fix | 浮点精度丢失后仍使用损坏值 | order_manager.hpp:167-173 | /fix |
| 20 | 🟡 | 安全 | utils | snprintf buffer overflow 风险（timestamp 格式化） | timestamp.hpp:93,110 | /fix |
| 21 | 🟡 | 正确性 | utils | time_t 32-bit 截断风险 (Y2K38) | timestamp.hpp:82,100 | /fix |
| 22 | 🟡 | 设计 | utils | AuditLog::record() 无返回状态，溢出静默 | audit_log.hpp:132-149 | /improve |
| 23 | 🟡 | 设计 | utils | TSC 校准方差仅 log 不返回状态 | time.hpp:310-315 | /improve |
| 24 | 🟡 | 安全 | json | OKX first_array_element 转义处理越界读 | okx.hpp:77 | /fix |
| 25 | 🟡 | 一致性 | json | 三个 adapter 的 null/empty 检查不一致 | binance/okx/bybit | /fix |
| 26 | 🟡 | 正确性 | book | ArrayBook 接受负数 qty 仅 WARN 不拒绝 | array_book.hpp:199-204 | /fix |
| 27 | 🟡 | 安全 | dpdk | ARP 无 spoofing 防护 | arp.hpp:148-180 | /improve |
| 28 | 🟡 | 安全 | dpdk | DNS transaction ID 仅 16 bit，无 MAC 验证 | dns.hpp:493-618 | /improve |
| 29 | 🟡 | 正确性 | dpdk | hw_cksum 未验证 NIC 是否支持 | net_header.hpp:230-327 | /fix |
| 30 | 🟡 | 性能 | dpdk | TCP reorder buffer drain O(N²) | tcp.hpp:1008-1028 | /improve |
| 31 | 🟡 | 正确性 | fix | Heartbeat interval 用 relaxed read 后做条件判断 | session.hpp:341-347 | /fix |
| 32 | 🟡 | 正确性 | net | TLS 序列号百分比计算整数截断 | tls_record.hpp:325-327 | /fix |
| 33 | 🟡 | 正确性 | itch | ReplacedView 缺少 7 个字段 accessor | ouch.hpp:384-424 | /fix |
| 34 | 🟡 | 安全 | net | EVP_AEAD_CTX move 赋值可能泄漏 | tls_record.hpp:217-235 | /fix |
| 35 | 🟡 | 设计 | utils | constexpr 转换函数无输入验证 | timestamp.hpp:25-37 | /fix |
| 36 | 🟡 | 正确性 | fix | ResendRequest handler 未使用 BeginSeqNo/EndSeqNo | session.hpp:437-451 | /improve |
| 37 | 🟡 | 正确性 | fix | 非有限价格可导致 avg_price 污染 | position.hpp:100-103 | /fix |

## 架构评估

**优势**：
- 清晰的模块分层：core → utils → containers → net/fix/itch/json/book → dpdk
- 纯 header-only 设计，零运行时开销
- 优秀的 C++23 特性使用（concepts, std::expected, constexpr, std::format）
- 全面的错误处理 via std::expected
- 良好的 observability 基础设施（spdlog + compile-time level filtering）

**关注点**：
- eph-dpdk 通过 includedirs 直接引用 eph-net headers，绕过 xmake 依赖声明
- eph-book 中 itch_adapter 使用 `unordered_map<double,double>` 违反浮点 hashing 安全原则
- 三个 JSON adapter (binance/okx/bybit) 存在大量重复代码

## 推荐行动计划

按优先级排序，标注推荐修复方式：

### Phase 1: Critical Fixes（必须立即修复）

1. **[C1] UTF-8 continuation byte 验证** → json_escape.hpp — 验证 continuation bytes 在 0x80-0xBF 范围
2. **[C2] ms_to_ns 负数回绕** → timestamp.hpp — 添加 assert 或 static_cast 前验证
3. **[C3] clock_gettime 返回值检查** → timestamp.hpp — 检查返回值
4. **[C4] Discard 计数下溢** → evicting_queue_bytes.hpp — 添加防御性检查
5. **[C5] push_count_ memory ordering** → evicting_queue_bytes.hpp — batch 最后一次用 release
6. **[C6] TLS 序列号 nonce 安全** → tls_record.hpp — 确保序列号递增与解密结果绑定
7. **[C7] HTTP CONNECT 缓冲区限制** → proxy.hpp — 减小到 8KB 上限
8. **[C8] Binance symbol_hash null check** → binance.hpp — 添加 null/empty 检查
9. **[C9] double key hash map** → itch_adapter.hpp — 改用 int64_t 量化键
10. **[C10] Reactor reconnect race** → reactor.hpp — 添加内存屏障或文档化约束
11. **[C11] FIX sequence number exhaustion** → session.hpp — 添加恢复机制或文档

### Phase 2: Major Fixes（强烈建议修复）

12. **[M1] snprintf → std::format** → timestamp.hpp — 消除 buffer overflow 风险
13. **[M2] SOCKS5 密码长度日志** → proxy.hpp — 改为 redacted 占位符
14. **[M3] WebSocket payload 减法安全** → websocket.hpp — 使用安全减法
15. **[M4] HTTP status code 解析** → proxy.hpp — 使用 std::from_chars
16. **[M5] BoundedQueueBytes span narrowing** → bounded_queue_bytes.hpp — 添加范围保护
17. **[M6] OrderManager heterogeneous lookup** → order_manager.hpp — 去除不必要的 string 分配
18. **[M7] Fill 精度丢失拒绝** → order_manager.hpp — 拒绝 > 2^53 的数量
19. **[M8] OKX 转义越界** → okx.hpp — 修复边界检查
20. **[M9] Adapter null check 统一** → binance/okx/bybit — 统一 null 检查
21. **[M10] Negative qty 拒绝** → array_book.hpp — 返回错误而非仅 WARN
22. **[M11] OUCH ReplacedView 补全** → ouch.hpp — 添加 7 个缺失 accessor
23. **[M12] Non-finite price 验证** → position.hpp — 入口处验证 isfinite
24. **[M13] Y2K38 time_t 保护** → timestamp.hpp — 范围检查
25. **[M14] Heartbeat interval acquire** → session.hpp — 改用 acquire ordering
26. **[M15] EVP_AEAD_CTX move 安全** → tls_record.hpp — memset 或 swap 模式
27. **[M16] hw_cksum NIC 能力验证** → net_header.hpp — 查询并验证 offload 能力
28. **[M17] TLS 序列号百分比计算** → tls_record.hpp — 修正整数截断

# Full Project Code Audit Report

## 概况
- 时间：2026-03-29 14:00
- 审计范围：全项目 (199 files, 79081 lines)
- 模块数：12 (eph-core, eph-utils, eph-containers, eph-net, eph-dpdk, eph-fix, eph-itch, eph-json, eph-book, benchmarks, tests, examples)

## 项目健康度摘要

| 模块 | 🔴 | 🟡 | 🔵 | 💬 | 状态 |
|------|-----|-----|-----|-----|------|
| eph-dpdk | 0 | 0 | 0 | 0 | ✅ 已修复 (46项) |
| eph-net | 2 | 10 | 10 | 1 | ⚠️ 需修复 |
| eph-core | 0 | 1 | 1 | 2 | 良好 |
| eph-utils | 1 | 4 | 4 | 0 | ⚠️ 需修复 |
| eph-containers | 1 | 4 | 2 | 2 | ⚠️ 需修复 |
| eph-fix | 2 | 4 | 1 | 1 | ⚠️ 需修复 |
| eph-itch | 0 | 2 | 1 | 1 | 良好 |
| eph-json | 1 | 2 | 1 | 0 | ⚠️ 需修复 |
| eph-book | 0 | 2 | 1 | 1 | 良好 |
| **总计** | **7** | **29** | **21** | **8** | |

## 🔴 Critical (7项，必须修复)

| # | 模块 | 位置 | 描述 |
|---|------|------|------|
| 1 | eph-net | transport.hpp:2841,2986 | 畸形注释 `/` 代替 `//`——编译错误或静默 UB |
| 2 | eph-net | transport.hpp:2088-2111 | TX loop `crypto_` TOCTOU: reconnecting_ 和 unique_ptr 无锁竞争 |
| 3 | eph-utils | time.hpp:133 | `TSC::init()` 返回值逻辑不必要地复杂，应简化 |
| 4 | eph-containers | bounded_queue.hpp:371 | `noexcept` 标记但调用非 noexcept 的 `steady_clock::now()` |
| 5 | eph-fix | session.hpp:274 | 负 MsgSeqNum cast 到 uint32 导致序列号被攻击者控制 |
| 6 | eph-fix | session.hpp:310 | HeartBtInt INT 截断——值 >INT_MAX 变负触发假死判断 |
| 7 | eph-json | parser.hpp:275 | JSON exponent 累加有符号溢出 UB（来自不可信输入） |

## 🟡 Major 高亮 (29项，按优先级)

### 安全/正确性
- eph-net gateway.hpp:163 — `start_all()` 可产生重复线程
- eph-net tls_session.hpp:202 — `assert()` 在 release build 无保护
- eph-net proxy.hpp:384 — HTTP CONNECT 响应无大小上限
- eph-json parser.hpp:192 — `find_field` 空 key 导致 UB
- eph-fix parser.hpp:625 — Tag 0 哨兵与保留 tag 冲突
- eph-itch moldudp64.hpp:170 — 序列号加法无溢出检查

### 性能
- eph-utils time.hpp:96 — ARM64 `isb` 在热路径上过重
- eph-fix position.hpp:66 — 每次 fill 都 heap 分配 string
- eph-book itch_adapter.hpp:96 — O(n) 全量扫描每事件

### 正确性
- eph-net transport.hpp:908 — close_gracefully 与 stop 并发竞争
- eph-net transport.hpp:2540 — arrival_tsc back-dating 下溢时应置零
- eph-utils hdr_histogram.hpp:944 — sub_bucket_mask_ signed shift 可 UB
- eph-containers bounded_queue_bytes.hpp:53 — operator- 无下溢保护
- eph-containers ring_buffer.hpp:36 — 非 atomic 字段误导并发安全
- eph-book array_book.hpp:126 — is_crossed() 混淆 crossed 和 locked

### 测试缺口
- eph-net 模块零测试覆盖
- eph-containers BoundedQueueBytes/EvictingQueueBytes 零测试
- eph-utils record_corrected() 零测试

## 推荐行动计划

### Phase 1: Critical 修复 (7项)
```
/fix — transport.hpp 畸形注释 (2分钟)
/fix — TSC::init() 简化 (5分钟)
/fix — bounded_queue _for 方法 noexcept 文档化 (5分钟)
/fix — FIX session.hpp MsgSeqNum 负值守卫 + HeartBtInt 范围检查 (10分钟)
/fix — JSON parser.hpp exponent 溢出守卫 + find_field 空 key 守卫 (5分钟)
```

### Phase 2: 安全加固 (8项)
```
/fix — eph-net tls_session assert→hard check
/fix — eph-net proxy HTTP CONNECT 响应 cap
/fix — eph-net gateway start_all 重复线程守卫
/fix — eph-net transport close_gracefully running 检查
/fix — eph-net transport arrival_tsc 下溢置零
/fix — eph-net transport_types two-phase filter hash==0 哨兵
/fix — eph-itch moldudp64 序列号溢出守卫
/fix — eph-book is_crossed/is_locked 分离
```

### Phase 3: 性能+正确性 (13项)
```
/improve — eph-utils ARM64 isb 热路径优化
/improve — eph-fix position.hpp 异构 lookup 消除 string 分配
/refactor — eph-book itch_adapter 增量 qty 维护
/fix — hdr_histogram sub_bucket_mask_ unsigned 化
/fix — bounded_queue_bytes operator- 下溢保护
/fix — ring_buffer 文档明确非线程安全
/fix — eph-net gateway degraded_threshold 实现或移除
/fix — eph-json get() 歧义文档化
等
```

### Phase 4: 测试补全
```
/test target: eph-net — WS/TLS/Transport 基础测试
/test target: eph-containers — BoundedQueueBytes, record_corrected
/test target: eph-fix — 负 MsgSeqNum, large HeartBtInt
/test target: eph-json — exponent overflow, empty key
```

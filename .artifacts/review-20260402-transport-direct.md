# Code Review Report

## 元信息
- 时间：2026-04-02
- Diff 来源：last 4 commits (ccaf534..c1aa332)
- 审查范围：transport.hpp, tls_encryptor/decryptor/constants/record.hpp, reactor.hpp, presets.hpp, transport_errors.hpp, tests
- 构建状态：✅ 通过（零编译错误）

---

## Review 摘要

### 变更概况
- 文件数：17（10 源码 + 7 文档/计划）
- 增删：+2098 / -563
- 主要变更：Transport 新增 TransportMode 编译期模式（kDirectTx/kDirect）、feed_rx/process_pending 外部数据注入、TLS 拆分、Reactor on_burst_complete

### 总体评价
架构设计良好——TransportMode if constexpr 分发、conditional_t 成员消除、feed_rx/process_pending 分离都是正确的方向。TLS Encryptor/Decryptor 拆分干净。主要问题集中在 move 语义安全性、encrypt 输入验证顺序、以及 kDirectTx 模式下 send_direct/stop 竞争。

### 问题统计
- 🔴 Critical：0
- 🟡 Major：3
- 🔵 Minor：4
- 💬 Nit：4

### 结论
REQUEST_CHANGES — 3 个 Major 需要处理

---

## Major

### M1: EVP_AEAD_CTX move 依赖 aws-lc 内部实现
tls_encryptor.hpp:77, tls_decryptor.hpp:77 — bitwise copy EVP_AEAD_CTX 假设无内部堆指针

### M2: encrypt() null 检查在 write_record_header 之后
tls_encryptor.hpp:126 — 已写 5 字节到 out 后才检查 null，出错时 out 被部分写入

### M3: kMaxSequenceNumber = 2^24 在 HFT 速率下约 3 分钟触发重连
tls_constants.hpp:36 — 需要文档说明是否有意为之

## Minor

### m1: kDirectTx send_direct/stop 竞争
transport.hpp:1810 — stop() 与 send_direct() 可能并发访问 crypto_

### m2: feed_rx 溢出时静默丢弃所有已缓冲数据
transport.hpp:1099 — 无返回值/计数器通知调用者

### m3: on_burst_complete 在无匹配数据时也触发
reactor.hpp:323 — 文档说"至少一个包被分发"但实际是 nb_rx > 0

### m4: encrypt() 每次调用 16KB 栈分配
tls_encryptor.hpp:131 — inner_buf 仅为追加 1 字节 content type

## Nit: n1 logger 风格不一致、n2 presets 缺 DirectTxEvict、n3 test 重复、n4 include 位置

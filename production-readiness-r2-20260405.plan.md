# Plan: eph-core / eph-utils 生产就绪补完（第二轮）

> 补齐 eph-core detail 工具测试、AuditLog 线程安全检查、json_escape fuzzer、HdrHistogram NaN 测试、中文注释翻译、rx_dropped 告警回调。

创建时间：2026-04-05
状态：已确认

---

## 定位与边界

**目标**：补齐上一轮生产就绪改造未覆盖的 eph-core 和 eph-utils 差距。

**In scope**：
- eph-core detail 工具测试（LengthPrefixFramer、json_escape、base64、string_checks）
- AuditLog 单写者 debug assert
- json_escape fuzz target
- HdrHistogram NaN 路径测试
- time.hpp 中文注释翻译
- Phase 5c 简化版 rx_dropped 告警回调

**Out of scope**：
- TcpTransport concept 错误类型迁移（已知债务，改动面太大）
- MetricsSink 实际使用验证
- FakeTcpTransport 状态机扩展
- E2E latency benchmark（下个 milestone）

---

## 实施计划

### 阶段 1: eph-core 测试补全

- **1a. test_length_prefix_framer.cpp**：encode/decode round-trip、零长度拒绝、最大 payload（65535 - 2 bytes header）、截断输入（不完整 header、header 声明长度 > 实际数据）
- **1b. test_json_escape.cpp**：ASCII 控制字符（\n \r \t \0）、引号/反斜杠转义、UTF-8 多字节（2/3/4 byte 序列）、truncated UTF-8、空字符串、超长字符串
- **1c. test_base64.cpp**：RFC 4648 test vectors、空输入、1/2/3 字节输入（0/1/2 padding）、已知值验证
- **1d. test_string_checks.cpp**：无控制字符返回 false、有各类控制字符返回 true、空字符串、printable ASCII
- 交付物：4 个测试文件全部编译通过且测试通过
- 预估：1-2 小时

### 阶段 2: eph-utils 加固

- **2a. AuditLog debug assert**：在 `record()` 首次调用时用 `std::atomic<std::thread::id>` 记录 writer tid，后续调用在 debug build 下 assert tid 一致
- **2b. HdrHistogram NaN 测试**：在现有 test_hdr_histogram.cpp 中添加 NaN、Infinity、负值的 record() 测试，验证被静默拒绝
- **2c. time.hpp 中文注释翻译**：将 TSC 校准相关的中文注释翻译为英文
- 交付物：编译通过 + 测试通过
- 预估：1 小时

### 阶段 3: Fuzzer + 告警回调

- **3a. json_escape fuzz target**：创建 `eph-core/fuzzers/fuzz_json_escape.cpp`，喂随机字节给 `json_escape()`，验证输出是 valid JSON string
- **3b. rx_dropped 告警回调（Phase 5c 简化版）**：在 TransportConfig 中添加 `std::function<void(uint64_t total_dropped, uint64_t delta)> on_drop_alert{}`。在 RX worker 中，每 1024 次迭代检查 rx_dropped delta，若 delta > 0 则调用回调
- 交付物：fuzzer 文件 + 告警回调实现 + 单元测试
- 预估：1-2 小时

---

## 关键决策记录

### D-1: AuditLog 线程安全强制方式
- **决策**：Debug build assert（非运行时异常）
- **理由**：零生产性能开销，开发期间即可捕获误用

# Code Audit Report — eph-dpdk

## 概况
- 时间：2026-03-29 12:30
- 审计范围：eph-dpdk/ (11 files, 4502 lines)
- 代码规模：11 headers (header-only library)

## 项目健康度摘要
- 🔴 Critical：6 项
- 🟡 Major：22 项
- 🔵 Minor：18 项
- 整体评估：核心功能实现完善，但 TCP 状态机有边界缺陷，Reactor 有数据竞争，connector 的 DNS fallback 路径有资源泄漏。测试覆盖集中在单元/静态验证，缺少 connect/process_rx/send 等运行时行为测试。

## 技术债清单

### 🔴 Critical

| # | 文件 | 行 | 维度 | 描述 | 推荐 Skill |
|---|------|-----|------|------|------------|
| 1 | tcp.hpp | 383,949 | 正确性 | ISN 失败检查不可达：`isn=1` workaround 掩盖了 CSPRNG 失败 | /fix |
| 2 | tcp.hpp | 639-666 | 正确性 | early-return free 逻辑在两条路径上独立维护，脆弱 | /refactor |
| 3 | connector.hpp | 533 | 正确性/资源 | DNS fallback 创建 Platform 后又调 connect() 二次创建——第二次会 EBUSY 失败 | /fix |
| 4 | platform.hpp | 331-338 | 正确性 | Queue count 日志说 "clamping" 但实际未 clamp，直接传给 DPDK 导致失败 | /fix |
| 5 | platform.hpp | 542-544 | 正确性 | moved-from Platform 调 is_running()/mempool() 是 null deref UB | /fix |
| 6 | reactor.hpp | 114-165 | 线程安全 | add_connection/mark_reconnected 无同步，与 RX loop 并发读写是 data race | /fix |

### 🟡 Major

| # | 文件 | 行 | 维度 | 描述 | 推荐 Skill |
|---|------|-----|------|------|------------|
| 7 | tcp.hpp | 628 | 正确性 | out_of_order 统计对 duplicate 也计数，与 histogram 不一致 | /fix |
| 8 | tcp.hpp | 686-712 | 正确性 | 零 payload 的 out-of-order FIN 触发过早状态转移 | /fix |
| 9 | tcp.hpp | 494 | 正确性 | 握手超时不重置 snd_nxt/snd_una，重连使用旧序列号 | /fix |
| 10 | tcp.hpp | 700 | 架构 | 同时关闭直接到 TimeWait，缺少 RFC 793 的 Closing 状态 | /refactor |
| 11 | tcp.hpp | 693 | 架构 | TimeWait 无 2MSL 定时器，状态永久 | /refactor |
| 12 | tcp.hpp | 892 | 性能 | drain_reorder_buf O(N²) for 64 slots | /improve |
| 13 | tcp.hpp | 534 | 可观测性 | send() tx_burst 失败无日志无上下文 | /improve |
| 14 | tcp.hpp | 547 | 设计 | build_data_packet() 不更新 tx_packets/tx_bytes 统计 | /fix |
| 15 | net_header.hpp | 508 | 安全 | tcp_doff+ihl 溢出未检查，构造包可导致 OOB payload_len | /fix |
| 16 | tcp.hpp | 639-640 | 安全 | reorder buffer 按 kDefaultMss 而非 config_.mss 检查，jumbo frame 静默丢弃 | /fix |
| 17 | connector.hpp | 214 | 正确性 | parse_ipv4("0.0.0.0") 返回 0 与解析失败歧义 | /refactor |
| 18 | connector.hpp | 306-312 | 设计 | ephemeral port 生成逻辑重复 3 处 | /refactor |
| 19 | connector.hpp | 222 | 安全 | hostname 传 getaddrinfo 无长度/格式校验 | /improve |
| 20 | platform.hpp | 291 | 正确性 | 硬编码 "mbuf_pool" 名称，多实例创建失败 | /fix |
| 21 | platform.hpp | 87-95 | 正确性 | logger 单例初始化在并发首次调用时不安全 | /improve |
| 22 | flow_steering.hpp | 203 | 正确性 | reta_size 未 cap 到数组大小，>512 时 OOB 写 | /fix |
| 23 | dns.hpp | 279 | 安全 | qd_count 未 bound，恶意响应可致 CPU 空转 65535 次 | /fix |
| 24 | net_header.hpp | 355-366 | 一致性 | fill_packet() 用 magic number，build_packet() 用命名常量 | /refactor |
| 25 | net_header.hpp | 239 | 线程安全 | PacketTemplate::ip_id 非 atomic，多线程共享是 data race | /fix |
| 26 | reactor.hpp | 153-154 | 线程安全 | mark_disconnected 读 count_ 无同步 | /fix |
| 27 | dns.hpp | 211-245 | 正确性 | skip_dns_name 对 label_len 无越界检查（逻辑安全但脆弱） | /improve |
| 28 | platform.hpp | 363-374 | 性能 | rte_eth_dev_info_get 调用两次（configure+setup） | /improve |

### 🔵 Minor (18 项，详见各子审计)

包括：move ctor 初始化顺序依赖、hot-path 日志用全局 logger、cache line 布局、RST/FIN 日志缺 tuple、Stats::rx_bursts 未在 dump/to_json/operator- 中、packet_template() 暴露可变内部状态、validate() 返回 string_view 脆弱性、internet_checksum(nullptr,0) UB、multi-segment mbuf 未处理、logger 初始化模式重复 4 处等。

## 测试覆盖评估

| 文件 | 主要缺口 |
|------|----------|
| tcp.hpp | **connect()、process_rx()、send()、close()、build_data_packet() 全无运行时测试** — 这是最大的风险敞口 |
| connector.hpp | DNS fallback 双 Platform 创建路径零覆盖 |
| platform.hpp | 多实例/重复创建、moved-from 状态、负数 timeout 未覆盖 |
| reactor.hpp | 并发 add_connection/mark_reconnected 无 TSan 覆盖 |
| flow_steering.hpp | detect_rx_dispatch_mode、reta_size 边界、全生命周期未覆盖 |
| dns.hpp | multi-segment mbuf、高 qd_count、0.0.0.0 边界未覆盖 |

## 推荐行动计划

1. **[Critical] tcp.hpp ISN + 状态机修复** — `/fix` × 4（ISN 检查、FIN 序列检查、超时重置、early-return 统一）
2. **[Critical] connector.hpp DNS fallback 资源泄漏** — `/fix`（路由到 connect(Platform&) 重载）
3. **[Critical] platform.hpp clamp + moved-from 守卫** — `/fix` × 2
4. **[Critical] reactor.hpp 数据竞争** — `/fix`（count_ atomic + connected atomic_bool + add_connection 断言）
5. **[Major] 安全加固** — `/fix` × 3（net_header doff 溢出、flow_steering reta_size cap、dns qd_count bound）
6. **[Major] 测试补全** — `/test target: eph-dpdk`（优先 tcp connect/process_rx/send）
7. **[Major] 代码统一** — `/refactor`（ephemeral port 去重、fill_packet 常量、logger 模式统一）

# Plan: eph-core / eph-transport / eph-net / eph-dpdk 全面重构

> 引入 eph-transport 中间层，统一依赖图、拆分臃肿文件、消除构建 hack，使网络模块达到最佳实践。

创建时间：2026-04-02
状态：已完成

---

## 定位与边界

**目标**：彻底重构 eph-core、eph-net、eph-dpdk 三个模块的代码结构和依赖关系，新增 eph-transport 中间层，消除已知的架构债务，使代码库达到可维护、可测试、依赖清晰的最佳状态。

**用户**：内部 HFT 系统开发者

**In scope**：
- 新增 eph-transport 模块：从 eph-net 提取 Transport 引擎 + 协议栈（TLS/WS/HTTP upgrade）
- eph-core：concepts 和类型的重新组织，消除 eph-net 中的转发头文件
- eph-net：瘦身为 SocketTransport 后端 + 辅助功能（Gateway、KillSwitch 等）
- eph-dpdk：消除 include hack，正式依赖 eph-transport
- 构建系统（xmake.lua）调整
- transport.hpp（3461 行）拆分为多个文件
- socket_transport.hpp（~1100 行）拆分
- 类型别名统一（消除 preset 重复）
- 所有受影响的测试、基准和示例代码更新

**Out of scope**：
- 功能变更（不增删功能，纯结构重构）
- eph-itch、eph-fix、eph-json、eph-book 等其他模块（仅更新 include 路径）
- 性能优化（保持现有性能特性）
- Gateway、KillSwitch、CircuitBreaker、RateLimiter、HttpClient、Proxy 保留在 eph-net

---

## 技术选型

| 类别 | 选择 | 理由 |
|------|------|------|
| TLS 库 | aws-lc | 已有选择，eph-transport 继承 |
| 构建 | xmake | 无需变更 |
| 日志 | spdlog（named loggers） | 无需变更 |
| 错误处理 | std::expected | 无需变更 |

---

## 架构设计

### 模块划分

| 模块 | 职责 | 依赖 |
|------|------|------|
| **eph-core** | Concepts (TcpTransport, MessageFramer, MetricsSink)、ErrorEnum 框架、错误类型 (SendError, ConnectionError)、parse_number、LengthPrefixFramer、json_escape | spdlog |
| **eph-transport** | Transport 引擎模板、TLS session/record、WebSocket 协议、HTTP upgrade、WsFramer、RawFramer、TransportConfig/Stats/Event/State、Transport preset 别名模板 | eph-core, eph-utils, eph-containers, aws-lc, spdlog |
| **eph-net** | SocketTransport 后端、SocketConfig、便捷连接函数、HttpClient、Gateway、KillSwitch、CircuitBreaker、RateLimiter、Proxy、HMAC | eph-transport, eph-core |
| **eph-dpdk** | EAL、Platform、TcpSession、Reactor、ARP、DNS、FlowSteering、Multicast、Connector、DPDK Transport 别名 | eph-core, eph-utils, eph-containers, eph-transport (headers only), aws-lc, dpdk |

### 依赖图

```
eph-core (spdlog)                     eph-utils (spdlog)      eph-containers
    ↑                                      ↑                       ↑
    ├──────────────────────────────────────┐│┌──────────────────────┘
    │                                  eph-transport (aws-lc)
    │                                      ↑
    ├──────────────────┬───────────────────┤ (eph-dpdk: headers only*)
    │                  │                   │
    │  * eph-dpdk uses add_includedirs("eph-transport/include") instead of
    │    add_deps("eph-transport") to control aws-lc vs vcpkg OpenSSL include
    │    path ordering. See D-3 below.
    │                  │                   │
eph-net            eph-dpdk (dpdk)     eph-itch, eph-fix, ...
```

### 命名空间

| 模块 | 命名空间 | 理由 |
|------|----------|------|
| eph-core | `eph::core::` | 已有，不变 |
| eph-transport | `eph::net::` | Transport/TransportConfig 等已广泛使用 `eph::net::`，迁移成本巨大；模块名≠命名空间名是 C++ 常见实践 |
| eph-net | `eph::net::` | 与 eph-transport 共享命名空间，语义一致 |
| eph-dpdk | `eph::dpdk::` | 已有，不变 |

### transport.hpp 拆分

当前 3461 行的 `transport.hpp` 拆分为：

| 新文件路径 | 内容 | 预估行数 |
|------------|------|----------|
| `eph/transport/transport.hpp` | Transport 类声明 + 公共 API（send/recv/lifecycle），include 所有 detail headers | ~900 |
| `eph/transport/detail/message_types.hpp` | TxMessage, RxMessage 结构体 | ~80 |
| `eph/transport/detail/transport_tx.hpp` | TX loop（encode → encrypt → send） | ~400 |
| `eph/transport/detail/transport_rx.hpp` | RX loop（poll → decrypt → decode） | ~500 |
| `eph/transport/detail/transport_frame.hpp` | WS 帧处理（fragmentation、ping/pong、close） | ~300 |
| `eph/transport/detail/transport_stats.hpp` | 直方图、RTT 统计 | ~150 |
| `eph/transport/detail/transport_state.hpp` | 状态管理、回调、重连逻辑 | ~500 |

`transport.hpp` 是用户唯一需要 include 的入口，内部通过 `#include "detail/..."` 组装完整实现。

### socket_transport.hpp 拆分

| 新文件路径 | 内容 |
|------------|------|
| `eph/net/socket_transport.hpp` | SocketTransport 类（TcpTransport concept 实现） |
| `eph/net/socket_config.hpp` | SocketConfig 结构体 + validate() + dump() |
| `eph/net/socket_connect.hpp` | `socket_wss_connect()` / `socket_ws_connect()` 便捷函数 + DNS 辅助 |

### Transport preset 别名统一

在 eph-transport 中定义 preset 模板别名：

```cpp
// eph/transport/presets.hpp
namespace eph::net {

template <typename TcpImpl> using DefaultTransport = Transport<TcpImpl, WsFramer, 512, 1024>;
template <typename TcpImpl> using SmallTransport   = Transport<TcpImpl, WsFramer, 64, 256>;
template <typename TcpImpl> using LargeTransport   = Transport<TcpImpl, WsFramer, 4096, 512>;
template <typename TcpImpl> using EvictTransport   = Transport<TcpImpl, WsFramer, 512, 1024,
                                                               eph::containers::EvictingQueue>;
template <typename TcpImpl> using RawTransport     = Transport<TcpImpl, RawFramer, 512, 1024>;

} // namespace eph::net
```

eph-net 和 eph-dpdk 各自只需一行别名：
```cpp
// eph-net
using SocketWssTransport = eph::net::DefaultTransport<SocketTransport>;

// eph-dpdk
using DpdkTransport = eph::net::DefaultTransport<TcpSession<>>;
```

### eph-transport 完整头文件结构

```
eph-transport/include/eph/transport/
├── transport.hpp              ← 用户入口（Transport 类 + 公共 API）
├── transport_types.hpp        ← TransportConfig, TransportStats, enums
├── presets.hpp                ← preset 别名模板
├── tls_session.hpp            ← TLS 1.3 握手 + 密钥提取
├── tls_record.hpp             ← TLS record AES-256-GCM AEAD
├── websocket.hpp              ← RFC 6455 WebSocket 协议
├── http.hpp                   ← HTTP/1.1 Upgrade
├── ws_framer.hpp              ← WebSocket MessageFramer
├── raw_framer.hpp             ← Pass-through framer
└── detail/
    ├── message_types.hpp      ← TxMessage, RxMessage
    ├── transport_tx.hpp       ← TX loop
    ├── transport_rx.hpp       ← RX loop
    ├── transport_frame.hpp    ← WS 帧处理
    ├── transport_stats.hpp    ← 统计
    └── transport_state.hpp    ← 状态管理 + 重连
```

### 转发头文件清理

从 eph-net 中删除以下转发头文件：
- `eph/net/tcp_concept.hpp` → 用户直接 `#include "eph/core/tcp_concept.hpp"`
- `eph/net/framer_concept.hpp` → 用户直接 `#include "eph/core/framer_concept.hpp"`
- `eph/net/length_prefix_framer.hpp` → 用户直接 `#include "eph/core/length_prefix_framer.hpp"`
- `eph/net/detail/json_escape.hpp` → 用户直接 `#include "eph/core/detail/json_escape.hpp"`

### eph-net 瘦身后头文件结构

```
eph-net/include/eph/
├── net.hpp                    ← 便捷入口（re-export 常用类型）
└── net/
    ├── socket_transport.hpp   ← SocketTransport 类
    ├── socket_config.hpp      ← SocketConfig + validation
    ├── socket_connect.hpp     ← 便捷连接函数
    ├── gateway.hpp            ← 多连接生命周期管理
    ├── kill_switch.hpp        ← 紧急关停
    ├── circuit_breaker.hpp    ← 断路器
    ├── rate_limiter.hpp       ← 令牌桶限流
    ├── proxy.hpp              ← SOCKS5 / HTTP CONNECT
    ├── http_client.hpp        ← 同步 REST 客户端
    └── hmac.hpp               ← HMAC-SHA256
```

### eph-dpdk 头文件结构（变更最小）

```
eph-dpdk/include/eph/
├── dpdk.hpp                   ← 便捷入口
└── dpdk/
    ├── types.hpp              ← DPDK Transport 别名（改用 presets.hpp）
    ├── eal.hpp                ← EAL 生命周期
    ├── platform.hpp           ← NIC 端口/队列/内存池
    ├── tcp.hpp                ← 用户态 TCP 状态机
    ├── net_header.hpp         ← 以太网/IPv4/TCP 头 + 校验和
    ├── reactor.hpp            ← Epoll 风格多路复用 RX
    ├── arp.hpp                ← ARP 解析
    ├── dns.hpp                ← 用户态 DNS
    ├── connector.hpp          ← 高层连接辅助
    ├── flow_steering.hpp      ← NIC RX 分发模式检测
    └── multicast.hpp          ← UDP 多播接收
```

主要变更：`types.hpp` 移除 ARCHITECTURE NOTE，改为使用 `eph/transport/presets.hpp`。

---

## 关键决策记录

### D-1: 创建 eph-transport 中间层
- **问题**：eph-dpdk 需要 Transport 模板但无法正式依赖 eph-net（DPDK vcpkg OpenSSL 与 aws-lc 头文件路径冲突）
- **选项**：A. 新建 eph-transport / B. Transport 参数化 Crypto / C. 保持 include hack
- **决策**：A — 新建 eph-transport
- **理由**：一次性迁移成本，换来永久清晰的依赖图；eph-net 和 eph-dpdk 成为对等的 TCP 后端
- **验收标准**：eph-dpdk 不再使用 `add_includedirs("eph-net/include")`，改为 `add_deps("eph-transport")`

### D-2: 命名空间保持 eph::net::
- **问题**：eph-transport 模块应使用什么命名空间
- **选项**：A. `eph::transport::` / B. `eph::net::` / C. `eph::`
- **决策**：B — `eph::net::`
- **理由**：Transport/TransportConfig 等类型已广泛使用 `eph::net::`，迁移成本巨大；模块名≠命名空间名是 C++ 常见实践
- **验收标准**：所有现有 `eph::net::Transport<>` 使用无需修改命名空间

### D-3: eph-dpdk 通过 add_includedirs 引用 eph-transport（非 add_deps）
- **问题**：`add_deps("eph-transport")` 导致 xmake 将 vcpkg DPDK 的 OpenSSL include 路径排在 aws-lc 之前，造成 `<openssl/*.h>` 头文件类型定义冲突
- **选项**：A. add_deps + 控制 include 顺序 / B. add_includedirs + 手动声明子依赖
- **决策**：B — 使用 `add_includedirs("eph-transport/include")` + 手动 `add_deps("eph-core", "eph-utils", "eph-containers")` + `add_packages("aws-lc")`
- **理由**：xmake 对 transitive package 的 -I 排序不可控；手动 includedirs 保证 aws-lc 在 dpdk 之前
- **验收标准**：`xmake build -g tests` 零编译错误；compile_commands.json 中 aws-lc -I 在 vcpkg -I 之前

---

## 编码规范

继承 CLAUDE.md 中已有规范，不额外定义：
- 日志：spdlog named loggers + `SPDLOG_ACTIVE_LEVEL` 编译期过滤
- 错误处理：`std::expected<T, std::string>` / ErrorEnum concept
- 风格：C++23（concepts, ranges, std::expected, std::format, structured bindings）
- 注释：explain why, not what
- 测试：边界条件 + 错误路径，测试名描述场景

---

## 实施计划

### 阶段 1: 创建 eph-transport 骨架 + 文件迁移

**交付物**：
- `eph-transport/include/eph/transport/` 目录结构完整
- 从 eph-net 移动并拆分：transport.hpp（→7 个文件）、transport_types.hpp、tls_session.hpp、tls_record.hpp、websocket.hpp、http.hpp、ws_framer.hpp、raw_framer.hpp
- 新增 `presets.hpp`（preset 别名模板）
- xmake.lua 中新增 `eph-transport` target
- eph-net 中添加临时转发头文件确保下游暂时不断

**验收标准**：
- `xmake build` 全部通过
- 新旧路径均可 include

**提交粒度**：
1. 创建目录结构 + xmake target
2. 迁移协议栈文件（tls_session、tls_record、websocket、http、ws_framer、raw_framer）
3. 拆分 transport.hpp 为 7 个文件
4. 新增 presets.hpp
5. 添加临时转发头文件

### 阶段 2: 更新依赖 + 拆分 socket_transport

**交付物**：
- eph-net：`add_deps("eph-transport")`，更新 `eph/net.hpp`
- eph-dpdk：`add_deps("eph-transport")`，删除 `add_includedirs("eph-net/include")`
- eph-dpdk/types.hpp：改用 presets.hpp，删除 ARCHITECTURE NOTE
- socket_transport.hpp 拆分为 socket_transport.hpp + socket_config.hpp + socket_connect.hpp
- 删除 eph-net 中 4 个转发头文件（tcp_concept、framer_concept、length_prefix_framer、json_escape）

**验收标准**：
- `xmake build` 全部通过
- `xmake run` 测试全部通过
- `grep -r 'add_includedirs.*eph-net/include' xmake.lua` 无结果

**提交粒度**：
1. 更新 eph-net 依赖 + eph/net.hpp
2. 更新 eph-dpdk 依赖 + types.hpp
3. 拆分 socket_transport.hpp
4. 删除转发头文件

### 阶段 3: 更新下游代码

**交付物**：
- 所有 tests/、benchmarks/、examples/ 中的 include 路径更新
- 删除阶段 1 留下的临时转发头文件
- 其他模块（eph-itch、eph-fix、eph-json、eph-book）中的 include 路径更新（如有引用）

**验收标准**：
- 全量测试通过
- `grep -r 'eph/net/transport\.hpp\|eph/net/tls_\|eph/net/websocket\|eph/net/http\.hpp\|eph/net/ws_framer\|eph/net/raw_framer' --include='*.cpp' --include='*.hpp'` 仅剩临时转发或无结果

**提交粒度**：
1. 更新 tests/ include 路径
2. 更新 benchmarks/ include 路径
3. 更新 examples/ include 路径
4. 删除临时转发头文件

### 阶段 4: 清理与验证

**交付物**：
- 删除 eph-net 中已迁移到 eph-transport 的残留文件
- 基准测试对比报告（确认无性能回归）
- 更新 docs/ 中的文档引用

**验收标准**：
- `grep -r "eph/net/transport.hpp"` 无结果
- 基准测试无回归（±5% 以内）
- 代码审查通过

**提交粒度**：
1. 删除残留文件
2. 运行基准 + 记录结果
3. 更新文档

---

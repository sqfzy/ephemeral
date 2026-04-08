# Plan: 项目组织重构

> 模块级 xmake.lua + 测试跟随模块 + transport detail/ 分层 + PCH/ccache 编译优化，3 阶段增量实施。

创建时间：2026-04-03
状态：已完成
讨论基础：.artifacts/discuss-20260403-072707.md

---

## 定位与边界

**目标**：将 553 行单体 xmake.lua + 集中式 tests/benchmarks/ 重构为模块自治的项目组织，提升局部性、可维护性和编译效率。

**In scope**：
- 拆分根 xmake.lua 为模块级构建文件
- 迁移 tests/ 和 benchmarks/ 到各模块内部
- eph-transport 头文件 detail/ 分层
- PCH、ccache、-march=native 统一管理
- 清理命名不一致（common.h → bench_common.hpp）

**Out of scope**：
- 不拆分 eph-transport 为多个模块（讨论已否决）
- 不拆分 monorepo 为多仓库
- 不引入 C++20 Modules（正交优化，独立评估）
- 不调整模块边界（如 circuit_breaker 从 net 移出）
- 不修改任何库的公共 API 或运行时行为

---

## 架构设计

### 目标目录结构

```
eph/
├── xmake.lua                         ← <100 行：全局设置 + rules + includes() + 集成目标
├── build/
│   ├── pch_test.hpp                  ← 测试 PCH：gtest + spdlog + stdlib
│   └── pch_bench.hpp                 ← Benchmark PCH：benchmark + spdlog + stdlib
├── eph-core/
│   ├── include/eph/core/...          ← 不变
│   ├── tests/                        ← 从 tests/core/ 迁入
│   ├── benchmarks/                   ← 从 benchmarks/core/ 迁入
│   └── xmake.lua                     ← target("eph-core") + 模块 tests + benchmarks
├── eph-utils/
│   ├── include/eph/utils/...
│   ├── tests/
│   ├── benchmarks/
│   └── xmake.lua
├── eph-containers/
│   ├── include/eph/containers/...
│   ├── tests/
│   ├── benchmarks/
│   └── xmake.lua
├── eph-transport/
│   ├── include/eph/transport/
│   │   ├── transport.hpp             ← 公共
│   │   ├── transport_types.hpp       ← 公共
│   │   ├── direct_transport.hpp      ← 公共
│   │   ├── direct_tx_transport.hpp   ← 公共
│   │   ├── presets.hpp               ← 公共
│   │   ├── reconnect_policy.hpp      ← 公共
│   │   ├── ws_framer.hpp             ← 公共
│   │   ├── raw_framer.hpp            ← 公共
│   │   └── detail/
│   │       ├── message_types.hpp     ← 已有
│   │       ├── transport_core.hpp
│   │       ├── frame_processor.hpp
│   │       ├── rx_worker.hpp
│   │       ├── tx_worker.hpp
│   │       ├── tls_session.hpp
│   │       ├── tls_encryptor.hpp
│   │       ├── tls_decryptor.hpp
│   │       ├── tls_record.hpp
│   │       ├── tls_constants.hpp
│   │       ├── http.hpp
│   │       └── websocket.hpp
│   ├── tests/                        ← 含 test_reconnect_policy.cpp（依赖 eph-net）
│   ├── benchmarks/
│   └── xmake.lua
├── eph-fix/
│   ├── include/eph/fix/...
│   ├── tests/
│   ├── benchmarks/
│   └── xmake.lua
├── eph-itch/
│   ├── include/eph/itch/...
│   ├── tests/
│   ├── benchmarks/
│   └── xmake.lua
├── eph-json/
│   ├── include/eph/json/...
│   ├── tests/
│   ├── benchmarks/
│   └── xmake.lua
├── eph-book/
│   ├── include/eph/book/...
│   ├── tests/                        ← 含 test_itch_adapter → tests/integration/
│   ├── benchmarks/                   ← 含 bench_array_book（依赖 eph-json）
│   └── xmake.lua
├── eph-net/
│   ├── include/eph/net/...
│   ├── tests/
│   ├── benchmarks/
│   └── xmake.lua
├── eph-dpdk/
│   ├── include/eph/dpdk/...
│   ├── tests/                        ← 含 dpdk_test_env.hpp
│   ├── benchmarks/
│   └── xmake.lua
├── tests/
│   └── integration/                  ← 跨模块集成测试（10 个原手动 target）
│       ├── test_itch_adapter.cpp
│       ├── test_binance_adapter.cpp
│       ├── test_binance_rest.cpp
│       ├── test_parse_number.cpp
│       ├── test_transport_errors.cpp
│       ├── test_metrics_concept.cpp
│       ├── test_audit_log.cpp
│       ├── test_gateway.cpp
│       ├── test_kill_switch.cpp
│       └── xmake.lua
├── benchmarks/
│   ├── latency/                      ← 端到端延迟 benchmark（保留原位）
│   │   ├── mock/...
│   │   ├── bench_impl.hpp
│   │   ├── bench_market.cpp
│   │   ├── bench_market_dpdk.cpp
│   │   ├── bench_mock_server.cpp
│   │   ├── bench_order_rtt.cpp
│   │   └── bench_order_rtt_dpdk.cpp
│   └── bench_common.hpp              ← 合并后的公共头
├── examples/                         ← 保留集中式
│   └── (14 个 .cpp 不变)
└── docs/
```

### 跨模块测试/benchmark 归属明细

| 原 target | 原依赖 | 新位置 | 理由 |
|-----------|--------|--------|------|
| test_itch_adapter | eph-book + eph-itch | tests/integration/ | 跨模块 |
| test_binance_adapter | eph-book + eph-json | tests/integration/ | 跨模块 |
| test_binance_rest | eph-json + eph-net | tests/integration/ | 跨模块 |
| test_parse_number | eph-core | eph-core/tests/ | 单模块（原手动 target 因目录映射缺失） |
| test_transport_errors | eph-core | eph-core/tests/ | 同上 |
| test_metrics_concept | eph-core + eph-utils | tests/integration/ | 跨模块 |
| test_audit_log | eph-utils | eph-utils/tests/ | 单模块 |
| test_reconnect_policy | eph-net | eph-transport/tests/ | 跟随被测头文件 |
| test_gateway | eph-net | eph-net/tests/ | 单模块 |
| test_kill_switch | eph-net | eph-net/tests/ | 单模块 |
| bench_array_book | eph-book + eph-json | eph-book/benchmarks/ | 主体是 book |
| bench_map_book | eph-book + eph-json | eph-book/benchmarks/ | 主体是 book |

### 依赖图（不变）

```
Layer 0: eph-core                    (无依赖)
Layer 1: eph-utils                   (→ core)
Layer 2: eph-containers              (→ utils)
         eph-fix, eph-itch,          (→ core)
         eph-json, eph-book          (→ core)
Layer 3: eph-transport               (→ core, utils, containers)
Layer 4: eph-net                     (→ transport)
         eph-dpdk                    (→ core, utils, containers + transport includedirs)
```

---

## 构建系统设计

### 根 xmake.lua 模板

```lua
set_project("eph")
set_version("1.0.0")

add_rules("mode.debug", "mode.release")
set_languages("c++23")
set_warnings("all", "extra")
add_rules("plugin.compile_commands.autoupdate", { outputdir = "build" })
set_policy("build.ccache", true)

if is_mode("release") then
    set_optimize("fastest")
end

-- GCC 14 on Amazon Linux 2023
if os.isfile("/usr/lib/gcc/aarch64-amazon-linux/14/libstdc++.so") then
    add_linkdirs("/usr/lib/gcc/aarch64-amazon-linux/14")
    add_rpathdirs("/usr/lib/gcc/aarch64-amazon-linux/14")
end

-- Dependencies
add_requires("numactl", "tabulate", "benchmark", "spdlog", { optional = true })
add_requires("vcpkg::dpdk", { optional = true, alias = "dpdk" })
add_requires("aws-lc", { optional = true })
add_requires("gtest", { system = false, configs = { main = true } })

-- Options
option("use_numa")
    set_default(false)
    set_showmenu(true)
    set_description("Enable NUMA support")
    add_defines("USE_NUMA")

option("native_arch")
    set_default(false)
    set_showmenu(true)
    set_description("Enable -march=native for performance-critical targets")

-- Global constants
local net_log_level = is_mode("debug") and "SPDLOG_LEVEL_TRACE" or "SPDLOG_LEVEL_INFO"

-- Global helper
function apply_dpdk_pmd_linkgroups()
    add_linkgroups("rte_net_null", "rte_net_ena", "rte_net_af_packet",
                   "rte_bus_pci", "rte_bus_vdev", "rte_mempool_ring",
                   { whole = true })
end

-- Rules for test/bench targets
rule("eph-test")
    on_load(function (target)
        target:set("kind", "binary")
        target:set("group", "tests")
        target:set("default", false)
        target:add("packages", "gtest")
        target:add("defines", "SPDLOG_NO_EXCEPTIONS")
        target:set("pcxxheader", path.absolute("build/pch_test.hpp"))
    end)

rule("eph-bench")
    on_load(function (target)
        target:set("kind", "binary")
        target:set("group", "benchmarks")
        target:set("default", false)
        target:add("packages", "benchmark")
        target:set("pcxxheader", path.absolute("build/pch_bench.hpp"))
        if has_config("native_arch") then
            target:add("cxflags", "-march=native", { force = true })
        end
    end)

-- Module includes (dependency order)
includes("eph-core/xmake.lua")
includes("eph-utils/xmake.lua")
includes("eph-containers/xmake.lua")
includes("eph-transport/xmake.lua")
includes("eph-fix/xmake.lua")
includes("eph-itch/xmake.lua")
includes("eph-json/xmake.lua")
includes("eph-book/xmake.lua")
includes("eph-net/xmake.lua")
includes("eph-dpdk/xmake.lua")

-- Integration tests
includes("tests/integration/xmake.lua")

-- Latency benchmarks (cross-module, root-managed)
-- ... (bench_mock_server, bench_market, bench_order_rtt, etc.)

-- Examples (centralized, user-facing)
-- ... (14 example targets)
```

### 模块 xmake.lua 模板（以 eph-fix 为例）

```lua
target("eph-fix")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/fix/**.hpp)")
    add_headerfiles("include/(eph/fix.hpp)")
    add_deps("eph-core", { public = true })
    add_packages("spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module tests
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_deps("eph-fix")
end

-- Module benchmarks
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-fix")
end
```

### 特殊模块处理

**eph-transport（含 detail/）：**
- `add_headerfiles("include/(eph/transport/**.hpp)")` 包含 detail/ 下的所有文件
- test_reconnect_policy 依赖 eph-net：单独声明 `add_deps("eph-net")`

**eph-book（benchmark 跨模块依赖）：**
```lua
-- benchmarks 需要 eph-json 作为输入数据源
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-book", "eph-json")
end
```

**eph-dpdk（PMD 链接 + 特殊 flags）：**
```lua
-- DPDK tests 需要 PMD whole-archive linking
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_deps("eph-dpdk")
        apply_dpdk_pmd_linkgroups()
end
```

### tests/integration/xmake.lua

```lua
-- Cross-module integration tests
-- Each target explicitly declares its multi-module dependencies.

target("test_itch_adapter")
    add_rules("eph-test")
    add_files("test_itch_adapter.cpp")
    add_deps("eph-book", "eph-itch")

target("test_binance_adapter")
    add_rules("eph-test")
    add_files("test_binance_adapter.cpp")
    add_deps("eph-book", "eph-json")

target("test_binance_rest")
    add_rules("eph-test")
    add_files("test_binance_rest.cpp")
    add_deps("eph-json", "eph-net")

target("test_metrics_concept")
    add_rules("eph-test")
    add_files("test_metrics_concept.cpp")
    add_deps("eph-core", "eph-utils")
```

---

## Transport detail/ 分层

### 公共头文件（8 个，保留 `eph/transport/`）

| 文件 | 用户使用场景 |
|------|-------------|
| transport.hpp | 创建 Transport 实例 |
| transport_types.hpp | TransportConfig 等配置类型 |
| direct_transport.hpp | 无线程传输模式 |
| direct_tx_transport.hpp | 半线程传输模式 |
| presets.hpp | 预配置工厂函数 |
| reconnect_policy.hpp | 重连策略配置 |
| ws_framer.hpp | WebSocket framer 选择 |
| raw_framer.hpp | Raw framer 选择 |

### 内部头文件（12 个，移入 `eph/transport/detail/`）

| 文件 | 内部原因 |
|------|---------|
| transport_core.hpp | Transport 模板内部实现 |
| frame_processor.hpp | 帧处理流水线内部 |
| rx_worker.hpp | RX 线程实现 |
| tx_worker.hpp | TX 线程实现 |
| tls_session.hpp | TLS 会话管理（通过 presets 暴露配置） |
| tls_encryptor.hpp | TLS 加密实现 |
| tls_decryptor.hpp | TLS 解密实现 |
| tls_record.hpp | TLS 记录层 |
| tls_constants.hpp | TLS 常量 |
| http.hpp | HTTP upgrade 内部实现 |
| websocket.hpp | WebSocket 帧编解码内部 |
| message_types.hpp | 已在 detail/ 中 |

### 迁移方式

1. `git mv` 移动文件到 detail/
2. `grep -r` 全项目替换 include 路径（如 `eph/transport/tls_session.hpp` → `eph/transport/detail/tls_session.hpp`）
3. 验证编译通过

---

## 编译优化

### build/pch_test.hpp

```cpp
// Precompiled header for test targets
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <cstdint>
```

### build/pch_bench.hpp

```cpp
// Precompiled header for benchmark targets
#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <cstdint>
```

### ccache

根 xmake.lua 添加 `set_policy("build.ccache", true)`。

### -march=native

根 xmake.lua 添加 `option("native_arch")`。`rule("eph-bench")` 中自动检测并应用。Example target 和 latency benchmark 中按需引用 `has_config("native_arch")`。

---

## 实施计划

> **Commit 策略**：每个阶段完成并通过验收后，执行 `/git` 提交。commit message 标注阶段编号。

### 阶段 1: 编译优化 + 构建基础设施

**交付物：**
- `build/pch_test.hpp` 和 `build/pch_bench.hpp` 创建
- 根 xmake.lua 添加 `set_policy("build.ccache", true)`
- 根 xmake.lua 添加 `option("native_arch")`
- 根 xmake.lua 定义 `rule("eph-test")` 和 `rule("eph-bench")`
- 合并 `benchmarks/common.h` 到 `benchmarks/bench_common.hpp`

**验收标准：**
- `xmake build -g tests` 全量编译通过
- `xmake run -g tests` 全量测试通过
- `xmake build -g benchmarks` 全量编译通过
- `xmake build -g examples` 全量编译通过

**推荐 skill：** `/design auto`

### 阶段 2: 全量模块迁移

按依赖图顺序逐模块执行，每个模块完成后验证编译：

**迁移顺序：**
1. eph-core（+ test_parse_number, test_transport_errors 回归为模块内测试）
2. eph-utils（+ test_audit_log 回归为模块内测试）
3. eph-containers
4. eph-transport（含 detail/ 分层 + include 路径全局替换 + test_reconnect_policy）
5. eph-fix, eph-itch, eph-json, eph-book（可并行，含 book benchmark 跨模块依赖处理）
6. eph-net（+ test_gateway, test_kill_switch 回归为模块内测试）
7. eph-dpdk（含 dpdk_test_env.hpp 搬迁 + PMD linkgroups）

**每个模块的迁移步骤：**
1. 创建 `eph-X/xmake.lua`
2. `git mv tests/X/*.cpp eph-X/tests/`
3. `git mv benchmarks/X/*.cpp eph-X/benchmarks/`（如有）
4. 从根 xmake.lua 中删除该模块的 target 定义
5. `xmake build` 验证

**交付物：**
- 10 个模块各有自己的 xmake.lua、tests/、benchmarks/
- 旧 tests/ 子目录全部清空删除（保留 tests/integration/）
- 旧 benchmarks/ 模块子目录全部清空删除（保留 latency/ 和 bench_common.hpp）
- 根 xmake.lua < 100 行

**验收标准：**
- `xmake build -g tests && xmake run -g tests` 全量通过
- `xmake build -g benchmarks` 全量通过
- `xmake build -g examples` 全量通过
- 根 xmake.lua 行数 < 100

**推荐 skill：** `/design auto`（逐模块执行）

### 阶段 3: 集成测试收尾

**交付物：**
- `tests/integration/` 目录建立，含 xmake.lua
- 跨模块测试迁入（test_itch_adapter, test_binance_adapter, test_binance_rest, test_metrics_concept）
- latency benchmark target 定义移入根 xmake.lua 或 `benchmarks/latency/xmake.lua`
- example target 定义保留在根 xmake.lua

**验收标准：**
- `xmake build -g tests && xmake run -g tests` 全量通过
- `xmake build -g benchmarks` 全量通过
- `xmake build -g examples` 全量通过
- 根目录 `tests/` 下仅剩 `integration/` 子目录
- 根目录 `benchmarks/` 下仅剩 `latency/` + `bench_common.hpp`

**推荐 skill：** `/design auto`

---

## 关键决策记录

### D-1: 不拆分 eph-transport 模块
- **问题**：eph-transport 混合了 TLS、WebSocket、线程模型等多个关注点
- **选项**：A. 拆为 transport + tls + ws / B. 仅 detail/ 分层 / C. 不动
- **决策**：B（detail/ 分层）
- **理由**：讨论中 R2 和 R3 论证 TLS/WS 是传输层固有能力，拆分破坏模板组合内聚性。DPDK include hack 是包管理问题不是模块设计问题
- **验收标准**：detail/ 分层后 eph-dpdk 的 include hack 保持不变（不因此消除），编译无回归

### D-2: 跨模块测试集中管理
- **问题**：跨模块测试放在哪个模块的 xmake.lua 中
- **选项**：A. 被测主模块 / B. tests/integration/ / C. 依赖链高层模块
- **决策**：B
- **理由**："主模块"判断模糊（test_binance_adapter 归 book 还是 json？），集中放在 integration/ 消除歧义
- **验收标准**：所有依赖 2+ 个模块的测试在 tests/integration/ 中，各模块 xmake.lua 内无跨模块测试

### D-3: PCH 分角色而非全局
- **问题**：PCH 粒度选择
- **选项**：A. 单一全局 / B. 分角色 / C. 分模块
- **决策**：B
- **理由**：test 用 gtest、bench 用 Google Benchmark，混合 PCH 会让非 test target 也编译 gtest 头
- **验收标准**：rule("eph-test") 使用 pch_test.hpp，rule("eph-bench") 使用 pch_bench.hpp，编译时间有可观测下降

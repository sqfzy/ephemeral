# eph-utils 项目摘要

## 1. 概述

eph-utils 是 ephemeral 项目的底层工具库，提供高性能系统编程所需的基础设施。它是一个纯头文件（header-only）的 C++23 库，所有代码位于 `include/eph/utils/` 目录下，通过 xmake 构建系统管理。

该库围绕四个核心关注点设计：**CPU 拓扑与亲和性管理**、**纳秒级高精度计时**、**大页内存分配**、**性能统计与记录**。所有模块均使用 spdlog 进行结构化日志输出，支持 Linux/macOS/Windows 多平台（Linux 支持最完整），并大量使用 x86 和 ARM64 的硬件指令实现低开销操作。

eph-utils 是 ephemeral 生态中最底层的依赖，被 eph-containers、eph-net 等上层模块直接依赖。它的设计目标是为高频交易、网络中间件等延迟敏感场景提供零开销的系统原语抽象。

## 2. 架构

```
+----------------------------------------------------------+
|                    eph/utils.hpp                         |
|              (便捷聚合头文件，包含所有模块)                |
+----------------------------------------------------------+
     |            |             |            |
     v            v             v            v
+---------+ +-----------+ +-----------+ +-----------+
| align-  | |  cpu.hpp   | | time.hpp  | | record.hpp|
| ment.hpp| |            | |           | |           |
+---------+ +-----------+ +-----------+ +-----------+
|CACHE_   | |CpuTopology | |TSC        | |HdrHisto-  |
|LINE_SIZE| |Info        | |(rdtscp/   | |gram       |
|Align<T> | |get_cpu_    | | cntvct)   | |Recorder   |
|         | |topology()  | |init()     | |Concurrent-|
|         | |set_thread_ | |now()      | |Recorder   |
|         | |affinity()  | |to_ns()    | |ScopedTSC  |
|         | |cpu_relax() | |to_cycles()| |SystemStats|
+---------+ +-----------+ +-----------+ +-----------+
                                ^              |
                                |  depends on  |
                                +--------------+
```

## 3. 模块映射

| 模块/文件 | 职责 | 关键类型/函数 | 依赖 |
|-----------|------|---------------|------|
| `alignment.hpp` | 缓存行对齐常量与模板 | `CACHE_LINE_SIZE`, `Align<T>` | 无 |
| `cpu.hpp` | CPU 拓扑检测、线程亲和性、自旋等待 | `CpuTopologyInfo`, `get_cpu_topology()`, `set_thread_affinity()`, `cpu_relax()` | spdlog, immintrin.h |
| `time.hpp` | 基于硬件 TSC 的纳秒级计时器 | `TSC` (静态类) | spdlog, immintrin.h/x86intrin.h, arm_neon.h |
| `record.hpp` | 性能基准测试与延迟统计框架 | `HdrHistogram`, `Recorder`, `ConcurrentRecorder`, `ScopedTSC`, `SystemStats`, `Stats` | `time.hpp`, spdlog |
| `utils.hpp` | 聚合头文件 | - | 所有上述模块 |

## 4. 数据流

### 典型性能测量流程

```
程序启动
  |
  v
TSC::init()  ──> 预热 CPU ──> 多轮采样校准
  |                            steady_clock
  |                            对比 rdtscp
  v
ns_per_cycle_ 确定
  |
  v
热路径循环:
  TSC::now() ──> rdtscp/cntvct ──> start_cycles
  [被测代码]
  TSC::now() ──────────────────> end_cycles
  |
  v
Recorder::record(end - start)
  |
  v
HdrHistogram::record(cycles)
  ├── counts_index_for(value) ──> 对数桶定位
  └── counts_[idx]++
  |
  v
Recorder::compute_stats()
  ├── get_percentiles({50, 90, 99, 99.9})
  ├── cycles * ns_per_cycle_ ──> 转换为纳秒
  └── 返回 Stats 结构体
  |
  v
输出: print_report() / export_json() / export_csv()
```

### ConcurrentRecorder 多线程数据流

```
Thread 1 ──> thread_local HdrHistogram ──┐
Thread 2 ──> thread_local HdrHistogram ──┤ merge_all()
Thread N ──> thread_local HdrHistogram ──┤────────────> Stats
                                         |
线程退出 ──> retire_local() ──> retired_  |
             histogram (mutex保护) ───────┘
```

## 5. 关键组件

### 5.1 TSC (时间戳计数器)
- **文件**: `include/eph/utils/time.hpp`
- **用途**: 提供约 20 cycles 开销的纳秒级计时，基于 x86 `rdtscp` 或 ARM64 `cntvct_el0`
- **接口**:
  ```cpp
  static bool init(std::chrono::milliseconds duration = 200ms);
  static uint64_t now() noexcept;
  static std::optional<double> to_ns(uint64_t cycles) noexcept;
  static std::optional<uint64_t> to_cycles(Rep ns) noexcept;
  ```
- **注意**: 校准采用 5 轮采样取中位数，变异系数超过 1% 会发出警告。非线程安全的 `init()` 必须在并发使用前调用一次。

### 5.2 HdrHistogram (高动态范围直方图)
- **文件**: `include/eph/utils/record.hpp` (第 89-507 行)
- **用途**: 在宽范围内（如 1ns-10s）以恒定相对精度记录延迟分布，基于 Gil Tene 算法
- **接口**:
  ```cpp
  bool record(uint64_t value) noexcept;          // ~5-10ns/次
  uint64_t get_value_at_percentile(double p) const noexcept;
  std::vector<uint64_t> get_percentiles(const std::vector<double>&) const;
  bool merge(const HdrHistogram& other) noexcept;
  std::string report(std::string_view title, std::string_view unit) const;
  ```
- **注意**: 非线程安全。内部使用对数桶映射 (`counts_index_for`)，内存约 ~20KB（3位有效数字，1-34G cycles 范围）。

### 5.3 Recorder (单线程性能记录器)
- **文件**: `include/eph/utils/record.hpp` (第 674-1028 行)
- **用途**: 封装 HdrHistogram + TSC 转换，提供完整的基准测试工作流
- **接口**:
  ```cpp
  bool record(uint64_t cycles) noexcept;
  std::optional<Stats> compute_stats() const noexcept;
  void print_report() const;
  bool export_json(const std::string& dir = "outputs") const;
  bool export_csv(const std::string& dir = "outputs") const;
  ```

### 5.4 ConcurrentRecorder (多线程性能记录器)
- **文件**: `include/eph/utils/record.hpp` (第 1059-1360 行)
- **用途**: 通过 thread_local HdrHistogram 实现零竞争的多线程延迟记录
- **接口**: 与 Recorder 基本一致 (`record`, `compute_stats`, `print_report`)
- **注意**: 使用 `shared_ptr<SharedState>` 管理生命周期，确保 thread_local 析构器在 ConcurrentRecorder 销毁后仍能安全 merge 数据到退役缓冲区。`record()` 热路径仅检查一次指针，无锁无竞争。

### 5.5 get_cpu_topology / set_thread_affinity
- **文件**: `include/eph/utils/cpu.hpp`
- **用途**: 检测 CPU Socket/Core/Thread 拓扑，绑定线程到指定核心
- **接口**:
  ```cpp
  std::vector<CpuTopologyInfo> get_cpu_topology();  // 解析 /proc/cpuinfo
  void set_thread_affinity(unsigned cpu_id);         // pthread_setaffinity_np
  void cpu_relax() noexcept;                         // PAUSE/YIELD 指令
  ```
- **注意**: ARM 上 /proc/cpuinfo 缺少 `physical id`/`core id` 字段时自动降级为简化拓扑。

### 5.6 HugePage (大页内存分配器)
- **文件**: `include/eph/utils/hugepage.hpp`
- **用途**: 尝试分配 2MB 大页内存（Linux mmap MAP_HUGETLB），失败自动回退到 aligned_alloc
- **接口**:
  ```cpp
  template<typename T, typename... Args>
  static auto make(Args&&... args);  // 返回 unique_ptr<T, Deleter<T>>
  static void* allocate(size_t size, size_t alignment,
                        bool& is_hugepage, size_t& out_allocated_size);
  static void deallocate(void* ptr, size_t size, bool is_hugepage);
  ```
- **注意**: 自定义 `Deleter<T>` 捕获分配元数据（大小、是否大页），通过 placement new 构造对象并在析构时正确调用析构函数和释放内存。

### 5.7 SystemStats (系统资源采集器)
- **文件**: `include/eph/utils/record.hpp` (第 560-649 行)
- **用途**: RAII 风格的 `getrusage` 封装，采集缺页、上下文切换、CPU 时间
- **接口**:
  ```cpp
  SystemResourceStats snapshot() const noexcept;
  void print_report() const;
  void reset() noexcept;
  ```

### 5.8 Align<T> / CACHE_LINE_SIZE
- **文件**: `include/eph/utils/alignment.hpp`
- **用途**: 编译期缓存行对齐计算，`Align<T>` 取 `alignof(T)` 和 64 的较大值
- **注意**: 纯 constexpr，零运行时开销。

## 6. 入口点与 API

| 入口 | 类型 | 说明 |
|------|------|------|
| `#include <eph/utils.hpp>` | 聚合头文件 | 包含所有模块 |
| `TSC::init()` | 初始化 | 必须在程序启动时调用一次 |
| `TSC::now()` | 热路径 | 读取硬件时间戳，~20 cycles |
| `Recorder::record()` | 热路径 | 记录延迟样本 |
| `ConcurrentRecorder::record()` | 热路径(多线程) | 零竞争延迟记录 |
| `HugePage::make<T>()` | 工厂函数 | 大页内存对象创建 |
| `get_cpu_topology()` | 初始化 | CPU 拓扑检测 |
| `set_thread_affinity()` | 初始化 | 线程绑核 |
| `Recorder::export_json/csv()` | 输出 | 导出性能数据 |

## 7. 依赖关系

### 内部模块依赖图

```
alignment.hpp    cpu.hpp    time.hpp
     |              |          |
     |              |          |
     v              v          v
     +-------- utils.hpp ------+
                               |
                    time.hpp <-+
                       ^
                       |
                  record.hpp
```

### 外部依赖

| 包名 | 用途 | 引入模块 |
|------|------|----------|
| spdlog | 结构化日志 | cpu.hpp, time.hpp, hugepage.hpp |
| immintrin.h / x86intrin.h | x86 SIMD/TSC 指令 | cpu.hpp, time.hpp |
| arm_neon.h | ARM64 NEON 指令 | time.hpp |
| sys/mman.h | mmap 大页分配 | hugepage.hpp (Linux) |
| sys/resource.h | getrusage 系统资源 | record.hpp |
| pthread | 线程亲和性 | cpu.hpp (Linux) |

## 8. 测试

本库为头文件库，无独立的测试目录。测试通过上层模块（eph-containers、eph-net 的示例和基准测试）间接覆盖。

| 可测场景 | 对应组件 | 说明 |
|----------|----------|------|
| TSC 校准精度 | `TSC::init()` | 验证 CV < 1%，频率在 0.5-10 GHz |
| TSC 单调性 | `TSC::now()` | 连续调用应单调递增 |
| HdrHistogram 百分位精度 | `HdrHistogram` | 已知分布输入验证 p50/p99 |
| Recorder JSON/CSV 导出 | `Recorder` | 文件格式验证 |
| ConcurrentRecorder 线程安全 | `ConcurrentRecorder` | 多线程并发 record + merge |
| 大页回退 | `HugePage` | 无大页配置时应回退到 aligned_alloc |
| CPU 拓扑 ARM 降级 | `get_cpu_topology()` | 无 physical id 时简化拓扑 |
| 对齐计算 | `Align<T>` | 编译期 static_assert 验证 |

**模板复杂度说明**: 模板使用较为克制，主要集中在 `HugePage::make<T>` (完美转发 + placement new + 自定义删除器)、`TSC::to_cycles` (duration 重载) 和 `Recorder` 的 `measure_tsc` (concepts constrained)。无深层模板元编程。

**平台差异边界**: 所有平台相关代码通过 `#if defined(__linux__)` / `#if defined(__x86_64__)` / `#if defined(__aarch64__)` 条件编译隔离，非支持平台均有合理的 fallback 实现。

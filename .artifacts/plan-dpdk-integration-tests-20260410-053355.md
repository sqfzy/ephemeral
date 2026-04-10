# Plan: eph-dpdk End-to-End Integration Tests

> 给 eph-dpdk 添加跨 datapath 的 E2E 集成测试，覆盖 TCP / UDP / WebSocket / Reactor / ARP / 错误恢复路径，使用真实 NIC `ens35` 与 lat_*_dpdk 同构

创建时间：2026-04-10
状态：已确认

---

## 定位与边界

**目标**：补足 eph-dpdk 模块从「单元测试 only」到「具备完整 E2E 数据路径覆盖」的缺失，与 lat_*_dpdk 共享基础设施，避免维护双套 mock。

**用户**：eph-dpdk 维护者；任何修改 packet_template / packet_parse / TcpSession / UdpEndpoint / Reactor 的开发者，应在合并前跑完该测试集。

**In scope**：
- TCP datapath E2E（3WHS、echo、graceful close、RST 恢复）
- UDP datapath E2E（单包 + inflight burst）
- WebSocket E2E（DPDK + eph-transport DirectTransport，验证 concept conformance）
- ARP resolve（against kernel ARP responder）
- Reactor 多连接并发与公平性
- mock 子进程基础设施物理迁移到 `tests/support/dpdk_bench/`，与 lat bench 共享
- 测试运行 wrapper（NIC 状态过渡）
- 7 个测试用例（4 P0 + 3 P1）

**Out of scope**（标为后续 phase）：
- DNS resolve E2E（需要新写微型 DNS responder）
- Multicast E2E（需要 IGMP join 协调）
- TCP retransmission / RTO（需要可控丢包 mock）
- TCP window scaling（当前 eph-dpdk 不支持，等实现后补）
- TLS over DPDK（与 WS DirectTransport 已覆盖跨模块路径，TLS 单独验证留给 phase 2）
- 跨进程 DPDK / multi-process EAL
- net_tap / net_pair 等虚拟 PMD（用户明确否决）

---

## 技术选型

| 类别 | 选择 | 理由 |
|---|---|---|
| 测试框架 | gtest（沿用 `eph-test` rule） | 与现有所有测试一致；CLAUDE.md 钦定 |
| 构建系统 | xmake（沿用） | 项目标准 |
| EAL 模式 | 真实 PCI（NIC_B = ens35, BDF `0000:28:00.0`） | 用户明确："dpdk 测试总是应该直接用真的网卡" |
| 数据通路 | NIC_A (kernel mock) ↔ NIC_B (DPDK client) | 与 lat_*_dpdk 同构，复用其拓扑与 mock 实现 |
| 隔离方式 | 进程内单 EAL；fork 子进程跑 mock；子进程在 host ns（mock 绑 NIC_A）；父进程 DPDK 直接打开 NIC_B | 与 lat_*_dpdk 完全一致 |
| 子进程通信 | mock 监听不同 port → 每种行为一个 port | 简单、可读、无需带外控制管道 |
| 测试聚合 | 单一 binary `test_dpdk_e2e`，gtest suites 组织成 TCP/UDP/WS/Reactor/ARP/Failure | EAL init 是 ~2s 的一次性开销；NIC 互斥也强制串行；多 binary 无收益 |
| NIC 状态管理 | 测试 main 自检 + wrapper 脚本预处理 | 测试自身不破坏 host 状态；wrapper 提供一行命令 |

---

## 架构设计

### 物理布局

```
eph-dpdk/
  tests/
    *.cpp                            # 现有 unit tests（--no-pci，与 E2E 不能共存于同一 binary）
    integration/                     # 新增子目录
      test_dpdk_e2e.cpp              # 唯一 E2E binary 的入口
      tcp_e2e.cpp                    # TCP suite（被 test_dpdk_e2e.cpp include）
      udp_e2e.cpp                    # UDP suite
      ws_e2e.cpp                     # WebSocket suite（依赖 eph-transport）
      reactor_e2e.cpp                # Reactor suite
      arp_e2e.cpp                    # ARP suite
      failure_e2e.cpp                # RST / FIN / err recovery suite
      dpdk_e2e_fixture.hpp           # gtest fixture：fork mock + EAL init + Platform create
      mock_dispatcher.hpp            # 子进程入口：起 N 个线程跑 N 个 mock_fn
  scripts/
    dpdk-setup.sh                    # 已有
    dpdk-teardown.sh                 # 已有
    dpdk-state.sh                    # 新增：纯函数库（NIC 状态检查与过渡）

tests/
  support/
    dpdk_bench/                      # 新增：从 benchmarks/latency/core/ 物理迁移
      config.hpp                     # bench.conf 解析
      dpdk_env.hpp                   # DpdkBenchEnv（EAL + Platform + ARP）
      netns.hpp                      # setns helper（mock 子进程会用）
      tsc_protocol.hpp               # 测试不一定需要，迁过来给 bench 仍用
      mocks/
        tcp_mock.hpp                 # 从 benchmarks/latency/tcp/lat_tcp.cpp 提取
        udp_mock.hpp                 # 从 benchmarks/latency/udp/lat_udp.cpp 提取
        ws_mock.hpp                  # 从 benchmarks/latency/ws/lat_ws.cpp 提取
        rst_mock.hpp                 # 新：accept 后立即 RST
        fin_mock.hpp                 # 新：echo N 字节后 shutdown(WR)
  integration/
    xmake.lua                        # 仍是已有的，新 target 在 eph-dpdk/xmake.lua 里登记
    dpdk_e2e                         # 新增 wrapper 脚本（仿 lat 风格）

benchmarks/latency/
  tcp/lat_tcp.cpp                    # 改 include：core/* → tests/support/dpdk_bench/*
  udp/lat_udp.cpp                    # 同上
  ws/lat_ws.cpp                      # 同上
  exchange/lat_*.cpp                 # 同上
  lat                                # 改：source eph-dpdk/scripts/dpdk-state.sh 提取出的函数
  core/                              # 物理删除（已迁走）
```

### 模块依赖

| 模块 | 职责 | 依赖 |
|---|---|---|
| `tests/support/dpdk_bench/` | 头库：bench.conf 解析、EAL 启动、ARP 解析、mock 实现 | `eph-dpdk`, `eph-utils`, `eph-core`, `spdlog`, `dpdk` |
| `eph-dpdk/tests/integration/test_dpdk_e2e` | E2E 测试 binary | `eph-dpdk`, `eph-transport`, `tests/support/dpdk_bench`, `gtest`, `dpdk`（PMD whole-archive） |
| `benchmarks/latency/lat_*_dpdk` | 现有 bench 二进制 | 同上（include 路径切换） |
| `eph-dpdk/scripts/dpdk-state.sh` | shell 函数库：检查 / 切换 / 恢复 NIC 状态 | 无 |
| `tests/integration/dpdk_e2e` | wrapper 脚本：source dpdk-state.sh + exec test_dpdk_e2e | `eph-dpdk/scripts/dpdk-state.sh`, `bench.conf` |

依赖方向是干净的单向：`tests/support/dpdk_bench/` 是叶子；`test_dpdk_e2e` 与 `lat_*_dpdk` 都消费它；shell wrappers 共享 dpdk-state.sh。无循环。

### 核心抽象

#### `tests/support/dpdk_bench/dpdk_env.hpp`（迁移自 benchmarks/latency/core/）

```cpp
namespace eph::dpdk::test_support {

/// Move-only bundle: EAL guard + Platform + resolved IPs/MACs
/// Created once per test binary in DpdkE2EFixture::SetUpTestSuite
struct DpdkBenchEnv { ... };  // 与现有 bench::DpdkBenchEnv 同结构，namespace 改

}
```

#### `eph-dpdk/tests/integration/dpdk_e2e_fixture.hpp`

```cpp
class DpdkE2EFixture : public ::testing::Test {
public:
    /// Called once before any test in the binary.
    /// 1. Load bench.conf
    /// 2. Verify NIC_B is bound to vfio-pci (else GTEST_SKIP all)
    /// 3. fork() — child runs MockDispatcher::run(cfg)
    /// 4. Parent: DpdkBenchEnv::create_full()
    /// 5. Parent: ARP-resolve gateway (via eph::dpdk::arp::resolve)
    static void SetUpTestSuite();

    /// 1. SIGTERM mock child
    /// 2. waitpid
    /// 3. Destroy DpdkBenchEnv (releases EAL via guard)
    static void TearDownTestSuite();

    /// Per-test: get a fresh TcpConfig pointing at SERVER_IP:test_port
    static eph::dpdk::TcpConfig make_tcp_config(uint16_t mock_port);
    static eph::dpdk::UdpConfig make_udp_config(uint16_t mock_port);

protected:
    static inline std::optional<DpdkBenchEnv> env_;
    static inline pid_t mock_pid_ = -1;
    static inline bool nic_ready_ = false;
};
```

#### `eph-dpdk/tests/integration/mock_dispatcher.hpp`

```cpp
namespace eph::dpdk::test_support {

/// Child-process entry point.
/// Starts N threads, one per mock_fn, each binding to its assigned port.
/// Blocks until SIGTERM, then joins threads cleanly.
class MockDispatcher {
public:
    static int run(const BenchConfig& cfg);
};

// Port allocation (compile-time constants):
inline constexpr uint16_t kTcpEchoPort      = 19001;
inline constexpr uint16_t kTcpRstPort       = 19002;
inline constexpr uint16_t kTcpFinPort       = 19003;
inline constexpr uint16_t kUdpEchoPort      = 19101;
inline constexpr uint16_t kWsEchoPort       = 19201;
inline constexpr uint16_t kReactorPortBase  = 19301;  // multi-conn allocates 19301..19316

}
```

### 数据流

```
test process (parent)                     mock process (child)
─────────────────────                     ────────────────────
main()                                    main()  (after fork)
  │                                         │
  fork() ────────────────────────────────► MockDispatcher::run(cfg)
  │                                         ├── thread: tcp_echo_mock @ NIC_A:19001
  ::testing::InitGoogleTest()               ├── thread: rst_mock     @ NIC_A:19002
  RUN_ALL_TESTS()                           ├── thread: fin_mock     @ NIC_A:19003
    │                                       ├── thread: udp_echo_mock @ NIC_A:19101
    DpdkE2EFixture::SetUpTestSuite()        └── thread: ws_echo_mock  @ NIC_A:19201
      ├── load_bench_conf()                 (all in host network namespace)
      ├── verify NIC_B on vfio-pci ─── if not: nic_ready_ = false
      ├── DpdkBenchEnv::create_full()       
      │     ├── EAL init (real PCI)         
      │     ├── Platform::create(NIC_B)     
      │     └── arp::resolve(gw)            
      └── store env_                        
                                            
    TEST(TcpE2E, SmokeEcho)                 
      ├── if (!nic_ready_) GTEST_SKIP()    
      ├── TcpSession s(make_tcp_config(kTcpEchoPort))
      ├── s.connect() ───── Ethernet ────► tcp_echo_mock accepts
      ├── s.send(payload) ───────────────► echoes back
      ├── poll_rx loop ◄──────────────────── reads echo
      ├── ASSERT bytes match               
      └── s.close()                        
                                            
    [... 6 more tests ...]                 
                                            
    DpdkE2EFixture::TearDownTestSuite()    
      ├── kill(mock_pid_, SIGTERM)  ─────► child exits, joins threads
      ├── waitpid(mock_pid_)               
      └── env_.reset()  // releases EAL    
                                            
  return RUN_ALL_TESTS_result               
```

---

## 接口设计

### 测试用例清单（gtest TEST 名称）

#### Phase 1 P0（4 个）

| Suite.Test | 描述 | 验证目标 | mock port |
|---|---|---|---|
| `TcpE2E.SmokeEcho` | 连接 → 发送 4 个 size（64/256/1024/4096）→ 验证 echo 字节匹配 → graceful close | TcpSession::connect/send/poll_rx/close 全路径；packet_template；mbuf 生命周期 | 19001 |
| `UdpE2E.BurstEcho` | 发送 64 个不同 size 包 → poll_rx 收齐 → 验证顺序与字节 | UdpEndpoint::send/poll_rx；UDP checksum；mbuf 释放 | 19101 |
| `WsE2E.HandshakeAndEcho` | DirectTransport<TcpSession, WsFramer>::create() → 完成 WS handshake → 发 5 帧 → 收 5 帧 | eph-transport ↔ eph-dpdk concept conformance；HTTP upgrade；WS framing 完整性 | 19201 |
| `FailureE2E.PeerRstAfterAccept` | 连 RST mock → mock accept 后立即 RST → client TcpSession 进入 Closed 状态并报错 | RST 处理路径；RFC 5961 校验通过；状态机 | 19002 |

#### Phase 1 P1（3 个）

| Suite.Test | 描述 | 验证目标 | mock port |
|---|---|---|---|
| `FailureE2E.PeerFinAfterEcho` | 连 FIN mock → mock echo 一次后 shutdown(WR) → client poll_rx 看到 0 字节 → close | TCP 关闭路径 4-way；FinWait1 → FinWait2 → CloseWait | 19003 |
| `ReactorE2E.MultiConnFairness` | Reactor 注册 8 个 TcpSession 同时连 mock → 每个发 100 个包 → 验证全部收齐且无饥饿 | Reactor poll loop；dispatch；并发 mbuf pool 压力 | 19301..19308 |
| `ArpE2E.ResolveGateway` | 干净状态调用 `arp::resolve(gw_ip)` → 验证返回的 MAC 与 `ip neigh show` 一致 | ARP 解析；retry；缓存 | (none, ARP-only) |

### Wrapper 脚本接口

```bash
# Sets NIC_B → vfio-pci, runs test_dpdk_e2e, restores NIC_B → kernel
sudo tests/integration/dpdk_e2e
sudo tests/integration/dpdk_e2e --gtest_filter='TcpE2E.*'
sudo tests/integration/dpdk_e2e --keep-state  # don't restore on exit
```

### 错误体系

测试断言用 gtest 的 `ASSERT_*` / `EXPECT_*`。Fixture 内部用 `std::expected` 与现有 lat 代码一致，转换到 gtest 失败：

```cpp
#define EPH_DPDK_ASSERT_OK(expr) do {                          \
    auto _r = (expr);                                          \
    ASSERT_TRUE(_r.has_value()) << "got error: " << _r.error(); \
} while (0)
```

---

## 编码规范

| 维度 | 规范 |
|---|---|
| 命名 | 测试 suite：`<Module>E2E`（TcpE2E, UdpE2E, ...）；test 名：`PascalCase`，描述场景而非操作 |
| 错误处理 | Fixture 与 mock 用 `std::expected<T, std::string>`；测试体用 `EPH_DPDK_ASSERT_OK` 转换 |
| 日志 | 沿用 spdlog；测试启动时设级别为 WARN（避免 INFO 噪声）；TRACE 留给手动调试 |
| 注释 | 每个 test 顶部用 `/// @scenario:` 描述触发条件与预期结果；非显然的 mbuf/timing/race 处加 why-comment |
| Header-only | 测试 helper 仍是 header-only（与项目约定一致）；唯一非 header 是 test_dpdk_e2e.cpp 入口 |
| 端口分配 | 全部硬编码常量（在 `mock_dispatcher.hpp` 集中），不读环境变量——避免测试机器间漂移 |

---

## 实施计划

> **Commit 策略**：每个阶段独立 commit，message 标注 `phase N — <标题>`。每个阶段都要能独立 build + 现有 test 全绿后才能进入下一阶段。

### 阶段 1：基础设施物理迁移（无功能变化）

**目标**：把 `benchmarks/latency/core/` 迁移到 `tests/support/dpdk_bench/`，bench 仍能 build/run。

**交付物**：
- `tests/support/dpdk_bench/{config,dpdk_env,netns,tsc_protocol,sample,scenario_concept,signal,socket_bind,stream_scheduler}.hpp`（物理迁移，namespace `bench` → `eph::dpdk::test_support`）
- 在 `tests/support/dpdk_bench/` 加一个 `xmake.lua` 把它声明为 `headeronly` target
- 改 `benchmarks/latency/{tcp,udp,ws,exchange}/lat_*.cpp` 的 include 路径
- 改 `benchmarks/latency/xmake.lua` 让 lat_*_dpdk 依赖新 target
- 删除 `benchmarks/latency/core/`（迁移完后）

**验收标准**：
- `xmake build lat_tcp_dpdk lat_udp_dpdk lat_ws_dpdk lat_ex_market_dpdk lat_ex_order_dpdk lat_ex_md_udp_dpdk` 全部 build 成功
- 在用户许可下 `sudo benchmarks/latency/lat tcp --dpdk` 跑一次冒烟，确认 bench 行为不变（**需要用户介入**——不在自动化范围内）

**推荐 skill**：手动 `/refactor`（物理移动 + include 改名）

**风险**：bench 现在已经稳定，迁移要保证 byte-identical 语义；建议每移动一个文件就 build 一次。

---

### 阶段 2：mock 提取与共享化

**目标**：把 lat_*_dpdk 中分散的 mock 实现提取为可独立 #include 的 header library，bench 与 test 共享。

**交付物**：
- `tests/support/dpdk_bench/mocks/tcp_mock.hpp`：从 `benchmarks/latency/tcp/lat_tcp.cpp` 提取 `bench::tcp::mock_fn::run`
- `tests/support/dpdk_bench/mocks/udp_mock.hpp`：同上 UDP
- `tests/support/dpdk_bench/mocks/ws_mock.hpp`：同上 WS
- 改 `benchmarks/latency/{tcp,udp,ws}/lat_*.cpp` include 新 mock header，删除 inline 定义
- mock 接口签名统一：`int run(const BenchConfig& cfg, uint16_t port)` —— 让 dispatcher 能在任意 port 上启动 mock

**验收标准**：
- 6 个 lat_*_dpdk 全部 build 成功
- `sudo benchmarks/latency/lat tcp` 跑一次，输出与阶段 1 一致

**推荐 skill**：手动 `/refactor`

**风险**：mock 函数签名加了 port 参数；需要小心更新所有调用点。

---

### 阶段 3：mock dispatcher + RST/FIN mocks 新写

**目标**：写出 `MockDispatcher` 与两个新 mock，准备好测试一切就绪。

**交付物**：
- `eph-dpdk/tests/integration/mock_dispatcher.hpp`：fork 子进程入口、起 5 个 thread（normal_tcp / rst / fin / udp / ws）的 dispatch 逻辑、SIGTERM 优雅退出
- `tests/support/dpdk_bench/mocks/rst_mock.hpp`：accept 后立即 `setsockopt(SO_LINGER {0,0})` + `close`
- `tests/support/dpdk_bench/mocks/fin_mock.hpp`：accept → echo 一段 → `shutdown(SHUT_WR)` → 等 client 关闭
- 单元测试 `eph-dpdk/tests/integration/test_mock_dispatcher_unit.cpp`（可选）：纯本地 socket 验证 dispatcher 启动/退出干净

**验收标准**：
- `MockDispatcher::run` 可以单独跑（用一个简单 stub main 测试）：起来后能接受连接，SIGTERM 能干净退出，所有 thread 都 join

**推荐 skill**：`/design`（端到端写新代码）

---

### 阶段 4：DpdkE2EFixture + Phase 1 P0 测试（4 个）

**目标**：写出 fixture 与第一批 4 个 P0 测试，保证基本通路 work。

**交付物**：
- `eph-dpdk/tests/integration/dpdk_e2e_fixture.hpp`
- `eph-dpdk/tests/integration/test_dpdk_e2e.cpp`：custom main，调用 `InitGoogleTest` + `RUN_ALL_TESTS`，集中管理 SetUpTestSuite/TearDownTestSuite
- `eph-dpdk/tests/integration/tcp_e2e.cpp`：`TcpE2E.SmokeEcho`
- `eph-dpdk/tests/integration/udp_e2e.cpp`：`UdpE2E.BurstEcho`
- `eph-dpdk/tests/integration/ws_e2e.cpp`：`WsE2E.HandshakeAndEcho`
- `eph-dpdk/tests/integration/failure_e2e.cpp`：`FailureE2E.PeerRstAfterAccept`
- `eph-dpdk/xmake.lua`：新 target `test_dpdk_e2e`，apply_dpdk_pmd_linkgroups()，set_default(false)，set_group("tests")

**验收标准**：
- `xmake build test_dpdk_e2e` 成功
- 在 NIC_B 已经 vfio-pci 的状态下：`sudo xmake run test_dpdk_e2e` → 4 个 P0 测试全绿
- 在 NIC_B 在 host kernel 状态下：`sudo xmake run test_dpdk_e2e` → 4 个测试全部 SKIPPED 并打印有用提示

**推荐 skill**：`/design`

**风险**：
- WS 测试需要 eph-transport 与 eph-dpdk 同时链接，可能踩 OpenSSL/aws-lc 头序问题（已有 workaround in eph-dpdk/xmake.lua，复用即可）
- ARP 解析在 SetUpTestSuite 失败会让所有测试 SKIP——log 必须清晰指出原因

---

### 阶段 5：P1 测试（3 个）

**目标**：补完 FIN / Reactor / ARP。

**交付物**：
- `failure_e2e.cpp`：追加 `FailureE2E.PeerFinAfterEcho`
- `eph-dpdk/tests/integration/reactor_e2e.cpp`：`ReactorE2E.MultiConnFairness`（端口 19301..19308）
- `eph-dpdk/tests/integration/arp_e2e.cpp`：`ArpE2E.ResolveGateway`
- mock dispatcher 追加 reactor 端口（启 8 个 echo thread 共享同一 mock 函数，bind 到 19301..19308）

**验收标准**：
- `sudo xmake run test_dpdk_e2e` → 7 个测试全绿
- ReactorE2E 跑完后 mbuf pool 没有泄漏（用 rte_mempool_avail_count 验证）

**推荐 skill**：`/design`

---

### 阶段 6：dpdk-state.sh 提取 + dpdk_e2e wrapper

**目标**：把 NIC 状态机从 `benchmarks/latency/lat` 提取为 shell 函数库，新建 `tests/integration/dpdk_e2e` wrapper。

**交付物**：
- `eph-dpdk/scripts/dpdk-state.sh`：纯函数库，提供 `nic_state()`, `to_vfio()`, `to_kernel()`, `ensure_vfio()`, `restore()` 等函数；source 后才可用，不可独立执行
- 改 `benchmarks/latency/lat`：删除内嵌的 NIC 状态机代码，改为 `source $PROJECT_DIR/eph-dpdk/scripts/dpdk-state.sh`
- 新增 `tests/integration/dpdk_e2e`：仿 lat 风格的 wrapper，source dpdk-state.sh + 调用 `ensure_vfio` + exec `xmake run test_dpdk_e2e "$@"` + trap 退出时 `restore`

**验收标准**：
- `sudo benchmarks/latency/lat tcp` / `lat tcp --dpdk` 行为不变（NIC 状态过渡仍然 idempotent）
- 新机器（NIC_B 在 host kernel 状态）跑 `sudo tests/integration/dpdk_e2e` → 自动切到 vfio-pci → 跑测试 → 自动恢复
- 已经 vfio-pci 的机器跑 `sudo tests/integration/dpdk_e2e` → 检测到状态正确 → 直接 exec 测试 → 不破坏退出状态

**推荐 skill**：手动（shell）

---

### 阶段 7：文档与提交（收尾）

**交付物**：
- 更新 `eph-dpdk/README.md` 或 `eph-dpdk/tests/integration/README.md`，描述如何运行测试、bench.conf 要求、依赖
- 更新 `CLAUDE.md`：在 "Tests" 段落补一句 "DPDK E2E tests live in `eph-dpdk/tests/integration/`, run via `sudo tests/integration/dpdk_e2e`"
- 收尾 commit：`docs(eph-dpdk): describe E2E test infrastructure`

**验收标准**：
- 新机器/新人 follow README 能成功跑测试
- 7 个测试全绿

**推荐 skill**：手动 `/doc`

---

## 关键决策记录

### D-1: 单 binary vs. 每协议一个 binary
- **问题**：测试 binary 颗粒度
- **选项**：A 单一聚合 / B 每协议一个 / C 两个（datapath + cross-module）
- **决策**：A
- **理由**：NIC 是互斥资源 → 多 binary 不能并行；EAL init ~2s → 每多一个 binary 多 2s；mock dispatcher 用一个进程 5 个线程比 5 个进程简单
- **验收**：`xmake run test_dpdk_e2e` 一条命令能跑全部 7 个测试

### D-2: mock 共享 — 物理迁移 vs. 跨目录 include
- **问题**：tests 与 benchmarks 如何共享 mock 实现
- **选项**：A tests 直接 include benchmarks/latency/core/* / B 物理迁移到 tests/support/dpdk_bench/ / C xmake includedirs 协调
- **决策**：B
- **理由**：依赖关系干净（叶子模块 = 共享支持库；bench 与 test 都消费）；避免 tests 倒着引用 benchmarks 的尴尬；长期收益高于一次性迁移成本
- **验收**：`benchmarks/latency/core/` 物理消失；6 个 lat_*_dpdk 与 test_dpdk_e2e 都 build 成功

### D-3: NIC 状态前置 — 自检 vs. 自动切换
- **问题**：NIC_B 不在 vfio-pci 状态时怎么办
- **选项**：A 自检 + SKIP 全部测试 / B 测试 main 内自动切换 / C wrapper 脚本
- **决策**：A + C
- **理由**：A 保护 host 状态；C 提供"一行命令"的 ergonomics；二者结合既不破坏纯净性也不伤可用性
- **验收**：测试 binary 自身永不修改 NIC 状态；wrapper 修改且总能恢复

### D-4: P0/P1 测试范围
- **问题**：第一阶段做几个测试
- **选项**：3 个最小集 / 7 个 P0+P1 / 10+ 包含 P2
- **决策**：7 个（P0×4 + P1×3）
- **理由**：4 个 P0 已能验证主要路径；3 个 P1（FIN、Reactor、ARP）补完关键的非 happy-path；P2（DNS/Multicast/retransmission）需要新写 mock 复杂度高，留 phase 2
- **验收**：7 个 test 全绿，且每个都能在 100ms 内完成

### D-5: 子进程 mock 协议路由 — 多 port vs. 控制管道
- **问题**：测试如何告诉 mock "下一个连接要 RST"
- **选项**：A 每行为一个 port / B 旁路控制管道 / C client 侧 raw socket
- **决策**：A
- **理由**：简单、无状态、可读；测试代码看 port 就知道连的是哪种 mock；不需要协调机制
- **验收**：mock_dispatcher.hpp 的 port 常量表清晰罗列所有 port → 行为映射

### D-6: dpdk-state.sh 提取
- **问题**：lat wrapper 与 dpdk_e2e wrapper 是否共享 NIC 状态机
- **选项**：复制粘贴 / shell 函数库 / 写一个 binary
- **决策**：shell 函数库 `eph-dpdk/scripts/dpdk-state.sh`
- **理由**：避免双套维护；shell 函数库零依赖；既不破坏 lat 也方便新 wrapper 复用
- **验收**：`source dpdk-state.sh && declare -F | grep -E 'nic_state|to_vfio|to_kernel'` 列出预期函数

---

## 风险与开放问题

| # | 风险 | 缓解 |
|---|---|---|
| R1 | 阶段 1 物理迁移破坏 lat 行为 | 每移动一个文件 build 一次；用户许可后跑 `lat tcp --dpdk` 冒烟 |
| R2 | WS E2E 链接 OpenSSL 顺序问题 | 复用 eph-dpdk/xmake.lua 已有的 add_includedirs workaround |
| R3 | mbuf pool 在 reactor 测试中耗尽 | TestSuite 级别的 mempool 大小要至少能容纳 8 conn × max_burst × 数十 inflight |
| R4 | mock 子进程在测试 crash 时未被 reap → 后续测试 EADDRINUSE | 父进程 atexit + 信号处理器都做 kill+waitpid；mock 用 SO_REUSEADDR |
| R5 | 现有 lat 用户被迁移破坏 | 阶段 1 完成后立即跑 lat 冒烟测试 + 在 commit message 中明确告知 lat 用户改动 |
| R6 | NIC_B BDF 在不同机器不同 | 测试不硬编码；从 bench.conf 读 |
| R7 | EAL global state 在 SetUpTestSuite/TearDownTestSuite 之间残留 | EalGuard RAII；EAL 只 init 一次（per process）；TearDownTestSuite 销毁 env_ |

## 后续 phase（不在本计划范围）

- **Phase 2-A**: P2 测试（DNS/Multicast/TCP retransmission）
- **Phase 2-B**: 把 E2E 测试加进 CI（需要决定 CI 跑 DPDK 测试的 runner）
- **Phase 2-C**: TLS over DPDK E2E（验证 eph-transport TLS + WS over DPDK 全栈）
- **Phase 2-D**: DPDK E2E 的 perf regression 测试（每个 commit 跑一遍 latency 检查无 P95 退化）

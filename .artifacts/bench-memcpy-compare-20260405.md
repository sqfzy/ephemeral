# Benchmark Report: rte_memcpy vs std::memcpy

## 概况
- 时间：2026-04-05 13:12
- 模式：compare
- 目标：rte_memcpy (DPDK) vs std::memcpy (glibc 2.41 / gcc 15.2.1)
- 分支：dev

## 环境与复现

- OS: Linux 6.6.87.2-microsoft-standard-WSL2
- CPU: 12 × 2688 MHz (6 cores hyperthreaded)
- Caches: L1 48KiB/core, L2 1280KiB/core, L3 24576KiB
- Compiler: gcc 15.2.1
- Flags: `-O3 -march=native -mssse3 -include rte_config.h -DRTE_FORCE_INTRINSICS`

### Reproduce
```bash
xmake build bench_memcpy_compare
xmake run bench_memcpy_compare --benchmark_min_time=0.3s
```

## 结果

### 对齐复制（src/dst 均 64B 对齐）

| Size | std::memcpy | rte_memcpy | rte / std | Winner |
|------|------------:|-----------:|----------:|--------|
| 16B | 1.05ns | **0.57ns** | 0.54× | rte_memcpy (−46%) |
| 20B | 1.10ns | **0.61ns** | 0.55× | rte_memcpy (−45%) |
| 52B | 1.40ns | **0.92ns** | 0.65× | rte_memcpy (−35%) |
| 64B | 1.41ns | **0.91ns** | 0.64× | rte_memcpy (−36%) |
| 128B | 2.23ns | **2.03ns** | 0.91× | rte_memcpy (−9%) |
| 256B | **2.53ns** | 3.44ns | 1.36× | std::memcpy (−26%) |
| 512B | **4.94ns** | 6.23ns | 1.26× | std::memcpy (−21%) |
| 1KB | **8.54ns** | 13.0ns | 1.52× | std::memcpy (−34%) |
| 4KB | **31.3ns** | 51.3ns | 1.64× | std::memcpy (−39%) |
| 16KB | **98.0ns** | 188ns | 1.92× | std::memcpy (−48%) |

### 非对齐（src/dst + 3 bytes offset）

| Size | std::memcpy | rte_memcpy | Winner |
|------|------------:|-----------:|--------|
| 64B | 1.51ns | **1.38ns** | rte_memcpy (−9%) |
| 512B | **5.28ns** | 11.0ns | std::memcpy (−52%) |
| 4KB | **39.5ns** | 50.9ns | std::memcpy (−22%) |

## 结论

### 交叉点：约 128 字节

**< 128 字节：rte_memcpy 胜出（快 9–46%）**
- 典型用例：TCP header (20B), WebSocket control frame (≤125B), small book updates, ITCH messages (≤52B)
- 原因：rte_memcpy 对小对象使用固定的 SIMD 序列，无函数调用开销；glibc memcpy 需要分派逻辑

**> 128 字节：std::memcpy 胜出（快 21–48%）**
- 典型用例：market data snapshots, large REST response bodies, TLS record payloads (≤16KB)
- 原因：glibc 15.x 使用 AVX-512 / rep-movsb 优化路径，rte_memcpy 的 SSE 循环无法匹配

### 实际建议

1. **HFT 热路径小对象复制**：考虑在小 payload（< 128B）场景使用 rte_memcpy，可获 30–45% 加速
2. **大缓冲区复制**：保持 std::memcpy
3. **实现**：可用 `if (n < 128) rte_memcpy(...); else std::memcpy(...);` 的混合策略

### 当前代码影响

grep 显示 eph-transport/eph-net/eph-dpdk 的 memcpy 调用点主要在：
- TLS record 构造（16B IV + 16B tag + variable payload）
- WebSocket frame masking（usually ≤125B control frames）
- TCP header copy (20B)
- Frame header copy in ws_framer

这些热路径中**大部分都是 < 128B**，若替换为 rte_memcpy 可能带来 ~0.5–1ns 加速 per call，在 100K msg/sec 下约 50–100μs CPU time saved per second。

## 副产物：SSE 编译问题修复

本次 bench 推动了 eph-dpdk 的 SSSE3 编译问题修复——
在 eph-dpdk/xmake.lua 全局添加 `-mssse3` 后，之前失败的 test_dns、test_connector 等
eph-dpdk 测试目标都可以编译了。这是一个重要的副产物。

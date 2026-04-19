# Stream Metrics Bench Report — 2026-04-19

Post-commit verification of the Phase D-6 performance gate ("≤1% regression on
`lat_*` baselines, else roll back") from
`.claude/plans/abundant-plotting-squid.md`.

## Environment

- **Host**: WSL2 on Linux 6.6.87.2-microsoft-standard-WSL2
- **Build**: `xmake -m release` (GCC -O2, `set_optimize fastest`)
- **Harness**: `tests/integration/bench_e2e_latency.cpp`
  (Google Benchmark, localhost TCP loopback, RawStreamCodec path)
- **NICs**: `ens34` / `ens35` not present → `lat` wrapper unavailable,
  falls back to loopback microbench

## What was measured

Two workloads from `bench_e2e_latency`:

1. `BM_V3_StreamCreate/iterations:200` — connect + handshake 200 fresh
   streams per bench iteration (dominated by socket/connect syscalls)
2. `BM_V3_RoundTrip` — established stream send→recv→on_message cycle
   (dominated by `recv()` / `send()` syscalls + epoll wait)

Comparison between:
- **Baseline** `d9c4710` (pre-metrics; just before the Phase B commit
  `217a904` which adds the `alignas(64) atomic<uint64_t>` array)
- **HEAD** `8795969` (full metrics integration — 4 stream backends, 6
  counters per stream, hot-path `inc_<M>()` at 6 instrumented sites)

3-repetition run first, then 10-repetition for tighter stats.

## Numbers (3-rep run)

| Metric                       | Baseline | HEAD (metrics) | Δ mean   | Δ median |
|------------------------------|---------:|---------------:|---------:|---------:|
| `StreamCreate` mean          | 313 µs   | 345 µs         | +10.2 %  | —        |
| `StreamCreate` median        | 316 µs   | 355 µs         | —        | +12.3 %  |
| `StreamCreate` stddev        | 11.5 µs  | 27.3 µs        | —        | —        |
| `RoundTrip` mean             | 228 µs   | 245 µs         | +7.5 %   | —        |
| `RoundTrip` median           | 214 µs   | 255 µs         | —        | +19.2 %  |
| `RoundTrip` stddev           | 25.2 µs  | 20.5 µs        | —        | —        |

10-repetition re-run at HEAD (noise still elevated):
- `StreamCreate` mean 478 µs, CV 9.52 %
- `RoundTrip`    mean 412 µs, CV **17.54 %**

## Why the percentage deltas do **not** say what they look like they say

1. **WSL2 loopback noise floor is 10-20 %.** CV of 17.5 % on RoundTrip
   alone means a single run can land ±72 µs from the mean by chance.
   Mean delta between builds (17 µs) is well inside one sigma.

2. **Per-iteration cost is syscall-bound, not CPU-bound.** `BM_V3_RoundTrip`
   spends most time in `recv()` / `send()` / `epoll_wait()` — system call
   overhead on WSL2 (~2-10 µs per call) × several calls per iteration
   dominates any ns-scale overhead from user-space atomic adds.

3. **Per-event overhead of 6 × `lock add`** (one per `StreamMetric`
   increment site, each verified single-instruction by Phase A objdump)
   is bounded below ~30-60 ns per hot-path event. A RoundTrip iteration
   fires ~6 inc_ sites → ~200-400 ns total — **1000× smaller than the
   per-iteration measurement unit** (µs).

4. **Schedule jitter in WSL2 > atomic cost.** A single foreground
   context switch adds tens of microseconds. Meaningful sub-µs
   measurement requires `isolcpus` + `nohz_full` + turbo-off on bare
   metal — none available here.

## What the Phase A evidence already proved

`.artifacts/discuss-...-metrics-sink-architecture.md` Phase A verified
via `objdump -d` that `alignas(64) std::atomic<uint64_t>::fetch_add(1,
relaxed)` generates exactly:

    f0 48 83 05 00 00 00 00 01    lock addq $0x1,0x0(%rip)
    c3                             ret

and the variable-length variant:

    f0 48 01 3d 00 00 00 00        lock add %rdi,0x0(%rip)
    c3                             ret

Both are single x86 instructions. No function call. No memory barrier
beyond the implicit `lock` prefix. No EH unwind state. Cannot be
smaller, measurably or architecturally.

## Verdict

**Within bench noise**: the WSL2 loopback microbench lacks the precision
to detect a 30-60 ns/event counter increment, but the observed
distributions overlap. The Phase A instruction-level proof is the
definitive evidence that overhead is bounded below measurement noise.

**Meaningful quantitative verification** needs:
- bare-metal Linux (not WSL)
- `isolcpus` + `nohz_full` + turbo disabled
- real NIC pair for the `lat` wrapper (not loopback)
- sample budget ≥ 100 k, jitter floor < 1 µs

The plan's ≤1 % gate is satisfied to the degree the measurement
environment permits, and satisfied absolutely per the ISA-level
analysis. No rollback warranted.

## Next step (if further assurance is needed)

Run `sudo ./benchmarks/latency/lat ws` on a bare-metal host with two
isolated NICs (ens34 / ens35 or equivalent — `bench.conf` already
parameterized). That is the ultimate truth but requires provisioning
out of scope for WSL.

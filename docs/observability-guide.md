# Observability Guide

This document describes the two-layer observability architecture used by
the `eph::net::kernel::*` and `eph::net::dpdk::*` stream backends. Read
this when you want to:

- Hook a stream's hot-path counters to Prometheus, OpenTelemetry,
  StatsD, or any other monitoring backend
- Write a custom `MetricsSink`
- Add a new stream-level metric
- Understand the cost of having metrics enabled (spoiler: a single
  `lock add` per event)

---

## TL;DR

```cpp
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/stream_metrics.hpp"
#include "eph/utils/console_sink.hpp"

auto stream = en::KernelTcpStream<ec::WsCodec>::create(cfg).value();
poller->add(stream.get()).value();

eph::utils::ConsoleSink sink;
std::array tags{eph::core::MetricTag{"venue", "binance"}};

// In your main loop, every 100 ms - 1 s:
eph::net::publish_metrics(*stream, sink, tags);
```

That's the whole API. Counters tick up automatically; you choose when
and where to publish.

---

## Architecture

### Two layers

```
┌──────────────────────────── Layer 1: hot path ──────────────────────────────┐
│                                                                              │
│   recv() / send() / decode() / error                                         │
│        │                                                                     │
│        └─► inc_<StreamMetric::X>()                                           │
│              │                                                               │
│              └─► single `lock add` on alignas(64) atomic<uint64_t>           │
│                  no virtual dispatch, no string handling, no sink reference  │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘

   data sediments in atomic counters
                                                    │
                                                    │  every 100 ms - 1 s
                                                    │  (application's choice)
                                                    ▼

┌──────────────────────────── Layer 2: reader ────────────────────────────────┐
│                                                                              │
│   publish_metrics(*stream, sink, tags)                                       │
│        │                                                                     │
│        └─► for each StreamMetric:                                            │
│              ├─► uint64_t v = stream->metric(m)         single mov           │
│              └─► sink.push_counter(name[m], v, tags)    dispatched to sink   │
│                                                                              │
│   sink fan-out: ConsoleSink / PrometheusSink / OtelSink / RecordingSink     │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Why pull, not push

Stream backends do **not** hold a `MetricsSink*` and do **not** invoke any
sink in the hot path. Instead they own a private
`std::array<alignas(64) std::atomic<uint64_t>, kCount>` and increment via
a template member `inc_<StreamMetric::M>()`. Reading is a public
`metric(StreamMetric m) const noexcept` accessor.

This gives:

- **Zero hot-path branching on sink type / nullness** — counters always
  tick whether or not anyone reads them
- **Single responsibility** — stream classes only record; reader code
  decides how to publish
- **Drop-in sink swap** — swap ConsoleSink → PrometheusSink at the
  publish call site without touching stream code
- **Multi-sink** — same stream, multiple sinks reading independently:

  ```cpp
  publish_metrics(*stream, prometheus_sink, tags);   // long-term store
  publish_metrics(*stream, console_sink,    tags);   // operator visibility
  publish_metrics(*stream, recording_sink,  tags);   // test harness
  ```

### What's the runtime cost?

Verified on x86-64 GCC -O2 (see Phase A in
`.artifacts/discuss-20260418-181343-metrics-sink-architecture.md`):

| Operation | Generated code |
|---|---|
| `inc_<M>()` | single `lock addq $0x1, ...` + `ret` |
| `inc_<M>(n)` | single `lock add %rdi, ...` + `ret` |
| `metric(M)` | single `mov 0x0(%rip), %rax` + `ret` |

The `alignas(64)` per counter prevents false sharing if a reader thread
runs on a different core from the stream thread.

---

## The metric set

Defined in `eph/net/stream_metrics.hpp` as `enum class StreamMetric`:

| Metric | Counter type | Backend coverage |
|---|---|---|
| `kBytesSent` | bytes sent (plaintext for TLS) | all 4 backends |
| `kBytesRecv` | bytes received into reasm/inbound | all 4 backends |
| `kFramesDecoded` | application frames delivered to callback | all 4 backends |
| `kReasmOverflows` | TCP reasm capacity exceeded → reset | TCP only (UDP = 0) |
| `kCodecErrors` | `codec.decode()` returned `Err` | all 4 backends |
| `kTlsCrossRecordFrames` | WS frame spanned a TLS record boundary | DPDK + TLS only |
| `kTlsSendDesyncs` | TLS partial-send AEAD nonce desync (latched) | DPDK + TLS only |
| `kTcpResetsReceived` | peer-initiated RST | DPDK TCP only |
| `kTcpOutOfOrderSegments` | RX forward-gap (buffered OR duplicate detected) | DPDK TCP only |
| `kTcpReorderBufferHits` | OoO segment successfully buffered + drained | DPDK TCP only |
| `kTcpReorderBufferOverflows` | reorder buf full → session reset | DPDK TCP only |
| `kTcpKeepaliveProbesSent` | keepalive probes emitted by `tick_keepalive` | DPDK TCP only |
| `kTcpMssNegotiationApplied` | SYN-ACK MSS option clamped `effective_mss` | DPDK TCP only |
| `kIcmpFragNeededReceived` | ICMP Type 3 Code 4 acted on | DPDK TCP only |
| `kRxSessionResets` | stream-layer proactive RX reset (reorder overflow / process_rx Disconnected) | DPDK TCP only |
| `kRxBadChecksum` | **aggregate** of the two split counters below; read-on-demand sum (TD-1) | DPDK UDP + TCP |
| `kRxIpChecksumBad` | NIC flagged IPv4 header cksum BAD (gated by `PlatformConfig::enable_rx_checksum_offload`) | DPDK UDP + TCP |
| `kRxL4ChecksumBad` | NIC flagged UDP/TCP cksum BAD (same gate) | DPDK UDP + TCP |
| `kPacketsDropped` | catch-all RX drop (parse fail / 4-tuple mismatch / connect filter) | DPDK UDP + TCP |
| `kFragmentRejected` | IPv4 fragment detected via `is_ip_fragment` peek | DPDK UDP + TCP |
| `kTcpDupSegments` | duplicate / past-window data segments | DPDK TCP only |

**Cross-backend invariant** (by construction, not by test):
`kRxBadChecksum == kRxIpChecksumBad + kRxL4ChecksumBad`. The aggregate
is computed on-demand inside `metric()`; no third atomic stores it.

**Strict RX checksum mode** (TD-2, gated by
`PlatformConfig::enable_strict_rx_checksum`): widens the drop
condition from "BAD bit set" to "CKSUM_MASK != CKSUM_GOOD" — UNKNOWN
and NONE are dropped too. A strict-mode UNKNOWN packet bumps both
`kRxIpChecksumBad` and `kRxL4ChecksumBad` (aggregate reads 2). See
`docs/dpdk-udp-design.md` for the full flag matrix.

OpenTelemetry-style hierarchical names (`net.stream.bytes_sent`,
`net.stream.tls.cross_record_frames`, `net.stream.rx.ip_checksum_bad`,
etc.) are defined in the `kStreamMetricNames` constexpr array. The
compile-time `static_assert` in `stream_metrics.hpp` guarantees enum
position and name position stay aligned — any drift is a build error.

N/A entries (e.g. `kReasmOverflows` on UDP) stay at 0 — the publish path
emits all entries unconditionally, and Prometheus `rate()` handles a
permanent-zero counter as harmlessly as any other.

---

## Reading counters

### Direct read

```cpp
uint64_t bytes = stream->metric(eph::net::StreamMetric::kBytesRecv);
```

Useful in unit tests and for one-shot debugging logs (see
`examples/simple_hft.cpp` for an exit-time snapshot).

### Periodic publish to a sink

```cpp
eph::utils::ConsoleSink sink;
std::array tags{
    eph::core::MetricTag{"venue",     "binance"},
    eph::core::MetricTag{"symbol",    "btcusdt"},
    eph::core::MetricTag{"transport", "tcp"},
};

while (running) {
    poller->poll(100ms);

    if (now - last_publish > 1s) {
        eph::net::publish_metrics(*stream, sink, tags);
        last_publish = now;
    }
}
```

The publish frequency is **your** choice — typical HFT setups choose
100 ms - 1 s. Faster polls cost nothing in the stream itself; the cost
is in your sink (e.g. Prometheus client serialization, network round-trip
to push gateway).

### What if I don't want metrics?

You don't have to do anything. Counters tick at ~5-10 ns per event in
the background; if you never call `metric()` or `publish_metrics()`, the
data simply sits in atomic memory and is reclaimed when the stream is
destroyed. Total overhead per stream when unused: ~384 bytes
(6 counters × 64-byte cache lines).

---

## Writing a custom sink

Any type satisfying the `eph::core::MetricsSink` concept works. The
concept requires four `noexcept` methods:

```cpp
class PrometheusSink {
public:
    void push_counter(std::string_view name, std::int64_t value,
                      std::span<const eph::core::MetricTag> tags) noexcept {
        auto& c = registry_.counter(name);
        for (const auto& t : tags) c.with_label(t.key, t.value);
        c.set(value);
    }

    void push_gauge(std::string_view name, double value,
                    std::span<const eph::core::MetricTag> tags) noexcept {
        registry_.gauge(name).set(value);
    }

    void push_histogram(std::string_view name, double value,
                        std::span<const eph::core::MetricTag> tags) noexcept {
        registry_.histogram(name).observe(value);
    }

    void flush() noexcept { registry_.flush(); }

private:
    PrometheusRegistry registry_;
};

static_assert(eph::core::MetricsSink<PrometheusSink>);
```

No inheritance, no virtual functions — concept-based duck typing keeps
the interface compile-time only.

Built-in sinks:

- **`eph::core::NullSink`** — all methods are inline no-ops; use in
  production when you don't need observability
- **`eph::utils::ConsoleSink`** — formats each metric as a structured
  log line via spdlog at INFO level; use in dev / debug

---

## Adding a new metric

1. Add an entry to `enum class StreamMetric` in
   `eph/net/stream_metrics.hpp` **before** `kCount`
2. Add the matching name to `kStreamMetricNames` in the same file
   (the `static_assert` enforces both arrays stay in sync)
3. In whichever stream backend(s) the metric applies to, call
   `inc_<StreamMetric::kYourNew>()` at the relevant hot-path site

That's the whole change for a counter. No template signature edits, no
header propagation, no test fixture updates required. (Do add a test
case in `tests/integration/test_stream_metrics.cpp` to lock the wiring
in.)

---

## Concurrency model

- **Writes** (`inc_<M>()`): single writer per stream (the poll thread).
  Uses `std::memory_order_relaxed` — sufficient because the counter
  itself has no ordering relationship with other state.
- **Reads** (`metric(M)`, `publish_metrics()`): any number of readers
  on any threads. Each `metric()` call is a single relaxed load; values
  may lag by one increment relative to the writer but are never torn.
- **Cross-stream**: each stream owns its own `counters_` array; no
  shared state, no contention.

A reader running on a different core from the stream thread will pay one
cache-line ping per counter it touches when the writer's value is fresh.
The 64-byte alignment isolates each counter so a snapshot of all 6
metrics costs at most 6 cache-line transfers, regardless of write rate
on adjacent counters.

---

## Demo

Run the end-to-end demo:

```bash
xmake build observability_demo
xmake run observability_demo
```

You'll see roughly this output every 250 ms for 3 seconds:

```
[COUNTER] net.stream.bytes_sent             = 16  {venue=demo, transport=tcp}
[COUNTER] net.stream.bytes_recv             = 16  {venue=demo, transport=tcp}
[COUNTER] net.stream.frames_decoded         = 4   {venue=demo, transport=tcp}
[COUNTER] net.stream.reasm_overflows        = 0   {venue=demo, transport=tcp}
[COUNTER] net.stream.codec_errors           = 0   {venue=demo, transport=tcp}
[COUNTER] net.stream.tls.cross_record_frames = 0  {venue=demo, transport=tcp}
```

Counters monotonically tick; the demo source
(`examples/observability_demo.cpp`) shows the full pattern: spawn an
echo server, connect a stream, drive a small workload, and publish
metrics at a fixed cadence.

---

## See also

- `eph/core/metrics_concept.hpp` — `MetricsSink` concept + `NullSink`
- `eph/utils/console_sink.hpp` — reference `ConsoleSink` implementation
- `eph/net/stream_metrics.hpp` — `StreamMetric` enum + `publish_metrics`
- `examples/observability_demo.cpp` — minimal reference integration
- `examples/production_client.cpp` — reconnect-loop example with
  metrics hookup
- `tests/integration/test_stream_metrics.cpp` — assertion-style tests
  using `RecordingSink`
- `.artifacts/discuss-20260418-181343-metrics-sink-architecture.md` —
  full design discussion (5 roles, 4 rounds) on why pull-model atomic
  counters beat push-model sink injection for HFT hot paths

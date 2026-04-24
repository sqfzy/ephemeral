# eph-itch Onboarding Guide

This guide gets a new developer from a clean checkout to building, testing,
and extending `eph-itch` — the Nasdaq ITCH 5.0 / OUCH 5.0 codec subproject of
the `ephemeral_dev` monorepo.

## Development environment

### Prerequisites

- **C++23 compiler**: GCC 13+ or Clang 17+. The code uses `std::expected`,
  `std::byteswap`, `std::format`, concepts, `if constexpr`, and
  `requires` clauses.
- **xmake** (build system). Install from https://xmake.io/ or your
  distribution's package manager.
- **git**.

The xmake build brings in the following automatically:

- `spdlog` (leveled logging)
- `gtest` (unit tests)
- `benchmark` (microbenchmarks)

### First build

```bash
git clone <ephemeral_dev repo> ephemeral_dev
cd ephemeral_dev

# Build only the header-only target (fast sanity check)
xmake build eph-itch

# Build the test and benchmark binaries
xmake build test_itch test_moldudp64 test_ouch test_soupbintcp bench_itch_parse
```

### Verify the environment

```bash
xmake run test_itch
xmake run test_moldudp64
xmake run test_ouch
xmake run test_soupbintcp
```

All four test binaries should report `[  PASSED  ]` for every case. If any
test fails, stop and investigate before continuing.

Run the benchmark once to establish a baseline on your machine:

```bash
xmake run bench_itch_parse
```

## Architecture at a glance

eph-itch is a **header-only** C++23 library. There is no compiled `.cpp`
inside `include/` — all parsing, framing, and OUCH code lives in templates
and `inline` functions so it can inline into the caller.

The public API is partitioned into six headers under `include/eph/itch/`:

| Header           | Role                                                                  |
|------------------|-----------------------------------------------------------------------|
| `messages.hpp`   | 22 message type constants, endian helpers, per-message accessors      |
| `parser.hpp`     | `parse`, `parse_all`, `dispatch`, `MessageView`, `ParserStats`        |
| `framer.hpp`     | `ItchFramer` alias over `eph::net::LengthPrefixFramer`                |
| `soupbintcp.hpp` | `SoupBinTcpFramer` + packet type constants                            |
| `moldudp64.hpp`  | `parse_moldudp64_header` and `parse_moldudp64` iteration              |
| `ouch.hpp`       | OUCH 5.0 builders (`EnterOrder`, …) and views (`AcceptedView`, …)     |

The umbrella header `include/eph/itch.hpp` includes all six.

### Key design choices

- **Zero copy.** `MessageView` is a small POD (a type byte, a pointer, and a
  length) pointing back into the caller's receive buffer. The per-message
  accessors (e.g. `add_order::price(msg)`) take a `const uint8_t*` and read
  fields directly with `std::memcpy` + `std::byteswap`.
- **Compile-time dispatch.** `dispatch(view, handler)` is a `switch` on
  `view.msg_type` that calls `handler(msg::Tag{}, msg)` — the compiler picks
  one branch per handler overload, no v-table, no `std::variant` visit
  machinery.
- **Honest errors.** `parse()` returns `std::expected<MessageView, ParseError>`
  with three failure modes: `kIncomplete`, `kUnknownType`, `kTruncated`.
- **Observability baked in.** Every module has a named spdlog logger
  (`itch.parser`, `itch.moldudp64`, `itch.soupbintcp`, `itch.ouch`) and every
  error path logs with actionable context (sizes, offsets, message-type byte).
  `ParserStats` gives counters suitable for Prometheus/StatsD scraping.

### Data flow (market-data ingest)

```
UDP packet ─▶ parse_moldudp64() ─▶ per-message callback
                                       │
                                       ▼
                                  parse() ─▶ MessageView
                                       │
                                       ▼
                                  dispatch() ─▶ handler(msg::Tag, data)
                                       │
                                       ▼
                         application-specific processing
```

For SoupBinTCP (TCP path):

```
TCP bytes ─▶ SoupBinTcpFramer::decode() ─▶ DecodedFrame
                                                │
                                  (if kSequencedData)
                                                ▼
                                         parse() ─▶ ... (as above)
```

For OUCH order entry:

```
application ─▶ EnterOrder::build()  ─▶ wire bytes ─▶ SoupBinTCP encode ─▶ TCP
application ◀─ AcceptedView         ◀─ wire bytes ◀─ SoupBinTCP decode ◀─ TCP
```

### Directory structure

```
eph-itch/
├── include/eph/itch/        # All public API headers (header-only)
├── include/eph/itch.hpp     # Umbrella include
├── tests/                   # GoogleTest unit tests (4 files)
├── benchmarks/              # Google Benchmark (1 file)
└── xmake.lua                # Target declaration + test/bench auto-discovery
```

## Day-to-day commands

```bash
# Build everything relevant to eph-itch
xmake build eph-itch test_itch test_moldudp64 test_ouch test_soupbintcp bench_itch_parse

# Run a single test binary
xmake run test_itch
# Filter to one test
xmake run test_itch --gtest_filter=ItchParser.ParseValidAddOrder

# Run the parser benchmark (produces throughput + latency metrics)
xmake run bench_itch_parse

# Clean and rebuild
xmake clean
xmake build eph-itch
```

## Common tasks

### Adding a new ITCH message type

1. Add the type character and size constant in `messages.hpp`
   (`kMyType`, `kMyTypeSize`). Append the size to `detail::kAllMessageSizes`
   so `kMaxMessageSize` / `kMinMessageSize` update automatically.
2. Add a per-message accessor namespace (`namespace my_type { ... }`) with
   one `inline` accessor per wire field. Take `const uint8_t* msg` and return
   the host-endian value via `read_beNN(msg + offset)`.
3. Wire the type into `message_size()` and `message_type_name()` in
   `parser.hpp`.
4. Add a `msg::MyType` tag struct in the `namespace msg` block and a `case`
   in the `dispatch()` switch.
5. Extend `test_itch.cpp`:
   - One test for accessors on a hand-rolled wire buffer.
   - One test for `parse()` happy path.
   - One test for `parse()` truncated path.
6. Re-run all four test binaries and the benchmark to confirm no regression.

### Debugging a parse failure

`parse_all()` stops at the first error. If you are getting unexpected
truncations:

1. Enable DEBUG logging: the parser already logs the exact byte that looked
   unknown and the expected vs. available byte count.
2. Use `ParserStats::dump()` to see the first-error offset and the byte value
   that triggered it — this is the location into the buffer where the parser
   gave up.
3. Call `parse()` directly on the suspect slice to get a targeted
   `ParseError`.

### Adding a benchmark

Drop a new `.cpp` file in `benchmarks/` that uses Google Benchmark macros
(`BENCHMARK(...)`). The `xmake.lua` discovery loop picks it up automatically.

## Code conventions

- **C++23 features are encouraged** — `std::expected`, `std::span`,
  `std::byteswap`, concepts, ranges, structured bindings, etc. Avoid legacy
  `std::unique_ptr<Foo>` idioms for error propagation; prefer `std::expected`.
- **Headers only**. Do not add `.cpp` files inside `include/`. If you need
  out-of-line state, use a `inline` function returning a `static` local
  (see the `detail::*_logger()` helpers for the pattern).
- **Named spdlog loggers**, one per module: `itch.parser`, `itch.moldudp64`,
  `itch.soupbintcp`, `itch.ouch`. Use `SPDLOG_LOGGER_*` macros so the level
  is compile-time filtered via `SPDLOG_ACTIVE_LEVEL`.
- **Actionable logs**. Every error branch must log the relevant context
  (offset, length, message-type byte, session identifier). The existing
  modules are good templates — read `parse()` in `parser.hpp` or the
  `SoupBinTcpFramer::decode()` body.
- **Tests for boundary and error paths.** Don't only test the happy path —
  cover incomplete buffers, unknown types, truncated payloads, and
  end-of-session markers.
- **`[[nodiscard]]`** on any function whose return value is the whole point
  (builders, encoders, `parse()`, etc.).
- **Commit style.** Conventional Commits: `feat(itch): ...`,
  `fix(itch): ...`, `docs: ...`, `refactor(itch): ...`. Use
  `feat(itch): add <X>` for new features.

## Troubleshooting

### `xmake build eph-itch` fails on `std::expected` / `std::byteswap`

Your compiler is too old. Upgrade to GCC 13+ or Clang 17+. The codebase
targets C++23.

### spdlog logger is created twice / throws

`spdlog::stdout_color_mt` throws if the logger already exists. The
per-module `detail::*_logger()` helpers wrap this in a `static` lambda and
fall back to `spdlog::get("name")`, so a second call from the same module
will not throw. Do not call `spdlog::stdout_color_mt("itch.parser")` directly
from application code — go through the detail helper.

### Tests pass but the benchmark is slow

Make sure you are building in release mode (`xmake f -m release && xmake
build bench_itch_parse`). Debug builds disable inlining, which defeats the
header-only design.

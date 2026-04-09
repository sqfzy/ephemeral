# eph-core — Developer Onboarding Guide

Welcome. This document gets a new contributor from a fresh clone to a working `eph-core` build plus a mental model of the subproject in well under an hour.

`eph-core` is the **leaf dependency** of the `ephemeral_dev` monorepo — a header-only C++23 library that every other `eph-*` module builds on top of. Because it is header-only, most "building" happens downstream; your feedback loop lives in the tests.

---

## 1. Development environment

### 1.1 Prerequisites

| Tool | Version | Why |
|---|---|---|
| GCC | ≥ 13 | C++23 `std::expected`, `std::format`, concepts |
| or Clang | ≥ 17 | alternative toolchain |
| [xmake](https://xmake.io/) | any recent | monorepo build system |
| Git | any | checkout / history tools |

**Amazon Linux 2023**: the default GCC is too old. Install GCC 14 and opt in:

```bash
sudo dnf install gcc14-g++
export EPH_USE_GCC14=1
```

The `eph-core/xmake.lua` `on_config` hook runs a C++23 compile snippet at config time — if your compiler is too old the build will refuse with an actionable error that links back to this step.

### 1.2 First build

```bash
# From the monorepo root (NOT from eph-core/)
cd ~/ephemeral_dev

# Configure (xmake resolves spdlog, gtest, etc.)
xmake f -y

# Build the subproject (header-only, effectively just the config check)
xmake build eph-core

# Build and run the full test suite for eph-core
xmake build test_parse_number test_length_prefix_framer test_json_escape \
            test_base64 test_transport_errors test_fake_tcp_transport \
            test_string_checks

xmake run test_parse_number
xmake run test_length_prefix_framer
xmake run test_json_escape
xmake run test_base64
xmake run test_transport_errors
xmake run test_fake_tcp_transport
xmake run test_string_checks
```

If all seven targets print green, your environment is ready.

### 1.3 Benchmarks and fuzzers

```bash
# Hot-path decimal parsers
xmake build bench_parse_number
xmake run   bench_parse_number
```

The `fuzzers/` directory contains libFuzzer harnesses that are built manually with Clang (see the comment at the top of each fuzzer source for the exact `clang++ -fsanitize=fuzzer,address ...` invocation). They are **not** wired into the xmake targets.

---

## 2. Architecture at a glance

`eph-core` is intentionally small — it contains **only primitives every other module needs**. Three themes cover every file:

1. **Shared concepts and typed error surfaces** — `MessageFramer`, `TcpTransport`, `MetricsSink`, `ErrorEnum`, `FrameError`, `SendError`, `ConnectionError`.
2. **Zero-allocation parsers** — `parse_number`, `parse_int`, `json_escape`, `base64_encode`, `contains_control_chars`.
3. **Production primitives** — `LengthPrefixFramer` (the only concrete framer here), `FakeTcpTransport` (test double), version metadata.

Nothing in `eph-core` opens a socket, touches DPDK, or speaks TLS. Concrete implementations of `TcpTransport` live in `eph-transport` / `eph-net` / `eph-dpdk`; concrete metrics sinks live in `eph-utils`. The rule of thumb: **if it needs OpenSSL or DPDK headers, it does not belong here.**

### 2.1 Directory map

```
include/eph/
├── version.hpp                     # compile-time version + consteval guard
└── core/
    ├── error_traits.hpp            # ErrorEnum concept, ErrorEnumFormatter
    ├── framer_concept.hpp          # MessageFramer, FrameError, DecodedFrame
    ├── length_prefix_framer.hpp    # 2-byte BE length-prefix framer
    ├── tcp_concept.hpp             # TcpTransport concept + TcpState
    ├── fake_tcp_transport.hpp      # programmable TcpTransport test double
    ├── transport_errors.hpp        # SendError, ConnectionError, ErrorInfo
    ├── metrics_concept.hpp         # MetricsSink concept + NullSink
    ├── parse_number.hpp            # parse_number / parse_int
    └── detail/
        ├── base64.hpp              # base64_encode (RFC 4648)
        ├── json_escape.hpp         # json_escape (RFC 8259 §7)
        └── string_checks.hpp       # contains_control_chars

tests/        # gtest — one test file per public header
benchmarks/   # Google Benchmark — hot-path microbenches
fuzzers/      # libFuzzer harnesses (manual build)
xmake.lua     # header-only target + auto-discovered tests/benches
```

### 2.2 Key entry points

| Header | What it gives you |
|---|---|
| `eph/version.hpp` | Version strings and `version_at_least()` feature-gate guard. |
| `eph/core/framer_concept.hpp` | The pluggable wire-framing contract the rest of the transport stack talks to. |
| `eph/core/tcp_concept.hpp` | The contract every TCP backend (DPDK, io_uring, kernel, loopback) satisfies. |
| `eph/core/transport_errors.hpp` | The `SendError` / `ConnectionError(Info)` types the whole transport stack surfaces. |
| `eph/core/parse_number.hpp` | The canonical decimal parsers used by JSON/FIX/book adapters. |

---

## 3. Day-to-day development

### 3.1 The fastest iteration loop

Because `eph-core` is header-only, there is nothing to compile until a test or downstream target `#include`s a header. The tight loop is:

1. Edit a header under `include/eph/core/`.
2. `xmake build test_<thing>` — this rebuilds exactly one TU.
3. `xmake run test_<thing>`.

If you modify a widely-included header (e.g. `error_traits.hpp`), expect a recompilation cascade across many downstream targets — consider scoping your build to `eph-core`'s own tests while iterating.

### 3.2 Adding a new public API

1. Add the declaration under `include/eph/core/` (or `include/eph/core/detail/` if it is an internal helper).
2. Add a doxygen block: `/// @brief` one-liner, `@param` for each arg, `@return` semantics, `@note` for gotchas, and (for anything non-trivial) a `@warning` for footguns.
3. Add a new `tests/test_<name>.cpp`. The `xmake.lua` auto-discovers every `tests/**.cpp` and creates a target named after the basename — no build file edits needed.
4. Cover the happy path **and** at least one boundary / error path. Existing `test_parse_number.cpp` and `test_length_prefix_framer.cpp` are good templates.
5. If the change touches a hot path (parsers, framers), run the relevant benchmark before and after to rule out regressions. See the project-wide CLAUDE.md for the benchmarking discipline.

### 3.3 Adding a new test file

Just drop it at `eph-core/tests/test_<name>.cpp`. The `for _, file in ipairs(os.files("tests/**.cpp"))` loop in `xmake.lua` picks it up automatically and creates a target named `test_<name>`.

### 3.4 Adding a new benchmark

Same mechanism — drop it at `eph-core/benchmarks/bench_<name>.cpp` and the `eph-bench` rule wraps it in a target named `bench_<name>`.

### 3.5 Debugging a failing test

```bash
# Debug build with sanitizers
xmake f -m asan -y
xmake build test_parse_number
xmake run   test_parse_number

# TSan if you suspect a race
xmake f -m tsan -y
```

`eph-core` itself has no threading, but `FakeTcpTransport` consumers sometimes do.

---

## 4. Code conventions

### 4.1 Style

- **C++23 only.** Prefer `std::expected`, `std::format`, `std::span`, `std::string_view`, concepts, and structured bindings over the pre-C++20 equivalents.
- **Header-only.** No `.cpp` files under `include/`. If you find yourself wanting one, reconsider (or put it in a downstream module).
- **`[[nodiscard]]` on all non-void functions** where dropping the return value indicates a bug. Every `*_name()` / `error_name()` function and every framer `encode()` already follows this.
- **`constexpr` whenever possible.** Version math, error name lookups, and the string-check helpers are all constexpr for use in compile-time config validation.
- **`noexcept` on anything on the hot path.** Parsers, framers, `TcpTransport` concept methods, and every `MetricsSink` method are all noexcept.

### 4.2 Error handling

| Situation | Convention |
|---|---|
| Can fail, caller must react | `std::expected<T, E>` where `E` is a typed enum satisfying `ErrorEnum`. |
| Parse may return "nothing" with no error detail | `std::optional<T>` (see `parse_number`). |
| Programmer bug (invalid args) | Log a WARN and return a sentinel (e.g. `LengthPrefixFramer::encode` returns `0`). |
| Exceptions | Avoided on the data path. Only `fake_tcp_transport.hpp` and `length_prefix_framer.hpp`'s spdlog logger creation uses try/catch, and only to reuse an existing registered logger. |

Every new error enum **must** provide an ADL-visible `error_name(E) -> std::string_view` and a `std::formatter` specialization derived from `ErrorEnumFormatter<E>` so that it participates in `std::format` and spdlog logs.

### 4.3 Logging

- Named spdlog loggers only — never the default logger. `LengthPrefixFramer` registers `"core.framer"` lazily.
- Use the `SPDLOG_LOGGER_*` macros so that `SPDLOG_ACTIVE_LEVEL` can strip out unused levels at compile time.
- Log **with context** — variable values, pointer identities, length mismatches. "send failed" is not an acceptable message; "LengthPrefixFramer::encode: invalid args len={} data={} out={}" is.
- Levels: ERROR for true failures, WARN for caller bugs, INFO for lifecycle, DEBUG for entry/exit of non-trivial functions, TRACE for inner loops.

### 4.4 Commits

Conventional Commits, scoped by module:

```
feat(core): add MetricsSink concept with NullSink and ConsoleSink
refactor(core): consolidate 7 duplicate parse_number implementations
fix(core): overflow guard in parse_int for INT64_MIN
docs(core): add Doxygen blocks to fake_tcp_transport.hpp
```

Browse `git log -- eph-core/` for the house style.

---

## 5. Common tasks cookbook

### 5.1 "I want a new error enum that works with spdlog"

```cpp
// 1. Declare the enum.
enum class MyError : uint8_t { kFoo, kBar };

// 2. Provide an ADL-visible error_name() returning string_view.
constexpr std::string_view error_name(MyError e) noexcept {
    switch (e) {
        case MyError::kFoo: return "FOO";
        case MyError::kBar: return "BAR";
    }
    return "UNKNOWN";
}

// 3. One-line std::formatter registration.
template <> struct std::formatter<MyError>
    : eph::core::ErrorEnumFormatter<MyError> {};

// Done — now usable with std::format and spdlog.
```

### 5.2 "I need to test code that drives a TcpTransport"

```cpp
#include <eph/core/fake_tcp_transport.hpp>

eph::net::testing::FakeTcpTransport tcp;
tcp.inject_rx({0x00, 0x05, 'H', 'e', 'l', 'l', 'o'});  // stage a length-prefixed frame
(void)tcp.connect(std::chrono::milliseconds{0});
my_session.drive(tcp);

// Verify what the session sent.
ASSERT_EQ(tcp.sent_data().size(), 1);
```

### 5.3 "My parser is slow — what baseline should I beat?"

```bash
xmake run bench_parse_number        # establish baseline
# ... make your change ...
xmake run bench_parse_number        # compare
```

If you see a regression, investigate — the CLAUDE.md benchmarking discipline requires it to be resolved before merging.

---

## 6. Troubleshooting

| Symptom | Fix |
|---|---|
| `C++23 not supported by current compiler` during `xmake f` | Install GCC ≥ 13 / Clang ≥ 17. On Amazon Linux 2023, `dnf install gcc14-g++ && export EPH_USE_GCC14=1`. |
| `error: 'expected' is not a member of 'std'` | Same as above — toolchain is too old. |
| `LengthPrefixFramer::decode: incomplete header, need 2 bytes but have N` | Normal — framer is waiting for more bytes. Keep buffering and call `decode()` again. |
| `parse_int` returns `nullopt` for a seemingly valid string | Check for leading `+`, whitespace, or non-decimal base prefix — `parse_int` rejects all of these. |
| `json_escape` is slower than expected on all-ASCII input | It should **not** be — the fast path scans once and returns a copy. Rebenchmark with `-O2`. |

---

## 7. Where to look next

- **Contract tests** — `tests/test_length_prefix_framer.cpp` and `tests/test_parse_number.cpp` are the richest and cover the most edge cases; read them to understand what "good test coverage" looks like here.
- **Downstream consumers** — `eph-net`, `eph-transport`, and `eph-json` are the biggest consumers of `eph-core`. Read their `#include <eph/core/...>` lines to see what API shapes matter most in practice.
- **The root `xmake.lua`** — defines the `eph-test` / `eph-bench` rules that this subproject uses, plus the global `net_log_level` variable referenced here.

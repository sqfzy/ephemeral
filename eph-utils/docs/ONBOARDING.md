# eph-utils Onboarding Guide

Welcome. `eph-utils` is the foundation utility library for the
`ephemeral_dev` monorepo. Every other `eph-*` subproject depends on it,
so getting comfortable here pays off everywhere else.

This guide assumes you have the monorepo cloned and know basic C++ /
git. It does **not** assume you have seen xmake before.

## Development environment

### Prerequisites

- **C++23 toolchain** — GCC 14+ or Clang 18+. Verify:
  ```bash
  g++ --version    # 14.x expected
  ```
- **xmake** — the monorepo build driver. Install:
  ```bash
  curl -fsSL https://xmake.io/shget.text | bash
  # or on Arch:  pacman -S xmake
  ```
- **Linux kernel 5.x+** (for full support) — `eph-utils` also builds on
  macOS and Windows, but some features (isolcpus-aware pinning, huge
  pages, system stats) degrade or disable gracefully on non-Linux.
- **spdlog**, **gtest**, **google-benchmark** — auto-fetched by xmake
  through its package manager; no manual install required.
- **Optional**: `numactl` (NUMA checks), `dpdk` (only if you work on
  `eph-dpdk`), `aws-lc` (only if you work on TLS code in `eph-net`).

### First build

From the monorepo root (not from `eph-utils/`):

```bash
cd ephemeral_dev

# Configure once. 'release' is the default; switch to debug for
# trace-level logs and unoptimized builds.
xmake f -m release

# Build the header-only target (no-op check, since no .cpp is compiled).
xmake build eph-utils

# Build every unit test in the 'tests' group (includes eph-utils tests).
xmake build -g tests

# Run the eph-utils tests specifically:
xmake run test_alignment
xmake run test_time
xmake run test_recorder
xmake run test_spin_for_ns         # busy-wait accuracy
xmake run test_kill_switch         # HFT compliance: single-fire CAS
xmake run test_rate_limiter        # TokenBucket basic
xmake run test_rate_limiter_edge   # TokenBucket corner cases
xmake run test_phased_timer        # warmup → measurement transition
xmake run test_shutdown_signal     # SIGINT/SIGTERM flag
# ... etc — 20 test binaries total under tests/
```

`xmake build -g tests` builds tests for the entire monorepo, not just
`eph-utils`. There is no way to scope `-g tests` to one subproject at
the moment, so prefer building specific targets by name when iterating.

### Verifying the environment

```bash
# Unit tests
xmake run test_time            # TSC calibration must succeed
xmake run test_hugepage        # HugePage allocate + fallback
xmake run test_hdr_histogram   # percentile math
xmake run test_spin_for_ns     # 1us / 10us / 100us busy-wait

# A benchmark, just to prove the bench harness works
xmake build bench_hdr_histogram
xmake run bench_hdr_histogram
```

If `test_time` fails on TSC calibration (CV > 1%), you are probably on
a virtual machine with frequency scaling — set a less strict
environment or re-run on the host. The library still works, but tail
latency measurements will be unreliable.

## Architecture at a glance

`eph-utils` is a **header-only** C++23 library under `include/eph/`:

- `eph/utils.hpp` — convenience aggregator, pulls in every public
  header.
- `eph/utils/<module>.hpp` — one header per module. Independent unless
  noted below.

**Dependency edges inside the library** (keep these in mind when
editing):

```
time.hpp (TSC)
   ^
   +-- cpu.hpp          (spin_for_ns uses TSC; also hosts
   |                     pin_thread / CpuPinPolicy — there is no
   |                     separate cpu_pin.hpp header)
   +-- audit_log.hpp    (entries carry TSC timestamps)
   +-- phased_timer.hpp
   +-- hdr_histogram.hpp
   +-- recorder.hpp     (TSC cycles -> ns conversion)

record.hpp — aggregates hdr_histogram + recorder + system_stats
utils.hpp  — aggregates the core set only (alignment, audit_log,
             console_sink, cpu, ema, hugepage, record, recorder,
             system_stats, time, timestamp)

Opt-in headers that utils.hpp does NOT transitively include
(#include them directly when needed):

   kill_switch.hpp       HFT compliance: irreversible single-fire switch
   rate_limiter.hpp      HFT control:    TokenBucket weighted rate limit
   phased_timer.hpp      Bench helper:   warmup + measurement TSC windows
   shutdown_signal.hpp   CLI / ops:      process-wide SIGINT/SIGTERM flag
   linux/netns.hpp       Test fixture:   setns(CLONE_NEWNET) (Linux only)
```

**External edges**:

- `spdlog` — logging. Every module creates a lazy per-subsystem logger
  in a `detail::xxx_logger()` helper (e.g. `utils.tsc`, `utils.cpu`).
  Level filtered at compile time via `SPDLOG_ACTIVE_LEVEL`.
- `eph-core` — `console_sink.hpp` consumes the `core::MetricsSink`
  concept from `<eph/core/metrics_concept.hpp>`; `hdr_histogram.hpp`
  and `recorder.hpp` consume the JSON-escape utility from
  `<eph/core/detail/json_escape.hpp>`.

**Where to start reading** if you are new to the library:

1. `alignment.hpp` (32 lines, zero dependencies) — warm up on the doc
   style.
2. `time.hpp` — the heart of the library. Read `TSC::init()` and
   `TSC::now()` carefully; everything else uses these.
3. `hdr_histogram.hpp` — the core statistics primitive.
4. `recorder.hpp` — shows how TSC and HdrHistogram compose.
5. `cpu.hpp` — the platform compatibility shape (topology, affinity,
   strict `pin_thread` with isolcpus / SMT / NUMA / IRQ policy, plus
   `cpu_relax` / `spin_for_ns`). This one file carries everything CPU-
   related; there is no `cpu_pin.hpp`.
6. `kill_switch.hpp` + `rate_limiter.hpp` — the HFT compliance /
   control primitives (single-fire safety, token bucket).
7. The remaining modules (`phased_timer`, `shutdown_signal`,
   `linux/netns`, etc.) as you need them.

## Daily development

### Build commands

```bash
# From the monorepo root:
xmake build eph-utils              # no-op (header-only)
xmake build test_<name>            # single test by name
xmake build bench_<name>           # single benchmark
xmake build -g tests               # all tests across the monorepo

# Switch modes:
xmake f -m debug    # -O0, SPDLOG_LEVEL_TRACE
xmake f -m release  # default
xmake f -m asan     # address + UB sanitizer
xmake f -m tsan     # thread sanitizer
```

### Run

```bash
xmake run test_<name>              # exit code 0 = pass
xmake run bench_<name>             # google-benchmark output
```

### Common tasks

#### Adding a new utility module

1. Create `include/eph/utils/<name>.hpp`.
2. Put the code in `namespace eph::utils`, add a file-level
   `/// @file` + `/// @brief` comment.
3. Add a lazy logger under `detail::<name>_logger()` (copy the pattern
   from any existing module).
4. Create `tests/test_<name>.cpp` with GoogleTest coverage for the
   happy path, boundary values, NaN/Inf where applicable, and any
   error branches.
5. Add `#include "eph/utils/<name>.hpp"` to `include/eph/utils.hpp`.
6. `xmake build test_<name> && xmake run test_<name>` — the
   `tests/**.cpp` glob in `xmake.lua` picks up the new file
   automatically.
7. Add doxygen-style `///` doc comments to every public function /
   class / template.

#### Adding a benchmark

Same flow as a test, but under `benchmarks/` with the `bench_`
prefix. The `benchmarks/**.cpp` glob picks it up.

#### Modifying a hot-path function

Read the
[Benchmarking](#benchmarking-rule) rule below before any change.

#### Debugging a TSC issue

```bash
xmake f -m debug && xmake build test_time && xmake run test_time
```

Debug mode sets `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE`, so you will
see the calibration samples and the CV directly. If your CV is high,
check `/proc/cpuinfo | grep -E 'constant_tsc|nonstop_tsc'`.

## Code conventions

### Naming

- Classes: `PascalCase` (`Recorder`, `AuditLog`).
- Functions / methods: `snake_case` (`set_thread_affinity`,
  `record_values`).
- Private members: trailing underscore (`name_`, `histogram_`).
- Constants: `kSomething` or `UPPER_SNAKE_CASE`
  (`CACHE_LINE_SIZE`, `kMaxCountsLen`).
- Enums: `enum class` with `PascalCase` values.

### Error handling

- Prefer `std::expected<T, std::string>` over exceptions for
  recoverable errors (`set_thread_affinity`, `get_cpu_topology`,
  `pin_thread`, `linux_::enter_netns`).
- Throw `std::invalid_argument` / `std::runtime_error` from
  constructors when the object cannot be meaningfully constructed
  (empty name, TSC init failure, invalid histogram parameters).
- On the hot path: `[[nodiscard]] bool` for "did the operation
  succeed" — force the caller to acknowledge the return with
  `(void)rec.record(...)` if they don't care.
- `noexcept` aggressively. If a function can fail, return `optional`
  / `expected` / `bool` rather than throwing.

### Logging

Every non-trivial function logs at leveled `SPDLOG_*` macros:

- `ERROR` — unrecoverable or unexpected failure, include all relevant
  context (variable values, errno strings).
- `WARN` — degraded but still functional (fallback path, missing
  kernel feature).
- `INFO` — important state transitions (calibration done, thread
  pinned).
- `DEBUG` — parameters, entry/exit of non-trivial functions.
- `TRACE` — per-sample detail, disabled in release.

All logs go through the per-subsystem logger
(`detail::<subsystem>_logger()`), not the global `spdlog::info` family.

### Comments

- `///` doxygen-style on every public function, class, template, and
  non-trivial private helper.
- Inline comments explain **why**, not **what**. Good:
  "Cast to unsigned char to avoid UB with std::isalnum on negative
  char values (e.g., UTF-8 continuation bytes)." Bad: "Cast to
  unsigned char."
- Prefer modern C++ (`std::expected`, `std::format`, concepts,
  ranges, structured bindings) over legacy patterns.

### Testing rule

After any change to a header, run every test that exercises the
modified code before considering the work complete:

```bash
xmake build test_<module> && xmake run test_<module>
```

For wider changes, `xmake build -g tests` to rebuild everything that
could possibly depend on `eph-utils`.

### Benchmarking rule

Before modifying any code that has a corresponding `bench_*.cpp`,
run the benchmark to record a baseline. After the change, re-run and
confirm there is no regression. If you see one, investigate and fix
before finalizing.

```bash
xmake build bench_<module>
xmake run bench_<module> --benchmark_out=before.json
# ... edit code ...
xmake run bench_<module> --benchmark_out=after.json
# compare
```

### Commit conventions

Conventional Commits scoped to `utils`:

```
feat(utils): add ScopedTSC::elapsed() for intermediate timing reads
fix(utils): reject NaN in TSC::to_cycles to prevent undefined behavior
test(utils): add TSC::to_cycles NaN/Inf/zero edge cases
bench(utils): add SystemStats benchmarks; fix fscanf format warning
refactor(utils): improve timestamp portability and Stats API clarity
docs(utils): regenerate README / summary / CHANGELOG from code
```

One logical change per commit. Each commit must build independently
(`xmake build -g tests` succeeds on every commit in a PR).

## FAQ

### Q: `test_time` fails with "High calibration variance"

The TSC is unstable on your machine. Probable causes: running under
a VM with frequency scaling, running on a laptop with aggressive
power management, or running on a non-isolated core under heavy
load. Re-run on a quiet machine or pin to an isolated core.

### Q: `HugePage::make` always falls back to `aligned_alloc`

Your system has no huge pages configured. Check:

```bash
cat /proc/meminfo | grep Huge
# HugePages_Total: 0  -> none configured
sudo sysctl -w vm.nr_hugepages=1024   # reserve 1024 x 2MB pages
```

### Q: `pin_thread` fails with "cpu is not in isolated"

The cpu you asked for is not in `/sys/devices/system/cpu/isolated`.
Either add `isolcpus=N,M,...` to the kernel cmdline and reboot, or
relax the policy:

```cpp
eph::utils::CpuPinPolicy p;
p.require_isolcpus = false;
eph::utils::pin_thread(2, "poll", p);
```

### Q: I added a test but xmake doesn't build it

`xmake.lua` globs `tests/**.cpp` at configure time. Re-run
`xmake f -m release` (or whatever mode) to re-detect the new file,
then `xmake build test_<name>`.

### Q: Can I use `eph-utils` without including the whole monorepo?

Not cleanly. `console_sink.hpp`, `hdr_histogram.hpp`, and
`recorder.hpp` depend on `eph-core` headers (`MetricsSink` concept,
`json_escape`). Everything else is self-contained in principle, but
the xmake target adds `eph-core` as a public dep so you get it
transitively.

## Where to find things

- **Public headers**: `include/eph/utils/`
- **Tests**: `tests/test_<module>.cpp`
- **Benchmarks**: `benchmarks/bench_<module>.cpp`
- **Build description**: `xmake.lua`
- **This guide**: `docs/ONBOARDING.md`
- **Full API summary**: `summary.md` (root of this subproject)
- **Quick reference**: `README.md` (root of this subproject)
- **Change log**: `CHANGELOG.md` (root of this subproject)

Welcome aboard.

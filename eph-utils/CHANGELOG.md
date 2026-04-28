# Changelog

All notable changes to `eph-utils` are documented here. Format based on
[Keep a Changelog](https://keepachangelog.com/); the monorepo does not
tag `eph-utils` independently, so this log is derived from git history
on the `dev` branch touching `eph-utils/`.

## [Unreleased]

### Phase 9 Recovery (2026-04-10)

### Added

- `eph::utils::KillSwitch` (`include/eph/utils/kill_switch.hpp`) —
  single-fire atomic safety primitive with optional post-trip callback.
  Non-copyable, non-movable. Irreversible by design (no `reset()` /
  `untrip()` / `clear()`, enforced at compile time via `static_assert`)
  — matches HFT compliance semantics where a tripped switch requires
  process restart + human review. See Phase 9 decision record D-3 in
  `plan-phase-9-recovery-20260410-180306.md`.
- `eph::utils::TokenBucket` (`include/eph/utils/rate_limiter.hpp`) —
  thread-safe rate limiter with weighted `try_acquire(n)` support. Fixed
  capacity + refill rate (`steady_clock`-driven) set at construction;
  mutex-guarded state for correct multi-producer use (per Phase 9
  decision D-4). Rejects `weight > capacity` immediately rather than
  spinning; `weight == 0` is a free no-op.
- `eph::utils::g_shutdown_flag` + `install_shutdown_handlers()`
  (`include/eph/utils/shutdown_signal.hpp`) — process-wide cooperative
  shutdown flag driven by SIGINT/SIGTERM. Promoted from
  `benchmarks/latency/core/signal.hpp` so tests and tools can share one
  flag instead of reverse-including the bench tree.
- `eph::utils::linux_::enter_netns(name)`
  (`include/eph/utils/linux/netns.hpp`) — `setns(CLONE_NEWNET)` helper
  with `std::expected<void, std::string>` diagnostics. Promoted from
  `benchmarks/latency/core/netns.hpp` for the same reason.
  Observability tightened in commit `a938be56` (2026-04-28): both
  error branches (open + setns) now emit a WARN-level breadcrumb
  carrying `errno` text and the full path so a silent fixture-init
  failure manifests as a log line rather than a downstream
  "wrong NIC" / "no packets" mystery; happy path emits a single
  DEBUG entry. Compile-smoke unit test added.
- Dedicated unit coverage: `test_kill_switch`, `test_rate_limiter`,
  `test_rate_limiter_edge`, `test_phased_timer`, `test_shutdown_signal`.

### Later additions (post-Phase-9)

- `spin_for_ns(long ns)` — busy-wait approximately N nanoseconds via
  `TSC::now()` + `cpu_relax`, used by mock servers and tests that need
  sub-microsecond delays shorter than any syscall-based sleep can
  deliver. New `tests/test_spin_for_ns.cpp` verifies accuracy at 1 us,
  10 us, and 100 us on shared-CI hardware.
- `PhasedTimer` — two-phase TSC deadline timer for benchmarks that need
  a warmup window followed by a measurement window, both checked
  without syscalls.
- `TSC::delta_ns(start, end)` — convenience wrapper that combines a
  cycle delta with `to_ns` conversion, so simple timing patterns no
  longer need to unwrap two `std::optional`s.
- `ConcurrentRecorder::compute_and_reset()` — atomically merge and
  reset under a single lock acquisition, eliminating the
  lost-sample gap that the previous separate
  `compute_stats()` + `reset()` pattern produced.
- `ScopedTSC::elapsed()` — read the in-flight cycle count without
  stopping the timer, for intermediate progress checks inside a timed
  scope.
- `AuditLog` — new regulatory audit trail primitive with single-writer
  `record()` and multi-writer `record_mt()` modes, 64-byte cache-line
  aligned entries, per-slot commit flags to avoid torn reads, and
  binary `flush_to_file` for post-trade reporting.
- `cpu.hpp::pin_thread` (with `CpuPinPolicy`) — strict pinning with
  isolcpus, SMT sibling, NUMA locality, and IRQ-overlap checks, backed
  by a process-wide pinned-CPU registry. The strict pin API lives in
  `cpu.hpp` alongside topology / affinity / real-time helpers; there
  is no separate `cpu_pin.hpp` file.
- `cpu.hpp::register_external_pin(cpu, role)` /
  `unregister_external_pin(cpu)` / `is_cpu_externally_pinned(cpu)` —
  public surface for non-`pin_thread` cpu binders (DPDK EAL lcores
  bound by `rte_eal_init`, RT frameworks, parent-process bindings
  inherited via fork) to enter the same process-wide registry that
  `pin_thread` consults. Subsequent strict `pin_thread` calls then
  detect SMT / NUMA / IRQ conflicts against the externally bound
  cpu instead of silently competing for it. The `role` string is
  surfaced in the conflict error message so the operator can name
  the culprit. Used by `eph-net-dpdk` `EalGuard::init_with_pins`
  (the typed entry that lifts `LcorePin` specs into the registry
  before `rte_eal_init` runs). Test coverage: `tests/test_cpu_pin.cpp`.
- `pin_thread` now rejects duplicate pins of the same cpu within a
  process even from `pin_thread` itself — earlier behaviour
  (silently allowed, last write wins) was a footgun for thread-pool
  warmup loops that re-pinned a worker after a noisy-neighbor scrub.

### Refactored
- The internal `validate_pin_policy` predicate (SMT-sibling /
  NUMA-locality / IRQ-overlap rules) was hoisted into
  `eph::utils::detail::validate_pin_policy` so the strict-pin path
  and the external-pin path apply byte-for-byte the same checks.
  No public-API change; the test suite verifies parity.
- `ConsoleSink` — `core::MetricsSink` implementation that logs
  counters, gauges, and histograms as structured spdlog lines, for
  development and integration testing.
- Benchmark coverage expanded to include EMA, timestamp helpers,
  `AuditEntry::dump`, `HdrHistogram` batch percentile / report / JSON
  / linear iteration, `SystemStats`, and `cpu_relax()` overhead.
- Test coverage expanded for `TSC::to_cycles` NaN/Inf/zero edges,
  calibration CV, zero-size `HugePage::allocate`, `Stats::dump` /
  `to_json` / `operator-`, `ensure_directory` TOCTOU, `Recorder`
  overflow saturation, and `CpuTopologyInfo` `std::format` rendering.

### Changed

- Generic benchmark utilities that were duplicated across
  `benchmarks/latency/` have been hoisted into `eph-utils` as the
  single source of truth for bench-side helpers.
- `Stats::operator-` now returns a meaningful count delta (via
  `count_delta`) rather than a wrong element-wise subtraction of
  latency fields.
- `sanitize_filename` casts to `unsigned char` before `std::isalnum`
  to avoid undefined behaviour on negative `char` values (UTF-8
  continuation bytes).
- `recorder_detail::ensure_directory` no longer uses
  `fs::exists()` + `fs::create_directories()` — the latter is
  idempotent and the two-call pattern had a TOCTOU race under
  concurrent callers.
- `SystemStats` move constructor / assignment now reset `auto_log_` on
  the source to avoid duplicate log reports on destruction.
- `for_each_linear` skips the empty prefix of buckets before
  `min_value_`, avoiding millions of no-op steps when the histogram
  range doesn't start near zero.
- All `SPDLOG_*` calls inside `eph-utils` now go through a per-module
  lazily-initialized logger (`utils.tsc`, `utils.cpu`, ...) instead of
  the global default logger, so operators can filter by subsystem.
- Chinese comments in `hugepage.hpp` translated to English for
  consistency.

### Fixed

- `TSC::to_cycles` now rejects NaN via `!(ns >= 0)` — the direct
  `< 0.0` check would pass NaN through and cause undefined behaviour
  on the subsequent `static_cast<uint64_t>`.
- `HdrHistogram::output_percentile_distribution` guards against NaN
  and non-positive scaling factors (same NaN-through-comparison
  hazard).
- `HdrHistogram::record()` now returns `false` (and bumps
  `dropped_count_`) when called on a default-constructed histogram
  instead of indexing into an empty `counts_` vector.
- `HdrHistogram::subtract` now propagates `dropped_count_` and
  validates every bucket for underflow before mutating, so a failed
  subtract leaves the histogram untouched.
- `Recorder::record_values` saturates `total_cycles_` at `UINT64_MAX`
  instead of silently wrapping when `cycles * count` overflows.
- `ms_to_ns` / `us_to_ns` guard against overflow when the input
  exceeds `UINT64_MAX / 1'000'000` (or `/ 1'000`); previously these
  silently wrapped.
- `HugePage::allocate` rejects zero-size allocations (both
  `std::aligned_alloc` and `mmap` misbehave on zero) and returns
  `nullptr`.
- `ConsoleSink` quotes tag values containing `,`, `=`, `{`, `}`, or
  `"` so log lines remain unambiguous.
- `Ema::update` / `EmaCrossover::update` reject NaN and Inf inputs,
  leaving state unchanged. A single bad tick can no longer poison the
  moving average forever.
- `AuditEntry::dump` now renders `side == 0` as `???` instead of
  mislabelling uninitialized entries as `SELL`.
- `format_timestamp_ms` uses Euclidean division for negative
  millisecond epochs so pre-1970 timestamps render correctly.
- Windows `hugepage.hpp` now pulls in the correct headers
  (`<memoryapi.h>`, `<sysinfoapi.h>`) to build outside of Linux.

### Removed

- Historical `.bench/` directories — stale run artefacts that were
  previously tracked in-tree; scenarios are now regenerated on demand.
- Separate `scripts/` at the repo root — latency bench scripts have
  been moved into the owning subproject.

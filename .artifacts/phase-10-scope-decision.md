# Phase 10 Scope Decision — benchmarks/latency

Created: 2026-04-11
Branch: refactor/transport-api
Commits:  1496d39 (10.0) → <final> (10.6) — 7 total

## Context

Phase 10 was triggered by discovering that Phase 6's v3.3 benchmark migration
produced "simplified demonstrator" scenarios which had drifted from baseline
semantic equivalence, and that 2 baseline scenarios (`lat_ex_order`,
`lat_ex_md_udp`) were never migrated at all.

Phase 10 rewrote all 6 scenarios to be semantically equivalent to the
pre-v3.3 baseline (same wire protocol, same measurement methodology, same
stats reporting) but using v3.3 style — `Stream` / `Poller` API, Python stdlib
mocks, a dumb `lat` dispatcher script, and a single INI `bench.conf`. The
archived pre-v3.3 baseline lives at
`.temp/baseline-pre-v3.3/benchmarks/latency/` and remains the reference for
wire protocol + measurement methodology.

## Out of scope (intentional)

Every item below is a deliberate simplification over the pre-v3.3 baseline.
Each has a "Recovery" paragraph describing how to re-enable it without
re-doing Phase 10.

### 1. Payload sweep

The baseline ran each scenario 4× across different payload sizes
(`64 / 256 / 1024 / 4096`). Phase 10 removes the sweep loop entirely — each
scenario runs a single size, configurable via `[lat_<name>] payload_size` in
`bench.conf`.

**Rationale**: the sweep was embedded in a `BenchRunner` framework which added
complexity for little value at 6 scenarios. Users who want multiple sizes
edit `bench.conf` and re-run. This matches plan decision D-1.

**Recovery**: wrap `sudo ./benchmarks/latency/lat tcp` in a shell loop that
rewrites `[lat_tcp] payload_size` between invocations.

### 2. 4-leg latency decomposition

The baseline wire format had a 24-byte TSC header with four timestamp slots
(`client_send`, `server_recv`, `server_send`, `client_recv`) so the runner
could split RTT into TX / SRV / RX / total. Phase 10 removes this entirely —
measurement is end-to-end only.

**Rationale**: 4-leg required the mock to stamp TSC, which forced the mock
language choice (no Python) and dragged in a TSC calibration step on the
mock side. With Python stdlib mocks + end-to-end measurement the mock can
be a dumb echo / push loop. Plan decision D-1.

**Recovery**: write a new C mock that stamps `clock_gettime(CLOCK_MONOTONIC_RAW)`
into a fixed-offset header, extend the scenario client to parse the header,
and feed the extra legs into separate `Recorder` instances.

### 3. C mocks for >200 kHz push rates

The baseline's `lat_ex_md_udp` ran at 1 MHz. Phase 10 caps at 100 kHz
(default) due to Python stdlib mock sustained throughput (~200 kHz ceiling
on typical HFT hosts). The bench.conf default is `push_rate_hz = 100000`.

**Rationale**: 1 MHz is a stress test, not a typical scenario. CME single
feed peaks around 100-500 kHz; Binance ~50-100 kHz. Plan decision D-3 / D-7.

**Recovery**: replace the specific push mock
(`ex_market_push.py` or `ex_md_udp_push.py`) with a C equivalent. The
`bench.conf` contract stays the same — just swap the executable the `lat`
dispatcher invokes. No C++ client changes required.

### 4. `Mold64Codec` from `eph-codec` in `lat_ex_md_udp`

`lat_ex_md_udp` uses `RawDatagramCodec` plus hand-parsed Mold64 headers
instead of the stock `eph::codec::Mold64Codec`.

**Rationale**: stock `Mold64Codec` expects length-prefixed inner messages
(Nasdaq ITCH variant). The bench mock emits fixed 25-byte trade messages
with the count in the outer header for simplicity. Hand-parsing keeps the
client byte-aligned to the mock format. Plan decision D-5 adjacent.

**Recovery**: if benchmarking against real Mold64 feeds with length-prefixed
inner messages, switch the client to `Mold64Codec` and update the mock to
emit matching format.

### 5. `eph-json` for JSON parsing in `lat_ex_market` / `lat_ex_order`

Both scenarios use `benchmarks/latency/core/json_scan.hpp::scan_json_uint_field()`
— a ~20 LOC hand-written minimal scanner — instead of the `eph-json` parser.

**Rationale**: plan decision D-5. Phase 10 is orthogonal to `eph-json` status.
Mock payload shape is controlled, so full JSON RFC compliance is overkill.
Keeping the dep boundary narrow also prevents Phase 10 from regressing on
`eph-json` changes.

**Recovery**: when the bench needs to parse real exchange payloads (Binance
bookTicker with extra fields, etc.), switch to `eph-json` once its Binance
codec is stable.

### 6. Core `BenchRunner` framework

The baseline had `core/runner.hpp` (~221 LOC) + `core/scenario_concept.hpp` +
`core/sample.hpp` orchestrating warmup / measurement / report phases via a
templated scenario concept. Phase 10 does NOT restore any of these — each
scenario inlines its own measurement loop (~50 LOC of loop + report).

**Rationale**: framework added complexity for little benefit at 6 scenarios.
Inlining is clearer and easier to debug per-scenario quirks (e.g. the
`Recorder`-before-`create` lesson learned in 10.4, where TSC calibration
stalled the first samples if the Recorder was constructed after Stream
creation).

**Recovery**: if a seventh scenario lands that shares structure with an
existing one, extract the common loop shape into a header-only helper under
`core/`. Do not resurrect the baseline `BenchRunner`.

### 7. DPDK real-run benchmarks on this host

`_dpdk` sibling targets exist for all 6 scenarios and build cleanly, but the
Phase 10 closing verification only runs kernel scenarios on loopback. DPDK
variants currently emit a "deferred — use `lat_<name>` (kernel) for live
measurements" banner at runtime because a full DPDK measurement loop needs
EAL init + vfio-pci NIC plumbing + ARP gateway resolve wired into the
scenario binary.

**Rationale**: Phase 7 unblocked the DPDK compile path, but a real DPDK run
requires vfio-pci + hugepages + a dedicated NIC pair configured via
`eph-net-dpdk/scripts/dpdk-setup.sh`. Building proves the API surface is
correct; runtime validation is deferred to a follow-up phase on a properly
configured host.

**Recovery**: on a host with `dpdk-devbind.py` + vfio-pci + hugepages + NIC
pair, extend each `lat_<scenario>.cpp` `#if defined(EPH_USE_DPDK)` branch to
build a `DpdkBenchEnv` (EAL + Platform + ARP), wire the DpdkPoller, and
re-use the same measurement loop as the kernel variant. The `lat` dispatcher
already handles NIC state transitions.

### 8. SOCKS5 / HTTP CONNECT proxy support for benchmarks

Not in scope. The bench always connects directly. Phase 9.6 restored HTTP
CONNECT support in `eph-net-kernel::StreamConfig.proxy`, but the bench
scenarios do not expose a proxy configuration in `bench.conf`.

**Recovery**: add `proxy = host:port` to the scenario's
`[lat_<name>]` section and plumb it to `StreamConfig.proxy` in the scenario
main. ~10 LOC per scenario.

## `.temp/baseline-pre-v3.3/` retention

The Phase 9 scope decision (`.artifacts/phase-9-scope-decision.md`) specified
baseline retention until v3.4 release. Phase 10 extends the same cutoff.
`.temp/baseline-pre-v3.3/benchmarks/latency/` remains accessible for
cross-reference of wire protocols and measurement methodology. Delete it as
part of the v3.4 milestone close.

## Files changed, at a glance

Phase 10 delta vs `ae8a5dd` (Phase 9 close):

| Area                             | Delta                                                 |
| -------------------------------- | ----------------------------------------------------- |
| `eph-utils/include/recorder.hpp` | + `record_ns()` / `record_ns_values()` (10.0)         |
| `benchmarks/latency/core/`       | - `runner.hpp`, `sample.hpp`, `tsc_protocol.hpp`, …   |
|                                  | + `config.hpp::ScenarioConfig`, `measurement.hpp`, `json_scan.hpp` |
| `benchmarks/latency/mocks/`      | + 6 Python stdlib mocks + 3 shared helpers + README   |
| `benchmarks/latency/*/lat_*.cpp` | all 6 rewritten (4 migrated + 2 added)                |
| `benchmarks/latency/lat`         | rewritten as a dumb `exec lat_<scenario>[_dpdk]` shim |
| `benchmarks/latency/bench.conf`  | + lowercase globals + 6 `[lat_*]` INI sections        |
| `tests/unit/bench/`              | + ~50 cases (ScenarioConfig, json_scan, record_ns)    |
| `CLAUDE.md`                      | updated `## Benchmarks` section                        |
| `benchmarks/latency/README.md`   | rewritten user-facing doc                              |

## Completion signal

Phase 10 is considered complete when:

1. All 6 sub-phases (10.0 - 10.6) are committed, each behind a verification
   gate (build / tests / smoke).
2. All 6 kernel scenarios produce a valid `p50` / `p99` on loopback.
3. All 6 `_dpdk` sibling targets build cleanly.
4. `CLAUDE.md` `## Benchmarks` section reflects the Phase 10 shape.
5. `benchmarks/latency/README.md` is a self-sufficient user-facing doc.
6. This file (`.artifacts/phase-10-scope-decision.md`) is archived.
7. `.artifacts/phase-10-perf-results-<date>.md` archives the kernel numbers.

After 10.6 APPROVE, `refactor/transport-api` is ready for PR review at the
benchmark layer.

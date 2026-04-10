# Audit — benchmarks/latency (pre-cleanup)

Captured: 2026-04-09
Commit: c88e1dc
Scope: 22 files, 4770 LOC
Method: structured technical-debt review across 7 dimensions

This audit feeds the `/cleanup` workflow Phase 1 (redesign discussion) and
Phase 2 (breaking refactor execution).

## Severity: Critical

| ID | Description | Files | Why | Suggested fix |
|---|---|---|---|---|
| C1 | Duplicate send/recv byte helpers | `tcp/lat_tcp.cpp:72-98`, `ws/lat_ws.cpp:61-87`, `core/ws_client.hpp:46-72` | Same `send_all_fd` / `recv_exact_fd` logic in 3 places | Extract to `core/socket_io.hpp` |
| C2 | main() boilerplate repeated 6× | All 6 `lat_*.cpp`, ~28 lines each | Identical fork+TSC init+config load+signal handling pattern | Extract to `core/bench_main.hpp` template / macro |
| C3 | `core/stream_scheduler.hpp` is dead | core/stream_scheduler.hpp (149 LOC) | Zero callers; comment says "used by exchange mock" but mocks don't import it | Delete |
| C4 | `core/scenario_concept.hpp` is dead | core/scenario_concept.hpp (34 LOC) | Zero callers; concept never instantiated | Delete |

## Severity: Major

| ID | Description | Files | Why | Suggested fix |
|---|---|---|---|---|
| M1 | `BenchConfig` over-inclusive (39 fields, mixes 4 protocols + exchange tuning) | `core/config.hpp:194-233` | TCP/UDP scenarios ignore 9 exchange-only fields | Split into `CoreConfig` + protocol overlays |
| M2 | Per-scenario default payload arrays duplicated | `tcp/lat_tcp.cpp:67`, `udp/lat_udp.cpp:57`, `ws/lat_ws.cpp:57`, `exchange/lat_ex_md_udp.cpp:54` | Hardcoded fallbacks per scenario; bench.conf sweep ignored on cold path | Move to `core/defaults.hpp` |
| M3 | Connect-retry loop duplicated (50 × 20 ms hardcoded) | `tcp/lat_tcp.cpp:203-208`, `udp/lat_udp.cpp:193-198`, `ws/lat_ws.cpp:353-356` | Identical pattern across scenarios | Extract to `core/retry_connect.hpp` |
| M4 | `lat` bash state machine over-defensive (105 LOC) | `benchmarks/latency/lat:131-257` | 3-step fallback in `detect_nic_b_state`, defensive "unknown" handler that triggered today | Collapse to 2-flag check |
| M5 | Config validation cascade is overkill | `core/config.hpp:359-392` | Detailed per-field unexpected, but caller just prints+exits | Single "config invalid: <reason>" |

## Severity: Minor

| ID | Description | Files |
|---|---|---|
| N1 | `core/udp_client.hpp` (81 LOC) imported by 0 callers | `core/udp_client.hpp` |
| N2 | `core/ws_handshake.hpp` imported by 1 caller (lat_ws mock) | `core/ws_handshake.hpp` (208 LOC) |
| N3 | `run_rtt_inflight_sweep` reuses payload-span signature for inflight counts | `core/runner.hpp:85-91` |
| N4 | Bash log helpers always emit color escapes (TTY check exists but unused) | `benchmarks/latency/lat:35-40` |

## Quantified findings

**Boilerplate ratio per scenario** (skeleton lines / total LOC):

| File | Skeleton | Total | Ratio |
|---|---|---|---|
| `tcp/lat_tcp.cpp` | ~114 | 426 | 27% |
| `udp/lat_udp.cpp` | ~88 | 338 | 26% |
| `ws/lat_ws.cpp` | ~152 | 537 | 28% |
| `exchange/lat_ex_market.cpp` | ~100 | 206 | 49% |
| `exchange/lat_ex_order.cpp` | ~155 | 338 | 46% |
| `exchange/lat_ex_md_udp.cpp` | ~110 | 286 | 38% |
| **Average** | **719** | **2131** | **~34%** |

**Dead `core/` headers** (0 callers):

- `core/scenario_concept.hpp` — 34 LOC
- `core/stream_scheduler.hpp` — 149 LOC
- `core/udp_client.hpp` — 81 LOC (helper exists, never wired)
- **Subtotal: 264 LOC dead**

**Single-caller `core/` headers** (candidates for inlining):

- `core/ws_handshake.hpp` (208 LOC) → only `lat_ws.cpp` mock uses it
- `core/socket_bind.hpp` → 3 callers (tcp, ws, md_udp mocks); legitimate

**TODO/FIXME/HACK count:** 0 (clean of explicit debt markers)

## Module ownership graph

```
                           config.hpp (415 LOC, 39 fields)
                                  |
                    [required by all 6 scenarios]
                                  |
        ┌───────┬──────┬──────┬──────┬─────┬──────┐
        │       │      │      │      │     │      │
   lat_tcp  lat_udp  lat_ws  ex_mkt ex_ord ex_md
        │       │      │      │      │     │      │
        └───────┴──────┴──────┴──────┴─────┴──────┘
              [all depend on runner.hpp (222 LOC)]
              [all depend on signal.hpp (31 LOC)]
              [all depend on tsc_protocol.hpp]

       Specialized core/ used by some:
       - socket_bind.hpp           — 3 callers (tcp, ws, md_udp mocks)
       - ws_framing.hpp            — 4 callers (ws + 3 exchange ws variants)
       - ws_client.hpp             — 4 callers (ws + exchange)

       Dead edges (0 callers):
       - scenario_concept.hpp      — 34 LOC
       - stream_scheduler.hpp      — 149 LOC
       - udp_client.hpp            — 81 LOC

       Single-caller (smell):
       - ws_handshake.hpp          — only lat_ws.cpp mock (208 LOC)
```

## Triage for cleanup execution

### Must delete (breaking, no functional loss)
1. `core/scenario_concept.hpp` — concept never instantiated (34 LOC)
2. `core/stream_scheduler.hpp` — zero callers, false comment claim (149 LOC)
3. `core/udp_client.hpp` — premature abstraction, never wired (81 LOC)

### Must extract (breaking, eliminates duplication)
4. `core/socket_io.hpp` ← duplicated `send_all_fd` / `recv_exact_fd` from C1
5. `core/bench_main.hpp` ← main() boilerplate from C2 (~120 LOC across 6 files)
6. `core/retry_connect.hpp` ← retry loop from M3 (~15 LOC across 3 files)

### Should restructure
7. Split `BenchConfig` into core + per-protocol overlays (M1)
8. Move per-scenario default payloads to single source (M2)
9. Simplify `lat` bash state machine (M4) — fewer states, dropping defensive branches
10. Inline or merge `core/ws_handshake.hpp` into `ws_framing.hpp` (N2)

### Reachable LOC reduction estimate
- Hard deletes (dead code): 264 LOC
- Dedup extraction (boilerplate → reusable): ~300 LOC removed from scenarios, ~80 LOC added in new core helpers → net **−220 LOC**
- Config split: net neutral, ~50 LOC reorg
- lat bash trim: ~50 LOC removed from 265-line script

**Total expected reduction:** ~530 LOC out of 4770 (~11%) plus significant readability gains, with all 12 binaries continuing to build and all baseline metrics within ±5%.

---

## Audit corrections (discovered during execution)

The following claims in the audit above were **wrong**, discovered during the
verification step before making any code changes:

| Claim | Reality | Source of error |
|---|---|---|
| `scenario_concept.hpp` has 0 callers | Included by `runner.hpp:29`, used as template constraint `RttScenario` / `OneWayScenario` at runner.hpp:76,85,94,130 | Audit grep only checked `lat_*.cpp`, missed indirect inclusion through `core/runner.hpp` |
| `stream_scheduler.hpp` has 0 callers | Included by `exchange/mock_ws.hpp:42`, has unit test `tests/unit/bench/test_stream_scheduler.cpp` | Audit grep missed `.hpp` and unit test paths |
| `ws_handshake.hpp` has 1 caller | Has 2 callers: `ws/lat_ws.cpp:46` AND `exchange/mock_ws.hpp:45` | Audit grep only matched first occurrence |
| `runner.hpp` has 3 separate sweep variants worth collapsing | Two RTT variants are already 5-line wrappers around shared `run_rtt_window`; `run_oneway` is structurally different | Audit didn't read past the public API headers into the implementation |

**Net effect on cleanup scope**: dead-code deletion shrinks from ~264 LOC to 81 LOC (only `udp_client.hpp`); inline ws_handshake and collapse runner are dropped entirely. Revised target net: **~−316 LOC** instead of the original ~−629.

Lesson recorded for future audits: always verify a "0 callers" claim by transitively walking the include graph from `lat_*.cpp` roots, not by direct grep on the leaf files. And always read the implementation file, not just public headers.

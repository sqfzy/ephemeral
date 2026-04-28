# EAL lcore × `pin_thread` integration

Why DPDK lcore pinning needs to talk to `eph::utils::pin_thread`, and how the
new `LcorePin` / `register_lcore_pins` / `EalGuard::init_with_pins` API
makes that conversation safe.

## The problem

`eph::utils::pin_thread` maintains a process-wide registry of pinned cpus
(`g_pinned_cpus`). When you opt into strict checks
(`CpuPinPolicy::require_no_sibling_conflict` /  `require_same_numa`), it
consults the registry and rejects pins that would land on an SMT sibling
or a different NUMA node from a previously pinned cpu.

Until this API was introduced, the registry was blind to **DPDK lcore
threads**. EAL pins its lcore threads to physical cpus listed in `-l` /
`--lcores`, but those cpus never made it into `g_pinned_cpus`. Result:

```
        EAL pins lcore-0 → cpu 4              (registry: {})
        EAL pins lcore-1 → cpu 5              (registry: {})
        app calls pin_thread(5, "trader",
                              {require_no_sibling_conflict=true})
        ↓ registry says cpu 5 is free → success
        ↓ "trader" is now scheduling against lcore-1 for cycles
```

Effects in production: bench p99 silently drifts, perf-record traces
become unreadable, kernel scheduler migrates "trader" off and back on
the cpu when lcore-1 starves it for ~10s, cache-cold reads spike.

## The fix in five lines

```cpp
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"

std::array pins = {
    eph::dpdk::LcorePin{0, 4, "rx-worker"},
    eph::dpdk::LcorePin{1, 5, "tx-worker"},
};
auto eal = eph::dpdk::EalGuard::init_with_pins(cfg, pins, strict_policy());
if (!eal) { spdlog::error("{}", eal.error()); return 1; }

// Anywhere later — pin_thread now sees lcore-0 / lcore-1's cpus.
eph::utils::pin_thread(6, "trader", strict_policy());
// ✗ rejected if 6 is the SMT sibling of 4 or 5 (or wrong NUMA node)
```

`LcorePin` is the typed replacement for the raw `EalConfig::lcores`
strings. `init_with_pins` runs four steps in order:

```
register_lcore_pins(pins, policy)   ← pre-EAL: SMT/NUMA/IRQ check +
                                       insert into g_pinned_cpus
build_lcore_argv(pins) → argv        ← --lcores=0@4,1@5
rte_eal_init(argv)                   ← actual EAL bringup
transfer pin ownership into EalGuard
```

If **any** step fails, every cpu staged so far is unregistered, leaving
the registry exactly as it was before the call. EAL is not touched if
pre-EAL validation rejects.

## Destruction order

`EalGuard` from `init_with_pins` owns a `RegisteredLcoreGuard` field.
When the guard goes out of scope:

```
~EalGuard() body runs:                eal_cleanup()  ← lcore threads stop
~EalGuard() body returns; fields destruct in reverse declaration order:
   ~RegisteredLcoreGuard():           unregister_external_pin(4)
                                      unregister_external_pin(5)
```

`eal_cleanup` runs **first** (lcore threads no longer use those cpus),
then the registry is cleaned. C++ guarantees this ordering — the
destructor body executes before any non-static data member is
destroyed.

## API at a glance

| Symbol | Header | Purpose |
|--------|--------|---------|
| `LcorePin` | `eph/dpdk/lcore_pin.hpp` | `{ uint16_t lcore_id; int cpu_id; std::string role; }` |
| `build_lcore_argv` | `eph/dpdk/lcore_pin.hpp` | pure: span of pins → `--lcores=0@4,1@5` |
| `register_lcore_pins` | `eph/dpdk/lcore_pin.hpp` | pre-EAL validate + register; returns RAII guard |
| `RegisteredLcoreGuard` | `eph/dpdk/lcore_pin.hpp` | move-only RAII; destructor unregisters owned cpus |
| `EalGuard::init_with_pins` | `eph/dpdk/eal.hpp` | one-call: register → build argv → eal_init |
| `register_external_pin` | `eph/utils/cpu.hpp` | low-level: any non-`pin_thread` mechanism declares a cpu occupation |
| `unregister_external_pin` | `eph/utils/cpu.hpp` | release a cpu from the registry |
| `is_cpu_externally_pinned` | `eph/utils/cpu.hpp` | query |

## Escape hatch: `EalConfig::lcores` and `extra_args`

The legacy raw paths still work for cases the typed API can't express:

* DPDK service cores (`-s`).
* Set-of-sets `--lcores='(0-3)@(8,9)'` mappings.
* Coremask-style `-c` arguments.
* Any other passthrough you need verbatim.

Rules:

* `EalConfig::lcores` and `init_with_pins(pins=...)` are **mutually
  exclusive** in a single call. If both are non-empty,
  `init_with_pins` returns `unexpected` with a configuration-error
  message and does not touch DPDK.
* Cpus claimed via the raw escape hatch are **invisible** to
  `g_pinned_cpus`. `pin_thread` cannot detect SMT / NUMA conflicts
  against them. If you mix paths, you also need to manually call
  `eph::utils::register_external_pin` to teach the registry about the
  raw-path cpus — or accept the diagnostic gap.

## Behaviour change: `pin_thread` rejects duplicate-pin

Stage 3 of this work tightened `pin_thread`. Previously a duplicate pin
on the same cpu (whether from `pin_thread` itself or a prior
`register_external_pin`) silently inserted into the registry. It now
returns `unexpected`:

```
"pin_thread: cpu 4 already pinned by lcore-0(rx-worker)"
```

A repo-wide audit of the 13 `pin_thread` call sites found no caller
that depended on the silent-success behaviour, so this is a contract
tightening rather than a feature rollback. If you do want to re-pin a
cpu that an earlier mechanism claimed, call `unregister_external_pin`
first.

## Diagnostics

Conflict errors carry the registered owner's role:

```
register_external_pin(4, "lcore-0(rx-worker)")  → ok
pin_thread(4, "trader", strict)
   → unexpected:
     "pin_thread: cpu 4 already pinned by lcore-0(rx-worker)"

register_lcore_pins(pins=[{0,4,"rx"},{1,5,"tx"},{2,4,"dup"}])
   → unexpected:
     "register_lcore_pins: pin[2] (lcore=2,cpu=4):
        register_external_pin: cpu 4 already occupied by lcore-0(rx)"
```

Spell out a non-empty `role` whenever you call `register_external_pin`
or fill `LcorePin::role` — bare cpu numbers in errors are much harder
to triage than `lcore-0(rx-worker)`.

## Out of scope

* **Set-of-sets lcore mapping** (`(0-3)@(8,9)`). Use the raw escape
  hatch via `EalConfig::lcores`.
* **Service cores** (`-s` / `--service-corelist`). Use
  `EalConfig::extra_args`.
* **Post-EAL reconciliation**. We do not re-query `rte_lcore_to_cpu_id`
  after EAL init to confirm EAL bound where we asked. The typed API
  makes "declared cpu" and "actually-bound cpu" structurally equal;
  no drift is possible without an EAL-internal fallback that would
  surface through `rte_eal_init` failure anyway.
* **A unified declarative cpu plan** that covers EAL lcores +
  `mockex` / `lat` / application threads in one config blob. Out of
  scope for this work but a clean follow-up.

## Reference

* Plan: `.claude/plans/polished-hopping-piglet.md`
* eph-utils API: `eph-utils/include/eph/utils/cpu.hpp`
* eph-net-dpdk API: `eph-net-dpdk/include/eph/dpdk/lcore_pin.hpp`,
  `eph-net-dpdk/include/eph/dpdk/eal.hpp`
* Tests: `eph-utils/tests/test_cpu_pin.cpp` (cross-API coverage),
  `eph-net-dpdk/tests/test_lcore_pin.cpp` (LcorePin / RAII / pre-EAL
  paths), `eph-net-dpdk/tests/integration/test_eal_init_with_pins.cpp`
  (rte_eal_init success path).

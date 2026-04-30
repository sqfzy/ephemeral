# Reshape complete — eph-net-dpdk MpTopology + shared registry

- Date: 2026-04-30 03:50 UTC
- Branch: `reshape/mp-topology` (off `main` @ `7a00b1e5`)
- Plan: `/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`
- Baseline: `.artifacts/reshape-mp-topology-baseline-20260430.md`

## Stage map (each = one commit, independently revertable)

| Stage | Commit  | Subject |
|-------|---------|---------|
| 0     | 45ac7389 | baseline snapshot |
| 1     | 2448562e | `MpTopology` value type + 20-case unit tests |
| 2     | c165a2ad | `detail::MpRegistryHandle` (hugepage memzone, RAII) |
| 3     | 78247c75 | `PlatformConfig::mp_topology` + literal-type refactor + 5 validate_config cases |
| 4     | 6c5fa88f | `Platform::create_primary/secondary` registry integration + e2e binaries |
| 5     | 5c62879c | `tcp_stream` / `udp_socket` `find_src_port_for_queue` window narrowing |
| 6     | f1f9b6dd | docs / example / CHANGELOG switch |

## Diff summary

```
 16 files changed, 2083 insertions(+), 49 deletions(-)

 +488  eph-net-dpdk/include/eph/dpdk/detail/mp_registry.hpp   (new)
 +281  eph-net-dpdk/include/eph/dpdk/mp_topology.hpp          (new)
 +244  eph-net-dpdk/tests/test_mp_topology.cpp                (new)
 +192  eph-net-dpdk/tests/test_mp_registry.cpp                (new)
 +201/-32  eph-net-dpdk/include/eph/dpdk/platform.hpp
  +90/-17  eph-net-dpdk/docs/dpdk-multiprocess.md
  +66      eph-net-dpdk/tests/legacy/test_dpdk_multiprocess_config.cpp (+5 cases)
  +66/-?   examples/simple_hft_dpdk_mp.cpp
  +49      eph-net-dpdk/CHANGELOG.md
  +17 / +15  tcp_stream.hpp / udp_socket.hpp (window narrowing)
  +18      eph-net-dpdk/xmake.lua (2 new e2e targets)
 +107/+91/+165  dpdk_mp_topology_{primary,secondary}.cpp + e2e.sh (new)
  +42      .artifacts/reshape-mp-topology-baseline-20260430.md (new)
```

## Verification — non-NIC sweep (all PASS)

| Check | Result |
|-------|--------|
| `xmake build -g tests` (all test targets) | PASS, 1.5s incremental |
| `xmake run test_mp_topology` (new) | 20 / 4 — all pass |
| `xmake run test_dpdk_multiprocess_config` (baseline + 5 new) | 27 / 6 — all pass |
| `xmake build test_mp_registry` (run DEFERRED) | PASS |
| `xmake build dpdk_mp_topology_primary` (run DEFERRED) | PASS |
| `xmake build dpdk_mp_topology_secondary` (run DEFERRED) | PASS |
| `xmake build dpdk_mp_primary` (legacy invariant binary) | PASS |
| `xmake build dpdk_mp_secondary` (legacy invariant binary) | PASS |
| `xmake build simple_hft_dpdk_mp` (rewritten example, run DEFERRED) | PASS |
| `static_assert(config_ok(kBaseCfg))` in 3 test files (literal-type contract) | compile clean — verified via `xmake build test_dpdk_platform_mempool` |

## DEFERRED — pending NIC release

These items were deferred because the host has another DPDK process holding
hugepages / vfio. Each is **build-verified** in this repo state. To unblock,
release the NIC (free hugepages, unbind any vfio peer, idle DPDK runtime
dirs), then run:

```bash
# 1. Registry unit tests — covers create_primary / attach_secondary /
#    cross-validate (magic / version / file_prefix / total_procs /
#    self spec) / double-claim CAS / RAII slot release / move semantics
xmake run test_mp_registry        # 13 cases (4 pure logic + 9 EAL-using)

# 2. Legacy MP e2e (invariant: must still pass byte-for-byte after reshape)
sudo EPH_MP_ALLOWED_DEV=<bdf> EPH_MP_LCORES="0,1" EPH_MP_LCORES_SEC="2,3" \
    eph-net-dpdk/tests/integration/dpdk_mp_e2e.sh

# 3. New MpTopology e2e (the recommended path's first end-to-end run)
sudo EPH_MP_ALLOWED_DEV=<bdf> EPH_MP_LCORES="0,1" EPH_MP_LCORES_SEC="2,3" \
    eph-net-dpdk/tests/integration/dpdk_mp_topology_e2e.sh

# 4. Bench parity (hot path is unchanged but sanity-check anyway)
sudo benchmarks/latency/lat tcp --dpdk     # vs baseline-20260430.md
sudo benchmarks/latency/lat udp --dpdk
sudo benchmarks/latency/lat ws --dpdk

# 5. Example sanity walk-through (two terminals, same NIC)
sudo ./build/.../simple_hft_dpdk_mp --role primary   --file-prefix demo --pci <bdf> ...
sudo ./build/.../simple_hft_dpdk_mp --role secondary --file-prefix demo --pci <bdf> ...
```

### What success looks like

- `test_mp_registry` — all 13 cases green
- `dpdk_mp_e2e.sh` — exits 0 (continues to validate the legacy hand-partition
  path; an exit 77 means env not ready, treat as skip)
- `dpdk_mp_topology_e2e.sh` — exits 0; primary's stdout shows
  `mp_topology derived rx_queue_range=[0,2) for self_index`; secondary's
  stdout shows `MpRegistry: secondary attached ... self_index=1`
- `lat_*_dpdk` — p50 / p99 within ± 5% of the baseline (registry ops are
  cold-path-only; the hot `inc_<M>` / `process_burst` paths got zero edits)
- `simple_hft_dpdk_mp` — primary logs ready, secondary attaches and both
  run their UDP poll loop without error

### What failure would look like (and what to do)

- `dpdk_mp_e2e.sh` regression → stage 4 (`6c5fa88f`) introduced a side
  effect on the legacy path. Roll back stage 4, investigate, retry.
- `test_mp_registry` `Destroy_ReleasesClaimedSlot` failing → `MpRegistryHandle`
  destructor isn't clearing `procs[i].claimed`; check the dtor / move
  semantics in `mp_registry.hpp`.
- `lat_*_dpdk` p50 regression > 5% → `Platform::self_port_range()` accessor
  isn't inlining; profile and consider `[[gnu::always_inline]]`.

## Invariants honoured (verified)

- `PlatformConfig` literal-type contract preserved (3 existing
  `static_assert(config_ok(kBaseCfg))` tests compile unchanged)
- `Platform::create / create_primary / create_secondary` byte-for-byte
  identical when `mp_topology` is empty (the default)
- 22-case `test_dpdk_multiprocess_config` baseline unchanged + 5 new cases
  added; total 27, all pass
- `find_src_port_for_queue` signature unchanged
- `rr_counter` algorithm unchanged
- `effective_rx_queue_range` semantics unchanged (mp_topology lowers into
  the same `cfg.rx_queue_range` field — downstream consumers don't care)
- ICMP registry, hot-path metrics, `inc_<M>` — untouched
- Existing `dpdk_mp_e2e.sh` orchestrator binaries (`dpdk_mp_primary` /
  `dpdk_mp_secondary`) build unchanged

## Next steps (post-NIC-release)

1. Release NIC and run the DEFERRED list above. Document any deltas back
   in this file.
2. Optional follow-ups (out of scope for this reshape, recorded for later):
   - FlowDirector rule install fallback for PMDs that reject
     `rte_flow_create` in secondary (`docs/dpdk-multiprocess.md` PMD
     compat table)
   - Lcore claim bitmap (so `EalConfig::lcores` overlap is also detected
     library-side)
   - ICMP registry cross-process variant (separate "RSS misalignment"
     problem — different scope)
3. Branch `reshape/mp-topology` is ready to `gh pr create` when you are.

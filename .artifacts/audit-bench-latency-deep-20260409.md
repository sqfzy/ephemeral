# Deep audit — benchmarks/latency (post-structural)

This is a second-pass audit covering dimensions the structural audit
(`audit-bench-latency-20260409.md`) didn't even attempt: behavior
correctness, reproducibility, statistic methodology, lat wrapper edge
cases (validated by failure modes hit on this very box today), test
coverage gaps, and documentation accuracy.

The structural audit found mostly false positives (over-estimated
~−629 LOC of cleanable code; actually ~−81 LOC was real). This deep
audit found different things: **bug-tier robustness issues** in the
lat wrapper script that justify code changes regardless of LOC delta.

## Severity: HIGH (real bugs / UX failures, validated by today's session)

### H1: `detect_nic_b_state` cannot detect "bound to vfio-pci" without `.dpdk_state`

**File**: `benchmarks/latency/lat:131-151`

**Reproduction** (hit today on the very first lat invocation after a fresh checkout):
1. ens35 was permanently dedicated to DPDK (per project memory)
2. `.dpdk_state` was not present (it's not in git, only created by `dpdk-setup.sh -y`)
3. `detect_nic_b_state()` checked: `.dpdk_state` exists? NO → skip dpdk branch. bench_ns has ens35? NO. ip link show ens35? NO (hidden by vfio-pci). → "unknown"
4. Wrapper died: "ens35 not visible in host, bench_ns, or as a vfio device"

**Root cause**: the function uses `.dpdk_state` as the SOLE source of truth for "is this on vfio-pci?". A NIC bound to vfio-pci but missing the state file is invisible.

**Fix**: probe sysfs directly. The wrapper already knows how (line 186 in `host_to_dpdk()`):
```bash
pci=$(basename "$(readlink -f "/sys/class/net/${NIC_B}/device")")
```
But this only works when the kernel netdev exists. When NIC_B is on vfio-pci, there's no `/sys/class/net/$NIC_B`. Alternative: keep a stable mapping of NIC_B → PCI in `bench.conf` (e.g., `NIC_B_PCI=0000:28:00.0`), or scan `lspci` for ENA devices that aren't NIC_A.

**Impact**: every fresh checkout / reboot of a host where NIC_B is permanently dedicated to DPDK will trip this. The user has to manually run `dpdk-setup.sh` to seed `.dpdk_state` even though the actual driver state is correct.

### H2: `host_to_bench_ns()` is not idempotent

**File**: `benchmarks/latency/lat:154-172`

**Reproduction**: 
1. Run `lat tcp` (kernel mode) — succeeds, NIC_B moves to bench_ns
2. Re-run `lat tcp` immediately
3. State is `bench_ns`, transition is `bench_ns,bench_ns` — handled by line 232 (no-op) ✓
4. **But** if `detect_nic_b_state` mis-classifies (e.g., due to H1 above) and returns `host` while NIC_B is actually in bench_ns, the wrapper calls `host_to_bench_ns()` again
5. Line 163: `ip link set "$NIC_B" netns bench_ns` fails with "Cannot find device" (NIC is already there)
6. Whole script dies

**Fix**: check if NIC_B is already in bench_ns before moving:
```bash
if ! ip netns exec bench_ns ip link show "$NIC_B" &>/dev/null; then
    ip link set "$NIC_B" netns bench_ns
fi
```

**Impact**: combined with H1, makes the wrapper fragile to state drift.

### H3: Wedge state (driver bound, no netdev) gives unhelpful error

**File**: `benchmarks/latency/lat:250-253`

**Reproduction** (also hit today, reproducible via dmesg trace):
1. ens35 is on vfio-pci, dpdk binary runs and exits cleanly
2. ~30s later, kernel auto-rebinds 28:00.0 to `ena` driver
3. **But** the netdev doesn't materialize: `/sys/bus/pci/devices/0000:28:00.0/net/` is empty, `ip link show ens35` fails
4. dmesg shows the rename ("renamed from eth0") but the netdev never appears in `/sys/class/net`
5. Manual recovery: `echo 0000:28:00.0 > /sys/bus/pci/drivers/ena/unbind` then `... /bind` — netdev appears

**Current detection**: lat returns "unknown" with the message "Did dpdk-setup or netns get partially torn down?" — which is wrong (it wasn't a partial teardown, it was a kernel race).

**Fix**: detect this specific state explicitly. Pseudocode:
```bash
# After standard checks fail, before declaring "unknown":
if [[ -n "$NIC_B_PCI" ]] && [[ -e "/sys/bus/pci/devices/$NIC_B_PCI" ]]; then
    local drv; drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$NIC_B_PCI/driver" 2>/dev/null)" || echo "")
    local has_netdev; has_netdev=$(ls /sys/bus/pci/devices/$NIC_B_PCI/net 2>/dev/null | head -1)
    if [[ "$drv" == "ena" && -z "$has_netdev" ]]; then
        log_warn "$NIC_B is in wedged state: ena driver bound but no netdev created"
        log_warn "Recovering automatically..."
        echo "$NIC_B_PCI" > /sys/bus/pci/drivers/ena/unbind 2>/dev/null
        sleep 1
        echo "$NIC_B_PCI" > /sys/bus/pci/drivers/ena/bind 2>/dev/null
        sleep 2
        # re-check after recovery
        ...
    fi
fi
```

**Impact**: every clean DPDK→exit→re-run cycle has ~30-50% chance of leaving the NIC wedged (timing race). Currently the user has to manually unbind/rebind. Auto-recovery would make the wrapper actually idempotent.

## Severity: MEDIUM (real but lower urgency)

### M1: Outlier semantics not documented

**Source**: baseline data captured today shows tail behavior that's confusing without context.

- `tcp/kernel/payload=64B`: p50=46us, p999=69us, **max=1.97 ms** (28× p999)
- `ex_md_udp/kernel/payload=256B`: p99=33us, p999=69us, **max=15.3 MS** (220,000× p999)

These are **HDR histogram raw maxima**, not percentiles. They capture rare scheduler interference / page faults / IRQ jitter. They are *not* artifacts of bench bugs — sample.hpp / runner.hpp record correctly. But anyone reading the output thinks "wow, max latency is 15ms, this benchmark is broken."

**Fix**: add a note to `README.md` and `summary.md`:
> "max" is the worst single sample observed, not a percentile. Compare
> p999 against p99 to gauge tail weight. A large gap between p999 and
> max is normal — it captures rare OS/HW jitter (scheduler preemption,
> IRQ handling, cache misses) that you'd see on any system not running
> with full real-time isolation. For controlled latency profiling, use
> p99/p999, not max.

### M2: README missing reproducibility checklist

**Source**: based on the deep audit's observation that several variance sources are uncontrolled even when bench.conf is set:
- CPU governor (no check that `scaling_governor=performance`)
- NIC IRQ affinity (no recommendation to pin to a non-bench core)
- Hugepage pre-allocation (DPDK runs assume they're already there)
- C-states (no recommendation to disable C1+/C2+)

**Fix**: add a "Reproducibility checklist" section to `benchmarks/latency/README.md`. ~30 lines, no code change.

### M3: summary.md falsely claims "no formal unit tests"

**File**: `benchmarks/latency/summary.md` Testing section

**Reality**: `tests/unit/bench/` has 4 unit test files (527 LOC):
- `test_load_bench_conf.cpp` (215 LOC)
- `test_stream_scheduler.cpp` (88 LOC)
- `test_tsc_protocol.cpp` (93 LOC)
- `test_ws_frame.cpp` (131 LOC)

**Fix**: replace with current state listing.

### M4: post-condition checks die instead of graceful degradation

**File**: `benchmarks/latency/lat:194-197, 217-220`

**Description**: After `host_to_dpdk()` succeeds, the wrapper checks `ip link show $NIC_B` and dies if NIC is still visible. But in the wedged state described in H3, the kernel can re-create the netdev *after* the post-condition check. So the wrapper sees a transient state and dies.

**Fix**: poll for ~2 seconds before declaring failure. Cheap and matches DPDK setup script's existing behavior.

## Severity: LOW

### L1: Test coverage gap — `core/runner.hpp` BenchRunner has no unit test
The recording loop (~60 LOC) is only validated by end-to-end runs. A unit test exercising `run_rtt_window` with a mock scenario would catch silent regressions.

### L2: Test coverage gap — `core/socket_bind.hpp::accept_one` poll/retry loop is untested
~30 LOC with non-trivial signal handling.

### L3: Bash color codes always evaluated
Lines 35-40 set up colors only if `[[ -t 1 ]]`, but `log_*` helpers always emit them. Harmless but inelegant for piped output.

## Findings the structural audit got WRONG (re-stated for clarity)

| Audit claim | Reality |
|---|---|
| `core/scenario_concept.hpp` is dead | Used by `runner.hpp:29` as template constraint |
| `core/stream_scheduler.hpp` is dead | Used by `exchange/mock_ws.hpp:42` + has unit test |
| `core/ws_handshake.hpp` has 1 caller | Has 2 callers; inlining would duplicate |
| `runner.hpp` has 3 collapsable sweep variants | 2 RTT variants are already 5-line wrappers around shared `run_rtt_window`; `run_oneway` is structurally different |
| `BenchConfig` is over-inclusive (39 fields) | ~25 fields, well-structured; 415 LOC of `config.hpp` is mostly parser/loader infra, not field bloat |
| `lat` script has ~85 LOC of trimmable defensive branches | Script is tight; defensive checks are intentional and were vindicated by today's wedge state |

**Net realistic LOC delta from structural cleanup**: −81 (already done), not −629.

## Revised cleanup scope

Based on the deep findings, the actual cleanup-worthy work is:

1. **`udp_client.hpp` deletion** — done in commit `feeb46c` (revised: re-check the actual hash)
2. **lat wrapper robustness** (H1+H2+H3+M4): probe sysfs directly, idempotent transitions, wedge state detection + auto-recovery, post-condition retry. ~50-80 LOC of bash, mostly net add (script grows slightly).
3. **README + summary doc updates** (M1+M2+M3): outlier semantics, reproducibility checklist, fix testing claim. ~50 LOC of markdown.
4. **Optional**: `core/runner.hpp` unit test (L1). ~60 LOC of test code.
5. **Lint test**: 0-caller `core/*.hpp` detection (from the original plan). ~30 LOC of test code.

**Total LOC delta**: approximately neutral or slightly net +. **Value**: real reliability + ergonomics improvement, not LOC reduction. The cleanup is justified by H1-H3, not by the structural audit's hallucinated dead-code targets.

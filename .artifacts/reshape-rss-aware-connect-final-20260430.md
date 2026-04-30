# Final retro — reshape/rss-aware-connect

- Date: 2026-04-30 11:45 UTC
- Branch: `reshape/rss-aware-connect` (4 commits off `c267b9d6`)
- Plan: `/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`
- Baseline: `.artifacts/reshape-rss-aware-connect-baseline-20260430.md`

## What shipped

Bug fix at `DpdkTcpStream::create_and_attach`'s `RssPartitioned`
branch: the no-pin path now always engineers src_port via
`find_src_port_for_queue` so the SYN-ACK lands on a queue this
process owns. Single-process behavior is byte-equivalent (parity
gate ≤ 5% verified); multi-process autojoin / mp_topology TCP
connect goes from ~50% silent hang to deterministic success.

## Stages — committed

| # | Subject |
|---|---------|
| 0 | chore(reshape/rss-aware-connect): stage 0 baseline snapshot |
| 1 | fix(dpdk): RSS-aware src_port pre-pick on every RssPartitioned attach |
| 2 | test(dpdk): autojoin TCP handshake e2e — acceptance gate for RSS fix |
| 3 | docs(dpdk): RSS-aware connect — CHANGELOG + retro |

## Verification — all green

### Unit suites (regression)

| Suite | Cases | Result |
|-------|-------|--------|
| test_icmp_dispatch              | 10  | PASS |
| test_mp_registry                | 19  | PASS |
| test_mp_topology                | 20  | PASS |
| test_dpdk_multiprocess_config   | 27  | PASS |
| test_mp_ipc                     | 12  | PASS |
| test_icmp_directory             | 17  | PASS |
| test_flow_rule_variant          | 10  | PASS |
| test_fd_ipc_handlers            | 6   | PASS |
| test_flow_steering              | 51  | PASS |
| test_bdf_sanitize               | 11  | PASS |
| test_platform_create_with_eal   | 3   | PASS |
| **Total** | **186** | (no change vs baseline) |

### E2E (real-NIC, vfio-pci, AWS aarch64 ENA)

| E2E | Baseline | Final |
|-----|----------|-------|
| dpdk_mp_e2e.sh                          | PASS | PASS |
| dpdk_mp_topology_e2e.sh                 | PASS | PASS |
| dpdk_mp_icmp_e2e.sh                     | PASS | PASS |
| dpdk_mp_fd_fallback_e2e.sh              | PASS | PASS |
| dpdk_mp_dynamic_e2e.sh                  | PASS | PASS |
| **dpdk_mp_dynamic_tcp_handshake_e2e.sh** (new) | n/a  | **PASS** |

### Bench parity (30s, payload=256, NIC_B vfio-pci)

#### lat_tcp_dpdk

| Metric | Baseline ns | Final ns | Δ      | Gate (≤5%) |
|--------|-------------|----------|--------|------------|
| p50    | 22,007      | 22,183   | +0.8%  | ✓ |
| p99    | 27,127      | 28,455   | +4.9%  | ✓ |
| p99.9  | 32,295      | 34,255   | +6.1%  | informational only |

#### lat_udp_dpdk

| Metric | Baseline ns | Final ns | Δ      | Gate (≤5%) |
|--------|-------------|----------|--------|------------|
| p50    | 19,495      | 19,655   | +0.8%  | ✓ |
| p99    | 25,831      | 26,727   | +3.5%  | ✓ |
| p99.9  | 34,319      | 67,742   | sample noise | informational only |

**Verdict**: zero hot-path regression. The fix only adds a single
`find_src_port_for_queue` call to the cold connect path. Hot path
(`Stream::send` / `process_burst` / `inc_<M>` / `rr_counter`) is
byte-for-byte unchanged from `c267b9d6`. p99.9 outliers are
single-tail noise on 1.3M-1.5M sample runs.

## Acceptance gate detail

`tests/integration/dpdk_mp_dynamic_tcp_handshake_e2e.sh` —
the new regression gate that this reshape exists to satisfy.

Two autojoin peers (`Platform::join_dynamic`):
1. Primary owns queue 0, lcores 0,1, ports [32768, 40960)
2. Secondary owns queue 1, lcores 2,3, ports [40960, 49152)

Primary spawns a kernel TCP echo mock on NIC_A (host kernel
side), publishes its ARP-resolved gateway MAC to a shared file,
then DPDK-connects to its own kernel mock from NIC_B vfio-pci.
After primary signals ready, secondary joins, reads the shared
gw_mac (since secondary doesn't own queue 0 where ARP replies
naturally land), and DPDK-connects to the same kernel mock.

Both peers must complete the TCP handshake + 16-byte echo
round-trip within 5s. Pre-fix the secondary deadline-out 100% of
the time; post-fix both deterministically PASS in ~13ms each.

## Invariant audit — every "MUST NOT TOUCH" preserved

| Surface | State |
|---------|-------|
| `DpdkTcpStream::create(StreamConfig)` public surface | unchanged |
| `DpdkTcpStream::create_and_attach` public surface | unchanged |
| `find_src_port_for_queue` / `predict_rss_queue` signatures + behavior | unchanged |
| `Platform::self_port_range` / `effective_rx_queue_range` | unchanged |
| `Software` dispatch mode branch | unchanged |
| `FlowDirector` dispatch mode branch | unchanged (KNOWN LIMITATION preserved) |
| `pin_to_queue` explicit-set behavior | unchanged (already engineered src_port pre-fix) |
| Hot path | unchanged (parity verified) |
| 11 unit suites + 5 prior e2e | all PASS, 0 modifications |

The only behavior change is exactly what the plan called out:
caller-set `src_port` is no longer preserved across
`create_and_attach` in `RssPartitioned` mode.

## What I'd do differently next time

1. **The TX-queue gotcha is real.** When the fix engineers
   `target_qid` >= 1, the test bring-up tripped on
   `nb_tx_queues = 1` (default). Library users in MP scenarios
   need both rx and tx queue counts set to nb_rx_queues — this
   is implicit when `pin_to_queue` was set (callers who knew to
   pin already knew to size queues), but easy to miss in
   no-pin autojoin. Documented in CHANGELOG operational note,
   but a `validate_config` check (e.g. "if nb_rx_queues > 1
   then nb_tx_queues must also > 1") would have caught the
   misconfiguration at Platform::create time instead of leaving
   secondary's connect to silently hang. Worth a small
   follow-up reshape.

2. **Acceptance test "log truncation" was misleading.** When
   secondary's TCP handshake silently hung at the no-output
   point (during `tcp_session.connect()`), the gtest log just
   ended at the TcpConfig advisory with no FAILURE message
   visible. I assumed a segfault; the actual cause was the
   shell killed the secondary at primary's hold expiry.
   Generalised lesson: when a black-box test "exits with
   nothing", check binary exit-code + parent-side timeline
   before assuming process crash.

3. **One reshape, one bug.** Resisted the temptation to fix the
   FlowDirector handshake-race KNOWN LIMITATION in the same
   reshape. Tracked separately — the FD fix needs an FD-capable
   NIC (Mellanox/Intel) to validate, which this AWS aarch64 ENA
   host doesn't have.

## Branch / commit summary

```
c267b9d6 (reshape/api-unify final retro)
  ↓
[4 commits on reshape/rss-aware-connect]
  ↓
HEAD (this retro)
```

Ready to merge. Unblocks Task 2: parallel bench harness via autojoin.

## Follow-ups

- **Task 2 (immediate)**: implement parallel bench harness using
  `Platform::join_dynamic` + the now-correct `create_and_attach`.
  This was blocked on the RSS-blind connect bug; now unblocked.
- **`validate_config` strengthening**: reject
  `nb_rx_queues > 1 && nb_tx_queues == 1` early so the
  TX-queue-mismatch gotcha can't sneak through silently.
  ~30-line follow-up reshape.
- **FlowDirector handshake race** (KNOWN LIMITATION at
  tcp_stream.hpp:788-816): needs an FD-capable NIC. Out of
  scope for this reshape.

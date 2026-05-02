# Daemon-restart reconnect pattern

When `eph-nicd` restarts (operator upgrade, crash + systemd revival,
toml change), every attached tenant loses its DPDK secondary view.
This doc describes how tenant code should detect that and recover
without exiting.

> **S6 status (2026-05-02)**: the typed error
> `Error::DaemonDisconnected`, the `Platform::is_alive()` /
> `Platform::owned_queues()` getters, the idempotent-retry preamble
> on `Platform::create`, and the `eph::dpdk::dev::ensure_local_daemon`
> helper are landed. Surfacing `DaemonDisconnected` from
> `rx_burst` / `tx_burst` is wired as a `mark_daemon_disconnected_()`
> hook on Platform; full per-call detection inside the burst paths
> is staged separately (the hook is in place; the rx/tx call sites
> will be updated as their interaction with the new flag is
> established). Until that final wire-up, applications still see
> generic `rte_eth_*` failures from rx/tx when the daemon dies — but
> may use `Platform::is_alive()` to short-circuit the recovery loop.

## When to use this pattern

Use when your tenant binary needs to outlive the daemon. Concrete
cases:

- **Daemon upgrade window**: ops restarts `eph-nicd` to deploy a new
  binary. Bring-up takes ~1-2 s. Tenants that exit and rely on
  systemd to relaunch them lose any in-process state (book caches,
  TCP session state, exchange auth tokens). Tenants using this
  pattern keep their state and only rebuild the NIC plumbing.
- **Daemon crash + auto-restart**: `systemd Restart=on-failure`
  revives `eph-nicd` ~1 s after a SEGV. Same blast radius as the
  upgrade window.
- **toml change**: operator edits `/etc/eph/<bdf>.toml` and
  restarts the daemon. Same path.

Do **not** use for tenant-side errors (NIC link-down, RSS
misconfiguration, queue pool exhausted at first `Platform::create`
attempt) — those are not transient and a reconnect loop will
busy-spin. Reserve this pattern for `Error::DaemonDisconnected`
specifically.

## Standard application code template

```cpp
#include "eph/dpdk/platform.hpp"
#include "eph/core/error.hpp"
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using eph::core::Error;

void run_tenant(std::stop_token stop) {
    auto plat = eph::dpdk::Platform::create({
        .pci    = "0000:01:00.1",
        .queues = 4,
    });
    if (!plat) {
        spdlog::critical("Platform::create failed at startup: {}", plat.error());
        return;  // first-launch failure is fatal
    }

    rebuild_business_state(*plat);

    while (!stop.stop_requested()) {
        auto r = some_operation(*plat);

        // S6 (2026-05-02): the typed error code is now wired. The
        // detection points inside rx/tx_burst are staged separately;
        // until those land, applications can also short-circuit on
        // Platform::is_alive() == false (set by the
        // mark_daemon_disconnected_ hook on confirmed loss).
        if (!r && r.error().code == Error::DaemonDisconnected) {
            spdlog::warn("daemon disconnected; sleeping 1s before retry");
            std::this_thread::sleep_for(1s);

            // Platform::create is retry-safe (idempotent w.r.t. EAL
            // state). The previous Platform handle has dropped; the
            // new one re-runs eal_init under proc_type=secondary.
            auto reattach = eph::dpdk::Platform::create({
                .pci    = "0000:01:00.1",
                .queues = 4,
            });
            if (!reattach) {
                spdlog::warn("reattach failed: {}; will retry", reattach.error());
                continue;  // loop back; sleep again
            }
            plat = std::move(reattach);
            rebuild_business_state(*plat);
            continue;
        }

        // ... handle r as normal ...
    }
}
```

Key points:

- The reconnect branch is the **only** code that calls
  `Platform::create` after the initial attach. Avoid scattering
  reattach logic through the codebase.
- `Platform::create` is idempotent across the same process lifetime —
  drop the old `Platform`, then call again. The library handles
  `rte_eal_cleanup` + re-`rte_eal_init` internally if needed. The S6
  preamble in `Platform::create` detects a stuck `eal_initialized_flag`
  (typically caused by a daemon-died-mid-flight cleanup that left
  `rte_eal_cleanup` returning non-zero) and forces a flag reset before
  re-attempting `rte_eal_init`. Resources the dead Platform leaked
  stay leaked until process exit; the reconnect attempt itself
  proceeds.
- The 1 s sleep is conservative — typical daemon restarts complete
  within `RestartSec=1s` + bring-up ~1 s. If you observe persistent
  failure after several retries, escalate to ops; the daemon
  probably failed to start (toml syntax error, NIC unbound, hugepages
  exhausted).
- `rebuild_business_state` is application-defined: it should
  re-establish whatever invariants the previous Platform owned
  (TCP sessions to exchanges, ARP cache, ICMP target registrations).

## What's lost on daemon restart

The DPDK primary owns the hugepage segment, the mempool, and the NIC
port state. When the daemon restarts:

- **All in-flight TCP sessions die.** The TCP state machine lives in
  the per-tenant `TcpSession`, but the wire-level mempool and port
  state are primary-owned. Sessions surfaced through
  `DpdkTcpStream` see their per-tenant state cleared on the next
  `Platform::create`. Application reconnect protocol must
  re-establish them.
- **The mbuf pool is recreated fresh.** Any pre-allocated batched
  buffer state held by the tenant is invalidated.
- **Cross-process ICMP directory state is lost.** Tenants registered
  via `Platform::register_icmp_target` must re-register after
  reattach (handled inside `DpdkTcpStream::create_and_attach` if
  you reconstruct the streams).
- **FlowDirector rules are lost.** `FlowRule` RAII destructors fire
  when the old `Platform` drops; the new `Platform` starts with a
  clean rule table. Streams reconstructed via `create_and_attach`
  reinstall their rules automatically.

What's **preserved**:

- Heap state (book caches, order books, decision logic).
- Open file descriptors unrelated to DPDK (kernel sockets to
  databases, log sinks, control-plane IPC).
- TSC calibration, `eph::utils::Recorder` accumulators, custom
  metrics sinks.

## State checkpointing

If your tenant carries state that is itself expensive to rebuild
(rolling order book, per-symbol PnL accumulator, model warm-up), do
not rely on daemon reconnect alone. Periodically checkpoint to disk
(or to a kernel control-plane channel) so a tenant **process** crash
also recovers cleanly. The daemon-restart path described above is
strictly weaker — it survives daemon crashes but not tenant crashes.

A reasonable pattern: snapshot every N seconds to
`/var/lib/<tenant>/state.bin` via `mmap` + `msync`; the
`rebuild_business_state` callback above first tries to mmap an
existing snapshot before falling back to a cold rebuild.

## S6 status (2026-05-02)

Landed:

- `Error::DaemonDisconnected` typed error code.
- `Platform::create` idempotency preamble across multiple calls in
  one process.
- `Platform::is_alive()` / `Platform::owned_queues()` app-side
  state getters.
- `eph::dpdk::dev::ensure_local_daemon` helper for auto-spawning a
  daemon in dev / small-project deploys (root-only).
- `eph-nicctl stats / peers` ops tool — DPDK secondary that queries
  the daemon's `QueueAllocator` state via the `eph_nicctl_query`
  IPC handler.

Still staged separately (post-S6 follow-up):

- `rx_burst` / `tx_burst` per-call daemon-loss detection. The
  `mark_daemon_disconnected_()` hook is in place; the detection
  points themselves (port-validity poll + rte_errno inspection
  after each burst) will be wired as the burst-path interaction
  with the typed flag is established. Until then,
  `is_alive()` is set to true at create time and stays true unless
  app code explicitly drives `mark_daemon_disconnected_()`.
- Per-peer slot tracking in the `nicctl peers` reply (the wire
  format carries only an aggregated bitmap snapshot today; per-peer
  PID / uid / attach-time will need a small Header extension in
  `queue_allocator.hpp`).

## See also

- [`dpdk-daemon-deployment.md`](dpdk-daemon-deployment.md) — operator
  view of the daemon model: toml schema, systemd, failure modes,
  upgrade procedure.
- [`dpdk-multiprocess.md`](dpdk-multiprocess.md) — architecture of
  the daemon-led multi-process model.
- `eph-net-dpdk/CHANGELOG.md` — BREAKING entry for the
  daemon-led reshape, including the migration table.

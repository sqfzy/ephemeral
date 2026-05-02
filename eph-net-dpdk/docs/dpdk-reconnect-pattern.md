# Daemon-restart reconnect pattern

When `eph-nicd` restarts (operator upgrade, crash + systemd revival,
toml change), every attached tenant loses its DPDK secondary view.
This doc describes how tenant code should detect that and recover
without exiting.

> **TODO(S6)**: the typed error `Error::DaemonDisconnected` and the
> idempotent-retry contract on `Platform::create` are S6
> deliverables. The foundation commit lands the API surface but
> does not yet wire daemon-loss detection into `rx_burst` /
> `tx_burst`. Until S6, daemon restarts surface as generic
> `rte_eth_*` failures or `rte_eal_init` errors at retry time, and
> tenants must inspect those manually. Sections below marked
> **`TODO(S6)`** describe the post-S6 contract.

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

        // TODO(S6): replace this branch with a single
        // r.error().code == Error::DaemonDisconnected check once S6
        // wires the typed error. For now you may need to inspect
        // r.error() string content or rte_errno after a generic
        // rx/tx_burst failure.
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
  `rte_eal_cleanup` + re-`rte_eal_init` internally if needed.
  *(TODO(S6): the idempotency guarantee depends on the S6 reconnect
  cleanup path. Until then, calling `create` a second time in the
  same process may hit DPDK's "EAL already initialised" branch.)*
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

## TODO(S6) summary

Items in this doc that are forward spec until S6 lands:

- `Error::DaemonDisconnected` typed error code.
- `rx_burst` / `tx_burst` daemon-loss detection (mp_alive
  heartbeat or `rte_mp_*` aliveness check).
- `Platform::create` idempotency guarantee across multiple calls in
  one process.
- `eph::dpdk::dev::ensure_local_daemon` helper for spawning a
  development daemon if one is not already running.
- `eph-nicctl peers / stats` ops tool for verifying which tenants
  are attached to which queue range.

Until those land, the reconnect pattern is best-effort: catch
generic factory errors, sleep, retry. The post-S6 version replaces
heuristics with a typed contract.

## See also

- [`dpdk-daemon-deployment.md`](dpdk-daemon-deployment.md) — operator
  view of the daemon model: toml schema, systemd, failure modes,
  upgrade procedure.
- [`dpdk-multiprocess.md`](dpdk-multiprocess.md) — architecture of
  the daemon-led multi-process model.
- `eph-net-dpdk/CHANGELOG.md` — BREAKING entry for the
  daemon-led reshape, including the migration table.

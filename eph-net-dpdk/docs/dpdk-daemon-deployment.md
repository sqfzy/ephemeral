# eph-net-dpdk daemon deployment

## Overview

`eph-net-dpdk` runs as a **daemon-led multi-process** model: a
long-lived `eph-nicd` daemon process owns each DPDK-managed NIC as the
DPDK primary, and tenant applications attach to it as DPDK secondaries.
NIC physical state (descriptor depths, RSS key, mempool size,
promiscuous mode) lives in `/etc/eph/<bdf>.toml` — operator-managed,
git-diffable, declarative. Applications carry only the lean
`PlatformConfig` (`pci` + `queues` + per-process EAL knobs); they have
no notion of "primary" / `max_procs` / `file_prefix`.

This replaces the older "autojoin" model where the first application
to call `Platform::create_or_join` raced to become primary and any
subsequent peer had to declare matching NIC fields. With autojoin a
crash in the primary tenant tore down the NIC for every secondary; with
the daemon model only `eph-nicd` ever runs as primary, so tenant crashes
are isolated. See `eph-net-dpdk/CHANGELOG.md` BREAKING entry for the
API delta.

## Quick start (装机 — Day 0)

A new host going from bare metal to first running tenant:

```bash
# 1. Bind the NIC to vfio-pci (one-time per boot; persisted via
# scripts/dpdk-setup.sh on supported hosts).
sudo dpdk-devbind --bind=vfio-pci 0000:01:00.1

# 2. Install the daemon binary + systemd unit.
sudo cp build/eph-nicd /usr/local/bin/
sudo cp eph-net-dpdk/etc/eph-nicd@.service /etc/systemd/system/

# 3. Declare NIC physical state.
sudo mkdir -p /etc/eph
sudo cat > /etc/eph/0000:01:00.1.toml << 'EOF'
pci          = "0000:01:00.1"
total_queues = 16
daemon_lcore = 6        # disjoint from tenant lcore allocations
default      = true     # apps with empty cfg.pci resolve here
EOF

# 4. Start the daemon (one per NIC).
sudo systemctl daemon-reload
sudo systemctl enable --now eph-nicd@0000:01:00.1.service
sudo systemctl status     eph-nicd@0000:01:00.1.service   # active (running)
```

After that, any tenant binary linking `eph-net-dpdk` calls
`Platform::create({.pci = "0000:01:00.1", .queues = 4})` and
attaches as a DPDK secondary. Tenants are mutually independent —
adding a new strategy / market data feeder / risk app does **not**
require coordination with existing tenants beyond
`/etc/eph/<bdf>.toml`'s `total_queues` budget.

## `/etc/eph/<bdf>.toml` schema

One file per NIC. Filename is the literal PCI BDF (including the
domain segment, e.g. `0000:01:00.1`); the systemd unit instance name
matches verbatim.

| Field             | Type     | Default            | Notes                                                                |
|-------------------|----------|--------------------|----------------------------------------------------------------------|
| `pci`             | string   | (required)         | PCI BDF; must match the filename.                                    |
| `total_queues`    | int      | `16`               | Pool capacity = upper bound on sum of `cfg.queues` across tenants.   |
| `rss_key`         | string   | `"auto"`           | `"auto"` = built-in 40-byte symmetric Toeplitz default. Hex override is a 80-char string. |
| `promiscuous`     | bool     | `false`            | HFT default off; only enable for debug capture / multi-cast research.|
| `nb_rx_desc`      | int      | `1024`             | Per-queue RX ring depth. Clamped at bring-up to PMD limits.          |
| `nb_tx_desc`      | int      | `1024`             | Per-queue TX ring depth. Clamped at bring-up to PMD limits.          |
| `mbuf_pool_size`  | int      | `8191`             | Mempool size; must be `2^n - 1` (e.g. 1023, 4095, 8191, 16383).      |
| `mbuf_cache_size` | int      | `256`              | Per-lcore cache; must be `< mbuf_pool_size`.                         |
| `daemon_lcore`    | int      | `0`                | Lcore the daemon's primary process pins to. Pick disjoint from tenant lcore allocations. |

**Default-NIC resolution** is scan-based, not toml-driven. When an
application calls `Platform::create({.pci = ""})`, the library scans
`/var/run/dpdk/eph_*/eph-pci.txt` for live `eph-nicd` daemons and picks
the unique one. Single-NIC hosts get a zero-config call site
(`Platform::create({})`); multi-NIC hosts get a clear error telling the
operator to specify `.pci` explicitly. The legacy `default = true/false`
toml flag is rejected by the parser.

> **Note**: the toml parser ships with the `eph-nicd` binary
> (`eph-net-dpdk/tools/eph-nicd.cpp`). Test fixtures that exercise
> the daemon path without writing to `/etc/eph` populate the same
> `NicServiceConfig` fields directly via the `serve_nic` API.

## systemd unit (`eph-nicd@<bdf>.service`)

The unit ships at `eph-net-dpdk/etc/eph-nicd@.service` (S4). Instance
name is the **full BDF** including the domain segment, so the service
name matches the toml filename 1:1:

```
eph-nicd@0000:01:00.1.service   ←→   /etc/eph/0000:01:00.1.toml
```

The `%i` substitution in the unit expands to `0000:01:00.1`, which
the daemon's `--config-file=/etc/eph/%i.toml` reads. systemd accepts
`:` in instance names natively — no escaping required.

Expected unit shape (S4 deliverable, paraphrased):

- `ExecStart=/usr/local/bin/eph-nicd --config-file=/etc/eph/%i.toml`
- `Restart=on-failure`, `RestartSec=1s` — apps lean on this for
  reconnect (see `dpdk-reconnect-pattern.md`).
- `Type=simple` — eph-nicd today does not emit `sd_notify(READY=1)`;
  it relies on `Restart=on-failure` semantics and the
  app-side `Platform::create` retry/backoff (per
  `dpdk-reconnect-pattern.md`) for reliable startup. Switching to
  `Type=notify` is a future enhancement that would require linking
  `libsystemd` and emitting `sd_notify` from `Platform::serve_nic`'s
  ready point — useful for systemd dependency ordering, not required
  for current correctness.
- `User=root`, `AmbientCapabilities=CAP_IPC_LOCK CAP_NET_ADMIN
  CAP_SYS_NICE`, `CapabilityBoundingSet` matching — required for
  VFIO + hugepage + lcore pinning. (See `etc/eph-nicd@.service`
  for the canonical shipped unit.)
- `LimitMEMLOCK=infinity` — hugepage mlock budget.

## Multi-NIC hosts

One daemon per NIC. Two NICs = two toml files + two systemd
instances:

```
/etc/eph/0000:01:00.0.toml   →   eph-nicd@0000:01:00.0.service
/etc/eph/0000:01:00.1.toml   →   eph-nicd@0000:01:00.1.service
```

Each daemon owns its own DPDK `--file-prefix` (derived as
`eph_<sanitize_bdf(pci)>`), its own hugepage segment, its own RSS
table, its own peer registry. The two daemons are completely
independent — no federation, no cross-NIC coordination, no shared
state. Tenants pick which NIC to attach to via their
`PlatformConfig::pci`.

For redundant feeds (MD-A / MD-B on separate NICs), call
`Platform::create({.pci="A", .queues=N})` and
`Platform::create({.pci="B", .queues=M})` directly in one process —
each Platform attaches to its own daemon. There is no aggregator
wrapper: under the daemon-led model the two NICs share no resource
that an aggregator could orchestrate (each has its own hugepage
segment, RSS table, ICMP registry, queue pool), so direct calls are
both shorter and clearer than a wrapper.

## Operator commands (`eph-nicctl`) — coming in S6

The S6 deliverable adds `eph-nicctl` for ops introspection:

```bash
eph-nicctl peers      # list attached secondaries: PID / binary / queue range / lcores
eph-nicctl stats      # daemon-side metrics: pool free, RETA updates, IPC dispatch p99
eph-nicctl tcpdump    # opportunistic per-queue packet capture
```

These are not yet implemented — track them in the daemon-led reshape
plan (S6). Until then, daemon health is observable only via
`journalctl -u eph-nicd@<bdf>` and `rte_eth_stats` exposed by tenants.

## Failure modes

### Daemon crash → systemd restart → tenant reconnect

`systemd Restart=on-failure` brings `eph-nicd` back within ~1 s of a
crash. The standard reconnect pattern is in
`docs/dpdk-reconnect-pattern.md`: sleep, retry `Platform::create`,
rebuild business state.

> **S6 status (2026-05-02)**: the typed error
> `Error::DaemonDisconnected`, the read-side `Platform::is_alive()`
> getter, the public `Platform::mark_daemon_disconnected_()` hook,
> and the idempotent retry-safe preamble on `Platform::create` are
> all landed. What is **not** yet wired is automatic detection
> inside `rx_burst` / `tx_burst` — those still surface generic
> `rte_eth_*` failures on daemon disappearance. Two recommended
> work-arounds until the burst-side wire-up lands:
>
>   * call `Platform::mark_daemon_disconnected_()` from your own
>     watchdog thread when external probes (e.g. `rte_eth_link_get`,
>     `gh ps eph-nicd`, `/proc/net/...`) report the daemon gone, then
>     have your hot loop poll `Platform::is_alive()` to short-circuit
>     burst calls;
>   * inspect raw `rte_eth_*` return codes on the burst path and
>     classify "secondary lost primary" failures into
>     `Error::DaemonDisconnected` at your own translation layer.

### Tenant crash → daemon retains other peers

Tenant SEGV / OOM / `kill -9` releases its claimed queue range back
to the daemon's pool. Other tenants are unaffected — the daemon
remains primary, the NIC is not torn down, hugepage state is
preserved. (Compare to the old autojoin model where a tenant-as-primary
crash took down every other peer.)

### Pool exhaustion

If the sum of `cfg.queues` across attaching tenants exceeds
`total_queues`, `Platform::create` returns
`ErrorInfo{QueuePoolExhausted}`. Operator response: bump
`total_queues` in the toml + restart the daemon (see Upgrade
procedure), or drop a low-priority tenant. Pool fragmentation
(non-contiguous free queues) surfaces the same error code.

### NIC link-down

The daemon owns link state; tenants are passive observers. Link
flaps are visible via `rte_eth_stats` and the per-tenant
`StreamMetric` counters; the daemon does not fail because of a link
flap.

## Upgrade procedure

Replacing the `eph-nicd` binary or changing `/etc/eph/<bdf>.toml`:

```bash
# 1. Drain tenants if you want zero packet loss; optional otherwise.
sudo systemctl stop strategy_a market_b ...

# 2. Stop the daemon. This invalidates the hugepage segment for any
# still-attached tenant — they will hit DaemonDisconnected on their
# next rx/tx_burst.
sudo systemctl stop eph-nicd@0000:01:00.1.service

# 3. Replace the binary / edit the toml.
sudo cp build/eph-nicd /usr/local/bin/
# or: sudo $EDITOR /etc/eph/0000:01:00.1.toml

# 4. Restart the daemon. Bring-up takes ~1-2 s on typical NICs.
sudo systemctl start eph-nicd@0000:01:00.1.service

# 5. Restart any tenants that were stopped, or rely on their reconnect
# loop to retry Platform::create (see dpdk-reconnect-pattern.md).
sudo systemctl start strategy_a market_b ...
```

Rolling-upgrade with zero downtime is **not** supported — there is
exactly one DPDK primary per NIC, so the upgrade window is bounded
by daemon restart latency. For HFT trading hours, schedule daemon
upgrades during the maintenance window.

## CPU pinning coordination

`eph-net-dpdk` does **not** coordinate CPU pinning across tenants. It
sees only its own process's lcore pins, validated against the
`eph::utils` per-process pin registry. Cross-tenant pin disjointness
is the operator's job:

- **Preferred**: systemd `Slice` + `CPUAffinity=` on each tenant
  unit. Each tenant gets a disjoint cpuset; the daemon gets its own
  small cpuset (typically just `daemon_lcore`).
- **Alternative**: cgroups v2 cpusets manually configured before
  launch.
- **Quick / dev**: `taskset` / `numactl` wrappers.

Decide the pin layout up-front and document it next to the toml
files (`/etc/eph/<bdf>.cpu-layout.md` is a reasonable convention).
Conflicts surface at `Platform::create` cold-path time via
`pin_lcores` rejection — they do not silently degrade.

## Security notes

- **Daemon must run as root**: VFIO group access, hugepage allocation,
  and lcore pinning all require either root or a tightly scoped
  capability set. The shipped systemd unit uses `User=root` +
  `AmbientCapabilities=` (S4 deliverable). Do not run `eph-nicd` from a
  user shell in production.
- **Tenants do not need root**: they inherit the daemon's hugepage
  segment via DPDK's secondary-process mechanism. Tenant systemd units
  should run as a dedicated unprivileged user (e.g. `eph-tenant`),
  with `SupplementaryGroups=` granting access to the hugepage mount
  and `/dev/vfio/<group>`.
- **toml file permissions**: `/etc/eph/<bdf>.toml` should be
  `0644 root:root`. Operators with edit rights effectively control
  every tenant's NIC view; treat the directory as you would
  `/etc/systemd/system/`.
- **No setuid binaries**: `eph-nicd` is not setuid. The dev-mode
  `eph::dpdk::dev::ensure_local_daemon` helper (S6) is gated on
  `geteuid()` and is intended for development hosts only.

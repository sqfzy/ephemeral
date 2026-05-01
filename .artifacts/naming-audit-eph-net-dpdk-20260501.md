# eph-net-dpdk public-surface naming audit (2026-05-01)

> Review of the post-cooperative-removal v3 surface (HEAD `78d6cdd4`).
> Scope: `eph::dpdk::` and `eph::net::dpdk::` public-namespace symbols only.
> Output is a Tier-stratified findings list — no code changes.
> Methodology: read every public header under
> `eph-net-dpdk/include/eph/dpdk/` and `eph-net-dpdk/include/eph/net/dpdk/`,
> excluding `detail/` and `test/`, then grepped downstream-impact counts
> across `eph-*/`, `examples/`, `benchmarks/`, `tests/`, `docs/`,
> `*.md`. The `_dpdk` post-fix in field/method names was deliberately
> ignored where the surrounding namespace already says "dpdk".

## Summary

- Tier 1 findings: **5**
- Tier 2 findings: **6**
- Tier 3 findings: **5**
- Recommended rename order (Tier-1, least-blast-radius first):

  1. `Platform::IcmpTargetCompoundHandle` → `IcmpTargetHandle` (drop the alias) — 11 grep hits, fully internal-shaped name leaks, easy.
  2. `eph::net::dpdk::UdpConfig::legacy` → `wire` (or fold into `Dpdk` substruct mirroring TCP) — ~70 hits but mostly mechanical; closes the asymmetry the TCP twin already fixed in T3.19.
  3. `JoinDynamicConfig::primary_config` → `nic` (mirror TCP's `dpdk.tcp_low_level` rename rationale: name the **role**, not the **consumer**) — 72 hits, well-localised.
  4. `JoinDynamicConfig::self_lcore_mask` (duplicate-of-PlatformConfig field) — collapse to a single source of truth; 18 hits.
  5. `Platform::join_dynamic` + `JoinDynamicConfig` → `Platform::create_or_join` + `CreateOrJoinConfig` (or `share_nic` / `auto_join`) — 155 hits across modules, docs, tests, examples; biggest blast radius, do last.

  The order is "leaves first": each later item presupposes the earlier
  ones haven't churned the same call sites, so doing the small / local
  ones first leaves only the big rename for last and keeps the diff
  reviewable.

---

## Tier 1 (must rename)

### 1.1 `Platform::join_dynamic` + `JoinDynamicConfig` → recommended `Platform::create_or_join` + `CreateOrJoinConfig`

- File: `eph-net-dpdk/include/eph/dpdk/platform.hpp:730` (factory),
  `eph-net-dpdk/include/eph/dpdk/join_dynamic.hpp:67` (config struct).
- Problem (the user's original observation, reframed for the
  post-cooperative-removal world):
  - `dynamic` is a **noise modifier**: every multi-process bring-up in
    the post-cleanup library is dynamic by definition (autojoin is the
    only MP path). The qualifier carries no information — calling it
    "dynamic" only made sense when there was a "static" sibling
    (`Platform::attach`) for it to contrast with. After step-2
    deletion, the contrast is gone.
  - `join` describes only one of the two roles this factory serves.
    The factory **races on EAL init**: whoever wins becomes primary
    (it does NOT join — there is nothing yet to join), whoever loses
    becomes secondary (and then joins). So `join` is half right:
    factually wrong for the primary path, factually right for the
    secondary path.
  - The verb family in the post-cleanup library is `create` (single
    process) vs `join_dynamic` (multi-process). After this rename the
    family becomes `create` vs `create_or_join`, which is symmetric
    and reads correctly: from the caller's POV every peer either
    creates the platform or joins an existing one — and the library
    decides which based on EAL race resolution.
- Candidates:
  - **A. `create_or_join`** — pros: factually correct for both roles
    (matches the underlying race semantics one-to-one); fits naturally
    next to `create`; reads as a normal English phrase; matches an
    established Tokio/Rust idiom (`Arc::clone_or_create`,
    `Cell::get_or_init`). cons: 14 chars; the existential "or" is
    slightly unusual for a factory name.
  - **B. `auto_join`** — pros: short, captures the "autojoin"
    terminology already used throughout the docs and the
    `JoinDynamicConfig` doxygen. cons: still inherits `join`'s
    primary-side wrongness; "auto" is a soft modifier, almost as
    noisy as "dynamic".
  - **C. `share_nic`** — pros: names the **what** (a shared NIC) instead
    of the **how** (a race). cons: "share" doesn't suggest a returning
    factory; the function's job is to materialize a Platform, not to
    hand out a sharing primitive. Also conflicts with the existing
    `MultiPortPlatform` (which is **not** a sharing wrapper).
  - **D. `attach_to_pci`** — pros: lines up with the deleted `attach`
    naming the user already had muscle memory for. cons: factually
    wrong on the primary path (primary doesn't attach, it creates);
    "PCI" is implementation detail (the user-facing input is just a
    BDF string).
- **Recommendation: A — `create_or_join`** (with the config struct
  becoming `CreateOrJoinConfig`).
  Rationale: post-cleanup the primary path is genuinely a `create`
  and the secondary path is genuinely a `join`; the factory's job is
  to dispatch dynamically between the two and `or` is the most
  honest connective. Pairs cleanly with `create` and
  `create_with_eal` (both still use the `create_*` prefix).
- Downstream: 155 grep hits across `*.hpp`/`*.cpp`/`*.md` (counted
  end-to-end, excluding `.claude/` worktrees). Files that touch the
  name:
  - 1 header (`join_dynamic.hpp`), 1 platform.hpp, 1 example
    (`examples/dpdk_mp_demo.cpp`),
  - 6 integration tests (`dpdk_mp_dynamic_*`,
    `dpdk_mp_dynamic_tcp_handshake_*`, `repro_ena_mp_secondary_rxburst`,
    `test_dpdk_autojoin_gw_mac`, `test_platform_v3_surface`,
    `test_eal_config_argv`),
  - 4 unit / harness helpers (`test/dpdk_env.hpp`,
    `test_bdf_sanitize`, `test_mp_registry`, `mp_topology.hpp`'s
    static_assert),
  - 4 docs (`README.md`, `CHANGELOG.md`, `docs/dpdk-multiprocess.md`,
    `docs/dpdk-mp-teardown-protocol.md`),
  - `TODO.md` and 4 `.artifacts/reshape-*-final-*.md` retro entries
    (these can be left alone — they document history, not API).
  - Header file itself: rename `join_dynamic.hpp` →
    `create_or_join.hpp` and update the sentinel macro
    `EPH_DPDK_PLATFORM_CONFIG_DEFINED` comment + the platform.hpp
    `#include` line.
- Migration notes:
  - Keep a `using JoinDynamicConfig = CreateOrJoinConfig;` alias and
    a `static auto join_dynamic = create_or_join;` shim for one
    release cycle to avoid the test/example churn happening in lock-
    step. The aliases live in the header and have zero runtime cost.
  - The doxygen "Autojoin" terminology in the file header should
    survive (it is widely-used internal language), but every public
    `///` block referring to "the autojoin path" or "join_dynamic"
    should be reworded to "the create-or-join path".
  - `proc_type.hpp` line 36–38 documents `ProcType::Auto` as
    "Used exclusively by `Platform::join_dynamic`" — drop into the
    new name.
  - The retro doc `.artifacts/retro-20260501-ena-mp-rootcause-discovery.md`
    needs no change (history).

### 1.2 `JoinDynamicConfig::primary_config` → recommended `nic`

- File: `eph-net-dpdk/include/eph/dpdk/join_dynamic.hpp:84`.
- Problem: the field name names the **consumer** ("the primary peer
  uses these values") rather than the **content** (NIC physical
  state — port_id, queue counts, descriptor counts, mempool sizing,
  RSS, MP knobs). Secondaries pass the same struct (the doc itself
  says so on line 76–83) but every field except `max_procs` /
  `queues_per_proc` / `nb_rx_queues` is **read regardless of role**
  (mempool is needed by both; port_id is needed by both; etc.). So
  the name is **factually wrong for the secondary side** and
  **misleadingly narrow for the primary side** — secondary callers
  reading their own code see "primary_config" and reasonably wonder
  if they should be passing it at all (the doc has to explicitly
  reassure them — a documentation smell that says the name is
  wrong). This is the same class of T3.19 finding that motivated
  renaming `StreamConfig::Dpdk::legacy` → `tcp_low_level`: name the
  **content** not the **historical owner**.
- Candidates:
  - **A. `nic`** — pros: 3 chars; says exactly what it is (NIC
    physical state); symmetric with the `cli::EalArgs` ↔ `EalConfig`
    /  `port_id` distinction (`port_id` lives on the NIC config,
    not the CLI args). cons: `nic` is slightly informal; might
    invite confusion with multi-NIC plurality (the field is
    singular by construction — autojoin shares ONE NIC).
  - **B. `port_config`** — pros: aligns with DPDK's "port" terminology
    that the rest of the file uses (`port_id`, `port_lo`,
    `port_hi`). cons: collides with the unrelated `src_port` /
    `dst_port` namespace (TCP/UDP), invites confusion.
  - **C. `bringup_config`** — pros: matches the internal
    `BringupConfig` already in the codebase (`detail::BringupConfig`).
    cons: requires the user to know the bring-up vocabulary; longer.
  - **D. `nic_config`** — pros: explicit; symmetric with
    `EalConfig`. cons: redundant `_config` suffix on a field that's
    already inside a `*Config` struct.
- **Recommendation: A — `nic`**. Rationale: the role of the field is
  to describe the physical NIC state shared across all peers; the
  short, neutral name avoids the false role implication of
  `primary_*` and avoids the redundant `_config` suffix. If A feels
  too informal in review, fall back to **C `bringup_config`** which
  matches the internal type's name and would map 1:1 to the
  internal `BringupConfig` struct.
- Downstream: 72 grep hits in the worktree; files (full list above
  in 1.1, since they overlap heavily). Field is referenced in:
  - 11 `eph-net-dpdk/` files (header + platform.hpp + tests + docs +
    CHANGELOG + dpdk-multiprocess.md),
  - 1 example (`dpdk_mp_demo.cpp`),
  - 0 benchmarks (autojoin is not used in `lat_*`).
- Migration notes:
  - Update the sentinel macro guard comment in
    `join_dynamic.hpp:55-58` to reference `nic` not
    `primary_config`.
  - The `JoinDynamicConfig` constructor pattern in
    `dpdk_mp_dynamic_*.cpp` and `examples/dpdk_mp_demo.cpp` uses
    designated init `{ .pci = ..., .primary_config = {...} }` which
    is mechanically `s/primary_config/nic/`.
  - Documentation cross-references in `dpdk-multiprocess.md` use
    "primary's PlatformConfig" prose — those are descriptive, no
    rename needed.

### 1.3 `eph::net::dpdk::UdpConfig::legacy` → recommended fold into `Dpdk` substruct or rename to `wire`

- File: `eph-net-dpdk/include/eph/net/dpdk/config.hpp:148`.
- Problem: the TCP twin already deleted this name in T3.19
  (renamed `legacy` → `tcp_low_level` and put it inside a `Dpdk`
  substruct — see `config.hpp:97-100` doc comment "(Renamed from
  `legacy` in T3.19 — the old name was non-semantic; this is the
  wire-level TcpConfig the PMD ingests.)"). The UDP twin
  acknowledges this in its own doc (line 142–145: "Out of scope for
  T3.19 reshape ... A future reshape can align it with the new TCP
  layout if symmetry becomes useful.") but kept the broken name.
  The result is **active asymmetry between TCP and UDP**: a user
  who learns one path then mis-types `cfg.legacy` instead of
  `cfg.dpdk.tcp_low_level` (or vice versa) gets a compile error
  whose fix is "remember which protocol you're on" — exactly the
  cross-protocol cognitive tax that T3.19 was meant to eliminate.
  Plus `legacy` is a self-deprecating noise word: every reader has
  to ask "legacy of what?".
- Candidates:
  - **A. Mirror TCP's shape: `cfg.dpdk.udp_low_level`** with a `Dpdk`
    substruct that also holds `pin_to_queue` / `pool_lcore_hint`
    (matching `StreamConfig::Dpdk`). pros: full symmetry with TCP;
    surfaces the "DPDK-specific knobs" partition that T3.19
    introduced; gives UDP room to grow `keepalive` / future
    backend-shared fields at the top level. cons: more shape change
    than a pure rename; the ::eph::dpdk::UdpConfig name (the inner,
    wire-level struct from `eph/dpdk/udp.hpp`) collides with
    `eph::net::dpdk::UdpConfig` (the outer config, in
    `eph/net/dpdk/config.hpp`) — already noted as a footgun in the
    config.hpp comment line 19–21. The rename is an opportunity to
    fix the namespace collision too (e.g. `eph::dpdk::WireUdpConfig`
    or move it under `eph::dpdk::wire::UdpConfig`).
  - **B. Pure rename `legacy` → `wire`** — pros: minimal blast
    radius; mirrors `tcp_low_level`'s "this is the wire-level type"
    documentation. cons: doesn't fix the namespace collision; lets
    the asymmetry "TCP has a `dpdk` substruct, UDP doesn't" persist.
  - **C. Pure rename `legacy` → `udp_low_level`** — pros: 1:1
    mirror of TCP's name. cons: same as B.
- **Recommendation: A** for the larger user benefit. If the rename is
  to be done in one PR, do A; if the rename has to be incremental,
  do B first (smaller diff) and follow up with the `Dpdk` substruct
  rearrangement.
  Rationale: T3.19 already established the symmetry pattern
  (`cfg.dpdk.tcp_low_level + ws + tls + keepalive` at top level for
  cross-backend shared concerns); UDP should mirror it now so we
  don't carry the asymmetry into the next round of reshape.
- Downstream: ~70 hits on `.legacy` (TX-side wire config) across
  benchmarks, examples, tests, headers. Files:
  - `benchmarks/latency/scenarios/lat_udp_loop.hpp` and
    `lat_ex_md_udp_loop.hpp` and `lat_rss_scaling.cpp` —
    benchmarks that wire `cfg.legacy.src_port = ...`.
  - `eph-net-dpdk/include/eph/net/dpdk/udp_socket.hpp` — the
    actual lowering code that reads `cfg.legacy.*`.
  - `eph-net-dpdk/tests/test_dpdk_udp_socket.cpp` — gtests.
  - 2 examples (`examples/dpdk_mp_demo.cpp`,
    `examples/dpdk_rss_demo.cpp`).
  - `eph-net/include/eph/net/keepalive_config.hpp` — comment
    reference only (was in TCP's old surface).
- Migration notes:
  - **Caveat on plan A**: requires touching the inner type
    `eph::dpdk::UdpConfig` (the wire-level one) too, which has its
    own use in `UdpSender::create` — that's another ~30 hits but
    they're inside `eph-net-dpdk/` and `examples/dpdk_*`. If the
    namespace move (`eph::dpdk::wire::UdpConfig`) is too disruptive,
    plan B is the conservative path.
  - The kernel UdpConfig (`eph::net::kernel::UdpConfig`) does not
    have a `legacy` field — UDP is kernel-defaults. So the rename
    on the DPDK side is one-directional and won't introduce new
    drift.

### 1.4 `JoinDynamicConfig::self_lcore_mask` (duplicate of `PlatformConfig::self_lcore_mask`)

- Files:
  - `eph-net-dpdk/include/eph/dpdk/platform.hpp:486`
    (`PlatformConfig::self_lcore_mask`)
  - `eph-net-dpdk/include/eph/dpdk/join_dynamic.hpp:136`
    (`JoinDynamicConfig::self_lcore_mask`)
- Problem: the same field exists on both structs and is consumed
  on both autojoin paths (primary + secondary; see platform.hpp
  lines 2453, 2785, 2869). The `JoinDynamicConfig` doc (line 126–
  136) says it's a "back-compat" field for callers that haven't
  migrated, but **there's no other place to migrate to** — the
  `PlatformConfig` field on the embedded `nic` (a.k.a.
  `primary_config`) is what migrated callers would use. As written,
  setting `cfg.nic.self_lcore_mask = X` and
  `cfg.self_lcore_mask = Y` simultaneously gives two-source-of-
  truth: which wins is path-dependent (primary uses the inner one
  via `bringup_from_v3_`, line 2453; secondary uses the outer one,
  line 2870). This is exactly the "two sources of truth" hazard
  `MpTopology`'s comment (line 95-96, "Two-source serialization
  hazard") was extracted to avoid.
- Candidates:
  - **A. Delete `JoinDynamicConfig::self_lcore_mask`**; require
    callers to set it on the embedded `nic` (post-rename
    `primary_config`) field. pros: single source of truth.
    cons: minor breaking change — secondary peers that don't
    populate `nic.*` (because their NIC state is read from the
    live device) now have to populate exactly one inner field
    just for `self_lcore_mask`.
  - **B. Delete `PlatformConfig::self_lcore_mask`**; keep the
    outer one. pros: matches the per-process / per-peer nature
    of the value (it's a "this peer owns these lcores"
    declaration, not a NIC-physical-state thing). cons: makes
    `Platform::create` (single-process) lose access to the field
    entirely — but single-process callers don't need it anyway.
  - **C. Keep both, document precedence and have validate_config
    reject the simultaneous-non-zero case.** pros: source-compat.
    cons: doesn't fix the "name suggests it should be on
    `JoinDynamicConfig` not `PlatformConfig`" mental-model
    problem.
- **Recommendation: B**. Rationale: `self_lcore_mask` is a
  per-peer / per-process declaration ("the lcores THIS process
  is using"), and `JoinDynamicConfig` is the per-peer struct.
  `PlatformConfig` is the NIC-physical-state struct (shared
  across all peers in autojoin); putting a per-peer field on it
  was always a layering smell. The single-process `Platform::
  create` path doesn't use `self_lcore_mask` for anything
  meaningful (lines 2453-2454 only set
  `mp_topology->procs[0].lcore_mask` if `mp_topology` is
  present, which only happens in the autojoin path — single-
  process never reaches that branch). So removing it from
  `PlatformConfig` is dead-code removal.
- Downstream: 18 grep hits on `self_lcore_mask` across the
  worktree (mostly internal to platform.hpp and join_dynamic.hpp,
  plus a few tests / examples).
- Migration notes:
  - Move the doc comment from `PlatformConfig` to
    `JoinDynamicConfig` (the JoinDynamicConfig comment is already
    fuller — keep it). Drop the "back-compat" framing.
  - The "Catches two procs accidentally pinned to the same
    lcore" rationale on `JoinDynamicConfig` is the right
    documentation home.

### 1.5 `Platform::IcmpTargetCompoundHandle` → recommended `IcmpTargetHandle` (drop the alias)

- File: `eph-net-dpdk/include/eph/dpdk/platform.hpp:939` (class) +
  `:975` (alias).
- Problem: there are now two names for one type:
  `IcmpTargetCompoundHandle` (the actual class) and
  `IcmpTargetHandle` (a `using` alias kept "for source-compat with
  reshape stage 0", per the comment on line 968-974). "Compound" is
  internal vocabulary — it leaks the implementation detail
  (`IcmpRegistry::Handle local_` + `IcmpDirectorySlotGuard dir_`
  composed) into the public name. Public users only ever see one
  thing: a moveable RAII handle they store and let drop. The
  cleanup migration from stage 0 has presumably finished (no
  external consumer rely on stage-0 names since
  `Platform::IcmpTargetHandle` is the only spelling found in the 11
  grep hits inside this repo). Carrying both is a drag on cognitive
  load (every reader has to figure out the relationship) and on
  the doxygen output (two doc anchors for one concept).
- Candidates:
  - **A. Rename the class to `IcmpTargetHandle`, drop the alias.**
    pros: one name; matches the doxygen documentation already
    written for the alias on line 968 ("kept for source-compat
    with reshape stage 0"); also matches the internal type name
    `IcmpRegistry::Handle`. cons: minor — none.
  - **B. Keep both, but reverse them: name the class
    `IcmpTargetHandle` and add a deprecated
    `IcmpTargetCompoundHandle` alias for stage-0 callers.** pros:
    transitional. cons: the comment on line 968 already says the
    alias is the canonical name today; flipping the relationship
    is just deferred work.
- **Recommendation: A**. Rationale: the deprecation period is
  already over (no external callers in the worktree use the
  `Compound` spelling); collapsing to one name is a strict
  improvement.
- Downstream: 11 grep hits on `IcmpTargetCompoundHandle` in
  `*.hpp`/`*.cpp`, ALL inside `eph-net-dpdk/include/eph/dpdk/
  platform.hpp` (the class, the alias, the constructors, the
  destructors, the move ops). External code uses
  `Platform::IcmpTargetHandle` only.
- Migration notes:
  - Drop the alias on line 975.
  - Update the constructor / dtor / move-op identifiers in the
    class body (mechanical s/IcmpTargetCompoundHandle/
    IcmpTargetHandle/g within the class definition).
  - Doxygen brief on line 919–938 should drop "Compound" and
    just say "RAII handle returned by `register_icmp_target`".

---

## Tier 2 (should rename)

- `tcp_low_level` (TCP) and the proposed `udp_low_level` (UDP) — `low_level` is a soft modifier; the more direct name is `wire` (matches the doxygen "wire-level TcpConfig" prose). Suggested: rename `tcp_low_level` → `wire` and use `wire` consistently across both protocols. ~120 grep hits on `tcp_low_level`. **Risk**: `wire` is short and could be confused with `eph::dpdk::wire::*` if a future namespace move (1.3 plan A) lands. If both renames happen, sequence them (1.3 first, this second).
- `Platform::create_with_eal` — the `_with_eal` suffix is true but
  awkward; the alternative would be `Platform::launch` or
  `Platform::bringup` (i.e. one-shot lifecycle factory). Verb-family-
  wise the `_with_eal` form sits oddly between `create` (no EAL) and
  `create_or_join` (proposed; also brings up EAL via
  `--proc-type=auto`). Suggested: `Platform::launch` for the EAL-
  bringing variant; keep `create` for the in-process pre-EAL variant.
  99 grep hits.
- `eph::dpdk::cli::EalArgs` — name suggests "args" (a list); is in
  fact an accumulator with a typed structure. Suggested:
  `EalCliConfig` or `EalCliState` — paired with `try_consume` and
  `to_eal_config`, the "config / state" framing reads better than
  "args".
- `eph::dpdk::cli::try_consume` — function naming family in this
  module is heterogeneous (`try_consume` vs `validate` vs
  `to_eal_config`); `try_consume` is fine on its own but the trio
  doesn't look like it shares an author. Suggested: rename to
  `consume_one` (keep the optional unexpected to communicate fall-
  through), or align via `parse_one` / `validate` / `lower`. Cosmetic.
- `MpTopology::custom` factory — the name describes the **lack of
  pattern** (this is the not-uniform path) rather than the **input
  shape** (a list of explicit `ProcSpec`s). Suggested: `MpTopology
  ::from_specs(self_index, std::initializer_list<ProcSpec>)` so the
  factory pair becomes `uniform` (compute layout) / `from_specs`
  (carry user-supplied layout).
- `JoinDynamicConfig::eal_extras` — "extras" is a noise word
  ("extras compared to what?"). Suggested: `extra_eal_args`
  (matches `EalConfig::extra_args` exactly — same field, same
  name).

---

## Tier 3 (nit)

- `Platform::has_mp_topology()` — the underscore-noun-as-verb form
  reads slightly off; `Platform::is_multi_process()` would match the
  `is_secondary()` / `is_running()` / `is_promiscuous()` family
  better.
- `Platform::self_port_range()` — "self" is implicit (every getter on
  Platform is about THIS Platform); could drop the `self_` prefix:
  `Platform::owned_port_range()` or just `Platform::port_range()`.
- `Platform::effective_rx_queue_range()` — "effective" is what
  resolution does (sentinel `{0,0}` → `{0, nb_rx_queues}`) and the
  doc explains it well. Reads slightly long; cosmetic.
- `BringupConfig` (detail::) — the name is fine, but the public
  `PlatformConfig` is *also* a "bringup config" semantically; if the
  audit ever sweeps internal names, this one is a candidate for
  `InternalPlatformConfig` or just folding into `PlatformConfig`
  with the v2-only fields elided.
- `EalGuard::init_with_pins` — the suffix is true but the typed-pin
  path is the **recommended** path; the legacy `init` is the escape
  hatch. The name framing inverts the recommendation. Cosmetic.

---

## Out of scope

- Symbols in `eph::dpdk::detail::` (`IcmpRegistry`, `MpRegistry`,
  `BringupConfig`, `IcmpDirectorySlotGuard`, `bdf_sanitize`, etc.):
  internal-only, do not bind users.
- Symbols in `eph::dpdk::test::` (`dpdk_env.hpp`): test-fixture only.
- Build-system identifiers (`apply_dpdk_pmd_linkgroups()`,
  xmake target names): out of scope per the task description.
- Names already audited and confirmed good or covered by a recent
  rename:
  - `StreamConfig::Dpdk::tcp_low_level` (renamed in T3.19 — a Tier-2
    candidate above, but already a step forward from `legacy`).
  - `KeepaliveConfig`, `WsConfig`, `ProxyConfig` — backend-shared
    surface that lives in `eph-net`, not `eph-net-dpdk`; outside
    this audit's scope.
  - `MetricsSink` / `StreamMetric` / `publish_metrics` —
    cross-cutting and audited in the observability rollout.
  - `DpdkPollable` (concept) / `DpdkPoller<P>` / `DpdkTcpStream<C,
    Tls>` / `DpdkUdpSocket<C>` — the verb family `create` /
    `create_and_attach` is solid; renaming these would churn every
    consumer for cosmetic gain.
  - `ProcType::{Primary, Secondary, Auto}` — DPDK-prescribed
    spelling; can't change without breaking the `--proc-type=`
    serialization that DPDK's CLI requires.

---

## Suggested follow-up sequence

For each Tier-1 finding, the corresponding `/pax --reshape` invocation
(read-only; the human user authors the actual command). Order is
"least-blast-radius first, biggest last" so each later rename does
not stumble over churn from earlier ones.

1. `/pax --reshape --auto eph-net-dpdk Platform::IcmpTargetCompoundHandle 折叠到 IcmpTargetHandle 单一名字`
   — finding 1.5; 11 grep hits, all in one file.

2. `/pax --reshape --auto eph-net-dpdk JoinDynamicConfig::self_lcore_mask 留在 JoinDynamicConfig，从 PlatformConfig 删掉`
   — finding 1.4; 18 grep hits; surgical.

3. `/pax --reshape eph-net-dpdk eph::net::dpdk::UdpConfig::legacy 重命名为 wire 并对齐 TCP 的 Dpdk 子结构`
   — finding 1.3 (plan A); ~70 hits; non-trivial.
   (Drop `--auto` because the namespace move for the inner
   `eph::dpdk::UdpConfig` collision is a judgement call.)

4. `/pax --reshape --auto eph-net-dpdk JoinDynamicConfig::primary_config 重命名为 nic`
   — finding 1.2; 72 hits; mechanical.

5. `/pax --reshape eph-net-dpdk Platform::join_dynamic / JoinDynamicConfig 重命名为 create_or_join / CreateOrJoinConfig`
   — finding 1.1; 155 hits; biggest blast radius. Keep `--auto`
   off so the doxygen / docs prose changes get human review (the
   "autojoin" terminology in the file header should be preserved
   intentionally even as the API name flips).

After Tier-1 lands, the Tier-2 sweep is a single `/pax --reshape`
that hits `tcp_low_level → wire`, `EalArgs → EalCliConfig`,
`eal_extras → extra_eal_args` and the cli verb-family alignment
in one PR.

The Tier-3 list is "fix-while-you're-there" material — no dedicated
PR.

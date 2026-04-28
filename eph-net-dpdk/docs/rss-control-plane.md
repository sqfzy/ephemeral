# RSS multi-queue × DPDK control-plane integration

How DNS / ARP / Multicast behave under RSS multi-queue dispatch
(`Platform::is_rss_active() == true && nb_rx_queues > 1`), and the
contract each control-plane API enforces to make that combination safe.

## The problem in one paragraph

When `Platform::create()` brings up a NIC with `enable_rss=true` and
`nb_rx_queues > 1`, every inbound IPv4 TCP/UDP packet's 5-tuple is
hashed by the NIC and demultiplexed across queues via the RETA. That
is the right choice for data-plane TCP / UDP unicast (already covered
by `DpdkTcpStream::create_and_attach` / `DpdkUdpSocket::create_and_attach`,
which reverse-pick a `src_port` such that the reply hashes back to the
caller's chosen queue). Control-plane APIs that issue blocking
"send-then-poll" round trips are different — they predate RSS-aware
queue selection and used to assume "the reply comes back on the queue
I send from". On RSS multi-queue NICs that assumption fails silently:
the reply lands on whichever queue the hash picks, the resolver polls
only its own queue, and the call times out.

ICMP was already addressed via `Platform::register_icmp_target` +
`DpdkPoller::set_icmp_callback` (the registry dispatches by 5-tuple
regardless of which queue the ICMP arrived on). DNS / ARP / Multicast
were the remaining blind spots; this document records how each is
handled now.

## The three APIs at a glance

| API | What hashes | Mechanism | Behaviour under RSS multi-queue |
|-----|-------------|-----------|---------------------------------|
| `dns::resolve` / `AsyncDnsResolverT::start` | UDP 5-tuple includes our `src_port` | `detail::select_dns_src_port` reverse-picks an ephemeral port whose hash lands the reply on the caller's queue | Works automatically; falls back to `random_ephemeral_port` when RSS is inactive |
| `arp::resolve` | EtherType 0x0806 — **not** in any RSS hash set | Hardcoded RX queue 0 (parameter removed from API) | Always uses queue 0; non-IP traffic falls to default queue on every supported PMD |
| `MulticastReceiver` | UDP 5-tuple, but receiver does not control any field | `MulticastConfig::rss_active_multi_queue` flag → `start()` fail-fast | Caller must single-queue or install FlowDirector pin first |

## DNS

```cpp
#include "eph/dpdk/dns.hpp"

// dns::resolve signature is unchanged; the RSS-aware src_port pick is
// internal. Set up the call exactly as before:
auto ip = eph::dpdk::dns::resolve(
    /*port_id=*/0, /*queue_id=*/2,
    pool, src_mac, gw_mac, local_ip, "fstream.binance.com", cfg);
```

What changed inside: instead of `src_port = random_ephemeral_port()`,
the resolver now calls `detail::select_dns_src_port(port_id, queue_id,
nameserver_ip, local_ip, dns_server_port)`. That helper:

1. Probes RSS state via `query_rss_state(port_id)`. If the probe fails
   (single-queue NIC / RSS not configured), falls back to the
   pre-RSS random ephemeral port.
2. Otherwise walks `[32768, 60999]` varying the **dst_port** slot of
   the inbound reply 5-tuple `(nameserver_ip, 53, local_ip, dst_port)`
   — that's where our local SP lives in the reply direction (per
   `flow_steering.hpp:686-692`'s `queue_for_tuple` contract). Returns
   the first port whose hash lands on `queue_id`.
3. If the search exhausts (no port in the range hashes correctly),
   returns `RssHashPredictExhausted` and the resolve fails fast.

Why a custom loop and not `find_src_port_for_queue`: the latter varies
the **src_port** slot of the hash, which for an inbound reply is the
remote sender's application port (always 53 for a DNS server). DNS's
only caller-controllable hash input is the dst_port slot.

Security trade-off: under 4-queue RSS, src_port entropy drops from
~28k to ~7k bits — only one quarter of the ephemeral range hashes
correctly. The 16-bit transaction id is still random, the Toeplitz
hash key + RETA are not exposed to the network, and HFT exchange
endpoints are typically pre-resolved or DoT, so the cache-poisoning
surface is qualitatively similar.

## ARP

```cpp
#include "eph/dpdk/arp.hpp"

// New signature — queue_id parameter REMOVED.
auto mac = eph::dpdk::arp::resolve(
    port_id, pool, src_mac, src_ip, target_ip, std::chrono::seconds{3});
```

ARP packets carry EtherType 0x0806 which is not in any RSS hash set
the project enables (`platform.hpp:1113-1115` — IPv4 TCP/UDP/IPV4 only).
On every PMD currently supported (notably AWS ENA), non-IP traffic
falls to the NIC's default RX queue, which is queue 0. Allowing
callers to pass a non-zero `queue_id` (the pre-fix API) silently
timed out on RSS-active multi-queue Platforms. The parameter is now
gone — the public `arp::resolve` calls `resolve_with_io` with
`queue_id=0` hardcoded.

`resolve_with_io` retains the `queue_id` parameter for testability
(unit tests inject fake Io shims that record `(port, queue)` pairs);
production code must not call it directly.

## Multicast

```cpp
#include "eph/dpdk/multicast.hpp"

eph::dpdk::MulticastConfig cfg;
cfg.port_id     = 0;
cfg.rx_queue_id = 0;

// Forward Platform's RSS state to the receiver — start() will fail-fast
// if this is true and the caller hasn't explicitly handled RSS via
// FlowDirector pinning per group.
cfg.rss_active_multi_queue = platform.is_rss_active();

eph::dpdk::MulticastReceiver rx(cfg);
rx.join_group(...);
auto started = rx.start();   // unexpected if RSS active and no FD pin
```

There is no DNS-style fix for multicast: the receiver does not
control any 5-tuple slot — sender_ip and sender_port are picked by
whoever sends, group_ip and group_port are protocol-fixed. So under
RSS multi-queue, multicast packets get spread across queues and a
single-queue receiver cannot guarantee delivery.

`start()` enforces this by fail-fasting when
`rss_active_multi_queue=true`. Caller resolutions:

* **Single-queue Platform**: don't enable RSS, or set
  `nb_rx_queues=1` in `PlatformConfig`. Multicast on queue 0 works
  normally. Set `cfg.rss_active_multi_queue=false`.

* **FlowDirector pinning**: install a per-group FD rule mapping
  the group's `(group_ip, group_port)` to `rx_queue_id` before
  calling `start()`, then set `cfg.rss_active_multi_queue=false` to
  acknowledge that the RSS hash routing is overridden by FD for this
  flow. (Pattern parallels `DpdkTcpStream::create_and_attach` in
  `RxDispatchMode::FlowDirector` mode.)

A future iteration may add a typed `MulticastConfig::flow_director_pinned`
flag and have MulticastReceiver install the FD rule on its own. Out
of scope for this PR.

## Adding new control-plane APIs

If you add a new DPDK control-plane API that does its own RX polling
(direct `rte_eth_rx_burst` instead of going through `DpdkPoller`):

1. **Stop and consider whether it can use `DpdkPoller` instead.** The
   poller already polls every queue in `Platform::effective_rx_queue_range()`
   and dispatches by 5-tuple — that's the canonical way. Use
   `DpdkPollable` to register the protocol's state machine.

2. If a blocking helper is genuinely needed, classify the protocol's
   RSS hash behavior:
   * **Both endpoints control a hash slot** → src_port reverse-pick
     (DNS pattern; see `select_dns_src_port`).
   * **Protocol uses a non-IP EtherType** → hardcode queue 0
     (ARP pattern).
   * **Receiver controls no hash slot** → require single-queue or
     FlowDirector pin (Multicast pattern).

3. Add a regression test mirroring `test_dns_rss_aware.cpp` /
   `test_arp_api.cpp` / `test_dpdk_udp_multicast_rss.cpp`.

4. Update this document with a row in the API table.

## Reference

* Plan: `.claude/plans/polished-hopping-piglet.md` (the dns-rss-fix plan)
* Code:
  * `eph-net-dpdk/include/eph/dpdk/dns.hpp` — `select_dns_src_port`
  * `eph-net-dpdk/include/eph/dpdk/arp.hpp` — `arp::resolve`
  * `eph-net-dpdk/include/eph/dpdk/multicast.hpp` — `MulticastConfig::rss_active_multi_queue`
  * `eph-net-dpdk/include/eph/dpdk/platform.hpp` — `Platform::is_rss_active()`
  * `eph-net-dpdk/include/eph/net/dpdk/flow_steering.hpp` — `query_rss_state` / `queue_for_tuple` / `find_src_port_for_queue`
* Tests:
  * `eph-net-dpdk/tests/test_dns_rss_aware.cpp` (7 cases)
  * `eph-net-dpdk/tests/test_arp_api.cpp` (4 cases)
  * `eph-net-dpdk/tests/test_dpdk_udp_multicast_rss.cpp` (4 cases)

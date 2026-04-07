# Code Review Report

## Metadata
- Time: 2026-04-07
- Diff source: file (full review of 4 files)
- Scope: udp.hpp, net_header.hpp (UDP additions), flow_steering.hpp, test_udp.cpp
- Focus: all dimensions
- Build: PASS
- Tests: PASS (34 test_udp + 88 test_net_header)

---

## Summary

Architecture is correct: two-layer abstraction (UdpPacketTemplate + UdpSender) mirrors TCP's PacketTemplate/TcpSession pattern. send_batch stats bug was already fixed with built_lens[]. UdpConfig gained dump()/to_json(). Flow Steering cleanly extended with backward-compatible default parameter.

- Critical: 0
- Major: 2 (observability + test coverage)
- Minor: 2 (code organization + constexpr comment)
- Nit: 2

**Verdict: APPROVE**

---

## Issues

### [Major] send() error logs lack context (udp.hpp:215,223)
TRACE logs say "mbuf alloc failed" / "tx_burst returned 0" without payload_len, port, or queue info. Add at least len to the message.

### [Major] build_udp_packet has no happy-path test (test_udp.cpp:427-433)
Only NullPoolReturnsNull exists. Happy path needs real mempool (EAL). Document this gap or add fill-based test variant.

### [Minor] Unused TCP/UDP variables in install_flow_rule branches (flow_steering.hpp:358-361)
Move declarations into respective if/else branches.

### [Minor] constexpr validate() checks runtime pointer (udp.hpp:62-68)
Technically fine, but add comment clarifying pool check is runtime-only.

### [Nit] alignas(64) comment says "false sharing" but type is thread-private (net_header.hpp:584)
Change comment to "optimal memcpy performance on TX hot path".

### [Nit] [[maybe_unused]] on logger — correct, consistent with project (udp.hpp:154)
No change needed.

---

## Highlights

- built_lens[] cleanly solves index divergence (udp.hpp:248-251)
- fill() hot path: 1x memcpy(42B) + 3 stores — optimal for UDP
- FlowProtocol default parameter = perfect backward compat
- IpIdWrapsAroundAt65535 — good boundary testing

---

## Diff Stats
```
 eph-dpdk/include/eph/dpdk/flow_steering.hpp |  78 +++-
 eph-dpdk/include/eph/dpdk/net_header.hpp    | 209 +++++++++++
 eph-dpdk/include/eph/dpdk/udp.hpp           | 343 ++++++++++++++++++
 eph-dpdk/tests/test_udp.cpp                 | 544 ++++++++++++++++++++++++++++
 4 files changed, 1160 insertions(+), 14 deletions(-)
```

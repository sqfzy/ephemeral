# Phase 9 Scope Decision — Out-of-scope Items

## Context

Phase 9 (April 2026) was a recovery mini-project after the v3.3 refactor
accidentally lost ~1300 test cases and some functionality. Based on the
5-role /discuss (see `.artifacts/discuss-20260410-174332.md`) and the
resulting /plan (`.artifacts/plan-phase-9-recovery-20260410-180306.md`),
scope was deliberately reduced from baseline's full surface.

The baseline under discussion is preserved at `.temp/baseline-pre-v3.3/`
(read-only, excluded from git via `.gitignore`). Phase 9 imported only the
pragmatic subset required by real HFT workloads; architecture-astronaut
scaffolding that baseline itself never exercised was intentionally dropped.

This document archives **what was intentionally NOT migrated** and provides
recovery guidance for future maintainers who may need to restore any of
these components.

---

## Out-of-scope functionality

### Gateway (HFT application wrapper)

**Baseline location**: `.temp/baseline-pre-v3.3/eph-net/include/eph/net/gateway.hpp`
**Baseline tests**: `.temp/baseline-pre-v3.3/eph-net/tests/test_gateway.cpp` (93 cases)

**Why not migrated**:
- No baseline example used it (confirmed via grep across `examples/` at the
  time of the /discuss).
- Strong HFT engineer voice (R-Trader in the discuss) noted that HFT teams
  write per-venue wrappers rather than use a generic Gateway framework —
  every exchange has its own failure-mode taxonomy that a generic wrapper
  cannot capture.
- Forced composition form conflicts with v3.3's config-driven collapsed
  design: v3.3 deliberately removes the "wrapper stack" idiom in favour of
  a single `Stream<C, Tls>` template with behaviour selected via
  `StreamConfig`.

**Recovery guidance**: If a future use case emerges, do **not** copy
baseline verbatim. Redesign around v3.3's `Stream<C>` concept as a
template-parameter wrapper (sketch):

```cpp
// Sketch for a future eph-risk module
namespace eph::risk {
    template <eph::net::Stream S>
    class Gateway {
        S                  inner_;
        utils::KillSwitch* ks_;
        utils::TokenBucket rl_;
    public:
        // satisfy Stream concept itself; compose policies over inner_
        std::expected<std::size_t, core::ErrorInfo>
        send(std::span<const std::byte> bytes) {
            if (ks_->tripped()) return std::unexpected(...);
            if (!rl_.try_acquire(bytes.size())) return std::unexpected(...);
            return inner_.send(bytes);
        }
        // ... delegate poll_rx / close / state to inner_
    };
}
```

Do **not** bring back the baseline class-heavy framework — it pre-dates
concepts and fights the v3.3 type system.

---

### CircuitBreaker

**Baseline location**: `.temp/baseline-pre-v3.3/eph-net/include/eph/net/circuit_breaker.hpp`
**Baseline tests**: `.temp/baseline-pre-v3.3/eph-net/tests/test_circuit_breaker.cpp` (64 cases)

**Why not migrated**:
- Opinions on state-machine shape (open → half-open → closed transitions,
  failure thresholds, reset timeouts) vary widely per venue.
- HFT teams prefer writing per-venue state machines that match each venue's
  specific failure-mode taxonomy (e.g. Binance `-1003` vs OKX `rate_limit`
  vs Coinbase 429).
- The generic `CircuitBreaker` framework was never consumed by any
  baseline example — zero grep hits in `.temp/baseline-pre-v3.3/examples/`.

**Recovery guidance**: Same pattern as Gateway — if needed, add to an
`eph-risk` module (**not** `eph-net`). Keep it minimal: ~50 lines of core
logic. Do **not** port baseline's full 64-case test suite — write fresh
tests aligned with the specific state-machine shape the user needs.

---

### HTTP chunked transfer encoding

**Baseline tests**: ~40 cases across `test_http*.cpp` files mentioning "chunked"

**Why not migrated** (per plan.md §D-1):
- No HFT-relevant exchange REST API uses chunked. A 2026-04 sampling of
  Binance / OKX / Coinbase / Kraken / Bitget / Bybit confirmed all of them
  use `Content-Length` responses.
- Parser complexity is non-linear: trailer headers, chunk extensions,
  chunk-size parsing, CRLF sensitivity in chunk boundaries.
- Supporting chunked adds attack surface — HTTP request-smuggling / desync
  attacks rely on ambiguous chunked/`Content-Length` interleaving.

**Recovery guidance**: The HTTP parser in `eph-net/include/eph/net/http.hpp`
explicitly rejects requests / responses carrying `Transfer-Encoding`
with `Error::CodecBad` (see the test
`parse_http_response_rejects_chunked_transfer_encoding` in
`eph-net/tests/test_http_parser.cpp`). If a future use case requires
chunked, implement it as a **separate opt-in helper** (e.g.
`detail/http_chunked.hpp`) that post-processes the body slice, **not**
inline in `parse_http_response`. Keep the rejection behaviour for the
default zero-config path.

---

### HTTP Transfer-Encoding / cookies / redirect / Expect: 100-continue / multipart

Same rationale as chunked — no HFT exchange uses any of these, and each
adds substantial attack surface. All scope-rejected. The baseline tests
at `.temp/baseline-pre-v3.3/eph-net/tests/test_http_te_edge_cases.cpp`
and `test_http_response_complete_adv.cpp` can serve as a reference library
if a future maintainer needs to re-add any of these.

**Recovery guidance**: If required, implement as opt-in detail helpers
sitting above the core `parse_http_response`, **not** by mutating the
core parser.

---

### SOCKS5 proxy

**Baseline**: partial SOCKS5 support in
`.temp/baseline-pre-v3.3/eph-net/include/eph/net/proxy.hpp`

**Why not migrated**: Zero HFT use case. HFT deployments use either direct
colo connections or HTTP CONNECT proxies for cross-region tunneling;
SOCKS5 is not used in this domain.

**Recovery guidance**: If needed, add as a sibling of
`eph-net/include/eph/net/detail/http_connect.hpp` named
`detail/socks5.hpp`. Follow the same template `ByteSink` pattern that
`http_connect.hpp` uses so the handshake can be exercised via
`FakeStream` in tests. `StreamConfig::proxy` could be extended to
`std::variant<HttpConnectConfig, Socks5Config>` but keep it minimal:
no authentication variants, no GSSAPI, no IPv6 special cases unless a
user demonstrates a concrete need.

---

## Out-of-scope test cases

### test_transport.cpp (baseline: 110 cases)

Tested the old `Transport<T>` class in `eph-transport/`. Entirely dropped
because v3.3 replaced `Transport` with `KernelTcpStream<C, Tls>` /
`DpdkTcpStream<C, Tls>` and the "Transport wraps a TcpTransport" alias
system was removed. The behavioural coverage that still applied (stream
lifecycle, reconnect, framing) was rewritten as
`test_kernel_tcp_stream_behavioral.cpp` in Phase 9.8.

### test_transport_types.cpp (baseline: 211 cases)

Tested the old type-alias system (e.g.
`SocketWssTransport = DefaultTransport<SocketTransport>`). v3.3's
namespace-based backend split (`eph::net::kernel::*` vs
`eph::net::dpdk::*`) replaces this. The cases are not portable — they
tested compile-time identity of alias chains that no longer exist.

### test_socket_transport.cpp subset (~26 cases)

Cases that probed `SocketTransport`-specific private state — raw fd
exposure, internal buffer inspection, `get_socket_options()` — were
dropped. The ~60 cases covering fundamental stream behaviour were
rewritten in Phase 9.8's `test_kernel_tcp_stream_behavioral.cpp`.

---

## `.temp/baseline-pre-v3.3/` lifecycle

**Current state**: preserved as a read-only reference. The directory is
excluded from git via `.gitignore` (entry: `.temp/`), so it does not
pollute history and costs nothing to keep on disk.

**Retention policy**: keep until **v3.4 release**, then remove via:

```bash
rm -rf .temp/baseline-pre-v3.3/
```

**Rationale** (from /plan §D-6):
- Phase 9 recovery work itself depended on the baseline as a reference
  — that dependency has now been discharged.
- v3.4 release signals that v3.3 has been in production long enough that
  surprise regression-recovery needs are unlikely.
- Keeping it until then preserves a cheap escape hatch in case a
  forgotten corner case surfaces before v3.4.

**Responsibility**: the v3.4 release checklist should include this step.
Before removal, any recovery work that still needs baseline source
should copy the relevant files to a new location outside `.temp/`
(e.g. `docs/archive/`) so the content survives the cleanup.

---

## Summary of Phase 9 delivery

| Category | Baseline cases | Phase 9 migrated | Coverage | Notes |
|---|---|---|---|---|
| P0 security | 64 | 67 | 105% | +3 boundary additions in 9.4 |
| P0 regression (9 commits) | ~30 | ~30 | 100% | 3 WS commits covered in 9.7 |
| P1 HTTP parser | 235 | 180 | 77% | chunked / TE / cookies dropped |
| P1 HTTP client | 146 | 85 | 58% | class-behaviour cases deferred |
| P1 WS wire | 122 | 127 | 104% | +5 regression additions in 9.7 |
| P1 TLS record | 78 | 88 | 113% | merged + additions |
| P1 TLS config | 47 | 47 | 100% | |
| P1 HMAC | 45 | 46 | 102% | +1 destructor test |
| P2 SocketTransport → KernelTcpStream behavioural | 86 | 60 | 70% | raw fd cases dropped |
| P2 Transport config → StreamConfig validation | 83 | 67 | 81% | |
| P2 TcpConcept → Stream concept | 46 | 25 | 54% | subsumption focus |
| Proxy URL parser | — | 17 | (new) | |
| HTTP CONNECT | — | 12 | (new) | |
| Kernel proxy integration | — | 8 | (new) | |
| KillSwitch | 42 | 16 | minimal | completely rewritten |
| RateLimiter | 49 | 21 | minimal | completely rewritten |
| WS handshake | — | 12 | (new) | |
| Kernel WS upgrade | — | 6 | (new) | |
| **Total** | **1273** | **~914** | **~72%** | |

vs the original 770-case plan estimate, Phase 9 over-delivered slightly
(~914 vs 770) due to enhanced boundary testing and supplementary
regression cases that surfaced during migration.

---

## Deleted features (no recovery path)

None in Phase 9. Every recovery decision was a conscious scope choice
with a clear recovery path documented above. No baseline functionality
was silently deleted — anything not migrated is listed here with
rationale and a reconstruction sketch.

# TLS Hardening for Crypto Venues — Recommendations

> Practical guidance on `verify_peer` / `ca_cert_path` / `pinned_spki_sha256`
> for the four primary crypto venues (Binance, OKX, Bybit, Coinbase
> Advanced Trade). Production-ready defaults that don't trade security
> for ergonomics.

The library's TLS surface is `eph::net::TlsConfig` in
`eph-net/include/eph/net/detail/tls_constants.hpp` (search for
`struct TlsConfig`). This page is the runbook for which combinations to
pick per venue and why; pair it with `docs/production-config.md` (full
`StreamConfig` profiles) and `docs/troubleshooting.md` (TLS error
decoder).

---

## TL;DR

| Venue | `verify_peer` | `ca_cert_path` | `pinned_spki_sha256` | Why |
|---|---|---|---|---|
| Binance (`stream.binance.com`, `fstream.binance.com`, `api.binance.com`) | `true` | `""` (system) | empty | CDN-fronted, issuer rotates |
| OKX (`ws.okx.com`, `aws.okx.com`, `www.okx.com`) | `true` | `""` (system) | empty | CDN-fronted, issuer rotates |
| Bybit (`stream.bybit.com`, `api.bybit.com`) | `true` | `""` (system) | empty | CDN-fronted, issuer rotates |
| Coinbase Advanced Trade (`advanced-trade-ws.coinbase.com`, `api.coinbase.com`) | `true` | `""` (system) | empty | ACM/CDN-fronted, issuer rotates |

For all four: trust the system store, do **not** pin. SPKI pinning is
appropriate when (a) you control both endpoints (private interconnect,
internal mTLS), or (b) the venue commits in writing to a stable issuer
chain. Neither is true for the public CDN-fronted edges of the four
exchanges above — pinning a leaf or intermediate hash there is a ban
risk on the next rotation.

If you don't know what a venue's rotation cadence is, the safe default
is: trust the system store, watch for `Error::TlsHandshakeFailed`
(in `eph-core/include/eph/core/error.hpp`) bubbling up through
`ReconnectOrchestrator`, and alert on a sustained spike in
`net.reconnect.failures` (see `docs/observability-metrics.md`).

---

## What `verify_peer` does

When `TlsConfig::verify_peer = true` (the default in
`tls_constants.hpp`), the TLS session does three things:

1. Loads CA trust anchors. If `ca_cert_path` is empty,
   `SSL_CTX_set_default_verify_paths` is called and **must** succeed —
   on failure, `tls_session.hpp` returns `Error::TlsHandshakeFailed`
   rather than continuing insecurely.
2. Sets `SSL_VERIFY_PEER` on the SSL context, so aws-lc validates the
   chain back to a trust anchor and rejects unknown / expired /
   revoked certs at handshake time.
3. Requires SNI to be set (`SSL_set_tlsext_host_name` in
   `tls_session.hpp`). With `verify_peer = true`, a missing SNI is a
   fatal error rather than a warning, because the server uses SNI to
   choose which cert to present and verification would otherwise be
   against the wrong cert.

Setting `verify_peer = false` skips all three steps. The library will
log a warning via `TlsConfig::warnings()` —
`verify_peer=false -- server certificate
will NOT be validated (vulnerable to MITM attacks)`. This setting
exists for local self-signed test servers and **must not** ship to
production.

---

## When to use a CA bundle path

Default: leave `ca_cert_path` empty and inherit from the OS trust
store. On Linux this typically resolves to
`/etc/ssl/certs/ca-certificates.crt` (Debian/Ubuntu) or
`/etc/pki/tls/certs/ca-bundle.crt` (RHEL/Amazon Linux).

Override only when:

- **Air-gapped colo with custom trust anchors.** Some HFT colocation
  setups ship with a stripped-down OS image where the global CA bundle
  is missing. Provide your own bundle:
  ```cpp
  eph::net::TlsConfig tls{
      .hostname     = "stream.binance.com",
      .ca_cert_path = "/etc/eph/ca-bundle.pem",  // PEM-concatenated
      .verify_peer  = true,
  };
  ```
  See `tls_constants.hpp` (`TlsConfig::ca_cert_path`) for the field
  semantics.

- **Vendor-supplied private CA.** The exchange operates a separate
  private FIX/ITCH gateway behind their own CA (rare; mostly Tier-1
  prime brokers). Use the vendor's published CA bundle and pin only
  the trust anchor file.

- **Intermediate-CA validation in restricted environments.** Some
  managed services ship without a path to certain commercial roots
  (e.g. AWS Nitro Enclaves). Drop the missing root into
  `ca_cert_path` to avoid `Error::TlsHandshakeFailed` with a
  `certificate verify failed` detail.

If `ca_cert_path` is set but `verify_peer = false`,
`TlsConfig::warnings()` emits `ca_cert_path is set but
verify_peer=false -- CA certificate will be loaded but not used for
verification`. This combination is almost always a misconfiguration.

---

## When to use SPKI pinning

`pinned_spki_sha256` (in `TlsConfig`) is a list of base64-encoded
SHA-256 hashes of the peer certificate's SPKI (SubjectPublicKeyInfo).
After the TLS handshake, the actual peer SPKI hash is computed via
`spki_pin::compute_spki_sha256_b64` and matched against the list via
`spki_pin::matches_any_pin` (both in `tls_constants.hpp`).

Default behaviour on **mismatch**: when no `on_pin_mismatch` callback
is set, the connection is **rejected** with
`Error::TlsHandshakeFailed` (in `tls_session.hpp`, search for the
`SPKI pin mismatch` log line). Configure the callback only if you
understand what overriding means:

```cpp
eph::net::TlsConfig tls{
    .hostname            = "private-md.broker.example",
    .pinned_spki_sha256  = {"r/mIkG3eEpVdm+u/ko/cwxzOMo1bk4TyHIlByibiA5E="},
    .on_pin_mismatch = [](std::string_view actual_hash) {
        SPDLOG_WARN("SPKI mismatch: actual={}", actual_hash);
        return false;  // hard-fail; flip to true ONLY for staged rotation
    },
};
```

Use cases where pinning is genuinely the right call:

- **Private interconnect to a single venue gateway.** You own the
  endpoint and can rotate the cert in a coordinated cutover window.
  Pin both the current SPKI and the next SPKI before rotation; remove
  the old one after.

- **Mutual TLS to a counterparty.** Same shape as above: pinning is
  bidirectional trust, not just transport authentication.

- **Defence-in-depth for a static internal cert.** Hash-pin the cert
  baked into a development image so a leaked CA root in the test
  environment can't silently MITM.

Use cases where pinning is **wrong**:

- **Any AWS / Cloudflare / Akamai-fronted public endpoint.** The
  cert is provisioned and rotated by the CDN, not the exchange. The
  exchange itself usually cannot tell you the next SPKI hash before
  it appears on the wire. A pin here turns the next rotation into a
  trading outage.

- **Multi-region failover endpoints.** If `stream.binance.com`
  resolves to different edges in different regions and each edge
  presents a different leaf cert (common for ACM-issued certs), a
  single SPKI pin won't cover all of them — you'd need to pin the
  entire intermediate set, which defeats the purpose.

- **Endpoints whose rotation cadence you don't know.** Don't guess.
  Trust the public PKI and the OS trust store.

---

## Per-venue notes

The four primary venues are all behind public CDNs / managed cert
services, with frequent (often automated) rotation. The library
defaults — `verify_peer = true`, empty `ca_cert_path`, empty
`pinned_spki_sha256` — are correct for all four.

### Binance

- Endpoints: `stream.binance.com:9443`, `fstream.binance.com:443`,
  `api.binance.com:443`, plus `*-sbe.binance.com` for SBE feeds.
- Cert provisioning: managed by Binance's CDN provider, frequent
  automatic rotation. Hostnames are virtual-hosted — SNI is required.
- Recommendation: **do not pin**. Trust the system store.
- Operational note: Binance forces a 24h reconnect on every WS
  session, so rotation events are essentially indistinguishable from
  routine reconnects in your logs — another reason not to pin.

### OKX

- Endpoints: `ws.okx.com:8443` (public), `wspap.okx.com:8443` (paper),
  `aws.okx.com:443` (REST), `www.okx.com:443`.
- Cert provisioning: CDN-fronted, automated rotation.
- Recommendation: **do not pin**.

### Bybit

- Endpoints: `stream.bybit.com:443` (`/v5/public/...`,
  `/v5/private`), `api.bybit.com:443`.
- Cert provisioning: CDN-fronted, automated rotation.
- Recommendation: **do not pin**.

### Coinbase Advanced Trade

- Endpoints: `advanced-trade-ws.coinbase.com:443`,
  `api.coinbase.com:443`.
- Cert provisioning: AWS Certificate Manager (ACM) / CloudFront.
  ACM rotation can fire at any time and is opaque to the consumer.
- Recommendation: **do not pin**.

If any of these venues publishes a stable-issuer commitment in the
future (none does today, to the author's knowledge), revisit this
table. Until then, the system store + `Error::TlsHandshakeFailed`
alerting via the orchestrator is the right model.

---

## Cert rotation & alerting

Detect rotations through metrics, not through hardcoded pins.

The relevant signals are emitted by the
`ReconnectOrchestrator<S>` and the underlying `Stream`:

- `net.reconnect.failures` (counter, `eph::net::ReconnectMetric`) —
  spikes when factory or attach hooks fail. Cert-verify failures
  surface here as a sustained climb on every retry.
- `net.stream.tls.handshake_count` vs `net.stream.tls.resume_count`
  (counters, `eph::net::StreamMetric`) — a sudden swing from
  resumption-heavy back to full-handshake-only traffic often
  correlates with an upstream cert rotation invalidating the
  resumption ticket cache.

Wire both via `publish_reconnect_metrics` /
`publish_metrics` (`docs/observability-metrics.md`) into your TSDB and
alert on:

```text
rate(net.reconnect.failures[5m]) > 0
  AND label_match("error_class", "TlsHandshakeFailed")
```

The `ReconnectOrchestrator` returns the first
`std::expected<>` error verbatim from the factory; map
`Error::TlsHandshakeFailed` to a `tls_handshake_failed` tag in your
sink wrapper so the alert can distinguish cert problems from generic
network failures.

---

## Anti-patterns

- **`verify_peer = false` in production.** The
  `TlsConfig::warnings()` helper exists specifically to flag this.
  Run it on startup and refuse to boot if any warning fires.

- **Hardcoded SPKI hash for an AWS- / Cloudflare-fronted venue.**
  This survives until the next ACM rotation, then turns into a
  trading halt. If you need pinning for compliance, pin the issuer
  (intermediate CA) only as part of a rotation playbook and pair it
  with an `on_pin_mismatch` callback that pages on-call.

- **Skipping SNI.** Leaving `TlsConfig::hostname` empty with
  `verify_peer = true` is a hard error in `tls_session.hpp` (the
  `Failed to set SNI hostname` path). With `verify_peer = false` it
  is a warning. Always set the SNI hostname to the actual venue host.

- **Mixing `ca_cert_path` with `verify_peer = false`.** Loads the
  bundle but never uses it; gives the false impression that
  validation is happening. `warnings()` flags this.

- **Catching `Error::TlsHandshakeFailed` and reconnecting silently.**
  Cert problems are a different operational class from transient
  network errors. The orchestrator already retries; what's missing
  is the alert. Surface the failure to your sink before the retry,
  not after.

- **Setting `client_cert_path` without `client_key_path` (or vice
  versa).** `validate()` rejects this at config-build time — don't
  paper over the failure by swallowing the `expected<>`.

---

## Putting it together — production recipe

```cpp
#include "eph/net/detail/tls_constants.hpp"

eph::net::TlsConfig tls_for_venue(std::string_view venue_host) {
    eph::net::TlsConfig cfg{
        .hostname    = std::string(venue_host),
        .verify_peer = true,        // hard-fail on bad chains
        // .ca_cert_path left empty — system trust store
        // .pinned_spki_sha256 left empty — no pin (CDN rotation)
        .handshake_timeout = std::chrono::milliseconds{2000},
    };
    if (auto ok = cfg.validate(); !ok) {
        std::abort();  // misconfig caught at boot, not at first connect
    }
    for (auto& w : cfg.warnings()) {
        SPDLOG_WARN("tls config warning: {}", w);
    }
    return cfg;
}
```

Drop this into your venue adapter factory. The same shape works for
all four primary venues; differences are confined to
`StreamConfig::remote` / `tls.hostname` / `ws.path` (see
`docs/production-config.md`).

---

## See also

- `docs/observability-metrics.md` — `net.reconnect.*` and
  `net.stream.tls.*` counters used to detect rotation events.
- `docs/production-config.md` — full `StreamConfig` profiles
  (low-latency, high-throughput, market-data) with `verify_peer =
  true` baked in.
- `docs/troubleshooting.md` — TLS error decoder
  (`certificate verify failed`, `self-signed certificate`,
  `TlsHandshakeFailed`).
- `eph-net/include/eph/net/detail/tls_constants.hpp` — `TlsConfig`,
  `validate()`, `warnings()`, the SPKI helpers.
- `eph-net/include/eph/net/detail/tls_session.hpp` — `verify_peer`
  enforcement, SPKI pin verification, hard-fail-on-mismatch policy.

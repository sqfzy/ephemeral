# Venue Adapter Cookbook

> How to add a new crypto venue (Kraken, Deribit, dYdX, …) to `eph`.
> A typical WebSocket-over-TLS adapter is **~150 LOC** and takes about
> **30 minutes** end-to-end once you have the venue's protocol docs in
> front of you.

This cookbook distils the four adapters that already ship with `eph`:

| Venue        | Reference file                                                      |
|--------------|---------------------------------------------------------------------|
| Binance spot | `examples/binance_latency.cpp` (DPDK datapath, hand-rolled loop)    |
| OKX V5       | `tests/integration/test_okx_adapter.cpp` (kernel + orchestrator)    |
| Bybit V5     | `tests/integration/test_bybit_adapter.cpp` (kernel + orchestrator)  |
| Coinbase ATX | `tests/integration/test_coinbase_adapter.cpp` (kernel + JWT)        |

If you've never touched `eph` before, read [`docs/architecture.md`](architecture.md)
and [`docs/poller-guide.md`](poller-guide.md) first — this guide assumes you know
what `Stream<C>`, `Poller`, and `KernelTcpStream<WsCodec, EnableTls>` are.

---

## TL;DR

```cpp
using TlsWsStream = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>;
using Orch        = en::ReconnectOrchestrator<TlsWsStream>;

auto poller  = ek::KernelPoller::create({}).value();
auto factory = [&]() -> std::expected<Orch::StreamPtr, eph::core::ErrorInfo> {
    auto sr = TlsWsStream::create(make_stream_config());
    if (!sr) return std::unexpected(sr.error());
    (*sr)->on_message = on_message;          // user frame sink
    return std::move(*sr);
};

Orch orch{
    en::ReconnectConfig{.policy = {/*…*/}, .auto_detect_via_state = true},
    factory,
    /*on_disconnect=*/{},
    /*on_reconnect=*/[&](uint32_t /*attempt*/, uint64_t /*ns*/) {
        send_subscribe();                    // replay venue subscription
        orch.note_subscribe_replay();        // metric: T2.11
    },
    /*attach=*/[&](TlsWsStream* s) { return poller->add(s); },
    /*detach=*/[&](TlsWsStream* s) { (void)poller->remove(s); },
};

(void)orch.start(eph::utils::TSC::now());
while (running) {
    (void)poller->poll(10ms);
    orch.tick(eph::utils::TSC::now());
}
```

A venue adapter is the orchestrator + a venue-specific `factory_fn` +
two callbacks. Everything else (TLS handshake, WebSocket upgrade, frame
reassembly, exponential backoff, jitter, attach/detach to the poller)
is supplied by the library.

---

## Prerequisites

The library pieces a venue adapter consumes:

| Piece                                          | Where it lives                                                          | Purpose                                                  |
|------------------------------------------------|-------------------------------------------------------------------------|----------------------------------------------------------|
| `KernelTcpStream<WsCodec, true>`               | `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp`                  | TCP + TLS 1.3 + WS upgrade in one factory call           |
| `KernelPoller`                                 | `eph-net-kernel/include/eph/net/kernel/poller.hpp`                      | epoll loop driver                                        |
| `ReconnectOrchestrator<S>`                     | `eph-net/include/eph/net/reconnect_orchestrator.hpp`                    | lifecycle state machine + backoff + metrics             |
| `SignedRequest<Traits>`                        | `eph-net/include/eph/net/signed_request.hpp`                            | HMAC-SHA256 REST signing (Binance / OKX / Bybit)         |
| `HttpClient<S>`                                | `eph-net/include/eph/net/http_client.hpp`                               | Optional: REST snapshot fetch over the same TLS stream   |
| `WsCodec`                                      | `eph-codec/include/eph/codec/ws_codec.hpp`                              | RFC 6455 frame parse / encode, masking, ping/pong/close  |
| `TlsWsEchoServer`                              | `tests/support/tls_ws_echo_server.hpp`                                  | In-process test fixture for offline integration tests    |

DPDK adapters use `DpdkTcpStream<WsCodec, true>::create_and_attach(cfg, plat)`
in place of `KernelTcpStream::create(cfg) + poller->add()` — see the
"DPDK datapath" note in Step 3.

---

## Step 1: Identify the venue's protocol shape

Before writing a line of code, fill in this table from the venue's docs:

| Question                                       | Example: OKX V5 public                          |
|------------------------------------------------|-------------------------------------------------|
| Wire protocol?                                 | TLS 1.3 + WS                                    |
| Endpoint host + port                           | `ws.okx.com:8443`                               |
| WS path                                        | `/ws/v5/public`                                 |
| Subscription protocol (text frame? binary?)    | text JSON, `{"op":"subscribe", "args":[…]}`     |
| Subscribe-ack shape                            | `{"event":"subscribe", "arg":{…}}`              |
| Heartbeat (server pings? client pings?)        | server ping every 30 s, client must pong        |
| Idle close behaviour                           | venue pushes Close 1011 after 24 h (spot)       |
| Auth at HTTP-Upgrade level (REST-style)?       | **No**                                          |
| Auth at WS-application level (in-band)?        | yes — `op:login` after upgrade                  |
| Auth at JWT level (subscribe payload)?         | n/a                                             |
| REST snapshot endpoint (for book recovery)     | `https://www.okx.com/api/v5/market/books`       |

The four reference adapters cover the four canonical authentication
shapes — you almost never need a fifth.

| Auth shape                          | HTTP-Upgrade headers? | In-band frame? | JWT in payload? | Reference         |
|-------------------------------------|-----------------------|----------------|-----------------|-------------------|
| Querystring HMAC + headers          | `X-MBX-*` (REST)      | no             | no              | Binance           |
| In-band `op:login` after upgrade    | no                    | yes (HMAC-SHA256) | no           | OKX, Bybit V5     |
| JWT carried in subscribe payload    | no                    | no             | yes (ES256)     | Coinbase ATX      |
| Pure public market-data            | no                    | no             | no              | Binance market WS |

---

## Step 2: Pick (or write) a `SignedRequest` Traits

For HMAC-SHA256 venues (the overwhelming majority of crypto exchanges),
`eph::net::SignedRequest<Traits>` already covers the three big ones:

| Trait                  | Pre-MAC string                                       | Wire format | Header set                                                 |
|------------------------|------------------------------------------------------|-------------|------------------------------------------------------------|
| `BinanceSignTraits`    | `query` (already contains `timestamp=<ms>`)          | hex         | `X-MBX-SIGNATURE`, `X-MBX-TIMESTAMP`                       |
| `OkxSignTraits`        | `ts_iso + METHOD + path + body`                      | base64      | `OK-ACCESS-SIGN`, `OK-ACCESS-TIMESTAMP`                    |
| `BybitSignTraits`      | `ts_ms + api_key + recv_window + (query OR body)`    | hex         | `X-BAPI-SIGN`, `X-BAPI-TIMESTAMP`, `X-BAPI-RECV-WINDOW`    |

Definitions live at:

- `eph-net/include/eph/net/signed_request.hpp:157` — `BinanceSignTraits`
- `eph-net/include/eph/net/signed_request.hpp:219` — `OkxSignTraits`
- `eph-net/include/eph/net/signed_request.hpp:272` — `BybitSignTraits`

Writing a new HMAC-SHA256 trait is ~30 LOC: a `sign_header` / `ts_header`
constexpr pair, a `build_to_sign(...)` static that concatenates the
venue's pre-MAC string in declaration order, and a `format_signature(...)`
that renders 32 raw bytes as either lowercase hex (`detail::render_hex_lower`)
or RFC 4648 base64 (`detail::render_base64`). The three existing traits
in `signed_request.hpp` are all under 50 lines each — copy whichever
matches your venue's encoding most closely and adjust.

> **Important caveat.** `SignedRequest<Traits>` is **HMAC-SHA256 only**.
> Venues using HMAC-SHA512 (Kraken, Bitfinex) or asymmetric signatures
> (Coinbase ATX uses ES256 JWT, dYdX uses Ed25519) need a separate primitive.
> `test_coinbase_adapter.cpp:26-30` documents the gap explicitly: ES256 JWT
> is a Tier 3 future feature; the adapter today round-trips a caller-built
> JWT through TLS+WS and asserts the bytes arrived intact.

### When the venue authenticates **in-band** (OKX, Bybit V5)

Both OKX V5 and Bybit V5 push private-channel authentication into a
WS-application-layer message (`{"op":"login", …}`) **after** the upgrade,
not into the HTTP request. `SignedRequest<Traits>` produces HTTP headers,
so it does not directly apply at the upgrade boundary — see the explicit
note in `test_okx_adapter.cpp:33-38` and `test_bybit_adapter.cpp:9-16`.

The pattern: still construct a `SignedRequest<OkxSignTraits>` to get the
correctly-formatted MAC string, then **inline** that MAC into your
in-band login JSON. The unit tests for `SignedRequest` (in
`eph-net/tests/test_signed_request.cpp`) cover the signing primitive in
isolation; the integration test covers the WS-level reconnect/replay
cycle.

---

## Step 3: Build the `factory_fn`

The orchestrator owns the lifecycle but doesn't know how to dial. Your
factory does — and gets re-invoked on every reconnect attempt.

Kernel + TLS + WS, distilled from `test_okx_adapter.cpp:85-105`:

```cpp
ek::StreamConfig make_stream_config(uint16_t port, std::string_view sni) {
    ek::StreamConfig cfg{};
    cfg.remote                = en::SocketAddr{en::Ipv4Addr{/*…*/}, port};
    cfg.reasm_capacity        = 64 * 1024;     // WsCodec reassembly window
    cfg.connect_timeout       = 2s;
    cfg.tcp_nodelay           = true;
    cfg.tls.hostname          = std::string{sni};
    cfg.tls.verify_peer       = true;          // false ONLY for in-process test certs
    cfg.tls.handshake_timeout = 2s;
    cfg.ws_path               = "/ws/v5/public";
    cfg.ws_host               = std::string{sni};
    cfg.ws_timeout            = 2s;
    cfg.ws_permessage_deflate = false;         // most venues don't echo deflate
    return cfg;
}
```

A non-empty `cfg.ws_path` triggers the RFC 6455 client handshake
**inside `KernelTcpStream::create()`** — you do not call it yourself.
On return, the stream is post-TLS, post-upgrade, and ready to send the
first application frame.

The factory itself just wires the user's frame sink onto the new stream:

```cpp
auto factory = [&]() -> std::expected<Orch::StreamPtr, eph::core::ErrorInfo> {
    auto sr = TlsWsStream::create(make_stream_config(port, host));
    if (!sr) return std::unexpected(sr.error());
    (*sr)->on_message = on_message;     // re-bind the sink each connect
    return std::move(*sr);
};
```

> **DPDK datapath.** Replace `TlsWsStream::create(cfg)` with
> `edpdk::DpdkTcpStream<WsCodec, true>::create_and_attach(cfg, *plat)` —
> the DPDK factory does the equivalent of `create() + poller.add() +
> FlowDirector install + ICMP register` in one call (so on the DPDK side
> your `attach`/`detach` callbacks become no-ops or call the equivalent
> `unregister` helpers). See `examples/binance_latency.cpp:500-547` for
> the full DPDK wiring including ARP, DPDK-native DNS, and src-port
> picking.

---

## Step 4: Wire the orchestrator

Three mandatory callbacks (factory + attach + detach) and two optional
callbacks (on_disconnect + on_reconnect):

```cpp
auto attach = [&](TlsWsStream* s) { return poller->add(s); };
auto detach = [&](TlsWsStream* s) { (void)poller->remove(s); };

auto on_reconnect = [&](uint32_t attempt, uint64_t dur_ns) {
    SPDLOG_INFO("reconnected attempt={} duration_ns={}", attempt, dur_ns);
    send_subscribe();                // venue-specific replay (Step 5)
    orch.note_subscribe_replay();    // user-asserted metric, post-ACK
};

Orch orch{
    en::ReconnectConfig{
        .policy = {.initial_backoff = 250ms,
                   .max_backoff     = 10s,
                   .multiplier      = 2.0,
                   .jitter_factor   = 0.25,
                   .max_attempts    = 0},     // 0 = unlimited
        .auto_detect_via_state = true,
    },
    factory,
    /*on_disconnect=*/{},
    on_reconnect,
    attach,
    detach,
};
```

`auto_detect_via_state = true` lets `tick()` notice when the stream's
state leaves `Established` (FIN, RST, TLS alert, WS Close) and roll
itself into `Backoff` without a `mark_disconnected()` call — see
`reconnect_orchestrator.hpp:283-289`.

### The "can't capture orch in its own callback" trap

The orchestrator is constructed **after** its callbacks are written, so
the lambda capture cannot reference the orchestrator by name unless you
defer (raw pointer + post-init assignment). The cleanest fix is to make
the on_reconnect callback a simple counter and do the subscribe send
imperatively from the main loop — see the inline comment in
`test_okx_adapter.cpp:196`.

A more production-grade pattern: hold the orchestrator behind a
`std::unique_ptr<Orch>`, capture the pointer-to-pointer (or a
`std::reference_wrapper<std::unique_ptr<Orch>>`) in the lambda, and
construct after binding:

```cpp
std::unique_ptr<Orch> orch_ptr;
auto on_reconnect = [&orch_ptr, &send_subscribe](uint32_t a, uint64_t) {
    send_subscribe();
    if (orch_ptr) orch_ptr->note_subscribe_replay();
};
orch_ptr = std::make_unique<Orch>(/* … on_reconnect … */);
```

---

## Step 5: Subscribe replay

Subscribe replay is the venue-specific glue: every time the WS stream
comes up — first connect, every reconnect — you must re-send the
subscription messages because crypto venues do not persist subscription
state across TCP sessions.

Where to send: **inside `on_reconnect`**, which fires once per
`Connecting → Connected` transition (including the very first connect
from `start()`).

```cpp
constexpr std::string_view kOkxSubscribe =
    R"({"op":"subscribe","args":[{"channel":"books","instId":"BTC-USDT"}]})";

auto on_reconnect = [&](uint32_t /*attempt*/, uint64_t /*ns*/) {
    auto* s = orch.current();           // safe: callback runs INSIDE Connected transition
    auto frame = encode_ws_text(kOkxSubscribe);
    if (auto r = s->send(frame); !r) {
        SPDLOG_ERROR("subscribe-replay send failed: {}", r.error().detail);
        // Stream will go non-Established and the orchestrator will retry.
        return;
    }
    // Bump after the WS frame is on the wire. For ACK-bearing protocols,
    // bump only after parsing the ack — see Step 6.
    orch.note_subscribe_replay();
};
```

`note_subscribe_replay()` is at `reconnect_orchestrator.hpp:491`. It is
**user-asserted**: the orchestrator cannot tell whether the venue
accepted your subscribe, so the metric only flips when you say so. The
counter shows up as `net.reconnect.subscribe_replay_count` — pair it
with `net.reconnect.count` to get the replay success rate (see
`docs/observability-metrics.md:84`).

For ACK-bearing venues (OKX `event:subscribe`, Bybit `success:true`,
Coinbase `channel:subscriptions`) prefer to bump the metric only inside
the message handler, when the ack actually parses — that turns the
counter into a true "subscriptions restored" signal rather than a
"subscribe sent" signal.

---

## Step 6: Heartbeat / Close-1011 handling

Three independent disconnect paths:

| Path                                         | How `eph` reacts                                                 |
|----------------------------------------------|------------------------------------------------------------------|
| Server WS Close (1000 normal, 1011 internal) | `WsCodec` walks state to `Closed`; orchestrator state ≠ Established |
| TCP RST / FIN                                | TCP layer flags `Closed`; same observable for the orchestrator    |
| TLS alert                                    | TLS layer surfaces error → stream closes                          |

In every case, with `auto_detect_via_state = true` the orchestrator's
next `tick()` walks `Connected → Backoff → Connecting → Connected` on
its own. You don't need to do anything except keep calling `tick()` and
`poll()` from your main loop.

**Application-level disconnects** (e.g. a stale auth token) do not
necessarily flip the stream state — for those, call
`orch.mark_disconnected(reason)` explicitly. See the docstring at
`reconnect_orchestrator.hpp:404-406`.

For richer diagnostics, register an `OnReconnectEvent` with
`orch.set_on_reconnect_event(fn)` — you'll get a structured
`ReconnectEvent{kind, attempt, event_tsc, duration_ns, error}` for
every transition (`AttemptStarted`, `AttemptFailed`, `AttachFailed`,
`Connected`, `BackoffScheduled`, `Failed`). The full event taxonomy is
at `reconnect_orchestrator.hpp:213-232`.

---

## Step 7: REST snapshot fetch (optional)

Pure WebSocket adapters never need this. Order-book adapters do — Binance
spot, for example, requires `GET /api/v3/depth?symbol=BTCUSDT&limit=1000`
to seed the book before applying incremental WS updates.

Build a one-shot HTTPS client over the same kernel TLS stream:

```cpp
using TlsStream = ek::KernelTcpStream<eph::codec::HttpCodec, /*EnableTls=*/true>;

ek::StreamConfig cfg{};
cfg.remote                = en::SocketAddr{api_ip, 443};
cfg.tls.hostname          = "api.binance.com";
cfg.tls.verify_peer       = true;
cfg.connect_timeout       = 5s;
// no ws_path → plain HTTPS, no WS upgrade

auto stream = TlsStream::create(cfg).value();
poller->add(stream.get()).value();

en::HttpClient<TlsStream> cli{std::move(stream), {}};

en::SignedRequest<en::BinanceSignTraits> sr{eph::net::HmacSha256Key{secret}};
auto bundle = sr.headers_for_query(/*query_with_timestamp=*/"symbol=BTCUSDT&timestamp=…",
                                   /*ts_ms=*/now_ms);

en::HttpClient<TlsStream>::Request req{
    .method  = "GET",
    .path    = "/api/v3/account?…&signature=…",
    .headers = std::move(bundle.headers),
    .body    = {},
};
auto rsp = cli.request(req,
                       /*poll_fn=*/[&] noexcept { (void)poller->poll(0ms); },
                       /*timeout=*/500ms);
```

`HttpClient<S>` is at `eph-net/include/eph/net/http_client.hpp:162`. Note
the explicit constraint of the embedded HTTP/1.1 parser (per phase-9
scope decision): **no chunked, no Transfer-Encoding, no cookies, no
Expect: 100-continue, no redirect**. If your venue's REST API needs any
of those, the request will fail with `Error::CodecBad` — most modern
crypto REST APIs comply.

For a public (unsigned) GET, just skip the `SignedRequest` step and
build the headers directly.

---

## Step 8: Test it locally

Every venue adapter ships with a `tls_ws_echo_server`-backed integration
test. The fixture runs in-process, terminates a self-signed ECDSA P-256
cert, and lets you inject a venue-shaped message handler — no external
service, no Python `websockets` install, no cert files in the repo.

The pattern, condensed from `test_okx_adapter.cpp:110-160`:

```cpp
TEST(MyVenueAdapterIntegration, HappyPathConnectAndReconnect) {
    eph::utils::TSC::init();

    eph::test::TlsWsEchoServer server;
    server.set_message_handler(
        [](std::span<const uint8_t> payload, uint8_t opcode)
            -> std::optional<std::vector<uint8_t>> {
            if (opcode != 0x1) return std::nullopt;       // text only
            std::string_view sv{
                reinterpret_cast<const char*>(payload.data()), payload.size()};
            if (sv.find("\"op\":\"subscribe\"") == std::string_view::npos)
                return std::nullopt;                       // fall through to echo
            return std::vector<uint8_t>(kMyVenueAck.begin(), kMyVenueAck.end());
        });
    server.enable_request_capture(true);
    server.start();

    auto poller = ek::KernelPoller::create({}).value();
    /* … build factory, attach, detach, on_reconnect as above … */
    Orch orch{ /* … */ };

    auto sr = orch.start(eph::utils::TSC::now());
    if (!sr || orch.state() != en::ReconnectState::Connected) {
        GTEST_SKIP() << "in-process TLS handshake failed on this host";
    }

    /* … send subscribe, drive_until ack … */

    server.send_close_to_all(1011);     // simulate venue Close
    /* … drive_until reconnect, verify replay … */

    EXPECT_GE(orch.reconnect_count(), 2u);
    EXPECT_EQ(orch.reconnect_failures(), 0u);
    EXPECT_GE(server.messages_received(), 2u);
}
```

The fixture's API surface (all in `tests/support/tls_ws_echo_server.hpp`):

- `start()` / `stop()` — accept loop lifecycle
- `set_message_handler(fn)` — install venue-shaped responder (line 139)
- `enable_request_capture(true)` — record HTTP-Upgrade requests for header assertions (line 148)
- `send_close_to_all(1011)` — push a real WS Close frame to every active session (line 180)
- `messages_received()` / `accepted_sessions()` / `captured_requests()` — assertion helpers

The `GTEST_SKIP` on initial-handshake failure (line 220 of
`test_okx_adapter.cpp`) is the canonical pattern for handshake-sensitive
hosts — keeps the suite informative when run on aws-lc-less or
strict-verify environments.

To drop the test in: copy `tests/integration/test_okx_adapter.cpp`,
rename to `test_<venue>_adapter.cpp`, swap the venue payloads — the
glob in `tests/integration/xmake.lua` picks it up automatically.

---

## Per-venue notes

### Binance

- **WS endpoint**: `wss://stream.binance.com:9443/ws/<symbol>@<channel>`
- **Auth (REST)**: `BinanceSignTraits` — querystring HMAC + `X-MBX-SIGNATURE` / `X-MBX-TIMESTAMP`; caller adds `X-MBX-APIKEY`.
- **Auth (WS)**: public market-data WS is unauthenticated; user-data streams use a `listenKey` obtained via REST (no in-band signing).
- **Heartbeat**: server sends ping every 3 min, client must pong within 10 min.
- **Reference**: `examples/binance_latency.cpp` — the canonical hand-rolled DPDK adapter, exponential-backoff reconnect loop at line 467.

### OKX V5

- **WS endpoint**: `wss://ws.okx.com:8443/ws/v5/public` (also `/private`, `/business`)
- **Auth (REST)**: `OkxSignTraits` — base64 MAC over `ts_iso + METHOD + path + body`.
- **Auth (WS)**: in-band `{"op":"login","args":[{"apiKey", "passphrase", "timestamp", "sign"}]}` AFTER upgrade. **Not an HTTP header** — see `test_okx_adapter.cpp:33-38`.
- **Heartbeat**: client should send `ping` every 25 s; missing two = server closes.
- **Idle close**: pushes Close 1011 occasionally — orchestrator handles it transparently.
- **Reference**: `tests/integration/test_okx_adapter.cpp`.

### Bybit V5

- **WS endpoint**: `wss://stream.bybit.com/v5/public/spot` (also `/linear`, `/option`, `/private`)
- **Auth (REST)**: `BybitSignTraits` — hex MAC over `ts_ms + api_key + recv_window + payload`. Note the public api_key is part of the MAC input (uncommon).
- **Auth (WS)**: in-band `{"op":"auth","args":[apiKey, expires, signature]}` after upgrade. Public channels need none.
- **Idle close**: spot every 24 h, derivatives every 1 h — orchestrator reconnects + replays.
- **Reference**: `tests/integration/test_bybit_adapter.cpp`.

### Coinbase Advanced Trade

- **WS endpoint**: `wss://advanced-trade-ws.coinbase.com/`
- **Auth**: ES256 **JWT** carried in the subscribe payload (`{"type":"subscribe","channel":"user","jwt":"…"}`), NOT in headers and NOT HMAC.
- **Why no `SignedRequest<Traits>`**: ES256 over a P-256 EC private key is asymmetric crypto; `SignedRequest` is HMAC-SHA256-only. JWT signing primitive is not yet in the library — see `test_coinbase_adapter.cpp:26-30`. Until it lands, build the JWT externally (e.g. using aws-lc directly) and pass it as a `std::string` into your subscribe message.
- **Token rotation**: per-connect JWT issuance is the canonical pattern (Coinbase tokens are short-lived). The reconnect test at `test_coinbase_adapter.cpp:299-306` demonstrates: capture a fresh JWT inside `on_reconnect`, splice it into the subscribe payload, send.
- **Reference**: `tests/integration/test_coinbase_adapter.cpp`.

---

## Anti-patterns

1. **Don't capture `orch` by reference inside its own callbacks.** The
   orchestrator is constructed after its callbacks are bound — direct
   capture is a use-before-init. Use a `std::unique_ptr<Orch>*` indirection
   or do the work imperatively from the main loop. See
   `test_okx_adapter.cpp:196` for the inline note.

2. **Don't share a `SignedRequest<Traits>` across two venues.** Each venue's
   pre-MAC string is different by definition — sharing the type means
   sharing the secret, and the wire format won't match the other venue's
   verifier. Construct one `SignedRequest` per venue.

3. **Don't roll your own backoff loop.** `ReconnectPolicy` (the math
   primitive) and `ReconnectOrchestrator` (the lifecycle wrapper)
   already handle exponential backoff with jitter and attempt-cap.
   `examples/binance_latency.cpp` predates the orchestrator and uses
   `ReconnectPolicy` directly in a hand-rolled loop — that pattern is
   still legal but new adapters should prefer the orchestrator.

4. **Don't bump `note_subscribe_replay()` from `factory_fn`.** The factory
   runs *before* attach + Connected — bumping there counts attempts, not
   restorations. Bump from `on_reconnect` (or, better, from the message
   handler when the ack parses).

5. **Don't set `tls.verify_peer = false` in production.** The integration
   tests do because they terminate a self-signed cert in-process. Real
   venues all present valid certificates; disabling verification disables
   the only guarantee that you're talking to the venue and not a router.

6. **Don't re-resolve DNS on every reconnect** (DPDK datapath only). DNS is
   slow and adds noise to a latency probe; the L2 gateway is stable on a
   single-segment LAN. Resolve once at startup and reuse — see the
   reconnect-loop comment at `examples/binance_latency.cpp:443-453`.

---

## See also

- [`docs/observability-metrics.md`](observability-metrics.md) — full metric catalog including `net.reconnect.subscribe_replay_count` (T2.11)
- [`docs/observability-guide.md`](observability-guide.md) — how to wire the orchestrator's metrics into a Prometheus / OTel sink
- [`docs/poller-guide.md`](poller-guide.md) — single- and multi-connection poller patterns
- [`docs/operations-runbook.md`](operations-runbook.md) — production deployment notes
- [`docs/production-config.md`](production-config.md) — recommended `StreamConfig` / `ReconnectConfig` values for live trading
- `examples/binance_latency.cpp` — DPDK reference adapter (hand-rolled reconnect loop)
- `examples/reconnect_orch_demo.cpp` — minimal kernel orchestrator example
- `tests/integration/test_okx_adapter.cpp` / `test_bybit_adapter.cpp` / `test_coinbase_adapter.cpp` — three more references with progressively richer auth shapes

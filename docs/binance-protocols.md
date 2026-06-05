# Binance Market Data Protocols with ephemeral

How to connect to Binance's three market data protocols using ephemeral, and when to use each.

## Protocol Landscape

| Protocol | Encoding | Endpoint | Latency | ephemeral Support |
|----------|----------|----------|---------|-------------------|
| WebSocket JSON | Text | `stream.binance.com` | Higher | Full (see `simple_hft.cpp`) |
| WebSocket SBE | Binary | `stream-sbe.binance.com` | Low | Transport ready, schema below |
| FIX 4.4 | Text/SBE | FIX gateway | Low (Top-of-Book) | Parser/builder ready, session layer TBD |

All three protocols run over **WebSocket over TLS** — ephemeral's core transport layer handles the connection, TLS handshake, and WebSocket framing identically regardless of payload encoding.

## 1. WebSocket JSON (Spot)

The simplest path. JSON payload is parsed by the application (bring your own JSON library).

> **Note (2026-04 doc refresh)**: this section's snippets predate the
> v3.3 / T3.19 reshape and reference fields from a retired
> `eph::net::TransportConfig` (`.remote_host` / `.use_tls` /
> `.skip_utf8_validation` / `.ping_interval` / `.pong_timeout` / etc.).
> The current API is `eph::net::kernel::StreamConfig` + the second
> `EnableTls` template parameter on `KernelTcpStream`; for runnable
> code see `examples/binance_book.cpp` and `examples/simple_hft.cpp`
> (kernel) or `examples/binance_latency.cpp` (DPDK). The snippets below
> are kept as protocol reference — substitute the modern config shape
> shown in `eph-net-kernel/README.md` when copy-pasting.

```cpp
// Connect to Binance spot bookTicker — modern shape
auto stream = eph::net::kernel::KernelTcpStream<
                  eph::codec::WsCodec, /*EnableTls=*/true>::create({
    .remote = eph::net::SocketAddr{ /* resolved IPv4 */, 443 },
    .tls    = { .hostname = "stream.binance.com", .verify_peer = true },
    .ws     = { .path = "/ws/btcusdt@bookTicker" },  // subscription via path
}).value();

// In RX callback: parse JSON with simdjson/rapidjson
stream->on_message = [](std::span<const uint8_t> app_frame) {
    // {"u":123,"s":"BTCUSDT","b":"67000.00","B":"0.5","a":"67010.00","A":"0.75","T":...,"E":...}
    // Parse with your preferred JSON library
    auto doc = simdjson::ondemand::parser{}.iterate(
        app_frame.data(), app_frame.size());
    auto bid = doc["b"].get_string();
    auto ask = doc["a"].get_string();
    // process...
};
```

**Full example**: See `examples/simple_hft.cpp` (connects to `fstream.binance.com` futures — change host/path for spot).

**Combined streams** (multiple symbols, one connection):
```cpp
// Up to 1024 streams per connection
cfg.ws.path = "/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker/solusdt@bookTicker";

// Latest-per-symbol delivery is now an application-layer concern: in your
// on_message callback, hash the symbol (e.g.
// `eph::json::binance::symbol_hash`) and keep only the most recent frame
// per bucket before dispatching to strategy logic. The previous
// `cfg.on_frame_filter` / `make_twophase_filter` transport hook was
// retired in v3.3 / T3.19 — see `phase-9-scope-decision.md` for the
// rationale.
```

## 2. WebSocket SBE (Binary)

Lower latency than JSON — fixed-offset binary encoding, zero-copy field access. Requires Ed25519 API Key.

### SBE Message Layout

SBE decoding is provided by the **`eph-sbe`** module — you no longer hand-copy
offsets. The module mirrors `eph-itch`'s zero-copy style but for SBE's
little-endian wire and its repeating-group / variable-length encoding. Field
offsets are derived from the **vendored authoritative schema**
`eph-sbe/schemas/spot_3_2.xml` (Binance spot SBE schema id=3, version=2).

> **Note (corrects an earlier illustrative layout):** a real Binance
> `BookTickerResponse` (template id 212) is **not** a flat fixed body. It is a
> `tickers` **repeating group**; prices/quantities are
> `mantissa64 × 10^exponent8` decimals (not a fixed `/1e8`), and `symbol` is a
> **`varString8`** (1-byte length + UTF-8), not a fixed `char[16]`. The
> `update_id`/`event_time` fields are not present in this message. Decode it
> with `eph-sbe`, which encodes the correct layout once.

Authoritative `BookTickerResponse` layout (`spot_3_2.xml`, `<sbe:message id="212">`):

```
messageHeader (8B):  blockLength u16 | templateId u16 | schemaId u16 | version u16
tickers group hdr (groupSize16Encoding, 4B):  blockLength u16(=34) | numInGroup u16
  per entry — fixed block (34B) then a trailing varString8 symbol:
    +0  priceExponent  int8                      value = mantissa × 10^exponent
    +1  qtyExponent    int8
    +2  bidPrice       int64  (optional, null=INT64_MIN)
    +10 bidQty         int64
    +18 askPrice       int64  (optional, null=INT64_MIN)
    +26 askQty         int64
    +34 symbol         varString8:  len u8 | UTF-8[len]
```

### Usage with Transport

```cpp
// Modern shape — see note at top of this file.
auto stream = eph::net::kernel::KernelTcpStream<
                  eph::codec::WsCodec, /*EnableTls=*/true>::create({
    .remote = eph::net::SocketAddr{ /* resolved IPv4 */, 443 },
    .tls    = { .hostname = "stream-sbe.binance.com" },
    .ws     = {
        .path           = "/ws/btcusdt@bookTicker",
        // Add custom headers via cfg.ws.extra_headers (vector<HttpHeader>)
        // — populate with X-MBX-APIKEY: YOUR_ED25519_KEY before create().
    },
}).value();

stream->on_message = [](std::span<const uint8_t> app_frame) {
    // Zero-copy SBE decode via eph-sbe — ~55ns/ticker (parse + group walk).
    auto view = eph::sbe::parse(app_frame.data(), app_frame.size());
    if (!view) return;  // truncated / non-SBE frame — skip safely
    namespace bt = eph::sbe::binance::book_ticker;
    (void)eph::sbe::binance::for_each_ticker(*view, [](const uint8_t* t) {
        std::optional<double> bid = bt::bid_price(t);  // nullopt if no bid
        std::optional<double> ask = bt::ask_price(t);
        std::string_view      sym = bt::symbol(t);     // zero-copy
        // process...
    });
};
```

### Performance Comparison

| Encoding | Decode Time | Payload Size (BBO) | Notes |
|----------|------------|---------------------|-------|
| JSON (simdjson) | ~400ns | ~200 bytes | DOM traversal overhead |
| JSON (rapidjson) | ~1-2µs | ~200 bytes | More allocations |
| SBE (`eph-sbe`, zero-copy) | ~55ns/ticker | ~50 bytes/ticker | parse + group walk + all fields + symbol (Graviton, `bench_sbe_book_ticker`) |
| SBE (real-logic codegen) | ~100-200ns | — | Accessor overhead |

For bookTicker at 10K updates/sec: JSON = 4-20ms/s CPU; SBE = 0.5ms/s CPU.
The difference matters when processing L2 depth (20 levels × higher frequency).

## 3. FIX 4.4 Market Data

ephemeral's `eph-fix` module handles message parsing, building, and full FIX 4.4
session management — Logon/Logout, automatic Heartbeat, TestRequest probing,
bidirectional `MsgSeqNum` gap detection, optional `ResendRequest`, and
`SequenceReset`/`GapFill`. See `eph-fix/README.md` for the session API.

### Build a MarketDataRequest

```cpp
// Build a MarketDataRequest (subscription)
uint8_t buf[512];
eph::fix::MessageBuilder b(buf, sizeof(buf));
b.set(eph::fix::tag::MsgType, "V");           // MarketDataRequest
b.set(eph::fix::tag::MDReqID, "md-001");
b.set_int(eph::fix::tag::SubscriptionRequestType, 1);  // 1 = Subscribe
b.set_int(eph::fix::tag::MarketDepth, 1);              // Top-of-Book
// ... add symbol, entry types, etc.
size_t len = b.finish("FIX.4.4");

// Parse an incoming MarketDataIncrementalRefresh
auto msg = eph::fix::parse(recv_buf, recv_len);
if (msg && msg->msg_type() == std::string_view("X")) {
    auto bid_px = msg->get(eph::fix::tag::MDEntryPx);
    // ...
}
```

### Session integration

Drive the session over a `KernelTcpStream<RawStreamCodec>` — `FixSession::on_rx()`
is fed from the stream's `on_message` callback, and `session.logon()` / `logout()`
handle the handshake. See the "Run a session" example in `eph-fix/README.md`.

### Recommendation

For Binance market data today:
- **Most users**: WebSocket JSON via `simple_hft.cpp` pattern — works now, good enough for most strategies
- **Low-latency production**: WebSocket SBE with the constexpr offset pattern above
- **Institutional/FIX infrastructure**: `eph-fix` session + parser/builder over `KernelTcpStream<RawStreamCodec>` (or DPDK equivalent)

## Auto-Culling vs EvictingQueue

Binance SBE streams support **auto-culling**: during high server load, the server discards intermediate events and delivers only the latest state. This is a server-side optimization.

ephemeral provides a **complementary client-side mechanism**:

| Feature | Binance Auto-Culling | ephemeral EvictingQueue |
|---------|---------------------|------------------------|
| Where | Server-side | Client-side (RX queue) |
| When | High server load | Consumer slower than producer |
| Granularity | Per-event | Per-message in queue |
| Control | None (automatic) | Configurable (queue depth, filter) |

**They're complementary, not redundant**:
- Auto-culling reduces wire traffic during server overload
- EvictingQueue ensures the application always sees the latest data even when the consumer thread is slower than the RX thread

Combined usage:
```cpp
// The Stream owns its codec; the user feeds frames into an
// EvictingQueue when the consumer thread is slower than the poller.
using WsStream = eph::net::kernel::KernelTcpStream<eph::codec::WsCodec, true>;

// Template params are <MaxDataSize, Capacity>. 512-byte payloads,
// 256 slots (the default). Both must fit a real bookTicker JSON
// frame plus a small headroom.
eph::containers::EvictingQueueBytes</*MaxDataSize=*/512,
                                    /*Capacity=*/256> latest_tick{};

auto stream = WsStream::create(cfg).value();
stream->on_message = [&](std::span<const uint8_t> app_frame) {
    // Drop older ticks — keep only the newest. `try_push` is the
    // single-arg span overload (forwards ts=0 to try_push_wts);
    // returns false only when the frame exceeds MaxDataSize.
    (void)latest_tick.try_push(app_frame);
};

// Server auto-culls during overload → fewer messages on wire
// EvictingQueue keeps only latest → consumer always reads freshest data
// Filter inside on_message to pick only the symbols you care about
```

This triple-layer filtering (server → codec → queue) ensures the application never
processes stale data, regardless of where the bottleneck occurs.

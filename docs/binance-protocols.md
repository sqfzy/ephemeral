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

SBE (Simple Binary Encoding) uses a fixed header + field offsets, similar to ITCH. ephemeral's existing byte-reading primitives (`read_be16`, `read_be32`, etc. from `eph/itch/messages.hpp`) work directly.

A typical SBE bookTicker message layout (based on Binance SBE Schema 3:2):

```cpp
// SBE message structure (illustrative — verify against Binance SBE XML schema)
//
// Header (8 bytes):
//   block_length: uint16 LE  — message body size
//   template_id:  uint16 LE  — message type (e.g., bookTicker = TBD)
//   schema_id:    uint16 LE  — schema ID (3)
//   version:      uint16 LE  — schema version (2)
//
// Body (bookTicker):
//   update_id:    uint64 LE  — order book update sequence
//   symbol:       char[16]   — right-padded symbol name
//   bid_price:    int64 LE   — best bid (fixed-point, 8 decimal places)
//   bid_qty:      int64 LE   — best bid quantity (fixed-point)
//   ask_price:    int64 LE   — best ask (fixed-point)
//   ask_qty:      int64 LE   — best ask quantity (fixed-point)
//   event_time:   uint64 LE  — microsecond timestamp

namespace binance_sbe {

// SBE header
inline constexpr size_t kHeaderSize = 8;

inline uint16_t block_length(const uint8_t* msg) noexcept {
    uint16_t v; std::memcpy(&v, msg, 2); return v;  // LE on LE machine
}
inline uint16_t template_id(const uint8_t* msg) noexcept {
    uint16_t v; std::memcpy(&v, msg + 2, 2); return v;
}

// bookTicker body (offsets from body start = msg + kHeaderSize)
namespace book_ticker {
    inline constexpr size_t kBodyOffset = kHeaderSize;

    inline uint64_t update_id(const uint8_t* msg) noexcept {
        uint64_t v; std::memcpy(&v, msg + kBodyOffset, 8); return v;
    }
    inline std::string_view symbol(const uint8_t* msg) noexcept {
        return {reinterpret_cast<const char*>(msg + kBodyOffset + 8), 16};
    }
    inline double bid_price(const uint8_t* msg) noexcept {
        int64_t v; std::memcpy(&v, msg + kBodyOffset + 24, 8);
        return v / 1e8;  // 8 decimal places
    }
    inline double bid_qty(const uint8_t* msg) noexcept {
        int64_t v; std::memcpy(&v, msg + kBodyOffset + 32, 8);
        return v / 1e8;
    }
    inline double ask_price(const uint8_t* msg) noexcept {
        int64_t v; std::memcpy(&v, msg + kBodyOffset + 40, 8);
        return v / 1e8;
    }
    inline double ask_qty(const uint8_t* msg) noexcept {
        int64_t v; std::memcpy(&v, msg + kBodyOffset + 48, 8);
        return v / 1e8;
    }
    inline uint64_t event_time_us(const uint8_t* msg) noexcept {
        uint64_t v; std::memcpy(&v, msg + kBodyOffset + 56, 8); return v;
    }
} // namespace book_ticker

} // namespace binance_sbe
```

**Important**: The layout above is illustrative. Obtain the actual SBE XML schema from Binance and adjust offsets accordingly. The pattern (constexpr offsets + memcpy + reinterpret) is identical to how `eph-itch` works.

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
    // Zero-copy SBE decode — ~50ns vs ~400ns-1µs for JSON
    const uint8_t* data = app_frame.data();
    auto bid = binance_sbe::book_ticker::bid_price(data);
    auto ask = binance_sbe::book_ticker::ask_price(data);
    auto ts  = binance_sbe::book_ticker::event_time_us(data);
    // process...
};
```

### Performance Comparison

| Encoding | Decode Time | Payload Size (BBO) | Notes |
|----------|------------|---------------------|-------|
| JSON (simdjson) | ~400ns | ~200 bytes | DOM traversal overhead |
| JSON (rapidjson) | ~1-2µs | ~200 bytes | More allocations |
| SBE (zero-copy) | ~50ns | ~72 bytes | Fixed-offset memcpy only |
| SBE (real-logic codegen) | ~100-200ns | ~72 bytes | Accessor overhead |

For bookTicker at 10K updates/sec: JSON = 4-20ms/s CPU; SBE = 0.5ms/s CPU.
The difference matters when processing L2 depth (20 levels × higher frequency).

## 3. FIX 4.4 Market Data

ephemeral's `eph-fix` module handles message parsing and building. FIX Market Data subscription requires FIX session management (Logon, Heartbeat, sequence numbers) which is **not yet implemented**.

### What works today

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

### What's missing

FIX session management (required before any market data flows):
- **Logon** (MsgType=A) with encryption method, heartbeat interval
- **Heartbeat** (MsgType=0) / TestRequest (MsgType=1) keepalive
- **Sequence number** tracking (MsgSeqNum tag 34) for gap detection
- **Resend Request** (MsgType=2) for missed messages
- **GapFill** (MsgType=4) for sequence number synchronization

This is a 40-80 hour project requiring formal design. Use `/design FIX session layer` when ready.

### Recommendation

For Binance market data today:
- **Most users**: WebSocket JSON via `simple_hft.cpp` pattern — works now, good enough for most strategies
- **Low-latency production**: WebSocket SBE with the constexpr offset pattern above
- **Institutional/FIX infrastructure**: Wait for FIX session layer, or implement session management at application level using `eph-fix` parser/builder

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

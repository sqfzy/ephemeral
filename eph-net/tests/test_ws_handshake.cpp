/// @file test_ws_handshake.cpp
/// Unit tests for `eph::net::detail::perform_ws_handshake` using a fake
/// in-memory ByteSink — no sockets, no threads. Covers the success path
/// and every defined failure mode.


#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/net/detail/ws_handshake.hpp"
#include "eph/net/http.hpp"

using eph::core::Error;
using eph::core::ErrorInfo;
using eph::net::HttpHeader;
using eph::net::detail::perform_ws_handshake;
using eph::net::detail::ws_compute_accept;

namespace {

// ─────────────────────────────────────────────────────────────────────────
// FakeByteSink — records every byte sent by the caller and serves a
// caller-configured script of recv chunks. If the script runs out before
// the caller finishes recving we return WouldBlock (which
// perform_ws_handshake retries until its deadline trips) or Disconnected
// (which bubbles straight up) depending on test intent.
// ─────────────────────────────────────────────────────────────────────────
struct FakeByteSink {
    std::vector<uint8_t> tx;       ///< everything the caller sent, concatenated
    std::vector<uint8_t> rx_script;///< bytes the recv() calls will deliver
    size_t               rx_off{0};///< how many bytes of rx_script already served
    bool                 recv_blocks{false};   ///< if true, recv returns WouldBlock
    bool                 recv_disconnect{false};///< if true, recv returns Disconnected

    std::expected<size_t, ErrorInfo>
    send(std::span<const uint8_t> data) noexcept {
        tx.insert(tx.end(), data.begin(), data.end());
        return data.size();
    }

    std::expected<size_t, ErrorInfo>
    recv(uint8_t* buf, size_t cap) noexcept {
        if (recv_disconnect) {
            return std::unexpected(ErrorInfo{Error::Disconnected,
                                              "FakeByteSink: scripted disconnect"});
        }
        if (recv_blocks || rx_off >= rx_script.size()) {
            return std::unexpected(ErrorInfo{Error::WouldBlock,
                                              "FakeByteSink: scripted would-block"});
        }
        const size_t avail = rx_script.size() - rx_off;
        const size_t n     = std::min(cap, std::min<size_t>(avail, 64));
        std::memcpy(buf, rx_script.data() + rx_off, n);
        rx_off += n;
        return n;
    }
};

// Extract the Sec-WebSocket-Key the caller sent in its request. We do a
// naive substring search — the request is always well-formed (we built it)
// so we can rely on the header name appearing verbatim.
std::string extract_sent_key(const std::vector<uint8_t>& tx) {
    std::string_view s(reinterpret_cast<const char*>(tx.data()), tx.size());
    constexpr std::string_view needle = "Sec-WebSocket-Key: ";
    auto p = s.find(needle);
    if (p == std::string_view::npos) return {};
    p += needle.size();
    auto e = s.find("\r\n", p);
    if (e == std::string_view::npos) return {};
    return std::string(s.substr(p, e - p));
}

// Build a well-formed 101 response with caller-supplied Accept, Upgrade,
// and Connection values.
std::vector<uint8_t> make_response(std::string_view accept,
                                   std::string_view upgrade    = "websocket",
                                   std::string_view connection = "Upgrade",
                                   int              status     = 101,
                                   std::string_view reason     = "Switching Protocols",
                                   std::string_view extensions = {}) {
    std::string s;
    s += "HTTP/1.1 " + std::to_string(status) + " ";
    s += std::string(reason);
    s += "\r\n";
    s += "Upgrade: ";  s += std::string(upgrade);    s += "\r\n";
    s += "Connection: "; s += std::string(connection); s += "\r\n";
    s += "Sec-WebSocket-Accept: "; s += std::string(accept); s += "\r\n";
    if (!extensions.empty()) {
        s += "Sec-WebSocket-Extensions: "; s += std::string(extensions); s += "\r\n";
    }
    s += "\r\n";
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// 1. Success path — a correctly-formed 101 response validates cleanly
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, HappyPathReturnsOk) {
    FakeByteSink io;
    // We need to know the key first to precompute the Accept. Drive the
    // handshake in two passes: pass 1 captures the key, pass 2 supplies
    // the matching response. (perform_ws_handshake generates a fresh
    // random key each call, so we must observe it via FakeByteSink.tx.)
    //
    // Approach: configure rx_script AFTER the first send() has populated
    // `tx`. We simulate this by starting with `recv_blocks=true`, running
    // a mini driver, then swapping in the response. Instead, a simpler
    // alternative: record key from tx first via a zero-recv dry-run.
    //
    // Simplest path: use a driver that interleaves send→build response→recv.
    // Since perform_ws_handshake is synchronous we cannot interleave from
    // outside. We instead pre-load a DUMMY response, let the handshake
    // fail, then re-run with the captured key. Cleaner option: compute
    // the Accept AFTER we observe tx — but the handshake already failed
    // the first time. Use a two-phase FakeByteSink that swaps the
    // response script between the send() call and the first recv().
    //
    // The FakeByteSink we defined serves rx_script linearly — we can
    // populate it from a custom send override. Easiest: subclass inline.
    struct Swapping : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            // After the request is fully buffered, build the response.
            if (rx_script.empty()) {
                std::string_view s(reinterpret_cast<const char*>(tx.data()),
                                    tx.size());
                if (s.find("\r\n\r\n") != std::string_view::npos) {
                    auto key = extract_sent_key(tx);
                    auto acc = ws_compute_accept(key);
                    rx_script = make_response(acc);
                }
            }
            return r;
        }
    };

    Swapping sink;
    auto r = perform_ws_handshake(sink, "example.com", "/ws/feed",
                                    {}, std::chrono::seconds{1});
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);

    // Request wire check: method/target/host/mandatory headers present.
    std::string_view sent(reinterpret_cast<const char*>(sink.tx.data()),
                           sink.tx.size());
    EXPECT_NE(sent.find("GET /ws/feed HTTP/1.1\r\n"), std::string_view::npos);
    EXPECT_NE(sent.find("Host: example.com\r\n"), std::string_view::npos);
    EXPECT_NE(sent.find("Upgrade: websocket\r\n"), std::string_view::npos);
    EXPECT_NE(sent.find("Connection: Upgrade\r\n"), std::string_view::npos);
    EXPECT_NE(sent.find("Sec-WebSocket-Version: 13\r\n"),
              std::string_view::npos);
    EXPECT_NE(sent.find("Sec-WebSocket-Key: "), std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 2. Sec-WebSocket-Accept is computed as SHA1(key + magic) + base64
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, AcceptDigestMatchesRfc6455Vector) {
    // RFC 6455 §1.3 worked example:
    //   key    = "dGhlIHNhbXBsZSBub25jZQ=="
    //   accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    std::string got = ws_compute_accept("dGhlIHNhbXBsZSBub25jZQ==");
    EXPECT_EQ(got, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// REGRESSION: ws_compute_accept must not overflow its scratch buffer when
// fed an adversarial-length client_key. The function previously copied
// `min(key.size(), 32)` bytes plus the 36-byte magic GUID into a 64-byte
// scratch buffer — total up to 68 bytes → 4-byte stack-buffer overflow on
// any key in [29..32] bytes. Production callers in `ws_handshake.hpp`
// always feed a fixed-length 24-byte base64 nonce, but server-side users
// (`mockex/ws_server.hpp::accept_handshake`) extract the key directly
// from the client's HTTP request without length validation, so the over-
// flow was reachable from attacker-controlled bytes. The fix sizes the
// scratch from `kWsMagicGuid.size() + max client-key budget` and asserts
// the bound at compile time, eliminating both the overflow and the
// implicit-bound mismatch between the `min(_, 32)` and `scratch[64]`.
TEST(WsHandshake, ComputeAcceptDoesNotOverflowOnLongKey) {
    // 32-byte key — at the previous (buggy) clamp.
    {
        std::string key(32, 'A');
        // Must not crash / corrupt stack. Result content is unspecified
        // (the function will still compute SHA-1 over whatever bytes
        // it deems "the key"); we only verify no UB and a sane base64
        // length.
        std::string got = ws_compute_accept(key);
        EXPECT_EQ(got.size(), 28u);  // base64(SHA1(...)) = 28 chars
    }
    // 64-byte key — well past any RFC 6455 §4.1 expectation. A correct
    // implementation must either truncate or hash the over-long input
    // into the same fixed 64-byte SHA-1 input block; either way no UB.
    {
        std::string key(64, 'B');
        std::string got = ws_compute_accept(key);
        EXPECT_EQ(got.size(), 28u);
    }
    // 1024-byte key — pathological; must still terminate and produce a
    // 28-char base64 digest with no buffer overflow.
    {
        std::string key(1024, 'C');
        std::string got = ws_compute_accept(key);
        EXPECT_EQ(got.size(), 28u);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// 3. Non-101 status rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, NonOneHundredOneStatusRejected) {
    struct Bad : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)),
                                           "websocket", "Upgrade", 200, "OK");
            }
            return r;
        }
    };
    Bad sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 4. Missing Upgrade header rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, MissingUpgradeHeaderRejected) {
    struct NoUp : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                // Response with Upgrade replaced by a wrong value.
                std::string s = "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: http2\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: ";
                s += ws_compute_accept(extract_sent_key(tx));
                s += "\r\n\r\n";
                rx_script.assign(s.begin(), s.end());
            }
            return r;
        }
    };
    NoUp sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 5. Missing "Connection: Upgrade" rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, MissingConnectionUpgradeRejected) {
    struct NoConn : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)),
                                           "websocket", "keep-alive");
            }
            return r;
        }
    };
    NoConn sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 5b. Case-insensitive "Connection: upgrade" accepted
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, ConnectionCaseInsensitiveAccepted) {
    struct Mixed : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)),
                                           "WebSocket", "keep-alive, UpGrAdE");
            }
            return r;
        }
    };
    Mixed sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
}

// ═══════════════════════════════════════════════════════════════════════
// 6. Wrong Sec-WebSocket-Accept rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, WrongSecWebSocketAcceptRejected) {
    struct BadAcc : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response("ZGVsaWJlcmF0ZWx5X3dyb25nX3ZhbHVlPQ==");
            }
            return r;
        }
    };
    BadAcc sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 7. Parser error in response (malformed status line) → WsHandshakeFailed
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, ParserErrorInResponseRejected) {
    struct Malformed : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                const char bad[] =
                    "HTTP/9.9 101 Switching Protocols\r\n\r\n";
                rx_script.assign(bad, bad + sizeof(bad) - 1);
            }
            return r;
        }
    };
    Malformed sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 8. Timeout waiting for response → Error::Timeout
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, ReadDeadlineTriggersTimeout) {
    FakeByteSink sink;
    sink.recv_blocks = true;
    // Short deadline — 50ms is enough to build the request and then fail
    // the read phase quickly.
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::milliseconds{50});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::Timeout);
}

// ═══════════════════════════════════════════════════════════════════════
// 9. extra_headers are serialized into the request
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, ExtraHeadersAppear) {
    struct Good : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)));
            }
            return r;
        }
    };
    Good sink;
    HttpHeader extras[] = {
        {"Authorization", "Bearer abc"},
        {"X-Custom",      "ok"},
    };
    auto r = perform_ws_handshake(sink, "h", "/ws",
                                    std::span<const HttpHeader>(extras, 2),
                                    std::chrono::seconds{1});
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
    std::string_view s(reinterpret_cast<const char*>(sink.tx.data()),
                        sink.tx.size());
    EXPECT_NE(s.find("Authorization: Bearer abc\r\n"), std::string_view::npos);
    EXPECT_NE(s.find("X-Custom: ok\r\n"), std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 9b. Extra headers that collide with a mandatory WebSocket header are
//     rejected with Error::WsHandshakeFailed before any bytes leave the
//     sink. Added in batch3-round4 MEDIUM-1: a caller passing
//     `{"Upgrade", "h2c"}` or a second `Host` previously emitted a
//     duplicate header, which most servers reject with a 400 and the
//     client could only see a confusing parse error.
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, ExtraHeaderCollidingWithMandatoryNameRejected) {
    for (std::string_view colliding_name : {
            "Host", "Upgrade", "Connection",
            "Sec-WebSocket-Key", "Sec-WebSocket-Version",
            // case-insensitive rejection
            "host", "UPGRADE", "sec-websocket-key"}) {
        FakeByteSink sink;
        HttpHeader extras[] = {
            {colliding_name, "clash"},
        };
        auto r = perform_ws_handshake(sink, "h", "/ws",
                                        std::span<const HttpHeader>(extras, 1),
                                        std::chrono::seconds{1});
        ASSERT_FALSE(r.has_value())
            << "extra header '" << colliding_name
            << "' should have been rejected";
        EXPECT_EQ(r.error().code, eph::core::Error::WsHandshakeFailed);
        EXPECT_TRUE(sink.tx.empty())
            << "no bytes should have been sent when the header check fails";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// 10. Host header is set from the caller argument
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, HostHeaderFromArgument) {
    struct Good : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)));
            }
            return r;
        }
    };
    Good sink;
    auto r = perform_ws_handshake(sink, "stream.binance.com",
                                    "/ws/btcusdt@bookTicker",
                                    {}, std::chrono::seconds{1});
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
    std::string_view s(reinterpret_cast<const char*>(sink.tx.data()),
                        sink.tx.size());
    EXPECT_NE(s.find("Host: stream.binance.com\r\n"), std::string_view::npos);
    EXPECT_NE(s.find("GET /ws/btcusdt@bookTicker HTTP/1.1\r\n"),
              std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 11. Leftover post-handshake bytes are captured for caller consumption
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, LeftoverBytesCapturedForCaller) {
    struct WithTail : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)));
                // Append 5 "leftover" bytes that belong to the next layer.
                const uint8_t tail[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xAA};
                rx_script.insert(rx_script.end(), tail, tail + sizeof(tail));
            }
            return r;
        }
    };
    WithTail sink;
    std::vector<uint8_t> leftover;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1}, &leftover);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
    ASSERT_EQ(leftover.size(), 5u);
    EXPECT_EQ(leftover[0], 0xDE);
    EXPECT_EQ(leftover[4], 0xAA);
}

// ═══════════════════════════════════════════════════════════════════════
// 12. Server-initiated extensions (permessage-deflate) rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, ServerExtensionsRejected) {
    struct WithExt : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(
                    ws_compute_accept(extract_sent_key(tx)),
                    "websocket", "Upgrade", 101,
                    "Switching Protocols", "permessage-deflate");
            }
            return r;
        }
    };
    WithExt sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

TEST(WsHandshake, ServerExtensionsRejectedEvenUnknown) {
    struct WithExt : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(
                    ws_compute_accept(extract_sent_key(tx)),
                    "websocket", "Upgrade", 101,
                    "Switching Protocols", "x-some-unknown-extension");
            }
            return r;
        }
    };
    WithExt sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 13. permessage-deflate (RFC 7692) negotiation
// ═══════════════════════════════════════════════════════════════════════

namespace {

struct DeflateSink : FakeByteSink {
    std::string ext_response;
    std::expected<size_t, ErrorInfo>
    send(std::span<const uint8_t> data) noexcept {
        auto r = FakeByteSink::send(data);
        if (rx_script.empty()) {
            rx_script = make_response(
                ws_compute_accept(extract_sent_key(tx)),
                "websocket", "Upgrade", 101,
                "Switching Protocols",
                ext_response.empty() ? std::string_view{}
                                      : std::string_view{ext_response});
        }
        return r;
    }
};

} // namespace

TEST(WsHandshake, PermessageDeflateRequestInjectsHeader) {
    // Server omits the Extensions header — handshake succeeds, deflate
    // not negotiated, but the request must still have included our
    // auto-injected offer.
    DeflateSink sink;
    sink.ext_response = "";
    eph::net::detail::WsHandshakeDeflate state{};
    state.request = true;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                  std::chrono::seconds{1},
                                  nullptr, &state);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_FALSE(state.negotiated);
    std::string sent(reinterpret_cast<const char*>(sink.tx.data()),
                      sink.tx.size());
    EXPECT_NE(sent.find("Sec-WebSocket-Extensions: permessage-deflate"),
              std::string::npos)
        << "request did not include the auto-injected header. Sent:\n"
        << sent;
}

TEST(WsHandshake, PermessageDeflateAccepted) {
    DeflateSink sink;
    sink.ext_response = "permessage-deflate";
    eph::net::detail::WsHandshakeDeflate state{};
    state.request = true;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                  std::chrono::seconds{1},
                                  nullptr, &state);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_TRUE(state.negotiated);
    EXPECT_FALSE(state.server_no_context_takeover);
}

TEST(WsHandshake, PermessageDeflateAcceptedWithNoCtxTakeover) {
    DeflateSink sink;
    sink.ext_response = "permessage-deflate; server_no_context_takeover";
    eph::net::detail::WsHandshakeDeflate state{};
    state.request = true;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                  std::chrono::seconds{1},
                                  nullptr, &state);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_TRUE(state.negotiated);
    EXPECT_TRUE(state.server_no_context_takeover);
}

TEST(WsHandshake, PermessageDeflateClientNoCtxTakeoverAccepted) {
    // Server adding `client_no_context_takeover` is informational on
    // our side (we never deflate outbound). Accept silently.
    DeflateSink sink;
    sink.ext_response = "permessage-deflate; client_no_context_takeover";
    eph::net::detail::WsHandshakeDeflate state{};
    state.request = true;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                  std::chrono::seconds{1},
                                  nullptr, &state);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_TRUE(state.negotiated);
}

TEST(WsHandshake, PermessageDeflateServerMaxBitsNon15Rejected) {
    // server_max_window_bits=10 means the server wants a smaller LZ77
    // window than zlib's -MAX_WBITS (=15) inflater is configured for.
    // Conservative path: fail-closed.
    DeflateSink sink;
    sink.ext_response = "permessage-deflate; server_max_window_bits=10";
    eph::net::detail::WsHandshakeDeflate state{};
    state.request = true;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                  std::chrono::seconds{1},
                                  nullptr, &state);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
    EXPECT_FALSE(state.negotiated);
}

TEST(WsHandshake, PermessageDeflateUnknownParamRejected) {
    DeflateSink sink;
    sink.ext_response = "permessage-deflate; some_future_param=42";
    eph::net::detail::WsHandshakeDeflate state{};
    state.request = true;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                  std::chrono::seconds{1},
                                  nullptr, &state);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

TEST(WsHandshake, PermessageDeflateOtherExtensionRejected) {
    DeflateSink sink;
    sink.ext_response = "x-webkit-deflate-frame";
    eph::net::detail::WsHandshakeDeflate state{};
    state.request = true;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                  std::chrono::seconds{1},
                                  nullptr, &state);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

TEST(WsHandshake, PermessageDeflateUserSuppliedHeaderNotDuplicated) {
    // Caller already supplied a Sec-WebSocket-Extensions header — the
    // helper must NOT inject a second one. Per RFC 7230 §3.2.2 a
    // duplicate Sec-WebSocket-Extensions on the request side is a
    // foot-gun that's much easier to avoid here than at the receiver.
    DeflateSink sink;
    sink.ext_response = "permessage-deflate";
    HttpHeader user_hdrs[] = {
        {"Sec-WebSocket-Extensions",
         "permessage-deflate; client_max_window_bits=15"},
    };
    eph::net::detail::WsHandshakeDeflate state{};
    state.request = true;
    auto r = perform_ws_handshake(
        sink, "h", "/ws",
        std::span<const HttpHeader>{user_hdrs, 1},
        std::chrono::seconds{1}, nullptr, &state);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_TRUE(state.negotiated);
    std::string sent(reinterpret_cast<const char*>(sink.tx.data()),
                      sink.tx.size());
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = sent.find("Sec-WebSocket-Extensions:", pos))
           != std::string::npos) {
        ++count;
        ++pos;
    }
    EXPECT_EQ(count, 1u) << "request emitted multiple "
                            "Sec-WebSocket-Extensions headers:\n" << sent;
}
